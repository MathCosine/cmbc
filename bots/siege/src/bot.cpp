// Champion decisions. main.cpp dispatches here after the draft.
//
// Turn priorities: enemy champions, then awake camps (rotating out before a
// hit could kill), then going home, tributing, capturing and farming.
// Farming is a squad plan shared through team memory: everyone converges on
// one dangerous group and only engages once the party nearby can clear it,
// while gem and gold clusters (no damage) are claimed by single champions.
// Camps never heal, so groups too strong for one fight (monoliths, or the camps
// walling in every altar) are besieged: the squad wears them down over a few
// trips, rotating home to heal in between.
#include "bot.h"
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

namespace {
// Debug builds record fuel at each phase change to find expensive turns.
struct FuelMark {
    const char* phase;
    std::uint64_t fuel;
};
std::vector<FuelMark> g_marks;
void mark(const char* phase) {
    g_phase = phase;
    debug_block({ g_marks.push_back({phase, bc::fuel_remaining()}); });
}
} // namespace

namespace {
constexpr float kGoldWeight = 0.15f;
constexpr int kCaptureRounds = 5;
constexpr int kReplanRounds = 8;
// Camps never heal, so a group too strong for one fight can be worn down over
// several trips home to heal. Each trip spends about this share of team health.
constexpr float kTripShare = 0.45f;
constexpr float kMaxTrips = 4.f;
constexpr int kSiegeFrom = 500;  // rounds before sieges (other than walled-in altars)
constexpr float kMaxUnlockTrips = 8.f;  // opening a walled-in altar is worth more
// Damage taken has to be healed later: a fight costing the whole team's health
// costs roughly this many trips home and back on top of its own rounds.
constexpr float kHealCost = 1.0f;
// Tribute once the bag holds this many gems (or an altar is close): fewer trips, more farming.
constexpr int kCarryGems = 40;
// Planning assumes this share of the paper damage: travel, retreats and stragglers
// halve what a squad really deals (a golem group forecast at 19 rounds took 70).
// Rough share of the team's damage that lands while fighting camps. 0.6 made big camps near our
// spawn look like sieges early (v11: 0.8; with the loop and siege changes below, the ladder
// scrim against intercontinental fish turned in 30% more gems than v9, 717 against 443 on
// 01big-world where nobody fought).
constexpr float kFightEfficiency = 0.8f;
constexpr int kRewardTimeout = 8;  // rounds a reward out of sight is assumed to stay uncaptured
constexpr std::uint64_t kPlanFuel = 45000;  // fuel needed before optional planning work
// Fuel left when target planning stops looking at more camps. Running out mid-turn does not end
// the turn: the rest of it runs next round on that round's fuel (and that round's planning is
// lost). On 00lanes the solo loop spent ~2,500 a camp before reaching its old check at 12,000 and
// ran dry 45 times a game. kSoloFuelFirst checks first thing in each pass. Out of 140, solo fix:
// 78 against v19, 120 against a Cassi & Issac team, 88 against fishsim (v19: 113/93); farming
// 1,136 against 1,154 rounds to 1,000 gems. A 30,000 squad floor as well: 70/117/86.
constexpr std::uint64_t kSquadPlanFloor = 20000;
constexpr std::uint64_t kSoloPlanFloor = 20000;
constexpr bool kSoloFuelFirst = true;
constexpr std::uint64_t kSetupFuel = 60000;  // left after building the map to finish setup this turn
constexpr std::uint64_t kActFuel = 50000;    // left after setup to act this turn

// Staying alive. Most deaths come from enemy champions, mostly alone, and every one costs
// 60 rounds out plus the walk back. Each switch was tested on its own against seven local
// sparring bots (560 games) and head to head against the version without them. Only careful
// mode helped (469 vs 454, 42/80); the others cost more farming than the deaths they saved
// and are off: escort 439, danger memory 428, contested 451, small bags 451, burst 432,
// shunning the enemy half 451. All seven together farmed 10% slower even with no enemy around.
// After two team deaths, a champion that has just respawned waits on spawn for a teammate
// instead of walking back out alone (28 of 61 deaths on hooks came right after a respawn).
constexpr bool kEscort = true;
constexpr int kEscortWindow = 120;  // team deaths this recent mean the enemy is winning fights
constexpr int kEscortWait = 40;     // longest wait on spawn before going anyway
// Targets near where we died lately, or where 2+ enemies were seen, score lower; a squad
// target we died beside is dropped, so the whole squad moves on, not just us.
constexpr bool kDangerMemory = false;
// Camps about as far from the enemy spawn as from ours are where both teams meet.
constexpr bool kContested = false;
constexpr float kContestedFactor = 0.7f;
// A team that keeps killing us (3+ deaths in 150 rounds) gets a careful game: small bags,
// our own half, earlier trips home, no even fights.
constexpr bool kPressureMode = true;
constexpr int kPressureDeaths = 3;
constexpr int kPressureWindow = 150;
// Smaller bags: the killer takes the whole bag. Bank at 20 gems (10 with enemies seen in
// the last 20 rounds), bank a 50+ bag before fighting camps, and share big rewards.
constexpr bool kSmallBags = false;
constexpr int kSmallCarry = 20;
constexpr int kRecentEnemy = 20;
constexpr int kSmallBag = 10;
constexpr int kBigBag = 50;
// Judge a fight by what can land on us next turn (spell nukes included), not averaged damage.
constexpr bool kBurstModel = false;
// Camps nearer the enemy spawn: score x0.35 solo and x0.45 for the squad (0.6 and 0.7 before).
constexpr bool kShunEnemySide = false;
// Stand and fight. Ladder games showed our champions landing a hit in under half of their
// fight rounds (ladder teams: 55-79%): with an enemy within 7 tiles they kept farming 43% of
// the time, stepped away 21% (median health 58%) and chased 16%, and a ranged champion that
// moves cannot attack. With this on: count allies as far out as enemies (8 tiles), retreat only
// when in danger or clearly losing, fight back instead of fleeing an adjacent melee champion,
// engage enemies within 5 tiles, and let ranged enemies step into our range rather than step
// into theirs (unless a teammate is on them or they are nearly dead).
// On the ladder (14 games) this doubled our kills per game (3.1 to 6.4), evened champion
// damage (17k-35k a game before, 26.5k-28k after) and turned fights where we outnumbered
// the enemy from 19-62 into 27-18. Locally it looked worse: the sparring bots share our
// fight code, so they cannot show what a better fighter gains against the ladder.
constexpr bool kStandAndFight = true;
// Outnumbered fights were still 7 won, 71 lost (97 of our deaths against 20): with more
// enemies than allies within kCrowdRange, do not engage, and leave once they come close.
constexpr bool kAvoidOutnumbered = true;
constexpr int kCrowdRange = 6;
// Fights near camps: camps hit the nearest champion, and an enemy can wake one and leave us
// nearest. Retreats step out of awake camps' reach too, and a ranged champion does not hold
// its ground while a camp is hitting it (camp deaths rose to 1.2 a game with v3).
constexpr bool kCampAwareFights = true;
// Rounds a squad waits at its target for teammates before giving it up for another. On maps
// with long walks between targets (corridors) 20 rounds sent the squad back and forth.
constexpr int kStallRounds = 60;
// Safe opening. Games are decided early: our first death came at round 453 (median) in wins and
// round 58 in losses. In small-map losses 60% of our deaths before round 250 were at contested
// camps and 25% on the enemy side, while the teams beating us farmed their own half first.
// Until kOpeningRounds, contested and enemy-side camps are taken only when nothing else is left.
constexpr bool kSafeOpening = true;
constexpr int kOpeningRounds = 250;
// Soft opening (v12): contested camps count at kOpeningContested of their score in the opening
// instead of only as a fallback; enemy-side camps stay out.
constexpr bool kSoftOpening = true;
constexpr float kOpeningContested = 0.35f;
// Contested gem clusters (no fight, quick) in the opening. v21 lost 0-1,002 on 00lost-world and
// 0-1,001 on 02treasure-box to a team (closedai, our own draft) that farmed the middle cluster
// field from round 35 and banked at the altar inside it while ours went round the edges.
// Out of 140 against v21 / a Cassi & Issac team / fishsim (v21 70/115/86): clusters 1.0 71/114/91;
// both 0.7 75/113/86; both 1.0 74/116/86; gems by round 300 with no opponent 190 and 199 against 180.
// Neither got the squad into the middle sooner: the squad's camp pass skips clusters altogether
// (they are only a fallback, and solo work is capped at a third of the team).
// kSquadClusters: clusters compete with fighting camps in the squad's pass.
// kClusterSolos: up to all but two champions may take cluster jobs on their own.
// Out of 140 (as above), all with clusters at 1.0: kSquadClusters 84/118/97 and 111 against 106
// against hunter (on); first middle clear on 00lost-world at round 28-41 (was 71-165), 218 against
// 180 gems by round 300 with no opponent, farming 1,061 against 1,059. kClusterSolos 79/119/90
// (farming 1,042); both 78/120/99, 109 against hunter.
constexpr float kOpeningClusters = 1.0f;
constexpr bool kSquadClusters = true;
constexpr bool kClusterSolos = false;
// v22 against closedai (1-3): on 00lost-world 25 deaths, most of them alone near the middle they
// hold. kSoloSafe: no solo job within 8 tiles of where we saw enemies in the last kSoloDangerRounds
// or died in the last 150, nor on contested ground with enemies seen in the last 30 rounds: those
// are for the squad. kHoldMiddle: with enemies seen around a middle or enemy-side altar lately,
// camps beside it score kHoldBonus more for the squad, which goes in together and stays.
// kClusterPlanFloor: the squad only weighs clusters with this much fuel left (squad clusters took
// fuel-outs from 6 to 8 a game; 60 on a ladder 02treasure-box).
// With the cluster fuel guard, out of 140 against v22 / a Cassi & Issac team / fishsim / hunter:
// 71/119/100/111 (v22 70/118/97/111), 85 against 82 against a middle-holding stand-in (on).
constexpr bool kSoloSafe = true;
constexpr int kSoloDangerRounds = 40;
constexpr bool kHoldMiddle = true;
constexpr float kHoldBonus = 1.5f;
constexpr std::uint64_t kClusterPlanFloor = 35000;
// v23 test against Inter Fish (0-5, 0-1,037 on 00vault, 0-1,120 on 00stampede): we won 2 of 77
// fights and lost 81 champions to their 4; even outnumbering them we dealt 22k to their 37k, their
// Marcella 106 a fight round to ours 36, Vesper Vale 69 to 19 (twice our gold by round 300). The
// fight model kept taking those fights. kLearnEnemy: each champion compares the champion damage it
// takes (turns with no camp on it) with its share of what the model expected the enemies in range
// to deal, and scales enemy damage by the ratio (never below 1, at most kMaxEnemyScale; a prior of
// kLearnPrior expected damage keeps it at 1 until the evidence builds up).
// Locally we take 40-60% of the modelled damage (enemies move, pick other targets, wait on
// cooldowns), so the ratio is taken against kLearnBase of it.
// kDeathScale: enemy damage counts kDeathStep more per teammate lost in the last kDeathWindow
// rounds (team deaths are shared through the status bytes, so the whole team turns careful).
constexpr bool kLearnEnemy = false;
constexpr float kLearnPrior = 2000.f;
constexpr float kLearnBase = 0.55f;
// kDeathScale, out of 140 against the v23 test build / Cassi & Issac / fishsim / hunter: 73/119/92/109
// (base 70/115/92/109) (on); kLearnEnemy 73/115/94/103 (off).
constexpr bool kDeathScale = true;
constexpr float kDeathStep = 0.12f;
constexpr int kDeathWindow = 200;
constexpr float kMaxEnemyScale = 2.5f;
// v24 against Inter Fish (0-5 again, 80 deaths to 2): outnumbered in 31 of 89 fights (lost 26),
// and half our deaths with no teammate within 7 tiles, while all five of theirs held the cluster
// field. kFieldCamp: a gem field (kFieldGroups+ cluster groups with kFieldGems+ gems within 7 of an
// altar, or one cluster block of 100+ gems) in the middle or on our side becomes the team's camp:
// its groups score kFieldBonus times more for the squad, and no solo job is taken outside it.
// kStunDiscipline: a stun on an enemy that cannot move already (stunned) or cannot be stunned
// (Marcella) is wasted: our three stunners used to fire together at the team's focus. Pure stuns
// wait for another target or for the current stun to wear off; damaging stuns go to a free target
// when there is one. (Their stun-immune Marcella killed 34 of ours in the v24 Inter Fish games.)
constexpr bool kStunDiscipline = false;
// Out of 140 (against the v25 test build / Cassi & Issac / fishsim / hunter): 70/120/92/111 against
// 76/125/95/108: "cannot move" probably also shows on an enemy that has used its move this turn,
// not only a stunned one. kStunOverlap off keeps only the immunity part.
constexpr bool kStunOverlap = true;
// kStunChain: our two stun spells (Tavi's damaging stun, Ryth's area stun) used to fire together.
// With the other within 6 tiles, Tavi stuns only on even rounds and Ryth only on odd ones, so the
// 1-turn stuns land on consecutive turns. kCombatDash: dashes (Vendetta, Vesper Vale; 2 tiles)
// were used only to travel. Now Vendetta dashes in to finish an enemy just out of reach that one
// turn's hits can kill (her kill reset makes the dash ready again), and anyone in danger while
// retreating dashes to the reachable tile furthest from the enemies.
// Out of 140 against the v26 test build / Cassi & Issac / fishsim / hunter (base 70/121/93/121):
// kStunChain 62/121/105/111 (it also holds when the partner's stun is on cooldown, which the team
// cannot see); kCombatDash 70/122/96/118 (fires 1-13 times a game); kFightBuild 67/119/103/117. Off.
constexpr bool kFightBuild = false;  // Durability first in every build (see build_for)
constexpr bool kStunChain = false;
// Route plan look ahead (kRoutePlan in plan.h): the best kLookTop camps are compared on the best
// next camp among their kLookNear nearest; a camp with nothing worth doing next counts kLookDeadEnd.
// kRouteAhead: a champion far behind its squad (more than kAheadWalk tiles and kAheadMargin more
// than the squad needs to finish its camp, two teammates there) walks to the camp the plan says
// comes next and waits outside its reach, instead of arriving at a finished camp.
constexpr bool kRouteAhead = false;
// kCaptureTimeout: a capture that has not landed after this many rounds is given up and its camp
// avoided for 150 rounds. On 01yellow-brick-road a champion "captured" a 600-gold strip beside the
// enemy spawn for 360+ rounds: enemies standing in its region kept it from ever completing.
constexpr int kCaptureTimeout = 40;  // 0 = off
// Recall under fire. A recall takes 5 turns, any damage interrupts it, and starting one uses the
// turn. go_home only checked for enemies in sight, so a champion hit from a bush started a recall,
// was hit, started it again, and stood still until it died (ladder, rounds 162-170: Marcella from
// 976 health to 0 beside tall grass, never moving or hitting back). Against fishsim a third of recall
// starts (7 a game) came within 2 turns of taking damage. kRecallSafe: no recall within kRecallCalm
// turns of taking damage (1: any damage; 2: more than the awake camps in reach deal, so enemy
// champions); walk home instead.
// Out of 140 against v28 / Cassi & Issac / fishsim / hunter (v28 70/126/106/119, an equivalent build
// 68/123/109/120): mode 1 79/116/100/122, farming 1,103 against 1,088. On top of v29 (v29
// 70/118/107/121, farming 1,103): mode 1 72/112/98/127, 1,110; mode 2 62/124/105/123, 1,122. Mode 1
// lost 6-9 to both fish bots on both bases (it also skips recalls after camp hits, which almost
// always come from a camp just killed); mode 2 is even locally, where no bot ambushes from bushes.
constexpr int kRecallSafe = 2;
constexpr int kRecallCalm = 2;
// Fight inputs (v31 tests).
// kEngageFloor: below this share of our health we no longer walk into a fight (engage an enemy up
// to 5 tiles out, join a teammate's fight); we still hit whatever is already in reach. Health only
// counted against incoming damage, so a champion at 30% with nobody on it yet walked in. 0 = off.
constexpr float kEngageFloor = 0.4f;
// Grass hides whoever stands in it from everyone outside the patch (GameRules: bushes block sight).
// kUnseenWard: hit by an enemy champion when no enemy in sight could have reached us, drop our ward
// on the spot within 2 tiles that sees the most hidden grass, so the attacker shows.
// kHurtFlee: hurt enough to go home and hit by champions in the last kFleeMemory turns: no recall;
// step away from the hidden grass and the enemies seen around us, toward home, then recall.
constexpr bool kUnseenWard = true;
constexpr bool kHurtFlee = true;
constexpr int kFleeMemory = 4;
// kBushHold: a ranged champion waiting for an enemy to walk into its range (the "hold") steps into
// an adjacent grass tile when that leaves it no more exposed: unseen, it cannot be targeted.
constexpr bool kBushHold = true;
// kReadyFights: enemy cooldowns are readable in sight. Fight math counts every champion in sight
// (enemies, teammates, us) by what it can deal over the next kReadyHorizon turns from its live
// cooldowns (a ready spell once, one on cooldown not at all) instead of spells averaged over 8.
constexpr bool kReadyFights = false;
constexpr int kReadyHorizon = 3;
// v32 tests. Probes showed what the engine shows of champions in sight: ability cooldowns, stuns
// and shields; and of every enemy anywhere, whether it is alive (bc::champions(team::enemy)).
// 1 kStunBuffs: a stun is wasted on an enemy already stunned (read from its buffs; the old check
//   guessed from "cannot move", which also shows on one that has just moved) or immune; with it a
//   second stunner waits a turn and its stun lands as the first wears off.
// 2 kFocusStunned: a stunned enemy in reach becomes the target, and the team's focus.
// 3 kPunish: an enemy whose damage and stun spells all have kPunishTurns+ cooldown left counts with
//   its basic attacks only in the fight math.
// 4 kShieldHp: an enemy's shield counts as health (fight math, finishing blows, target choice).
// 5 kSafeWait: waiting at a camp with enemies seen in the last kSafeWaitRecent rounds, step to a
//   tile beside fewer hidden grass tiles (still in reach of the camp).
// 6 kGrassRetreat: retreat steps count grass as one tile further from the threats (enemies
//   outside a patch cannot see in).
// 7 kPowerPlay: with 2+ enemies dead, contested camps are not held back in the opening, solo jobs
//   there are allowed, contested and enemy-side camps score kPowerPlayBonus more, and no rally.
// 8 kHealFocus: Willow heals the teammate losing health fastest (what it lost since last turn
//   counts twice), not only the one missing most.
// 9 kCrystalEscape: carry a teleport crystal (10 gold); in danger with 3 turns of health to spare,
//   jump to a tile within 3 that is kCrystalSave+ tiles nearer home on foot (across a wall or hole).
// 10 kSiegeEarly: before kSiegeFrom, a camp that needs trips home counts when it scores
//   kSiegeEarlyEdge times better than anything else (01starvation: the 88-gem elk between the
//   altars needed 6,846 damage taken against 4,355 team health, so for 500 rounds the squad walked
//   100+ tiles between 4-gem wolves and 40-gold clusters, and earned 248 gold by round 500).
constexpr bool kStunBuffs = true;
constexpr bool kFocusStunned = true;
constexpr bool kPunish = true;
constexpr int kPunishTurns = 4;
constexpr bool kShieldHp = true;
constexpr bool kSafeWait = true;
constexpr int kSafeWaitRecent = 80;
constexpr bool kGrassRetreat = true;
constexpr bool kPowerPlay = true;
constexpr float kPowerPlayBonus = 1.5f;
constexpr bool kHealFocus = true;
constexpr bool kCrystalEscape = true;
constexpr int kCrystalSave = 6;
constexpr bool kSiegeEarly = true;
// 11 kRallyAll (v31 ladder, 01starvation rounds 300-800: their four moved as one group below the
// middle while ours walked in one or two at a time from the left and died, 46 deaths to 17 on
// 01starfish): the rally for every squad camp, not only the jackpot. With enemies seen within 12
// tiles of it in the last 60 rounds, or 2+ of us dead in the last 100 rounds and it is contested
// or on their side, wait about kRallyDist tiles out until the teammates with us (or already there)
// match the most enemies seen there at once less one, at most kRallyAllMax rounds. Not while they
// are short-handed (kPowerPlay).
constexpr bool kRallyAll = false;
constexpr int kRallyAllMax = 30;
constexpr float kSiegeEarlyEdge = 2.f;
// Out of 140 on top of v30 (v30 build 70/124/105/123; this code with all four off 68/124/108/123):
// floor 0.4 62/128/105/124 (deaths against fish 6.0/8.7, was 6.9/9.7); ward + flee 67/127/103/119;
// bush hold 67/122/113/124; ready fights 71/122/98/120 (off). Floor, ward, flee and bush hold
// together: 79 against the all-off build, 72/125/104/127 against v30 and the sparring bots, deaths
// 6.5/8.6/10.9 a game against 6.8/9.8/12.6, farming 1,132 against 1,144 rounds. On (v31 test).
constexpr int kAheadWalk = 10;
constexpr int kAheadMargin = 4;
// kLookCalm: look ahead only when the coast is clear (no enemy seen, no teammate lost, in the
// last kCalmRounds). Unconditional, it farmed 1.1% faster with no opponent (1,076 against 1,088
// rounds to 1,000 gems) but lost 57/140 against the same bot without it, 119 against 126 against
// a Cassi & Issac team, 96 against 106 against fishsim.
constexpr bool kLookCalm = false;
// With kLookCalm alone: 61/140 against the same bot without it, 118/99/122 against Cassi & Issac /
// fishsim / hunter (126/106/119), farming 1,080 against 1,088. kLookOwnSide: only camps on our
// side (not contested, not theirs) count as the next camp, so it chains our camps instead of
// drawing the squad toward the middle.
constexpr bool kLookOwnSide = false;
constexpr int kLookTop = 5;
constexpr int kLookNear = 6;
constexpr float kLookDeadEnd = 0.85f;
constexpr bool kCombatDash = false;
constexpr float kDashKill = 2.f;  // finish when its health is under this many of our hits
// Fields on 8 of 70 maps. Out of 140 against the v24 test build / Cassi & Issac / fishsim / hunter:
// 71/125/92/107; on the field maps 11 of 16 head to head, 11 against 8 of 16 against fishsim (on).
constexpr bool kFieldCamp = true;
// Patches and camps: every camp's place and worth are known from the map at round 1, and walks
// follow distance fields, but a camp's state is only known while in view (the engine refuses to
// report groups out of sight). kShareClears: a teammate's status byte saying it is capturing group
// g means g was just cleared, so every champion marks g taken (respawning about g.respawn rounds
// after the capture) instead of walking there to find out.
// Out of 140 against v28 / Cassi & Issac / fishsim / hunter (v28 70/126/106/119, farming 1,088):
// 66/126/104/122, farming 1,112. Off.
constexpr bool kShareClears = false;
// Jackpot maps (one group holds 40%+ of the gems: 17 of 70). On the ladder (v20-v24) 1 win in 8,
// 236 gems to 1,000 on average, 0 on 00vault and 00stampede; locally fishsim takes 01concentrated-
// wealth's 71-gem camp 12 times to our 4-5, and the squad there arrived one champion at a time.
// kJackpot (world.h) finds the jackpot. kJackpotPush: it is exempt from the opening's contested
// penalty and scores kJackpotBonus times more for the squad. kRally: with enemies seen around the
// jackpot in the last 60 rounds, a champion walking in waits about kRallyDist tiles out until two
// teammates are with it or already there (at most kRallyMax rounds).
// Out of 140 (as above): push 64/118/103/122, farming 1,082, jackpot clears against fishsim 134-87
// (v28 131-86); push and rally 62/118/105/121, 1,081, clears 161-57.
constexpr bool kJackpotPush = false;
constexpr float kJackpotBonus = 2.f;
constexpr bool kRally = true;
constexpr int kRallyDist = 10;
constexpr int kRallyMax = 25;
constexpr int kFieldGroups = 3;
constexpr int kFieldGems = 50;
constexpr float kFieldBonus = 3.f;
// Scout wards. Ladder teams re-place their ward about every 12 rounds (a quarter of the time in a
// bush) and land the first hit in more fights; ours sat at old capture spots (every 68 rounds,
// 1% in bushes). Whenever the ward is ready and we have moved on, re-place it within 2 tiles,
// in a bush if there is one, else toward the enemy.
constexpr bool kScoutWards = true;
constexpr int kScoutWardMove = 5;  // re-place once we are this far from our ward
// Early warning. Outnumbered fights became the biggest loss (76 of 169 deaths in 10 v6 games),
// mostly one of ours caught by 2-3 enemies after round 300, while enemies 8-10 tiles out were
// ignored. A group that outnumbers us at that range sends us toward a teammate, or away.
// Off: about 20 fewer wins out of 560 against the sparring bots in both combinations tested.
constexpr bool kEarlyWarning = false;
// Capture guard. A capture needs the region held uncontested for 5 rounds and an enemy stepping
// on it only resets the timer, but our capturer used to walk off to fight and the enemy took
// the reward (30 rewards, 500 gems, in 10 v6 games). Hold the tile and fight from it unless in
// danger or the fight is clearly lost (outnumbered, or out-damaged); holding even when
// outnumbered by one cost deaths locally.
constexpr bool kHoldCapture = true;
// Marcella banks. In a lost v7 game the other team's Marcella collected 514 of their 813 camp
// gems from camps her teammates had cleared and turned them in in small bags (Marcella gains 2
// gold per gem turned in): 5,161 gold against our Marcella's 750, who turned in nothing.
// Teammates leave gem rewards to our Marcella when she is free and within kBankerReach, and
// move on to the next camp; she claims gem rewards from further out and banks at kBankerCarry.
// Locally her share of our gems went from 23% to 48% and her gold by round 600 from 530 to
// 1,319: 457 against 434 wins out of 560, 46/80 against v7 and v6, though farming with no
// enemy around is 7% slower (more walking to rewards and the altar).
// v26: Inter Fish's Marcella banks ~40% of their gems (2 gold each, spent on Focus) and killed 34 of
// ours in 5 games. Out of 140: 90 against the v25 test build, 121/93/121 against Cassi & Issac /
// fishsim / hunter (without: 76, 125/95/108); farming 1,119 against 1,039 rounds to 1,000 gems
// (on). Banking at 30 gems 74/123/94/115 and a reach of 5 62/120/98/119 against it.
constexpr bool kMarcellaBank = true;
constexpr int kBankerReach = 8;
constexpr int kBankerClaim = 14;
constexpr int kBankerCarry = 20;
// Solo camps. The squad takes one camp at a time, and in that game never cleared the wolf
// nearest our spawn (the other team cleared theirs 11 times). One champion may take a
// fighting camp on our side alone when the forecast says it costs under kSoloShare of its health.
// Off: it fixed that (the nearest wolf on 00collision 0 to 5 clears a game) and farmed 1%
// faster, but lost head to head: 38/80 against v7, and 36/80 with the banker against the banker.
constexpr bool kSoloCamps = false;
constexpr float kSoloShare = 0.45f;
// Safe altar trips. 4 of 9 deaths in that game came at the only altar, with 66- and 59-gem
// bags, and gems lost on death go to the killer. Do not walk up to an altar with enemies around
// it unless we outnumber them, and do not stand alone hitting an enemy lock with enemies near:
// farm for kAltarWait rounds and come back.
// Off: a little behind in every combination tested (38/80 against v7 alone, 36/80 against the
// banker with it).
constexpr bool kSafeAltar = false;
constexpr int kAltarWait = 20;
// Bank escort. Our deaths at the centre altar were all one champion turning in alone, with the
// enemy arriving mid-trip. When a teammate walks to an altar in the middle (or on the enemy
// side), the nearest free teammate within kEscortRange goes along (two for an enemy-side altar),
// for the last kEscortStretch tiles of a kEscortBag+ trip; the banker waits up to 8 rounds for it.
// Escorting every trip took up to 28% of rounds. Locally even with the banker alone (455 against
// 457 of 560, 49/80 against v7, 38/80 against the banker alone, farming 2% slower): the sparring
// bots rarely hunt at the altar, so this is on for a ladder test.
constexpr bool kBankEscort = true;
// Loop and siege (v11). Squad targets may be waited for up to kRespawnWait rounds, and a camp
// the team can clear while losing under kSiegeShare of its health is not a siege (0.55 kept the
// 41-gem camp beside our spawn on 01big-world off limits until round 270; the other team took
// it at round 35 and every 85 rounds after).
constexpr int kRespawnWait = 100;
constexpr float kSiegeShare = 0.85f;
// Altar danger (v11). With a bag worth guarding, prefer an altar the enemy is not holding: on
// 00corners five of us died with 48-85 gems at the middle altar their team banks at, while
// the corner altars were free.
constexpr bool kAltarDanger = true;
// Side jobs (v12): while the squad's target respawns for more than kWaitJobMin rounds, any of us
// may clear a nearby camp alone (gold and gem clusters, and fighting camps on our side that cost
// under kSoloShare of our health), if it can be back before the respawn.
constexpr bool kWaitJobs = true;
// Ideas from the v12 scrim on adgato-gemcombat1 (their three met each of ours at the middle
// altar alone): team banking (bigger escorts for altar trips in the middle), an altar guard (our
// tank waits beside a middle altar between jobs) and intercepting enemy tributes (an enemy
// channelling cannot move, and its bag goes to whoever kills it).
// Tested on all 70 maps (out of 140 against v12, and against fishsim where v12 wins 74):
// team banking 64 and 77, the only one to win adgato-gemcombat1 against fishsim twice, farming
// 4% slower: on for a ladder test. Altar guard 61 and 67, intercepting 66 and 76, all three 54
// and 62: off.
constexpr bool kTeamBank = true;
constexpr bool kAltarGuard = false;
constexpr bool kInterceptTribute = false;
// v14, from the v13 scrim on 01elk-arena (0-1,008, 53 deaths to 1): in the first fight all six of
// theirs hit ours while our Vesper Vale kept hitting the elk and Tavi waited for range. Join a
// fight a teammate is in (an enemy within 7 tiles attacking one of us) instead of farming or
// holding: 76/140 against v13, 115 against 105 of 140 against a Cassi & Issac team, 78 against 77
// against fishsim, fewer deaths. Cassi and Issac (one pick, one life) count half a head each, so a
// team with them no longer shows six heads to our five: 70/140 against v13, 107 against 105.
constexpr bool kFairHeads = true;
constexpr bool kJoinFights = true;
// v15, from the v14 scrim on 01elk-arena: in the first fight (no items on either side) they dealt
// 1.76x our damage. 58% of ours went into Heart of Ruin (a tank whose damage grows as he loses
// health) because we hit the lowest-health enemy in reach, while Cassi (600 health; her death
// kills Issac too) took little. Smart focus picks the enemy that deals the most per point of
// health left (Cassi counts double, Heart of Ruin less until nearly dead). And a ranged champion
// whose target stepped out of range shot nothing while it walked after it (our Vesper Vale dealt
// 180 in that fight): with another enemy in range, shoot that one instead.
constexpr bool kSmartFocus = true;
constexpr bool kNoChase = false;
// Peel back: their hurt champions fell back behind a teammate and kept fighting (their Heart of
// Ruin took 1,086 in that fight, their Marcella 100, Issac 0), while ours ran for home alone. A
// champion that would retreat, unless nearly dead, steps to the tile fewest enemies can reach
// within kPeelRange of a healthier teammate, and shoots from there if it can.
constexpr bool kPeelBack = false;
// Call for help. Champions see only their own surroundings and the team shares 8 bytes, so a
// teammate fighting 8 tiles away was invisible: in a local replay of 01elk-arena our Ryth fought
// alone for 9 rounds while two of us collected a reward and one escorted a bag. A champion in a
// fight says so in its status byte; a healthy teammate within kHelpRange walks over.
// Out of 140: 81 against v14, 87 against 78 against fishsim (deaths 10.6 against 12.2), 106
// against 112 against a Cassi & Issac team. With no-chase 77/82/105; with no-chase and peel-back
// 71/94/104. Smart focus alone 63 against v14, no-chase 73, peel-back 76: those stay off.
constexpr bool kCallHelp = true;
// v16: open a walled-in altar however many trips it takes when no altar can be reached.
constexpr bool kUnlockAnyTrips = false;  // tested: worse (01boss-battle as team A 603 against 1,249 gems by round 1,000)
// v16: altar recheck (see Bot::altar_open).
constexpr bool kAltarRecheck = true;
constexpr int kHelpRange = 14;
constexpr int kPeelRange = 3;
// Altar patrol: with kAltarGuard on, the guard checks the altar for kPatrolStay rounds out of every
// kPatrolPeriod instead of standing there. Off: with team banking 61/140 against team banking
// alone; without it 67/140 against v12 and 76 against fishsim (v12 74), farming 2.4% slower.
constexpr bool kAltarPatrol = false;
constexpr int kPatrolPeriod = 50;
constexpr int kPatrolStay = 12;
constexpr int kInterceptRange = 10;
constexpr int kWaitJobMin = 20;
constexpr int kEscortRange = 12;
constexpr int kEscortStretch = 10;  // the banker is this close to the altar
constexpr int kEscortBag = 30;      // and carries at least this many gems
// Escort only when the coast is not clear: with no enemy seen and no teammate lost in the last
// kCalmRounds, banking trips go without escorts and without waiting for one. With no opponent at
// all, escorting took 6.6% of champion rounds by round 500 and waiting for escorts 1%.
// Out of 140: 75 against v20, 115 against a Cassi & Issac team (v20 120), 86 against fishsim
// (v20 88); farming 1,059 against 1,136 rounds to 1,000 gems (391 against 361 gems by round 500).
constexpr bool kCalmNoEscort = true;
constexpr int kCalmRounds = 60;
// v21 ladder games (0-5 against noobers): 02gift-wrapping turned on its 128-gem centre reward.
// One champion captured it alone while the other four walked to their next camps, and was killed
// 6 rounds later beside the centre; six such bags (128-144 gems) went to the other team. One bag
// was then walked 13 tiles to an altar on their side (an enemy seen by the near one put 30 tiles
// on it, an unseen one on their side nothing) and lost mid-channel.
// kShareBig: big rewards are captured together (3 for 40+ gems, 5 for 100+), which splits the bag.
// kAltarExposure: a bag of kEscortBag+ counts an altar this many tiles further per exposure step
// (middle 1, their side 2), on top of altar danger.
// kFarmFirst: while one of our target's camps is in range and no enemy can hit us next turn, keep
// hitting it rather than walk off to engage, join a teammate's fight or answer a call.
// kHelpRange applies to answering calls (14).
// Out of 140 against v21 / a Cassi & Issac team / fishsim (v21 70/115/86): kShareBig 67/112/90
// (farming 3% slower); kAltarExposure 15 64/118/88; kFarmFirst 67/119/84; kHelpRange 8 71/116/84.
constexpr bool kShareBig = false;
constexpr int kAltarExposure = 0;
constexpr bool kFarmFirst = false;
// kSmartEscort: after respawning, wait on spawn for an escort only while a teammate is dead or
// heading home, so someone will come (on 02fields Marcella waited the full 40 rounds for nobody).
// kSquadTravel: a squad camp's travel is the farthest living teammate's walk, not just ours (on
// 02famine, 91x75, Marcella reached a golem camp alone, waited, was hurt and recalled).
// Out of 140 (as above): kSmartEscort 70/115/90, no farming cost; kSquadTravel 70/116/90, farming
// 2% slower and no fewer abandoned trips.
constexpr bool kSmartEscort = false;
constexpr bool kSquadTravel = false;
// kSafeChannel: a long tribute (6+ rounds) at a middle or enemy-side altar starts only with a
// teammate beside us or no enemy seen in the last 15 rounds (on 02gift-wrapping a 128-gem bag
// started a 13-round channel on their side and died 3 rounds in).
// Out of 140 (as above): 69/113/81: off.
constexpr bool kSafeChannel = false;
// kStragglerSolo: far behind the squad (kStragglerDist+ to its camp, two teammates already there),
// clear something nearby instead: with no opponent, 1,340 of 9,065 walking rounds by round 500
// went on trips to a squad camp that was finished or replaced before we got there.
// Out of 140 (as above): 73/112/88, but it never fired: stragglers are 10-15 tiles behind.
constexpr bool kStragglerSolo = false;
constexpr int kStragglerDist = 15;
// Breach. On 02the-pack the only altar sits inside a ring of 48 wolves: our siege rules never
// took the ring on, and neither side turned in a gem in 2,000 rounds. When no altar can be
// reached past live camps, the squad kills only the camps on the shortest walk in (see
// World::plan_breach), and nobody walks to the altar until the way is open.
constexpr bool kBreach = true;
// v16 fight tests (v15 scrim against intercontinental fish): kCampWary leaves a contested or
// enemy-side fighting camp alone while enemy champions are within 10 tiles (our Vendetta was at
// 40% health from the middle elk on 01elk-arena before the fight began); kEngageFar reacts to
// enemies within 7 tiles instead of 5 (they waited 6-8 tiles out while we hit the elk); kLeash
// does not walk after an enemy in its own third of the map (8 of our 54 deaths there).
// Out of 140 (against v16 without them / a Cassi & Issac team / fishsim): camp wary 76/110/86,
// leash 72/110/90, both 80/110/82 with the fewest deaths against Cassi & Issac (7.0 against 8.9),
// farming 2% slower; engage far 56/113/82 (rushing enemies 7 tiles out splits the team): off.
// Valuing gold at 0.08 instead of 0.15 (gems first): 67/113/85, with the other two 78/106/78: off.
constexpr bool kCampWary = true;
// v17 tests. Soft clusters: the soft opening also for gem and gold clusters (00lost-world: every
// cluster is in the contested middle; the squad walked 42 tiles to a corner elk and our first
// clear came at round 265, theirs at 31). Bag caution: with kBagCautionGems or more, do not
// answer calls or join fights, and leave as soon as a fight looks lost (adgato-gemcombat4: 13
// deaths with 21-56 gems each, 455 gems handed over). Early solo: in the first 300 rounds up to
// half the team may farm clusters alone (they cleared 2-3.5x our camps by round 300).
// Out of 140 (against v16 / a Cassi & Issac team / fishsim): soft clusters 68/113/81 with 11% more
// gems by round 300 (167 against 151) and 1.4% faster farming; bag caution 71/108/83; both 69/113/80;
// early solo 64/108/85 (off).
constexpr bool kSoftClusters = true;
constexpr bool kBagCaution = true;
constexpr int kBagCautionGems = 30;
constexpr bool kEarlySolo = false;
// v18, from the v17 scrim on 00corners (46 camp clears to their 92; in a local replay every farming
// champion had the same target in 72% of rounds, walking 36% of the time and waiting beside
// respawning camps 8%): two squads. With 4+ champions the last two slots form a second squad with
// its own target, one the two of them can clear without a siege, never the main squad's; its
// leader announces it in its status byte and the partner follows. Either squad short of two
// living members merges back. Middle bias: camps nearer the middle of the map score up to
// kMiddleBias more (the best camps are often there).
// Two squads: out of 140, 50 against v17, 100 against 113 against a Cassi & Issac team, 63 against
// 80 against fishsim, 17% fewer gems by round 800 on big maps (more small camps, slower big ones,
// lost fights): off. Middle bias 0.3: 70, 115, 82, farming 1.8% faster (1,160 against 1,181
// rounds to 1,000 gems), 5-6% more gems on big and ladder maps: on.
constexpr bool kTwoSquads = false;
constexpr float kMiddleBias = 0.3f;
// Refined fight order (code review): join a teammate's fight only with nothing in reach of our own
// (walking toward one cost a ranged champion its shot), answer calls only after stepping out of
// camps' reach, and only when the caller is a short walk away (not just across a wall). Out of 140:
// 64 against v17, 115 against a Cassi & Issac team, 87 against fishsim (68/115/84 without): off.
constexpr bool kRefineFight = false;
// v19 ideas. Hunt the bag: a champion's carried gems go to whoever kills it, so among enemies we can
// hit, prefer the one carrying the most (per point of health). Lock denial: with a lock in hand,
// walk a few tiles to lock an unlocked altar in the middle or on their side (a lock blocks their
// tributes until broken and cancels one in progress). Altar zone: camps near a middle or enemy-side
// altar score kAltarZoneBias more, so our champions are about when theirs come to bank.
// Out of 140 (against v18 / a Cassi & Issac team / fishsim): altar zone 0.4 with lock denial
// 78/113/93 (on); lock denial alone 74/117/92; altar zone alone 72/120/87; hunt the bag 70/114/81
// (off); altar zone with tribute interception 63/113/88; all four 75/114/80. Farming 1,154 against
// 1,144 rounds to 1,000 gems, 6% more by round 800 on the five ladder maps.
constexpr bool kHuntBag = false;
constexpr bool kLockDenial = true;
constexpr bool kNearestLock = true;  // lock detours go to the nearest open altar, not the first listed
constexpr float kAltarZoneBias = 0.4f;
// Home defense: against teams that come to us (32 of our 54 deaths on 01elk-arena were in our own
// third), a losing fight near home falls back onto our spawn, where we heal every round and they
// cannot step, instead of being fought out in the open; only while 2+ enemies were seen together in
// our half lately, and only within kHomeDefenseRange of our spawn.
// Out of 140: 69 against v18, 110 against 115 against a Cassi & Issac team, 87 against 84 against
// fishsim, 107 against 105 against hunter (the most aggressive sparring bot): neutral, off.
constexpr bool kHomeDefense = false;
// Altar defense (with kHuntBag, which reads enemy bags): engage an enemy carrying 20+ gems within 6
// tiles of a middle or enemy-side altar from up to 7 tiles (5 otherwise), unless outnumbered.
// Out of 140 (against v19 / a Cassi & Issac team / fishsim): 67/114/88 (v19 70/113/93): off.
constexpr bool kAltarDefense = false;
// Worth-it bag hunt (refines altar defense): go after a bag near an altar only when the fight is
// clearly won (our side, counting champions around the carrier too, needs at most 1/kBagFightEdge
// of the rounds theirs does), we are healthy, and the bag holds at least kBagWorthGems and 10 more
// than ours (a lost fight hands ours over). Ranged champions chase such a bag instead of holding.
// With altar defense, out of 140: loose (1.2, 20 gems) 70/114/91, 1.5 and 30 gems 70/110/82,
// strict (2.0, 40 gems) 69/108/86: off.
constexpr bool kWorthBag = false;
constexpr float kBagFightEdge = 1.5f;
constexpr int kBagWorthGems = 30;
// Altar defense, more hands: kBagFocus off keeps the usual weakest-first targeting near altars
// (only the chase changes); kBagCall makes a champion near such a bag call teammates from
// kBagHelpRange (14 for other fights); kAltarGuards champions wait by the altar with kAltarGuard.
// Out of 140 against v20 / a Cassi & Issac team / fishsim (v20: 70/120/88), all with altar
// defense, an even fight and a 10-gem bag: alone 75/119/75; kBagFocus off 78/115/76; kBagCall
// 73/117/74; altar zone 1.0 71/118/82; two altar guards 52/97/62 (3 bags killed near altars a
// game against 1.9, but 15% less banked). All off.
constexpr bool kBagFocus = true;
constexpr bool kBagCall = false;
constexpr int kBagHelpRange = 22;
constexpr int kAltarGuards = 1;
constexpr int kHomeDefenseRange = 12;
constexpr bool kEngageFar = false;
constexpr bool kLeash = true;
constexpr int kEngageRange = 5;
constexpr int kMaxHold = 6;  // turns a ranged champion waits for an enemy before moving on

bool tank_type(bc::champion_type t) { return t == bc::champion_type::ryth || t == bc::champion_type::marcella; }

struct BuildStep {
    bc::item_type item;
    int count;
};

// Five shards of one kind combine into its emblem; the last step repeats forever.
// The repeat step is Bruiser: 5 shards of the chosen stat plus 1 of each other, 7 shards'
// worth for 150 gold where Power gives 6. Emblem passives do not stack, so once the useful
// passives are bought, raw stats are all that is left to buy.
const std::vector<BuildStep>& build_for(bc::champion_type t) {
    using it = bc::item_type;
    static const std::vector<BuildStep> tank{{it::jungle_shard_of_health, 5},
                                             {it::monster_shard_of_strength, 5},
                                             {it::durability_shard_of_health, 5},
                                             {it::bruiser_shard_of_strength, 1 << 30}};
    // Treasure first for the ranged champions who collect most rewards: 20% more of
    // their gold compounds into every later shard. Melee carries go straight to damage.
    // Jungle (30% less damage from camps) comes before Durability: camps deal most of the
    // damage a farming champion takes, so it saves trips home to heal.
    static const std::vector<BuildStep> earner{{it::treasure_shard_of_strength, 5},
                                               {it::monster_shard_of_strength, 5},
                                               {it::jungle_shard_of_health, 5},
                                               {it::durability_shard_of_health, 5},
                                               {it::bruiser_shard_of_strength, 1 << 30}};
    static const std::vector<BuildStep> fighter{{it::monster_shard_of_strength, 5},
                                                {it::jungle_shard_of_health, 5},
                                                {it::durability_shard_of_health, 5},
                                                {it::bruiser_shard_of_strength, 1 << 30}};
    // Spell damage scales with Focus and lands instantly at range 4: stacked Focus
    // turns a caster's spell into a one-shot, which wins champion fights.
    static const std::vector<BuildStep> caster{{it::durability_shard_of_health, 5}, {it::bruiser_shard_of_focus, 1 << 30}};
    // kFightBuild: Durability (10% less damage from everything) first, same stats, farming emblems after.
    static const std::vector<BuildStep> tank_f{{it::durability_shard_of_health, 5},
                                               {it::jungle_shard_of_health, 5},
                                               {it::monster_shard_of_strength, 5},
                                               {it::bruiser_shard_of_strength, 1 << 30}};
    static const std::vector<BuildStep> earner_f{{it::durability_shard_of_strength, 5},
                                                 {it::treasure_shard_of_strength, 5},
                                                 {it::monster_shard_of_strength, 5},
                                                 {it::jungle_shard_of_health, 5},
                                                 {it::bruiser_shard_of_strength, 1 << 30}};
    static const std::vector<BuildStep> fighter_f{{it::durability_shard_of_strength, 5},
                                                  {it::monster_shard_of_strength, 5},
                                                  {it::jungle_shard_of_health, 5},
                                                  {it::bruiser_shard_of_strength, 1 << 30}};
    // Focus builds: Marcella's spell, Heart of Ruin's dash and shield, Willow's heal.
    if (t == bc::champion_type::marcella || t == bc::champion_type::heart_of_ruin || t == bc::champion_type::willow) return caster;
    if (tank_type(t)) return kFightBuild ? tank_f : tank;
    return bc::champion{t}.base_stats().base_attack_range > 1 ? (kFightBuild ? earner_f : earner) : (kFightBuild ? fighter_f : fighter);
}

struct DpsModel {
    float strength = 1;
    float focus = 0;
    int range = 1;
};

// Damage per round from Strength and Focus, with spells averaged over their cooldown.
const DpsModel& model_for(bc::champion_type type) {
    static std::array<std::optional<DpsModel>, 13> cache;
    auto& slot = cache[static_cast<int>(type)];
    if (slot) return *slot;
    bc::champion design{type};
    DpsModel m;
    m.range = design.base_stats().base_attack_range;
    for (const auto& ab : design.abilities()) {
        const auto& info = ab.info();
        if (!info) continue;
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            const float cd = std::max(1, p.max_cooldown);
            if constexpr (std::is_same_v<T, bc::ability_attack_reset>) m.strength += 1.f / cd;
            else if constexpr (std::is_same_v<T, bc::ability_damage>) m.focus += p.modifier / cd;
            else if constexpr (std::is_same_v<T, bc::ability_cc>) m.focus += p.modifier / cd;
            else if constexpr (std::is_same_v<T, bc::ability_aoe_damage>) m.focus += p.modifier * 1.5f / cd;
            else if constexpr (std::is_same_v<T, bc::ability_dash>) m.focus += p.modifier * 1.5f / cd;
        }, *info);
    }
    if (design.passive() == bc::passive_type::splash_attack) m.strength += 0.5f;
    slot = m;
    return *slot;
}

float dps_of(bc::champion_type type, float strength, float focus) {
    const auto& m = model_for(type);
    return strength * m.strength + focus * m.focus;
}

// One-turn threat, unlike DpsModel which averages spells over their cooldown: a Stellar
// or Marcella with stacked Focus nukes for several times its averaged damage in one turn.
struct ThreatModel {
    int reach = 1;       // tiles from which it can hurt us on its next turn
    float strength = 1;  // basic attacks per turn (2 with an attack reset)
    float focus = 0;     // biggest single-cast Focus multiplier
};

const ThreatModel& threat_for(bc::champion_type type) {
    static std::array<std::optional<ThreatModel>, 13> cache;
    auto& slot = cache[static_cast<int>(type)];
    if (slot) return *slot;
    bc::champion design{type};
    const int range = design.base_stats().base_attack_range;
    ThreatModel t;
    // Ranged champions cannot walk and shoot in one turn; melee can step then hit.
    t.reach = range > 1 ? range : range + 1;
    for (const auto& ab : design.abilities()) {
        const auto& info = ab.info();
        if (!info) continue;
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            const int walk = p.exclusive_with_movement ? 0 : 1;
            if constexpr (std::is_same_v<T, bc::ability_attack_reset>) {
                t.strength = 2;
            } else if constexpr (std::is_same_v<T, bc::ability_damage> || std::is_same_v<T, bc::ability_cc>) {
                t.focus = std::max(t.focus, p.modifier);
                t.reach = std::max(t.reach, static_cast<int>(p.range) + walk);
            } else if constexpr (std::is_same_v<T, bc::ability_aoe_damage>) {
                t.focus = std::max(t.focus, p.modifier);
                t.reach = std::max(t.reach, static_cast<int>(p.range + p.radius) + walk);
            } else if constexpr (std::is_same_v<T, bc::ability_dash>) {
                t.focus = std::max(t.focus, p.modifier);
                // Dash in, then hit with the landing or a basic attack.
                t.reach = std::max(t.reach, static_cast<int>(p.range) + std::max(static_cast<int>(p.radius), range));
            }
        }, *info);
    }
    slot = t;
    return *slot;
}

float burst_of(bc::champion_type type, float strength, float focus) {
    const auto& t = threat_for(type);
    return strength * t.strength + focus * t.focus;
}
} // namespace

float Bot::design_dps(const bc::champion& c, float strength, float focus) const {
    return dps_of(c.champion_name(), strength, focus);
}

// ---------------------------------------------------------------------------
// Setup and observation

void Bot::init(bc::champion& me) {
    auto& W = world();
    mark("init-world");
    if (!W.ready) {
        W.init();
        // On big maps building the map takes most of the first turn's fuel: finish setting up
        // next turn rather than run out of fuel halfway (which loses the turn anyway).
        if (bc::fuel_remaining() < kSetupFuel) return;
    }
    mark("init-allies");
    // Safe to run again if a turn ran out of fuel in here.
    ally_dps_.clear();
    ally_hp_.clear();
    ally_types_.clear();
    spells_.clear();
    banker_slot_ = -1;
    type_ = me.champion_name();
    const auto stats = me.base_stats();
    range_ = stats.base_attack_range;
    ranged_ = range_ > 1;
    tank_ = tank_type(type_);
    allies_ = bc::champions(team::ally);
    if (kPowerPlay) enemy_roster_ = bc::champions(team::enemy);
    slots_ = std::min<int>(static_cast<int>(allies_.size()), kMaxSlots);
    for (int i = 0; i < static_cast<int>(allies_.size()); ++i) {
        if (allies_[i] == me) slot_ = i;
        const auto t = allies_[i].champion_name();
        if (t == bc::champion_type::marcella && banker_slot_ < 0) banker_slot_ = i;
        ally_types_.push_back(t);
        bc::champion design{t};
        const auto s = design.base_stats();
        ally_dps_.push_back(dps_of(t, s.base_strength, s.base_focus));
        ally_hp_.push_back(s.base_health);
    }
    if (slot_ >= kMaxSlots) slot_ = -1;
    for (const auto& ab : me.abilities()) {
        const auto& info = ab.info();
        if (!info) continue;
        Spell spell{ab, bc::ability_kind::damage};
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            spell.kind = T::kind;
            spell.exclusive = p.exclusive_with_movement;
            if constexpr (requires { p.range; }) spell.range = p.range;
            if constexpr (requires { p.modifier; }) spell.modifier = p.modifier;
            if constexpr (requires { p.radius; }) spell.radius = p.radius;
            if constexpr (requires { p.duration; }) spell.duration = p.duration;
        }, *info);
        spells_.push_back(spell);
    }
    my_design_dps_ = dps_of(type_, stats.base_strength, stats.base_focus);
    ready_ = true;
    // The first turn has double fuel: precompute the trip home and the nearest targets.
    mark("init-precompute");
    W.precompute(110000);
}

void Bot::observe(bc::champion& me) {
    auto& W = world();
    visible_.load(bc::vision::visible_tiles(), W.w, W.h);
    occupancy_loaded_ = false;

    // Camp knowledge comes from native bitboards; only nearby camps get per-camp queries,
    // since every host call and every loop iteration costs fuel.
    mark("obs-camps");
    const auto alive_board = bc::vision::camp_tiles();
    alive_vis_.load(alive_board, W.w, W.h);
    awake_vis_.load(bc::vision::awake_camp_tiles(), W.w, W.h);
    camps_.clear();
    bc::bitboard near;
    near.clear();
    near.draw_chebyshev(pos_, 6);
    near &= alive_board;
    for (const vec2 p : near.positions()) {
        auto* info = W.camp_info_at(p);
        if (!info) continue;
        const auto c = bc::vision::camp_at(p);
        const float hp = info->strength > 0 || cheb(p, pos_) <= range_ + 1 ? c.health() : info->hp_known;
        if (hp <= 0) continue;
        info->hp_known = hp;
        camps_.push_back({c, p, hp, info->strength, info->range, info->strength > 0 && awake_vis_.get(p), W.group_id_at(p)});
    }

    mark("obs-champs");
    // Strength and Focus change when items are bought (and Prism's Focus with every gem it
    // carries): with the burst model, enemies are refreshed often and allies now and then.
    auto seen = [&](const bc::champion& c, bool enemy) {
        const auto t = c.champion_name();
        SeenChamp s{c, c.position(), c.health(), c.max_health(), 0, model_for(t).range, 0};
        s.type = t;
        if ((kHuntBag || kAltarDefense) && enemy) {
            try {
                s.gems = static_cast<int>(c.gem_count());
            } catch (const std::exception&) {
            }
        }
        auto& cached = dps_cache_[static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)) & 31];
        const int refresh = kBurstModel && enemy ? 5 : 25;
        if (cached.round < 0 || round_ - cached.round > refresh) {
            const float strength = c.strength(), focus = c.focus();
            cached = {dps_of(t, strength, focus), burst_of(t, strength, focus), round_};
        }
        s.dps = cached.dps;
        s.burst = cached.burst;
        s.reach = threat_for(t).reach;
        s.now = s.dps;
        if (kReadyFights && cheb(s.pos, pos_) <= 10) s.now = dps_now(c, t, c.strength(), c.focus());
        if ((kStunBuffs || kFocusStunned || kShieldHp) && enemy && cheb(s.pos, pos_) <= 8) {
            try {
                for (const auto& b : c.buffs()) {
                    if (const auto* st = std::get_if<bc::buff_stun>(&b)) s.stunned = std::max(s.stunned, static_cast<int>(st->duration));
                    else if (const auto* sh = std::get_if<bc::buff_shield>(&b)) s.shield += sh->value;
                }
            } catch (const std::exception&) {
            }
            if (kShieldHp) s.hp += s.shield;
        }
        if (kPunish && enemy && cheb(s.pos, pos_) <= 8) {
            int spells = 0, down = 0;
            try {
                for (const auto& ab : c.abilities()) {
                    const auto& info = ab.info();
                    if (!info) continue;
                    bool offensive = false;
                    std::visit([&](const auto& p) {
                        using T = std::decay_t<decltype(p)>;
                        offensive = std::is_same_v<T, bc::ability_damage> || std::is_same_v<T, bc::ability_cc> || std::is_same_v<T, bc::ability_aoe_damage>;
                    }, *info);
                    if (!offensive) continue;
                    ++spells;
                    if (ab.cooldown() >= kPunishTurns) ++down;
                }
                if (spells > 0 && down == spells) {
                    s.disarmed = true;
                    s.basic = c.strength() * (model_for(t).strength >= 1.5f ? 1.5f : 1.f);
                }
            } catch (const std::exception&) {
            }
        }
        return s;
    };
    enemies_.clear();
    for (const auto& c : bc::vision::champions(team::enemy)) enemies_.push_back(seen(c, true));
    note_sightings();
    allies_seen_.clear();
    for (const auto& c : bc::vision::champions(team::ally)) {
        if (c == me) continue;
        allies_seen_.push_back(seen(c, false));
        // Health shards only add: keep the most a teammate has shown, so the team's
        // forecasts count its items while it is out of sight.
        for (int i = 0; i < slots_; ++i)
            if (allies_[i] == c) ally_hp_[i] = std::max(ally_hp_[i], allies_seen_.back().max_hp);
    }
    mark("obs-groups");
    update_groups();
}

void Bot::update_groups() {
    auto& W = world();
    for (auto& g : W.groups) {
        // Out of sight a reward does not wait: a teammate (or the enemy) captures it within a
        // few rounds and the group starts its respawn timer. Without this a champion that left
        // after the kill would never look at the group again.
        if (kShareClears) {
            if (g.state == GroupState::reward && round_ - std::max(g.seen_round, g.heard_round) > kRewardTimeout) {
                g.state = GroupState::respawning;
                g.respawn_round = (g.heard_round > g.seen_round ? g.heard_round + 1 : g.seen_round + kCaptureRounds + 1) + g.respawn;
            }
        } else if (g.state == GroupState::reward && round_ - g.seen_round > kRewardTimeout) {
            g.state = GroupState::respawning;
            g.respawn_round = g.seen_round + kCaptureRounds + 1 + g.respawn;
        }
        if (g.state == GroupState::respawning && round_ >= g.respawn_round) g.respawned();
        // Vision reaches 8 tiles; skip groups that cannot be in view.
        if (cheb(g.anchor, pos_) > g.extent + 9) continue;
        // One pass over the group's tiles: which are in view, which still hold a camp.
        bool any_visible = false, awake = false;
        int alive = 0;
        float hp_left = 0;
        for (auto& info : g.camps) {
            if (visible_.get(info.pos)) {
                any_visible = true;
                info.alive_known = alive_vis_.get(info.pos);
                if (info.alive_known) info.seen_alive_round = round_;
                if (info.alive_known && info.strength > 0) awake |= awake_vis_.get(info.pos);
            }
            if (info.alive_known) {
                ++alive;
                hp_left += info.hp_known;
            }
        }
        if (!any_visible) continue;
        g.seen_round = round_;
        bool visible_alive = false;
        for (const auto& info : g.camps)
            if (info.alive_known && visible_.get(info.pos)) {
                visible_alive = true;
                break;
            }
        if (visible_alive) {
            g.state = GroupState::alive;
            g.awake = awake;
            g.hp_left = hp_left;
            continue;
        }
        g.awake = false;
        // Asking the engine is costly for big groups; only do it when the answer can have changed.
        const bool ask = g.state == GroupState::alive || (g.state == GroupState::reward && round_ % 2 == 0);
        if (!ask) continue;
        try {
            const auto state = g.handle->state();
            if (std::holds_alternative<bc::map::camp_group_reward_available>(state)) {
                g.state = GroupState::reward;
            } else if (const auto* r = std::get_if<bc::map::camp_group_respawning>(&state)) {
                g.state = GroupState::respawning;
                g.respawn_round = round_ + r->timer;
            }
        } catch (const std::exception&) {
        }
    }
}

void Bot::load_occupancy() {
    if (occupancy_loaded_) return;
    occupied_board_ = bc::vision::champion_tiles() | bc::vision::camp_tiles() | bc::vision::teleport_tiles();
    occupied_.load(occupied_board_, world().w, world().h);
    occupancy_loaded_ = true;
}

bool Bot::free_tile(vec2 p) const {
    const auto& W = world();
    return W.in_bounds(p) && !W.blocked.get(p) && !occupied_.get(p);
}

void Bot::set_status(Task task, int target) { comms_.set_slot(slot_, task, target); }

void Bot::note_sightings() {
    int count = 0;
    vec2 sum{0, 0};
    for (const auto& e : enemies_) {
        // Enemies on their own spawn are shopping or healing, not hunting us.
        if (world().enemy_spawn.get(e.pos)) continue;
        sum += e.pos;
        ++count;
    }
    if (count == 0) return;
    last_enemy_round_ = round_;
    const vec2 centre = sum / count;
    // Merge with a recent nearby record rather than filling the buffer every round.
    for (auto& s : sightings_)
        if (round_ - s.round <= 5 && cheb(s.pos, centre) <= 4) {
            s = {centre, round_, std::max(s.count, count)};
            return;
        }
    sightings_[sighting_next_] = {centre, round_, count};
    sighting_next_ = (sighting_next_ + 1) % static_cast<int>(sightings_.size());
}

float Bot::danger_factor(vec2 p) const {
    // Champions used to respawn and walk straight back to where they died, one at a time,
    // and die again (every death on lost-world was at the same spot between the spawns).
    if (!kDangerMemory) return 1;
    float f = 1;
    if (round_ - death_round_ <= 100 && cheb(death_pos_, p) <= 8) f *= 0.3f;
    for (const auto& s : sightings_)
        if (s.count >= 2 && round_ - s.round <= 25 && cheb(s.pos, p) <= 10) {
            f *= 0.4f;
            break;
        }
    return f;
}

bool Bot::opening_blocks(Group& g, bool allow_contested) {
    // During the opening: contested camps only as a fallback, camps on the enemy side never.
    // The first v5 games showed why: with our side farmed out, falling back to any camp sent
    // the squad next to the enemy spawn (42% of early deaths there, against 19% before).
    if (!kSafeOpening || round_ >= kOpeningRounds) return false;
    if (kJackpotPush && g.id == world().jackpot) return false;
    if (power_play() && !enemy_side(g)) return false;
    const bool theirs = enemy_side(g);  // also works out g.contested
    if (g.contested) return !allow_contested;
    return theirs;
}

float Bot::target_danger(Group& g) {
    // Our own half is where we have to farm: recent danger there only halves the score.
    const bool theirs = enemy_side(g);
    const float f = danger_factor(g.anchor);
    return (theirs ? f : std::sqrt(f)) * (kContested && g.contested ? kContestedFactor : 1.f);
}

int Bot::recent_team_deaths(int window) const {
    int n = 0;
    for (int r : team_deaths_)
        if (round_ - r <= window) ++n;
    return n;
}

bool Bot::pressured() const { return kPressureMode && recent_team_deaths(kPressureWindow) >= kPressureDeaths; }

bool Bot::calm() const { return !enemy_recent(kCalmRounds) && recent_team_deaths(kCalmRounds) == 0; }

bool Bot::on_my_camp() const {
    // A live camp of our target within our attack range: we are farming it right now.
    if (target_ < 0 || (task_ != Task::farm && task_ != Task::solo)) return false;
    for (const auto& c : camps_)
        if (c.group == target_ && c.hp > 0 && cheb(c.pos, pos_) <= range_) return true;
    return false;
}

bool Bot::enemy_passive() const { return round_ > 300 && !enemy_recent(250) && recent_team_deaths(400) == 0; }

int Bot::carry_gems() const {
    if (pressured()) return kSmallBag;
    if (kMarcellaBank && type_ == bc::champion_type::marcella) return kBankerCarry;
    return kSmallBags ? kSmallCarry : kCarryGems;
}

float Bot::enemy_side_factor(bool solo) const {
    // Ground near the enemy spawn is where we lose champions: shun it when a team is hunting
    // us, and farm it when the other team never shows up.
    if (pressured()) return solo ? 0.1f : 0.15f;
    if (!kShunEnemySide) return solo ? 0.6f : 0.7f;
    if (enemy_passive()) return solo ? 0.7f : 0.8f;
    return solo ? 0.35f : 0.45f;
}

bool Bot::banker_free_near(vec2 p, int range, int group) const {
    // Our Marcella, alive and in sight within range of p, and not busy banking, going home or
    // collecting another reward.
    if (!kMarcellaBank || banker_slot_ < 0 || banker_slot_ == slot_ || banker_slot_ >= slots_) return false;
    if (slot_dead_[banker_slot_]) return false;
    const auto st = comms_.slot(banker_slot_);
    if (st.task == Task::other || (st.task == Task::capture && st.target != group)) return false;
    for (const auto& a : allies_seen_)
        if (a.handle == allies_[banker_slot_]) return cheb(a.pos, p) <= range;
    return false;
}

bool Bot::solo_camp_ok(Group& g, float dps) {
    // A fighting camp on our side, away from the middle, that we clear alone and stay healthy.
    if (enemy_side(g) || g.contested) return false;
    return forecast(g, dps).damage <= hp_ * kSoloShare;
}

bool Bot::altar_guarded(vec2 altar) const {
    // Enemies around the altar (or on us) and no more of us than of them close by.
    int theirs = 0, ours = 1;
    for (const auto& e : enemies_)
        if (!harmless_on_spawn(e) && (cheb(e.pos, altar) <= 6 || cheb(e.pos, pos_) <= 4)) ++theirs;
    if (theirs == 0) return false;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= kCrowdRange) ++ours;
    return ours <= theirs;
}

int Bot::altar_exposure(vec2 altar) const {
    // 0: our side, 1: the middle, 2: the enemy side.
    const auto& W = world();
    const int mine = cheb(altar, W.ally_centre), theirs = cheb(altar, W.enemy_centre);
    if (theirs < mine - std::max(4, (mine + theirs) / 10)) return 2;
    if (theirs <= mine + std::max(4, (mine + theirs) / 10)) return 1;
    return 0;
}

bool Bot::escort_banker(bc::champion& me) {
    // Walk with a teammate who is taking gems to an altar in the middle or on the enemy side,
    // when we are the nearest free teammate (the two nearest for the enemy side).
    if (task_ == Task::capture || hp_ < max_hp_ * 0.5f || on_spawn()) return false;
    if (kCalmNoEscort && calm()) return false;
    const auto& W = world();
    for (int i = 0; i < slots_; ++i) {
        if (i == slot_) continue;
        const auto st = comms_.slot(i);
        if (st.task != Task::other || st.target != kTribute) continue;
        const SeenChamp* banker = nullptr;
        for (const auto& a : allies_seen_)
            if (a.handle == allies_[i]) banker = &a;
        if (!banker || cheb(banker->pos, pos_) > kEscortRange || W.ally_spawn.get(banker->pos)) continue;
        // The altar it is heading for: the nearest one to it. Escort only the last stretch, and
        // only a bag worth guarding.
        vec2 altar = banker->pos;
        int best = 1 << 20;
        for (auto a : W.altars)
            if (cheb(a, banker->pos) < best) {
                best = cheb(a, banker->pos);
                altar = a;
            }
        if (best > (kTeamBank ? kEscortStretch + 4 : kEscortStretch)) continue;
        const int exposure = altar_exposure(altar);
        if (exposure == 0) continue;
        int bag = 0;
        try {
            bag = static_cast<int>(banker->handle.gem_count());
        } catch (const std::exception&) {
            continue;
        }
        if (bag < (kTeamBank ? kEscortBag / 2 : kEscortBag)) continue;
        // Escorts needed, and how many free teammates are closer to the banker than we are.
        const int needed = exposure + (kTeamBank ? 1 : 0);
        const int mine = cheb(banker->pos, pos_);
        int closer = 0;
        for (int j = 0; j < slots_; ++j) {
            if (j == slot_ || j == i) continue;
            const auto sj = comms_.slot(j);
            if (sj.task != Task::farm && sj.task != Task::solo) continue;
            for (const auto& a : allies_seen_)
                if (a.handle == allies_[j]) {
                    const int d = cheb(banker->pos, a.pos);
                    if (d < mine || (d == mine && j < slot_)) ++closer;
                }
        }
        if (closer >= needed) continue;
        mark("escort-bank");
        if (mine > 2) approach_unit(me, banker->pos, 2);
        return true;
    }
    return false;
}

bool Bot::breach_active() const {
    auto& W = world();
    if (!kBreach || W.breach.empty()) return false;
    for (auto t : W.breach)
        if (const auto* c = W.camp_info_at(t); c && c->alive_known) return true;
    return false;
}

const SeenCamp* Bot::breach_camp() const {
    // The outermost breach camp still standing, if we can see it.
    auto& W = world();
    for (auto t : W.breach) {
        const auto* info = W.camp_info_at(t);
        if (!info || !info->alive_known) continue;
        for (const auto& c : camps_)
            if (c.pos == t) return &c;
        return nullptr;
    }
    return nullptr;
}

std::vector<int> Bot::nearest_groups() const {
    const auto& W = world();
    std::vector<int> order(W.groups.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return cheb(W.groups[a].anchor, pos_) < cheb(W.groups[b].anchor, pos_); });
    return order;
}

int Bot::altar_danger(vec2 altar) const {
    // Extra tiles a big bag should walk to avoid an altar enemies were seen at (or we died at)
    // lately, more in the middle and on the enemy side; free altars cost nothing extra.
    bool held = round_ - death_round_ <= 150 && cheb(death_pos_, altar) <= 8;
    for (const auto& s : sightings_)
        if (round_ - s.round <= 60 && cheb(s.pos, altar) <= 8) held = true;
    return held ? 30 + 8 * altar_exposure(altar) : 0;
}

bool Bot::intercept_tribute(bc::champion& me) {
    // An enemy channelling a tribute cannot move and drops its bag to whoever kills it: go for it
    // unless its friends there outnumber us.
    if (hp_ < max_hp_ * 0.5f || task_ == Task::capture) return false;
    const SeenChamp* best = nullptr;
    for (const auto& e : enemies_) {
        if (harmless_on_spawn(e) || cheb(e.pos, pos_) > kInterceptRange) continue;
        bool channelling = false;
        try {
            channelling = e.handle.is_submitting_gems();
        } catch (const std::exception&) {
        }
        if (channelling && (!best || cheb(e.pos, pos_) < cheb(best->pos, pos_))) best = &e;
    }
    if (!best) return false;
    int theirs = 0, ours = 1;
    for (const auto& e : enemies_)
        if (!harmless_on_spawn(e) && cheb(e.pos, best->pos) <= 4) ++theirs;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, best->pos) <= cheb(pos_, best->pos) + 2) ++ours;
    if (theirs > ours) return false;
    mark("intercept");
    if (cheb(best->pos, pos_) <= range_) {
        use_combat_spells(me, nullptr, best);
        attack_target(me, best->handle, best->pos, best->hp);
    } else {
        fire_spells(false);
        approach_unit(me, best->pos, range_);
    }
    return true;
}

bool Bot::guard_altar(bc::champion& me) {
    // On maps whose altar nearest our spawn sits in the middle, one of us waits beside it between
    // jobs: enemies turning in there meet a defender (and our lock cancels their tribute), and our
    // own trips to it are not alone.
    if (task_ == Task::capture || hp_ < max_hp_ * 0.6f || on_spawn() || gems_ >= carry_gems()) return false;
    // Patrol: only kPatrolStay rounds out of every kPatrolPeriod, farming the rest.
    if (kAltarPatrol && round_ % kPatrolPeriod >= kPatrolStay) return false;
    const auto& W = world();
    vec2 altar{-1, -1};
    int best = 1 << 20;
    for (auto a : W.altars)
        if (cheb(a, W.ally_centre) < best) {
            best = cheb(a, W.ally_centre);
            altar = a;
        }
    if (altar.x < 0 || altar_exposure(altar) == 0 || slots_ < 3) return false;
    // The guard: the first living champion in this order (tanks first).
    static constexpr std::array kGuardOrder{bc::champion_type::ryth, bc::champion_type::marcella, bc::champion_type::vendetta,
                                            bc::champion_type::tavi, bc::champion_type::vesper_vale};
    // The guards: the first kAltarGuards living champions in this order.
    int guards = 0;
    bool me_guard = false;
    for (auto t : kGuardOrder) {
        for (int i = 0; i < slots_ && i < static_cast<int>(ally_types_.size()) && guards < kAltarGuards; ++i)
            if (ally_types_[i] == t && !slot_dead_[i] && !(comms_.slot(i).task == Task::other && comms_.slot(i).target == kHome)) {
                ++guards;
                if (i == slot_) me_guard = true;
                break;
            }
    }
    if (!me_guard) return false;
    mark("guard-altar");
    if (cheb(pos_, altar) > 3) approach_unit(me, altar, 2);
    return true;
}

float Bot::kill_value(const SeenChamp& e) const {
    // Damage it deals per point of health it has left: kill what hurts most and dies fastest.
    float v = std::max(e.dps, 1.f) / std::max(e.hp, 1.f);
    if (e.type == bc::champion_type::cassi) v *= 2.f;  // her death takes Issac with her
    if (e.type == bc::champion_type::heart_of_ruin && e.hp > e.max_hp * 0.3f) v *= 0.6f;  // stronger as he bleeds
    if (e.hp <= strength_ * 1.05f) v *= 4.f;  // dies to our next hit
    return v;
}

bool Bot::peel_back(bc::champion& me, const SeenChamp* target) {
    // A healthier teammate within reach to fall back behind.
    const SeenChamp* cover = nullptr;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= 6 && a.hp / std::max(a.max_hp, 1.f) > hp_ / std::max(max_hp_, 1.f) + 0.15f &&
            (!cover || cheb(a.pos, pos_) < cheb(cover->pos, pos_)))
            cover = &a;
    if (!cover) return false;
    auto exposure = [&](vec2 p) {
        int n = 0;
        for (const auto& e : enemies_)
            if (!harmless_on_spawn(e) && cheb(e.pos, p) <= e.reach) ++n;
        return n;
    };
    load_occupancy();
    vec2 best = pos_;
    int best_score = exposure(pos_) * 10 + std::max(0, cheb(pos_, cover->pos) - kPeelRange);
    if (can_move_ && !moved_)
        for (auto d : kDirs) {
            const vec2 n = pos_ + d;
            if (!free_tile(n)) continue;
            const int score = exposure(n) * 10 + std::max(0, cheb(n, cover->pos) - kPeelRange);
            if (score < best_score) {
                best_score = score;
                best = n;
            }
        }
    if (best != pos_) return try_move(me, best - pos_);
    // Already as covered as it gets: keep hitting from here.
    use_combat_spells(me, nullptr, target);
    if (target && cheb(target->pos, pos_) <= range_) return attack_target(me, target->handle, target->pos, target->hp);
    return false;
}

bool Bot::answer_call(bc::champion& me) {
    // A teammate within kHelpRange is fighting: walk over until we can see the fight ourselves.
    if (hp_ < max_hp_ * 0.5f || (on_spawn() && hp_ < max_hp_ * 0.95f)) return false;
    if (kFarmFirst && on_my_camp()) return false;
    if (kBagCaution && gems_ >= kBagCautionGems) return false;  // bank it first
    const SeenChamp* caller = nullptr;
    for (int i = 0; i < slots_ && i < static_cast<int>(allies_.size()); ++i) {
        if (i == slot_) continue;
        const auto st = comms_.slot(i);
        if (st.task != Task::other || (st.target != kFight && st.target != kBagFight)) continue;
        const int range = st.target == kBagFight ? kBagHelpRange : kHelpRange;
        for (const auto& a : allies_seen_)
            if (a.handle == allies_[i] && cheb(a.pos, pos_) <= range && cheb(a.pos, pos_) > 2 &&
                (!caller || cheb(a.pos, pos_) < cheb(caller->pos, pos_)))
                caller = &a;
    }
    if (!caller) return false;
    // Straight-line distance can be across a wall: a short walk only.
    if (kRefineFight && dist_from_me(caller->pos) > kHelpRange + 6) return false;
    mark("answer-call");
    return approach_unit(me, caller->pos, 2);
}

bool Bot::in_their_third(vec2 p) const {
    const auto& W = world();
    const int mine = cheb(p, W.ally_centre), theirs = cheb(p, W.enemy_centre);
    return theirs * 2 < mine;
}

bool Bot::split_active() const {
    // Two squads: both members of the second alive and at least two left in the first.
    if (!kTwoSquads || slots_ < 4 || slot_ < 0) return false;
    int a = 0, b = 0;
    for (int i = 0; i < slots_; ++i) {
        const auto s = comms_.slot(i);
        if (i != slot_ && s.task == Task::other && s.target == kDead) continue;
        if (i >= slots_ - 2) ++b;
        else ++a;
    }
    return b == 2 && a >= 2;
}

int Bot::party_of(int slot) const { return split_active() && slot >= slots_ - 2 ? 1 : 0; }

int Bot::party_b_target() const {
    // What the second squad's leader is farming (its status byte), or -1.
    const auto ls = comms_.slot(slots_ - 2);
    return ls.task == Task::farm ? ls.target : -1;
}

int Bot::my_party_target() const {
    if (!split_active() || party_of(slot_) == 0) return comms_.squad_target();
    if (slot_ == slots_ - 2) return party_target_ >= 0 ? party_target_ : comms_.squad_target();
    const int t = party_b_target();
    return t >= 0 ? t : party_target_;
}

int Bot::home_dist(const Group& g) const {
    // Walking distance from our spawn to the group never changes. Looking it up at the anchor (a
    // camp tile, which the field treats as a wall) searched the tiles around it on every call:
    // about 1,000 fuel per group per plan on 02harvest, most of the planning cost.
    if (g.home >= 0) return g.home;
    auto& W = world();
    const int d = W.dist(W.spawn_poi, g.anchor, 20000);
    if (d != kUnreached || W.has_field(W.spawn_poi)) const_cast<Group&>(g).home = d;
    return d;
}

bool Bot::near_contested_altar(vec2 p) const {
    for (auto a : world().altars)
        if (cheb(a, p) <= 6 && altar_exposure(a) > 0) return true;
    return false;
}

bool Bot::better_target(const SeenChamp& e, const SeenChamp& than) const {
    // With altar defense alone, bags count only around a contested altar (where they come to bank).
    if (kHuntBag || (kAltarDefense && kBagFocus && (near_contested_altar(e.pos) || near_contested_altar(than.pos)))) {
        // Carried gems change hands on the kill: weigh them against the health left to chew through.
        const float a = (e.gems + 10.f) / std::max(e.hp, 1.f), b = (than.gems + 10.f) / std::max(than.hp, 1.f);
        if (std::abs(e.gems - than.gems) >= 10) return a > b;
    }
    return kSmartFocus ? kill_value(e) > kill_value(than) : e.hp < than.hp;
}

bool Bot::lock_detour(bc::champion& me) {
    // A lock in hand and an unlocked altar in the middle or on their side a short walk away.
    if (!has_lock_ || hp_ < max_hp_ * 0.5f || gems_ >= kBagCautionGems || on_spawn()) return false;
    const auto& W = world();
    int best = -1;
    for (int i = 0; i < static_cast<int>(W.altars.size()); ++i) {
        const vec2 a = W.altars[i];
        if (cheb(a, pos_) > 8 || cheb(a, pos_) <= 2 || !visible_.get(a) || altar_exposure(a) == 0) continue;
        // The nearest one (taking the first in list order switched between two altars mid-walk).
        if (kNearestLock && best >= 0 && cheb(a, pos_) >= cheb(W.altars[best], pos_)) continue;
        if (bc::vision::has_altar_lock(a)) continue;
        if (!altar_open(i)) continue;
        best = i;
        if (!kNearestLock) break;
    }
    if (best < 0) return false;
    return approach_unit(me, W.altars[best], 2);
}

float Bot::altar_zone(const Group& g) const {
    // Camps a few tiles from an altar in the middle or on their side: farming there keeps us about
    // when their champions come to bank. Altars and camps never move: worked out once per group.
    if (g.zone >= 0) return g.zone;
    float z = 1.f;
    for (auto a : world().altars)
        if (cheb(a, g.anchor) <= 6 && altar_exposure(a) > 0) {
            z = 1.f + kAltarZoneBias;
            break;
        }
    const_cast<Group&>(g).zone = z;
    return z;
}

bool Bot::invaded() const {
    // Two or more enemies seen together in our half within the last 20 rounds.
    const auto& W = world();
    for (const auto& s : sightings_)
        if (s.count >= 2 && round_ - s.round <= 20 && cheb(s.pos, W.ally_centre) < cheb(s.pos, W.enemy_centre)) return true;
    return false;
}

bool Bot::capturing_here() const {
    if (task_ != Task::capture || target_ < 0) return false;
    for (auto t : world().groups[target_].tiles)
        if (t == pos_) return true;
    return false;
}

bool Bot::early_warning(bc::champion& me) {
    if (on_spawn() || capturing_here()) return false;
    int coming = 0;
    vec2 sum{0, 0};
    for (const auto& e : enemies_)
        if (!harmless_on_spawn(e) && cheb(e.pos, pos_) <= 10) {
            ++coming;
            sum += e.pos;
        }
    int with_us = 1;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= kCrowdRange) ++with_us;
    if (coming <= with_us) return false;
    // Join the nearest teammate we are not already beside, unless it is closer to them than we are.
    const vec2 centre = sum / coming;
    const SeenChamp* mate = nullptr;
    int best = 1000;
    for (const auto& a : allies_seen_) {
        const int d = cheb(a.pos, pos_);
        if (d > 3 && d < best && cheb(a.pos, centre) >= cheb(pos_, centre) - 1) {
            best = d;
            mate = &a;
        }
    }
    mark("early-warning");
    if (mate && approach_unit(me, mate->pos, 2)) return true;
    std::vector<std::pair<vec2, int>> threats;
    for (const auto& e : enemies_)
        if (!harmless_on_spawn(e)) threats.push_back({e.pos, e.range + 4});
    return step_away(me, threats);
}

bool Bot::wait_for_escort() {
    // A champion that respawns and walks back out alone into a team holding our half dies
    // again. Once teammates have been dying, leave spawn with someone.
    if (!kEscort || !on_spawn()) return false;
    if (round_ - respawn_round_ > kEscortWait) return false;
    if (recent_team_deaths(kEscortWindow) < 2) return false;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= 4) return false;
    if (kSmartEscort) {
        bool coming = false;  // a teammate respawning or walking home will be here soon
        for (int i = 0; i < slots_; ++i) {
            if (i == slot_) continue;
            const auto st = comms_.slot(i);
            if (st.task == Task::other && (st.target == kDead || st.target == kHome)) coming = true;
        }
        if (!coming) return false;
    }
    return true;
}

const vec2* Bot::field() {
    if (!kFieldCamp) return nullptr;
    if (!field_ready_) {
        const auto& W = world();
        if (!W.has_field(W.spawn_poi)) return nullptr;  // reachability needs the walk from spawn
        field_ready_ = true;
        int best = 0;
        auto consider = [&](vec2 centre) {
            if (altar_exposure(centre) > 1) return;
            int gems = 0, groups = 0;
            for (const auto& g : W.groups)
                if (g.harmless && g.gems > 0 && !g.unreachable && cheb(g.anchor, centre) <= 7 && home_dist(g) != kUnreached) {
                    gems += g.gems;
                    ++groups;
                }
            const bool ok = (groups >= kFieldGroups && gems >= kFieldGems) || gems >= 100;
            if (ok && gems > best) {
                best = gems;
                field_centre_ = centre;
                field_gems_ = gems;
            }
        };
        for (auto a : W.altars) consider(a);
        for (const auto& g : W.groups)
            if (g.harmless && g.gems >= 100) consider(g.anchor);
        LOG("FIELD " << bc::to_string(type_) << " centre " << bc::to_string(field_centre_) << " gems " << field_gems_);
    }
    return field_gems_ > 0 ? &field_centre_ : nullptr;
}

bool Bot::in_field(vec2 p) {
    const vec2* f = field();
    return f && cheb(*f, p) <= 8;
}

bool Bot::unsafe_alone(Group& g) {
    // Ground the enemy holds: our last death or a recent sighting nearby, or the contested middle
    // while they are about. Somewhere to go as a squad, not alone.
    enemy_side(g);  // works out g.contested
    if (power_play() && !g.contested && g.side != 1) return false;
    if (round_ - death_round_ <= 150 && cheb(death_pos_, g.anchor) <= 8) return true;
    for (const auto& s : sightings_)
        if (round_ - s.round <= kSoloDangerRounds && cheb(s.pos, g.anchor) <= 8) return true;
    return g.contested && enemy_recent(30);
}

bool Bot::held_altar_near(vec2 p) const {
    // A middle or enemy-side altar beside p where we have seen them lately (worked out once a turn:
    // checked for every camp the squad scores).
    if (held_round_ != round_) {
        held_round_ = round_;
        held_altars_.clear();
        for (auto a : world().altars) {
            if (altar_exposure(a) == 0) continue;
            for (const auto& s : sightings_)
                if (round_ - s.round <= 60 && cheb(s.pos, a) <= 8) {
                    held_altars_.push_back(a);
                    break;
                }
        }
    }
    for (auto a : held_altars_)
        if (cheb(a, p) <= 6) return true;
    return false;
}

int Bot::squad_dist(const Group& g) {
    // The longest walk among living teammates not heading home: the squad starts when they arrive.
    auto& W = world();
    int worst = 0;
    for (int i = 0; i < slots_ && i < static_cast<int>(allies_.size()); ++i) {
        if (i == slot_) continue;
        const auto st = comms_.slot(i);
        if (st.task == Task::other && (st.target == kDead || st.target == kHome)) continue;
        for (const auto& a : allies_seen_)
            if (a.handle == allies_[i]) {
                const int d = W.has_field(g.poi) ? W.dist(g.poi, a.pos) : cheb(a.pos, g.anchor);
                if (d != kUnreached) worst = std::max(worst, d);
            }
    }
    return worst;
}

// ---------------------------------------------------------------------------
// Turn entry


void Bot::play_turn() {
    auto& me = bc::self();
    debug_block({ g_marks.clear(); g_marks.push_back({"start", bc::fuel_remaining()}); });
    if (!ready_) {
        init(me);
        // Setup used most of this turn's fuel: start acting next turn instead of running out
        // of fuel halfway through planning.
        if (!ready_ || bc::fuel_remaining() < kActFuel) return;
    }
    round_ = static_cast<int>(bc::round_number());
    comms_.read();
    if (const int eb = static_cast<int>(bc::global_submitted_gems(team::enemy)); eb > enemy_banked_) {
        enemy_banked_ = eb;
        enemy_banked_round_ = round_;
    }
    // Team deaths, from slots switching to "dead" in team memory.
    for (int i = 0; i < slots_ && i < kMaxSlots; ++i) {
        const auto st = comms_.slot(i);
        const bool dead = st.task == Task::other && st.target == kDead;
        if (dead && !slot_dead_[i]) {
            team_deaths_[team_death_next_] = round_;
            team_death_next_ = (team_death_next_ + 1) % static_cast<int>(team_deaths_.size());
        }
        slot_dead_[i] = dead;
    }
    if (kShareClears) {
        // A teammate capturing a group: it was just cleared.
        auto& W = world();
        for (int i = 0; i < slots_ && i < kMaxSlots; ++i) {
            if (i == slot_) continue;
            const auto st = comms_.slot(i);
            if (st.task != Task::capture || st.target < 0 || st.target >= static_cast<int>(W.groups.size())) continue;
            auto& g = W.groups[st.target];
            if (g.state == GroupState::respawning) continue;
            g.state = GroupState::reward;
            g.heard_round = round_;
            g.awake = false;
        }
    }
    moved_ = false;
    cast_done_ = false;
    ready_cache_ = {-1, -1};

    const auto state = me.state();
    if (!state.contains(bc::champion_state::alive)) {
        mark("dead");
        if (was_alive_) {
            // pos_ still holds where we stood on our last living turn.
            death_pos_ = pos_;
            death_round_ = round_;
            was_alive_ = false;
        }
        // Dead champions still get a turn to shop.
        gold_ = static_cast<int>(me.gold_count());
        shop(me, true);
        set_status(Task::other, kDead);
        task_ = Task::farm;
        target_ = -1;
        return;
    }

    if (!was_alive_) respawn_round_ = round_;
    was_alive_ = true;
    // A respawned champion still shows as dead to the team until it says otherwise.
    if (slot_ >= 0) {
        const auto mine = comms_.slot(slot_);
        if (mine.task == Task::other && mine.target == kDead) set_status(Task::farm, -1);
    }
    pos_ = me.position();
    hp_ = me.health();
    max_hp_ = me.max_health();
    strength_ = me.strength();
    focus_ = me.focus();
    gems_ = static_cast<int>(me.gem_count());
    gold_ = static_cast<int>(me.gold_count());
    can_move_ = state.contains(bc::champion_state::can_move);
    can_attack_ = state.contains(bc::champion_state::can_attack);
    can_cast_ = state.contains(bc::champion_state::can_cast);
    // Teammates out of sight are estimated from their base stats times the team's item multiplier.
    comms_.blend_power(dps_of(type_, strength_, focus_) / std::max(my_design_dps_, 1.f));
    mark("observe");
    observe(me);
    damage_taken_ = last_hp_round_ == round_ - 1 ? std::max(0.f, last_hp_ - hp_) : 0.f;
    if (kLearnEnemy && last_expect_round_ == round_ - 1 && last_expect_ > 0 && incoming_camp_damage(pos_) <= 0) {
        dmg_seen_ += damage_taken_;
        dmg_expected_ += last_expect_;
    }
    last_expect_ = 0;
    if ((kLearnEnemy || kDeathScale) && round_ % 250 == 0)
        LOG("SCALE r" << round_ << " " << bc::to_string(type_) << " seen " << dmg_seen_ << " expected " << dmg_expected_ << " scale " << enemy_scale());
    if (damage_taken_ > 0) last_hurt_round_ = round_;
    if (kPowerPlay && !enemy_roster_.empty()) {
        int alive = 0;
        for (const auto& c : enemy_roster_) {
            try {
                alive += c.is_alive() ? 1 : 0;
            } catch (const std::exception&) {
                ++alive;
            }
        }
        enemies_alive_ = alive;
    }
    if (kHealFocus) {
        // Health each teammate lost since last turn (kept per slot).
        ally_drop_.fill(0.f);
        for (const auto& a : allies_seen_)
            for (int i = 0; i < slots_ && i < kMaxSlots; ++i)
                if (allies_[i] == a.handle) {
                    if (ally_seen_round_[i] == round_ - 1) ally_drop_[i] = std::max(0.f, ally_last_hp_[i] - a.hp);
                    ally_last_hp_[i] = a.hp;
                    ally_seen_round_[i] = round_;
                }
    }
    if (kRecallSafe == 2 || kUnseenWard || kHurtFlee) {
        const float from_champs = damage_taken_ - incoming_camp_damage(pos_);
        unseen_hit_ = false;
        if (from_champs > 1.f) {
            last_champ_hurt_round_ = round_;
            // None of the enemies in sight could have reached us: someone is hidden.
            unseen_hit_ = true;
            for (const auto& e : enemies_)
                if (cheb(e.pos, pos_) <= std::max(e.reach, 4) + 1) unseen_hit_ = false;
        }
    }
    last_hp_ = hp_;
    last_hp_round_ = round_;

    if (state.contains(bc::champion_state::recalling)) {
        set_status(Task::other, kHome);
        mark("recalling");
    } else if (state.contains(bc::champion_state::submitting_gems)) {
        set_status(Task::other, kTribute);
        mark("submitting");
        // A tribute only banks when it completes; dying mid-channel hands the bag to the killer.
        if (abandon_tribute(me)) act(me);
    } else if (!state.contains(bc::champion_state::teleporting)) {
        act(me);
    }
    debug_block({
        const auto start = g_marks.empty() ? 0 : g_marks.front().fuel;
        const auto now = bc::fuel_remaining();
        if (start > now && start - now > 60000) {
            std::cout << "FUEL r" << round_ << " " << bc::to_string(type_) << " used" << (start - now);
            for (std::size_t i = 1; i < g_marks.size(); ++i) std::cout << " " << g_marks[i - 1].phase << "=" << (g_marks[i - 1].fuel - g_marks[i].fuel);
            if (!g_marks.empty()) std::cout << " " << g_marks.back().phase << "=" << (g_marks.back().fuel - now);
            std::cout << "\n";
        }
    });
    // Debug builds tally where each champion spends its rounds.
    debug_block({
        static std::map<std::string, int> activity;
        ++activity[g_phase];
        if (std::string(g_phase) == "farm-travel" && !moved_) ++activity["travel-still"];
        if (round_ % 500 == 0) {
            std::cout << "P r" << round_ << " " << bc::to_string(type_);
            for (const auto& [k, v] : activity) std::cout << " " << k << "=" << v;
            std::cout << "\n";
        }
    });

    if (round_ % 20 == 0)
        LOG("S r" << round_ << " " << bc::to_string(type_) << " slot" << slot_ << " task" << static_cast<int>(task_) << " t" << target_
                  << " squad" << comms_.squad_target() << " pos" << bc::to_string(pos_) << " hp" << hp_ << "/" << max_hp_
                  << " gems" << gems_ << " gold" << gold_ << " str" << strength_ << " fuel" << bc::fuel_remaining()
                  << " camps" << camps_.size() << " en" << enemies_.size());
    if (stuck_rounds_ == 15) debug_block({
        std::cout << "STUCK r" << round_ << " " << bc::to_string(type_) << bc::to_string(pos_) << " phase " << g_phase << " task" << int(task_) << " t" << target_ << " gems" << gems_;
        if (target_ >= 0 && task_ != Task::other) {
            const auto& g = world().groups[target_];
            int vis = 0;
            for (const auto& c : camps_)
                if (c.group == target_) ++vis;
            std::cout << " anchor" << bc::to_string(g.anchor) << " state" << int(g.state) << " vis" << vis << " wait" << wait_rounds_;
        }
        std::cout << "\n";
    });
    stuck_rounds_ = pos_ == last_pos_ ? stuck_rounds_ + 1 : 0;
    last_pos_ = pos_;
    // Spend leftover fuel on distance fields we will need later.
    mark("precompute");
    if (bc::fuel_remaining() > kPlanFuel) world().precompute(kPlanFuel - 10000);
}

void Bot::act(bc::champion& me) {
    const bool at_spawn = on_spawn();
    mark("shop");
    // Tavi shops anywhere: with lock denial it also buys the next lock in the field (one at a time).
    if (at_spawn || type_ == bc::champion_type::tavi) shop(me, at_spawn || (kLockDenial && type_ == bc::champion_type::tavi));
    mark("locks");
    place_lock(me);
    if (kUnseenWard && unseen_hit_) reveal_ward(me);
    scout_ward(me);

    mark("enemies");
    if (fight_enemies(me)) {
        // Tell the team (retreats already said "home").
        if (kCallHelp && !capturing_here() && !(comms_.slot(slot_).task == Task::other && comms_.slot(slot_).target == kHome))
            set_status(Task::other, bag_fight_ ? kBagFight : kFight);
        return;
    }
    mark("help");
    if (kCallHelp && !kRefineFight && answer_call(me)) return;
    mark("flee");
    if (flee_camps(me)) return;
    mark("retreat");
    if (retreat_from_camps(me)) return;
    // Out of the camps' reach first, then go and help.
    if (kCallHelp && kRefineFight && answer_call(me)) return;
    // A big bag is a target: bank it before picking another fight (one champion who
    // captured 193 gems alone died seven rounds later and handed them over).
    if (kSmallBags && gems_ >= kBigBag && incoming_camp_damage(pos_) <= 0) {
        mark("bank");
        if (tribute(me)) return;
    }
    mark("camps");
    if (fight_camps(me)) return;
    // Spawn heals fully at turn end; stay a turn when hurt.
    if (at_spawn && hp_ < max_hp_ * 0.95f && enemies_.empty()) {
        set_status(Task::other, kHome);
        mark("heal");
        return;
    }
    if (at_spawn && wait_for_escort()) {
        set_status(Task::other, kHome);
        mark("escort");
        return;
    }
    mark("home");
    if (go_home(me, false)) return;
    mark("tribute");
    if (tribute(me)) return;
    mark("capture");
    if (capture(me)) return;
    mark("escort-bank");
    if (kBankEscort && escort_banker(me)) return;
    mark("guard-altar");
    if (kAltarGuard && guard_altar(me)) return;
    mark("lock-detour");
    if (kLockDenial && lock_detour(me)) return;
    mark("farm");
    if (farm(me)) return;
    mark("idle");
    // Nothing to farm: bank whatever we carry.
    if (gems_ > 0 && !breach_active()) {
        int d = 0;
        const int a = best_altar(d);
        if (a >= 0 && d != kUnreached) {
            set_status(Task::other, kTribute);
            if (cheb(pos_, world().altars[a]) <= bc::constants::submit_gems_range) {
                try {
                    me.submit_gems(world().altars[a]);
                } catch (const std::exception&) {
                }
            } else {
                travel(me, world().altar_poi[a]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Enemy champions

int Bot::enemy_id(const SeenChamp& e) {
    return static_cast<int>(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(e.handle)) & 0x7F);
}

void Bot::fire_spells(bool retreating) {
    if (cast_done_ || !can_cast_) return;
    for (int i = 0; i < static_cast<int>(spells_.size()) && !cast_done_; ++i) {
        auto& sp = spells_[i];
        const bool offensive = sp.kind == bc::ability_kind::damage || sp.kind == bc::ability_kind::cc || sp.kind == bc::ability_kind::aoe_damage;
        if (!offensive || (sp.exclusive && (retreating || moved_))) continue;
        const SeenChamp* best = nullptr;
        const bool stun = (kStunDiscipline || kStunBuffs) && sp.kind == bc::ability_kind::cc;
        if ((sp.kind == bc::ability_kind::cc || (sp.kind == bc::ability_kind::aoe_damage && sp.duration > 0)) && !stun_turn()) continue;
        for (const auto& e : enemies_) {
            if (world().enemy_spawn.get(e.pos) || cheb(e.pos, pos_) > static_cast<int>(sp.range)) continue;
            if (stun && stun_wasted(e)) continue;
            const bool focused = slots_ <= 5 && enemy_id(e) == comms_.focus();
            const bool better = best && better_target(e, *best);
            if (!best || focused || (better && !(slots_ <= 5 && enemy_id(*best) == comms_.focus()))) best = &e;
        }
        if (best && spell_ready(i)) cast_on(sp, best->handle);
    }
}

bool stun_immune(bc::champion_type t) {
    static std::array<int, 16> cache{};  // 0 unknown, 1 no, 2 yes
    auto& c = cache[static_cast<int>(t) & 15];
    if (c == 0) c = bc::champion{t}.passive() == bc::passive_type::stun_immunity ? 2 : 1;
    return c == 2;
}

bool Bot::stun_wasted(const SeenChamp& e) const {
    if (stun_immune(e.type)) return true;
    if (kStunBuffs) return e.stunned > 0;
    if (!kStunOverlap) return false;
    try {
        const auto st = e.handle.state();
        // Channelling a tribute or a recall also stops movement, but a stun still lands then.
        if (st.contains(bc::champion_state::submitting_gems) || st.contains(bc::champion_state::recalling)) return false;
        return !st.contains(bc::champion_state::can_move);
    } catch (const std::exception&) {
        return false;
    }
}

bool Bot::stun_turn() const {
    if (!kStunChain) return true;
    const int mine = type_ == bc::champion_type::tavi ? 0 : type_ == bc::champion_type::ryth ? 1 : -1;
    if (mine < 0) return true;
    for (const auto& a : allies_seen_)
        if ((a.type == bc::champion_type::tavi || a.type == bc::champion_type::ryth) && a.type != type_ && cheb(a.pos, pos_) <= 6)
            return round_ % 2 == mine;
    return true;
}

std::vector<vec2> Bot::dash_landings() {
    // Tiles a 2-tile dash can reach along free tiles.
    std::vector<vec2> out;
    load_occupancy();
    for (auto d1 : kDirs) {
        const vec2 a = pos_ + d1;
        if (!free_tile(a)) continue;
        out.push_back(a);
        for (auto d2 : kDirs) {
            const vec2 b = a + d2;
            if (b == pos_ || !free_tile(b)) continue;
            out.push_back(b);
        }
    }
    return out;
}

bool Bot::dash_toward(bc::champion& me, vec2 target, int within) {
    if (cast_done_ || !can_cast_) return false;
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::dash || s.modifier > 0 || !spell_ready(i)) continue;
        vec2 best{-1, -1};
        int best_d = cheb(pos_, target);
        for (auto t : dash_landings())
            if (cheb(t, target) <= within && cheb(t, target) < best_d + 1) {
                best_d = cheb(t, target);
                best = t;
            }
        if (best.x < 0) return false;
        if (cast_at(s, best)) {
            pos_ = best;
            return true;
        }
        return false;
    }
    (void)me;
    return false;
}

bool Bot::dash_away(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats) {
    if (cast_done_ || !can_cast_ || threats.empty()) return false;
    auto safety = [&](vec2 t) {
        int s = 1000;
        for (const auto& [p, r] : threats) s = std::min(s, cheb(t, p) - r);
        return s;
    };
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::dash || s.modifier > 0 || !spell_ready(i)) continue;
        const auto& W = world();
        vec2 best{-1, -1};
        int best_s = safety(pos_), best_home = cheb(pos_, W.ally_centre);
        for (auto t : dash_landings()) {
            const int sa = safety(t), home = cheb(t, W.ally_centre);
            if (sa > best_s || (sa == best_s && best.x >= 0 && home < best_home)) {
                best_s = sa;
                best_home = home;
                best = t;
            }
        }
        if (best.x < 0 || best_s <= safety(pos_)) return false;
        if (cast_at(s, best)) {
            pos_ = best;
            return true;
        }
        return false;
    }
    (void)me;
    return false;
}

bool Bot::harmless_on_spawn(const SeenChamp& e) const {
    // Enemies on their own spawn heal to full every round and cannot be hunted; they only
    // matter when they can hit us from there.
    return world().enemy_spawn.get(e.pos) && cheb(e.pos, pos_) > (kBurstModel ? e.reach : e.range) + 1;
}

bool Bot::fight_enemies(bc::champion& me) {
    if (enemies_.empty()) {
        hold_turns_ = 0;
        return false;
    }
    const float my_dps = kReadyFights ? dps_now(me, type_, strength_, focus_) : dps_of(type_, strength_, focus_);
    float ally_dps = my_dps, ally_hp = hp_;
    // Count allies as far out as enemies: a shorter ally range made even fights look lost.
    const int ally_range = kStandAndFight ? 8 : 6;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= ally_range) {
            ally_dps += kReadyFights ? a.now : a.dps;
            ally_hp += a.hp;
        }
    float enemy_dps = 0, enemy_hp = 0, threat = 0, burst = 0, expect = 0;
    const SeenChamp* nearest = nullptr;
    int nearest_d = 1000;
    // With the burst model we also look at groups 8-10 tiles out, to leave before they arrive.
    const int sense = kBurstModel ? 10 : 8;
    for (const auto& e : enemies_) {
        if (harmless_on_spawn(e)) continue;
        const int d = cheb(e.pos, pos_);
        const float edps = kReadyFights ? e.now : kPunish && e.disarmed ? e.basic : e.dps;
        if (d <= sense) {
            enemy_dps += edps;
            enemy_hp += e.hp;
        }
        if (d <= e.range + 1) {
            threat += edps;
            // Our share when it can also hit teammates beside us.
            if (kLearnEnemy) {
                int others = 0;
                for (const auto& a : allies_seen_)
                    if (cheb(a.pos, e.pos) <= e.range + 1) ++others;
                expect += e.dps / static_cast<float>(1 + others);
            }
        }
        // What could land on us before our next turn if we stay here.
        if (d <= e.reach) burst += e.burst;
        if (d < nearest_d) {
            nearest_d = d;
            nearest = &e;
        }
    }
    if (kLearnEnemy || kDeathScale) {
        last_expect_ = expect;
        last_expect_round_ = round_;
        const float k = enemy_scale();
        enemy_dps *= k;
        threat *= k;
        burst *= k;
    }
    if (!nearest) return false;
    if (kInterceptTribute && intercept_tribute(me)) return true;
    if (nearest_d > (kBurstModel ? 10 : 7)) return kEarlyWarning && nearest_d <= 10 && early_warning(me);
    // Cassi and Issac are one pick and one life in two bodies: with kFairHeads they count half each.
    // Otherwise a team with them showed six heads to our five and our champions held back.
    auto head = [](const SeenChamp& c) {
        return kFairHeads && (c.type == bc::champion_type::cassi || c.type == bc::champion_type::issac) ? 0.5f : 1.f;
    };
    float their_heads = 0, our_heads = 1;
    for (const auto& e : enemies_)
        if (!harmless_on_spawn(e) && cheb(e.pos, pos_) <= kCrowdRange) their_heads += head(e);
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= kCrowdRange) our_heads += head(a);
    const bool outnumbered = kStandAndFight && kAvoidOutnumbered && their_heads > our_heads;

    // Rounds for each side to wipe the other if everyone nearby commits.
    const float we_need = enemy_hp / std::max(ally_dps, 1.f);
    const float they_need = ally_hp / std::max(enemy_dps, 1.f);
    // Under pressure we stop taking even fights: those are the ones we kept losing.
    const bool losing = we_need > they_need * (pressured() ? 1.0f : 1.3f);
    // A bag worth guarding (it goes to whoever kills us): leave as soon as the fight looks lost.
    const bool bag = kBagCaution && gems_ >= kBagCautionGems;
    const bool clearly_losing = we_need > they_need * (bag ? 1.0f : pressured() ? 1.3f : 1.6f) || outnumbered;
    // A bag near a contested altar worth leaving our task for: big enough, bigger than ours, and a
    // fight we clearly win once the carrier's escort is counted too.
    auto worth_bag = [&](const SeenChamp& t) {
        if (t.gems < std::max(kBagWorthGems, gems_ + 10) || hp_ < max_hp_ * 0.6f || outnumbered) return false;
        if (!near_contested_altar(t.pos)) return false;
        float edps = 0, ehp = 0, adps = my_dps, ahp = hp_;
        for (const auto& e : enemies_)
            if (!harmless_on_spawn(e) && (cheb(e.pos, pos_) <= sense || cheb(e.pos, t.pos) <= kCrowdRange)) {
                edps += e.dps;
                ehp += e.hp;
            }
        for (const auto& a : allies_seen_)
            if (cheb(a.pos, pos_) <= ally_range || cheb(a.pos, t.pos) <= kCrowdRange) {
                adps += a.dps;
                ahp += a.hp;
            }
        return ehp / std::max(adps, 1.f) * kBagFightEdge <= ahp / std::max(edps, 1.f);
    };
    bag_fight_ = false;
    if (kBagCall && kWorthBag && kAltarDefense)
        for (const auto& e : enemies_)
            if (cheb(e.pos, pos_) <= 7 && worth_bag(e)) bag_fight_ = true;

    std::vector<std::pair<vec2, int>> threats;
    for (const auto& e : enemies_) {
        if (!kBurstModel) threats.push_back({e.pos, e.range + 1});
        else if (!harmless_on_spawn(e)) threats.push_back({e.pos, e.reach + 1});
    }
    if (kCampAwareFights)
        for (const auto& c : camps_)
            if (c.awake && c.strength > 0) threats.push_back({c.pos, c.range});
    if (nearest_d > 7) {
        // A group coming our way that we cannot fight: leave before it arrives rather than
        // finishing a camp and getting caught.
        if (!losing || on_spawn()) return false;
        set_status(Task::other, kHome);
        return step_away(me, threats);
    }

    // Target: the enemy we can reach this turn that dies soonest; melee may step first.
    const int reach = ranged_ ? range_ : range_ + 1;
    // Enemies on their own spawn heal to full every round and cannot be reached.
    auto huntable = [&](const SeenChamp& e) { return !world().enemy_spawn.get(e.pos); };
    const SeenChamp* target = nullptr;
    for (const auto& e : enemies_)
        if (huntable(e) && cheb(e.pos, pos_) <= reach && (!target || better_target(e, *target))) target = &e;
    // Everyone in reach of the team's chosen enemy hits it; otherwise ours becomes the choice.
    if (slots_ <= 5) {
        for (const auto& e : enemies_)
            if (enemy_id(e) == comms_.focus() && huntable(e) && cheb(e.pos, pos_) <= reach) target = &e;
        if (target) comms_.set_focus(enemy_id(*target));
    }
    if (kFocusStunned) {
        // A stunned enemy can neither hit back nor run: everyone in reach takes it.
        const SeenChamp* held = nullptr;
        for (const auto& e : enemies_)
            if (huntable(e) && e.stunned > 0 && cheb(e.pos, pos_) <= reach && (!held || e.hp < held->hp)) held = &e;
        if (held) {
            target = held;
            if (slots_ <= 5) comms_.set_focus(enemy_id(*held));
        }
    }
    const bool can_finish = target && target->hp <= strength_ * 1.05f;
    // One-turn burst decides whether we survive the next round; averaged damage hid spell
    // nukes and let enemies one-shot us.
    const bool in_danger = hp_ < threat * 2.5f + max_hp_ * 0.1f || (kBurstModel && burst >= hp_ * 0.7f);

    auto strike = [&](const SeenChamp& want) {
        // A ranged champion cannot walk and shoot in one turn: with another enemy in range, shoot it
        // rather than walk after one that stepped out.
        const SeenChamp* pick = &want;
        if (kNoChase && ranged_ && cheb(want.pos, pos_) > range_)
            for (const auto& o : enemies_)
                if (huntable(o) && cheb(o.pos, pos_) <= range_ && (pick == &want || kill_value(o) > kill_value(*pick))) pick = &o;
        const SeenChamp& e = *pick;
        bool acted = false;
        if (kLeash && cheb(e.pos, pos_) > range_ && in_their_third(e.pos)) return false;
        // Just out of reach and one turn of hits from dying: dash in (melee only; whether a ranged
        // champion may shoot after a dash is untested).
        if (kCombatDash && !ranged_ && can_attack_ && cheb(e.pos, pos_) > range_ + 1 && cheb(e.pos, pos_) <= range_ + 2 &&
            e.hp <= strength_ * kDashKill && dash_toward(me, e.pos, range_))
            acted = true;
        if (cheb(e.pos, pos_) > range_) acted |= approach_unit(me, e.pos, range_);
        if (cheb(e.pos, pos_) <= range_) {
            use_combat_spells(me, nullptr, &e);
            acted |= attack_target(me, e.handle, e.pos, e.hp);
        }
        return acted;
    };

    // A champion running alone from a fight its teammates are in is the one that dies:
    // with two allies beside us and the fight not lost, stay unless nearly dead.
    int beside = 0;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= 3) ++beside;
    const int danger_range = kBurstModel ? nearest->reach + 1 : nearest->range + 2;
    bool committed, retreating;
    if (kStandAndFight) {
        // One teammate beside us is enough to hold a fight that is not clearly lost.
        committed = beside >= 1 && !clearly_losing && hp_ > max_hp_ * 0.25f && (!kBurstModel || burst < hp_);
        retreating = (in_danger || (clearly_losing && nearest_d <= danger_range)) && !can_finish && !committed;
        if (kHomeDefense && !retreating && losing && !can_finish && !on_spawn() && invaded() &&
            cheb(pos_, world().ally_centre) <= kHomeDefenseRange)
            retreating = true;
        // A melee champion beside us walks and hits every turn: running only gives it free
        // hits, so hit back instead.
        if (retreating && !on_spawn() && nearest->range <= 1 && nearest_d <= 2 && target && cheb(target->pos, pos_) <= range_) retreating = false;
    } else {
        committed = beside >= 2 && !losing && hp_ > max_hp_ * 0.2f && (!kBurstModel || burst < hp_);
        retreating = (in_danger || (losing && nearest_d <= danger_range)) && !can_finish && !committed;
    }
    // Spells reach further than basic attacks (range 4): fire them at the team's focus
    // enemy, or the weakest in range, before deciding where to walk.
    fire_spells(retreating);
    if (kHoldCapture && capturing_here() && !in_danger && !clearly_losing) {
        // Hold the region: hit whatever is in range without stepping off it.
        use_combat_spells(me, nullptr, target);
        if (target && cheb(target->pos, pos_) <= range_) attack_target(me, target->handle, target->pos, target->hp);
        set_status(Task::capture, target_);
        mark("hold-capture");
        return true;
    }
    if (retreating && on_spawn()) {
        // Already home, where enemies cannot step and we heal every turn: shoot whatever
        // is in range, stay while healing, and otherwise get on with the game instead of
        // standing here for hundreds of rounds.
        use_combat_spells(me, nullptr, target);
        bool hit = false;
        if (target && cheb(target->pos, pos_) <= range_) hit = attack_target(me, target->handle, target->pos, target->hp);
        if (hit || hp_ < max_hp_ * 0.9f) {
            set_status(Task::other, kHome);
            return true;
        }
        return false;
    }
    if (retreating && kPeelBack && hp_ > max_hp_ * 0.15f && peel_back(me, target)) {
        mark("peel-back");
        return true;
    }
    if (retreating) {
        // Shoot first if we cannot get away anyway, then run toward home.
        use_combat_spells(me, nullptr, target);
        set_status(Task::other, kHome);
        if (kCombatDash && in_danger && dash_away(me, threats)) {
            mark("dash-away");
            return true;
        }
        if (kCrystalEscape && in_danger && hp_ > threat * 3.5f && crystal_escape(me, threats)) {
            mark("crystal");
            return true;
        }
        if (step_away(me, threats, kGrassRetreat)) return true;
        if (target && cheb(target->pos, pos_) <= range_) {
            attack_target(me, target->handle, target->pos, target->hp);
            return true;
        }
        // No tile is safer and nothing is in range. If nothing has hurt us for a while,
        // the threat is across a wall or a hole: get on with the game rather than stare
        // at it (one champion stood like this for 200+ rounds).
        return round_ - last_hurt_round_ <= 8;
    }
    // Farm first: on our camp with no enemy able to hit us next turn, leave the champions be.
    bool farming = false;
    if (kFarmFirst && !retreating && on_my_camp()) {
        farming = true;
        for (const auto& e : enemies_)
            if (!harmless_on_spawn(e) && cheb(e.pos, pos_) <= e.range + 1) farming = false;
    }
    if (farming && !(target && cheb(target->pos, pos_) <= range_)) return false;
    const bool fit = kEngageFloor <= 0 || hp_ >= max_hp_ * kEngageFloor;
    if (kJoinFights && fit && !retreating && !in_danger && !bag && !(kRefineFight && target)) {
        // A teammate is in a fight: hit the enemy on it rather than keep farming or wait for an
        // enemy to walk into range (on 01elk-arena our Vesper Vale hit the elk while three of us died).
        const SeenChamp* help = nullptr;
        int help_d = 1000;
        for (const auto& e : enemies_) {
            if (!huntable(e) || cheb(e.pos, pos_) > 7) continue;
            bool on_ally = false;
            for (const auto& a : allies_seen_)
                if (cheb(a.pos, e.pos) <= std::max(2, e.range)) on_ally = true;
            if (!on_ally) continue;
            const bool focused = slots_ <= 5 && enemy_id(e) == comms_.focus();
            int d = focused ? -1 : cheb(e.pos, pos_);
            const bool bag_here = kWorthBag ? kAltarDefense && worth_bag(e) : (kHuntBag || (kAltarDefense && near_contested_altar(e.pos))) && e.gems >= 20;
            if (bag_here && !focused) d -= 3;  // a bag worth the extra steps
            if (d < help_d) {
                help_d = d;
                help = &e;
            }
        }
        if (help && strike(*help)) {
            mark("join-fight");
            hold_turns_ = 0;
            return true;
        }
    }
    if (target && strike(*target)) {
        hold_turns_ = 0;
        return true;
    }
    if (kStandAndFight && fit && !losing && !outnumbered) {
        // Engage rather than keep farming while an enemy walks up and picks the moment.
        const SeenChamp* goal = nullptr;
        for (const auto& e : enemies_) {
            // Altar defense: a bag carrier coming to bank near a contested altar is worth a few more tiles.
            const bool bag_near_altar = kAltarDefense && cheb(e.pos, pos_) <= 7 &&
                                        (kWorthBag ? worth_bag(e) : e.gems >= 20 && near_contested_altar(e.pos));
            if (!huntable(e) || (cheb(e.pos, pos_) > (kEngageFar ? 7 : kEngageRange) && !bag_near_altar)) continue;
            const bool focused = slots_ <= 5 && enemy_id(e) == comms_.focus();
            const bool better = goal && better_target(e, *goal);
            if (!goal || focused || (better && !(slots_ <= 5 && enemy_id(*goal) == comms_.focus()))) goal = &e;
        }
        if (goal) {
            bool busy = false;  // a teammate is already on it
            for (const auto& a : allies_seen_)
                if (cheb(a.pos, goal->pos) <= 2) busy = true;
            const bool low = goal->hp <= strength_ * 2.f;
            // A ranged champion that steps in cannot attack that turn, and a ranged enemy at the
            // edge of its range shoots first: let it come to us instead.
            const bool under_fire = kCampAwareFights && incoming_camp_damage(pos_) > 0;
            // A carrier on its way to bank will not walk up to us: chase it.
            const bool chase_bag = kWorthBag && kAltarDefense && worth_bag(*goal);
            if (ranged_ && !busy && !low && hold_turns_ < kMaxHold && !under_fire && !chase_bag) {
                ++hold_turns_;
                mark("hold");
                if (kBushHold && !world().bushes.get(pos_) && can_move_ && !moved_) {
                    // Wait in the grass beside us if that is no closer to anyone's reach.
                    auto exposure = [&](vec2 p) {
                        int worst = 1000;
                        for (const auto& e : enemies_)
                            if (!harmless_on_spawn(e)) worst = std::min(worst, cheb(p, e.pos) - e.reach);
                        return worst;
                    };
                    load_occupancy();
                    const int here = exposure(pos_);
                    for (auto d : kDirs) {
                        const vec2 n = pos_ + d;
                        if (!free_tile(n) || !world().bushes.get(n) || exposure(n) < here) continue;
                        if (try_move(me, d)) break;
                    }
                }
                return true;
            }
            hold_turns_ = 0;
            if (strike(*goal)) return true;
        }
    }
    // Even or better: close in on the weakest enemy nearby rather than get kited.
    if (!kStandAndFight && !losing) {
        const SeenChamp* weakest = nullptr;
        for (const auto& e : enemies_)
            if (huntable(e) && cheb(e.pos, pos_) <= 4 && (!weakest || e.hp < weakest->hp)) weakest = &e;
        if (weakest && strike(*weakest)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Camps

bool Bot::camp_threatens(const SeenCamp& c, vec2 at) const {
    if (!c.awake || c.strength <= 0 || cheb(c.pos, at) > c.range) return false;
    // Camps hit the nearest champion; ties count against us.
    const int mine = cheb(c.pos, at);
    for (const auto& a : allies_seen_)
        if (cheb(c.pos, a.pos) < mine) return false;
    for (const auto& e : enemies_)
        if (cheb(c.pos, e.pos) < mine) return false;
    return true;
}

float Bot::incoming_camp_damage(vec2 at) const {
    float total = 0;
    for (const auto& c : camps_)
        if (camp_threatens(c, at)) total += c.strength;
    return total;
}

bool Bot::can_beat(const Group& g) const {
    if (g.harmless) return true;
    if (siege_ && g.id == target_ && task_ == Task::farm && hp_ > max_hp_ * 0.4f) return true;
    float dps = 0, hp = 0;
    local_power(dps, hp);
    return forecast(g, dps).damage <= hp * 0.8f;
}

bool Bot::hazardous(vec2 p, int except_group) const {
    const auto& W = world();
    for (const auto& g : W.groups) {
        if (g.id == except_group || g.harmless || !g.alive_by(round_) || cheb(g.anchor, p) > 12) continue;
        for (const auto& c : g.camps)
            if (c.alive_known && c.strength > 0 && cheb(c.pos, p) <= c.range) return true;
    }
    return false;
}

bool Bot::flee_camps(bc::champion& me) {
    std::vector<std::pair<vec2, int>> threats;
    // Awake camps we cannot beat with the champions here.
    for (const auto& c : camps_)
        if (c.awake && c.strength > 0 && c.group >= 0 && cheb(c.pos, pos_) <= c.range + 1 && !can_beat(world().groups[c.group]))
            threats.push_back({c.pos, c.range + 1});
    // Damage nothing visible explains: a camp behind a wall is hitting us.
    float explained = 0;
    for (const auto& c : camps_)
        if (camp_threatens(c, pos_)) explained += c.strength;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= e.range + 1) explained += e.dps * 2;
    if (damage_taken_ > explained + 25) {
        for (const auto& g : world().groups) {
            if (g.harmless || !g.alive_by(round_) || cheb(g.anchor, pos_) > 12) continue;
            for (const auto& c : g.camps)
                if (c.alive_known && c.strength > 0 && cheb(c.pos, pos_) <= c.range && !visible_.get(c.pos))
                    threats.push_back({c.pos, c.range + 1});
        }
    }
    if (threats.empty()) return false;
    use_combat_spells(me, nullptr, nullptr);
    return step_away(me, threats);
}

bool Bot::retreat_from_camps(bc::champion& me) {
    const float incoming = incoming_camp_damage(pos_);
    if (incoming <= 0) return false;
    // Melee need more steps to get out of range than ranged champions.
    // Every step out of reach may cost another hit (monoliths reach five tiles).
    int steps = 1;
    for (const auto& c : camps_)
        if (camp_threatens(c, pos_)) steps = std::max(steps, c.range + 1 - cheb(c.pos, pos_));
    const float buffer = incoming * std::max(ranged_ ? 2.5f : 3.5f, steps * 0.9f) + max_hp_ * 0.08f;
    if (hp_ > buffer) return false;
    std::vector<std::pair<vec2, int>> threats;
    for (const auto& c : camps_)
        if (c.awake && c.strength > 0) threats.push_back({c.pos, c.range});
    // A shield or heal might buy the hit instead.
    use_combat_spells(me, nullptr, nullptr);
    if (step_away(me, threats)) return true;
    return false;
}

bool Bot::engage_ready(const Group& g) {
    if (g.awake || g.harmless) return true;
    if (g.id == world().breach_group && breach_active()) {
        // Only the camps within reach of where we stand hit back: go in with a teammate.
        int here = 1;
        for (const auto& a : allies_seen_)
            if (cheb(a.pos, g.anchor) <= g.extent + 5) ++here;
        return here >= 2 || wait_rounds_ > 20;
    }
    if (siege_ && g.id == target_) {
        float dps = 0, hp = 0;
        int living = 0;
        team_power(dps, hp, living);
        int here = 1;
        for (const auto& a : allies_seen_)
            if (cheb(a.pos, g.anchor) <= 8) ++here;
        return here >= std::max(2, living - 1) || (wait_rounds_ > 15 && here >= 2);
    }
    float dps = dps_of(type_, strength_, focus_), hp = hp_;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, g.anchor) <= 7) {
            dps += a.dps;
            hp += a.hp;
        }
    const auto f = forecast(g, dps);
    const float margin = wait_rounds_ > 15 ? 0.8f : 0.6f;
    return f.damage <= hp * margin;
}

bool Bot::fight_camps(bc::champion& me) {
    auto& W = world();
    // Join any awake group within reach; otherwise engage our target once the party is ready.
    int gid = -1;
    for (const auto& c : camps_)
        if (c.awake && c.group >= 0 && cheb(c.pos, pos_) <= std::max(c.range, range_) + 2 && can_beat(W.groups[c.group])) {
            gid = c.group;
            break;
        }
    const bool weak = hp_ < max_hp_ * 0.4f;
    if (gid >= 0 && weak && !W.groups[gid].harmless) return false;
    if (kCampWary) {
        // A fighting camp in contested or enemy ground is not worth our health with enemy
        // champions about to arrive: leave it until they are dealt with.
        auto wary = [&](int id) {
            auto& g = W.groups[id];
            if (g.harmless || !(enemy_side(g) || g.contested)) return false;
            for (const auto& e : enemies_)
                if (!harmless_on_spawn(e) && (cheb(e.pos, g.anchor) <= 10 || cheb(e.pos, pos_) <= 10)) return true;
            return false;
        };
        if (gid >= 0 && wary(gid)) return false;
        if (gid < 0 && target_ >= 0 && (task_ == Task::farm || task_ == Task::solo) && wary(target_)) return false;
    }
    if (gid < 0 && (task_ == Task::farm || task_ == Task::solo) && target_ >= 0) {
        const auto& g = W.groups[target_];
        bool near = false;
        for (const auto& c : camps_)
            if (c.group == target_ && cheb(c.pos, pos_) <= range_ + 3) near = true;
        if (near && !(weak && !g.harmless) && engage_ready(g)) gid = target_;
    }
    if (gid < 0) return false;
    const auto& g = W.groups[gid];

    // Focus fire: strongest camp first (cuts incoming damage fastest), then the most wounded.
    auto better = [](const SeenCamp* a, const SeenCamp* b) {
        if (!b) return true;
        if (a->strength != b->strength) return a->strength > b->strength;
        return a->hp < b->hp;
    };
    const SeenCamp* focus = nullptr;
    const SeenCamp* in_range = nullptr;
    for (const auto& c : camps_) {
        if (c.group != gid) continue;
        if (better(&c, focus)) focus = &c;
        if (cheb(c.pos, pos_) <= range_ && better(&c, in_range)) in_range = &c;
    }
    if (gid == W.breach_group && breach_active()) {
        // Breaching: the outermost camp on the way in, nothing else in the ring.
        focus = breach_camp();
        in_range = focus && cheb(focus->pos, pos_) <= range_ ? focus : nullptr;
        if (!focus) {
            for (auto t : W.breach)
                if (const auto* info = W.camp_info_at(t); info && info->alive_known) {
                    approach_unit(me, t, range_);
                    break;
                }
            return true;
        }
    }
    if (!focus) return false;
    if (task_ != Task::capture) set_status(task_ == Task::solo ? Task::solo : Task::farm, gid);

    if (ranged_) {
        // Ranged cannot walk and attack in one turn: shoot whatever is in range.
        if (in_range) {
            use_combat_spells(me, in_range, nullptr);
            attack_camp(me, *in_range);
            return true;
        }
        // Out of range: a spell may still reach (range 4) before walking in.
        use_combat_spells(me, focus, nullptr);
        if (!moved_) approach_unit(me, focus->pos, range_);
        return true;
    }

    // Melee: step next to the focus, then hit it (or whatever is adjacent).
    if (cheb(focus->pos, pos_) > 1) approach_unit(me, focus->pos, 1);
    const SeenCamp* adjacent = nullptr;
    for (const auto& c : camps_)
        if (c.group == gid && cheb(c.pos, pos_) <= 1 && better(&c, adjacent)) adjacent = &c;
    if (focus && cheb(focus->pos, pos_) <= 1) adjacent = focus;
    if (adjacent) {
        use_combat_spells(me, adjacent, nullptr);
        attack_camp(me, *adjacent);
    } else {
        use_combat_spells(me, focus, nullptr);
    }
    (void)g;
    return true;
}

bool Bot::attack_camp(bc::champion& me, const SeenCamp& camp) { return attack_target(me, camp.handle, camp.pos, camp.hp); }

bool Bot::attack_target(bc::champion& me, const bc::unit& target, vec2 target_pos, float target_hp) {
    if (!can_attack_ || cheb(pos_, target_pos) > range_) return false;
    try {
        me.attack(target);
    } catch (const std::exception&) {
        return false;
    }
    can_attack_ = false;
    if (ranged_) can_move_ = false;
    // An attack reset buys a second hit this turn.
    if (cast_done_ || !can_cast_) return true;
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::attack_reset || !spell_ready(i)) continue;
        // A dead target leaves vision; querying it would throw.
        if (target_hp <= strength_) break;
        if (cast(s)) {
            can_attack_ = true;
            try {
                me.attack(target);
                can_attack_ = false;
            } catch (const std::exception&) {
            }
        }
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Spells

bool Bot::spell_ready(int index) {
    if (index < 0 || index >= static_cast<int>(spells_.size())) return false;
    if (ready_cache_[index] < 0) ready_cache_[index] = spells_[index].handle.is_ready() ? 1 : 0;
    return ready_cache_[index] == 1;
}

bool Bot::cast(Spell& spell) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use();
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::cast_on(Spell& spell, const bc::camp& target) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use(target);
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::cast_on(Spell& spell, const bc::champion& target) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use(target);
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::cast_at(Spell& spell, vec2 tile) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use(tile);
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::try_heal(bc::champion& me) {
    (void)me;
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::heal) continue;
        const SeenChamp* best = nullptr;
        const float worth = focus_ * s.modifier * 0.6f;
        float best_missing = worth;
        for (const auto& a : allies_seen_) {
            const float missing = a.max_hp - a.hp;
            if (cheb(a.pos, pos_) > static_cast<int>(s.range) || missing <= worth) continue;
            float score = missing;
            if (kHealFocus)
                for (int k = 0; k < slots_ && k < kMaxSlots; ++k)
                    if (allies_[k] == a.handle) score += 2.f * ally_drop_[k];
            if (score > best_missing) {
                best = &a;
                best_missing = score;
            }
        }
        if (best && spell_ready(i) && cast_on(s, best->handle)) return true;
    }
    return false;
}

void Bot::use_combat_spells(bc::champion& me, const SeenCamp* camp, const SeenChamp* enemy) {
    if (cast_done_ || !can_cast_) return;
    if (try_heal(me)) return;
    const bool threatened = incoming_camp_damage(pos_) > 0 || (enemy && cheb(enemy->pos, pos_) <= enemy->range + 1);
    for (int i = 0; i < static_cast<int>(spells_.size()) && !cast_done_; ++i) {
        auto& s = spells_[i];
        switch (s.kind) {
        case bc::ability_kind::shield:
            if (threatened && hp_ < max_hp_ * 0.9f && spell_ready(i)) cast(s);
            break;
        case bc::ability_kind::damage:
        case bc::ability_kind::cc: {
            // Stuns are wasted on harmless or sleeping camps; damage spells are not.
            const bool stun_only = s.kind == bc::ability_kind::cc && s.modifier <= 0;
            if (enemy && s.kind == bc::ability_kind::cc && !stun_turn() && cheb(enemy->pos, pos_) <= static_cast<int>(s.range)) break;
            if (enemy && cheb(enemy->pos, pos_) <= static_cast<int>(s.range)) {
                if ((kStunDiscipline || kStunBuffs) && s.kind == bc::ability_kind::cc && stun_wasted(*enemy)) {
                    // Another enemy in range takes it; a pure stun otherwise waits.
                    const SeenChamp* other = nullptr;
                    for (const auto& e : enemies_)
                        if (&e != enemy && !world().enemy_spawn.get(e.pos) && cheb(e.pos, pos_) <= static_cast<int>(s.range) && !stun_wasted(e)) other = &e;
                    if (other) {
                        if (spell_ready(i)) cast_on(s, other->handle);
                    } else if (!stun_only && spell_ready(i)) {
                        cast_on(s, enemy->handle);
                    }
                    break;
                }
                if (spell_ready(i)) cast_on(s, enemy->handle);
            } else if (camp && cheb(camp->pos, pos_) <= static_cast<int>(s.range)) {
                if (s.kind == bc::ability_kind::cc && (!camp->awake || camp->strength <= 0) && stun_only) break;
                if (camp->hp < strength_ && s.kind == bc::ability_kind::damage) break;  // the basic attack finishes it
                if (spell_ready(i)) cast_on(s, camp->handle);
            }
            break;
        }
        case bc::ability_kind::aoe_damage: {
            if (enemy && s.duration > 0 && !stun_turn() && cheb(enemy->pos, pos_) <= static_cast<int>(s.range)) break;
            if (enemy && cheb(enemy->pos, pos_) <= static_cast<int>(s.range)) {
                if (spell_ready(i)) cast_on(s, enemy->handle);
            } else if (camp && cheb(camp->pos, pos_) <= static_cast<int>(s.range)) {
                // Prefer a centre that also catches neighbours of the target.
                int hits = 0;
                for (const auto& c : camps_)
                    if (c.group == camp->group && cheb(c.pos, camp->pos) <= static_cast<int>(s.radius)) ++hits;
                if ((hits >= 2 || camp->hp > strength_ * 2) && spell_ready(i)) cast_on(s, camp->handle);
            }
            break;
        }
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Movement

bool Bot::try_move(bc::champion& me, vec2 delta) {
    if (!can_move_ || moved_) return false;
    const vec2 dest = pos_ + delta;
    bool ok = false;
    try {
        ok = me.move(delta);
    } catch (const std::exception&) {
    }
    if (ok) {
        pos_ = dest;
        moved_ = true;
        can_move_ = false;
        if (ranged_) can_attack_ = false;
        return true;
    }
    occupied_.set(dest);
    return false;
}

bool Bot::step_away(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats, bool grass) {
    if (!can_move_ || moved_) return false;
    load_occupancy();
    auto& W = world();
    auto margin = [&](vec2 p) {
        int worst = 1000;
        for (const auto& [t, r] : threats) worst = std::min(worst, cheb(p, t) - r);
        // Enemies outside a grass patch cannot see in: as good as a tile further away.
        return grass && W.bushes.get(p) ? worst + 1 : worst;
    };
    const auto* home = W.field(W.spawn_poi, 20000);
    int best = margin(pos_);
    std::uint16_t best_home = home ? W.dist_on(*home, pos_) : 0;
    vec2 best_tile = pos_;
    for (auto d : kDirs) {
        const vec2 n = pos_ + d;
        if (!free_tile(n)) continue;
        const int m = margin(n);
        const std::uint16_t h = home ? home->get(n) : 0;
        if (m > best || (m == best && h < best_home)) {
            best = m;
            best_home = h;
            best_tile = n;
        }
    }
    if (best_tile == pos_) return false;
    return try_move(me, best_tile - pos_);
}

bool Bot::approach(bc::champion& me, bc::bitboard goal) {
    if (!can_move_ || moved_) return false;
    load_occupancy();
    auto& W = world();
    bc::bitboard walls = W.blocked_board | occupied_board_;
    walls.set(pos_, false);
    goal &= ~walls;
    if (goal.is_empty() || goal.is_set(pos_)) return false;
    bc::bitboard dest;
    dest.clear();
    dest.set(pos_);
    bc::bitboard visited;
    scratch_field_.compute_to(walls, goal, dest, visited);
    if (scratch_field_.get(pos_) == kUnreached) return false;
    for (auto step : scratch_field_.next_moves_from(pos_))
        if (try_move(me, step)) return true;
    return false;
}

bool Bot::approach_unit(bc::champion& me, vec2 target, int range) {
    if (cheb(pos_, target) <= range) return false;
    bc::bitboard goal;
    goal.clear();
    goal.draw_chebyshev(target, static_cast<float>(range));
    return approach(me, goal);
}

bool Bot::try_dash_along(bc::champion& me, const bc::distance_field& field, bool walkable_roots) {
    if (cast_done_ || !can_cast_) return false;
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::dash) continue;
        const std::uint16_t here = world().dist_on(field, pos_);
        if (here == kUnreached || here < 5) return false;
        // Two greedy steps down the field.
        vec2 a = pos_, b = pos_;
        std::uint16_t da = here, db = here;
        for (auto d : kDirs) {
            const vec2 n = pos_ + d;
            if (!free_tile(n)) continue;
            const auto v = field.get(n);
            if (v < da) {
                da = v;
                a = n;
            }
        }
        if (a == pos_) return false;
        for (auto d : kDirs) {
            const vec2 n = a + d;
            if (n == pos_ || !free_tile(n)) continue;
            const auto v = field.get(n);
            if (v < db && v < da) {
                db = v;
                b = n;
            }
        }
        if (b == pos_ || (!walkable_roots && db < 1)) return false;
        // A landing that damages would wake sleeping camps.
        if (s.modifier > 0)
            for (const auto& c : camps_)
                if (cheb(c.pos, b) <= static_cast<int>(s.radius) + 1) return false;
        if (!spell_ready(i)) return false;
        if (cast_at(s, b)) {
            pos_ = b;
            return true;
        }
        return false;
    }
    (void)me;
    return false;
}

bool Bot::make_way(bc::champion& me, vec2 blocked) {
    // An ally holds the only tile that brings us closer. In a one-tile corridor two
    // champions walking opposite ways used to wait for each other for the rest of the
    // game. The lower slot goes first and the other steps aside; if the stand-off
    // drags on, either one steps aside.
    const SeenChamp* blocker = nullptr;
    for (const auto& a : allies_seen_)
        if (a.pos == blocked) blocker = &a;
    if (!blocker) return false;
    int their_slot = -1;
    for (std::size_t i = 0; i < allies_.size(); ++i)
        if (allies_[i] == blocker->handle) their_slot = static_cast<int>(i);
    if (their_slot > slot_ && stuck_rounds_ < 6) return false;
    yield_slot_ = their_slot;
    yield_until_ = round_ + 3;
    return step_away(me, {{blocker->pos, 3}});
}

bool Bot::travel(bc::champion& me, int poi) {
    if (!can_move_ || moved_) return false;
    auto& W = world();
    if (round_ <= yield_until_ && yield_slot_ >= 0 && yield_slot_ < static_cast<int>(allies_.size())) {
        // Still making way: keep clear of that ally until it has passed.
        for (const auto& a : allies_seen_)
            if (a.handle == allies_[yield_slot_] && cheb(a.pos, pos_) <= 1) {
                yield_until_ = std::max(yield_until_, round_ + 1);
                if (step_away(me, {{a.pos, 3}})) return true;
            }
    }
    const auto* f = W.field(poi, 15000);
    load_occupancy();
    const bool walkable = W.pois[poi].roots_walkable;
    if (!f) {
        // No fuel for a field this turn: search locally toward the goal instead.
        bc::bitboard goal = W.pois[poi].roots();
        if (!walkable) {
            bc::bitboard ring;
            ring.clear();
            for (auto p : goal.positions()) ring.draw_chebyshev(p, 1);
            goal = ring;
        }
        return approach(me, goal);
    }
    std::uint16_t here = W.dist_on(*f, pos_);
    if (here == kUnreached && W.group_id_at(pos_) >= 0) {
        // Boxed in by camp tiles the static field treats as walls: search the live map.
        bc::bitboard goal = W.pois[poi].roots();
        if (!walkable) {
            bc::bitboard ring;
            ring.clear();
            for (auto p : goal.positions()) ring.draw_chebyshev(p, 1);
            goal = ring;
        }
        return approach(me, goal);
    }
    if (here == kUnreached || here == 0 || (!walkable && here <= 1)) return false;
    if (try_dash_along(me, *f, walkable)) here = W.dist_on(*f, pos_);
    if (here == 0 || (!walkable && here <= 1)) return true;

    std::array<std::pair<std::uint16_t, vec2>, 8> options;
    int count = 0;
    for (auto d : kDirs) {
        const vec2 n = pos_ + d;
        if (!W.in_bounds(n) || W.blocked.get(n)) continue;
        const auto v = f->get(n);
        if (v < here) options[count++] = {v, n};
    }
    std::sort(options.begin(), options.begin() + count, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int i = 0; i < count; ++i)
        if (!occupied_.get(options[i].second) && try_move(me, options[i].second - pos_)) return true;
    if (count > 0 && stuck_rounds_ >= 2 && make_way(me, options[0].second)) return true;
    // Every downhill tile is taken: route around the blockers.
    if (stuck_rounds_ >= 2 || count > 0) {
        bc::bitboard goal = W.pois[poi].roots();
        if (!walkable) {
            bc::bitboard ring;
            ring.clear();
            for (auto p : goal.positions()) ring.draw_chebyshev(p, 1);
            goal = ring;
        }
        if (bc::fuel_remaining() > 30000) return approach(me, goal);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Home, tribute and capture

void Bot::place_lock(bc::champion& me) {
    auto& W = world();
    for (int i = 0; i < static_cast<int>(W.altars.size()) && i < 32; ++i) {
        const vec2 a = W.altars[i];
        if (cheb(a, pos_) > 8 || !visible_.get(a)) continue;
        // Remember enemy locks so tributes go elsewhere.
        bool locked = bc::vision::has_altar_lock(a);
        bool ours = false;
        if (locked) {
            ours = bc::vision::altar_lock_at(a).is_ally();
            if (!ours) enemy_lock_round_[i] = round_;
        } else {
            enemy_lock_round_[i] = -1000;
        }
        if (!has_lock_ || locked || cheb(a, pos_) > 2) continue;
        // Lock altars the enemy will use: anything when there are only one or two,
        // otherwise those on their side, and always one an enemy is tributing at.
        bool channeling = false;
        for (const auto& e : enemies_)
            if (cheb(e.pos, a) <= 1 && e.handle.is_submitting_gems()) channeling = true;
        const bool their_side = cheb(a, W.enemy_centre) <= cheb(a, W.ally_centre);
        if (!channeling && !their_side && W.altars.size() > 2 && !(kLockDenial && altar_exposure(a) > 0)) continue;
        bool placed = false;
        try {
            placed = me.consumables()[bc::consumable_type::altar_lock].use(a);
        } catch (const std::exception&) {
        }
        if (placed) {
            has_lock_ = false;
            enemy_lock_round_[i] = -1000;
        }
    }
}

void Bot::drop_ward(bc::champion& me) {
    // One free ward that recharges over 10 turn ends; a new one replaces the old.
    if (round_ < ward_ready_round_) return;
    load_occupancy();
    for (auto d : kDirs) {
        const vec2 n = pos_ + d;
        if (!free_tile(n)) continue;
        bool placed = false;
        try {
            placed = me.place_ward(n);
        } catch (const std::exception&) {
        }
        if (placed) {
            ward_ready_round_ = round_ + bc::constants::ward_cooldown + 1;
            ward_pos_ = n;
        } else {
            ward_ready_round_ = round_ + 3;
        }
        return;
    }
}

void Bot::reveal_ward(bc::champion& me) {
    // Drop the ward where it sees the most grass we cannot see into now.
    if (round_ < ward_ready_round_) return;
    auto& W = world();
    load_occupancy();
    // Hidden grass around us (what an attacker could stand in), then the ward tile seeing most of it.
    std::vector<vec2> hidden;
    for (int dy = -5; dy <= 5; ++dy)
        for (int dx = -5; dx <= 5; ++dx) {
            const vec2 p{pos_.x + dx, pos_.y + dy};
            if (W.in_bounds(p) && W.bushes.get(p) && !visible_.get(p)) hidden.push_back(p);
        }
    if (hidden.empty()) {
        ward_ready_round_ = round_ + 1;  // nothing to look at here
        return;
    }
    vec2 best{-1, -1};
    int best_score = 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const vec2 n{pos_.x + dx, pos_.y + dy};
            if (!free_tile(n)) continue;
            int score = 0;
            for (auto p : hidden)
                if (cheb(p, n) <= 3) ++score;
            if (score > best_score) {
                best_score = score;
                best = n;
            }
        }
    if (best.x < 0) return;
    bool placed = false;
    try {
        placed = me.place_ward(best);
    } catch (const std::exception&) {
    }
    if (placed) {
        ward_ready_round_ = round_ + bc::constants::ward_cooldown + 1;
        ward_pos_ = best;
        mark("reveal-ward");
    } else {
        ward_ready_round_ = round_ + 3;
    }
}

float Bot::dps_now(const bc::champion& c, bc::champion_type t, float strength, float focus) const {
    // Damage per round over the next kReadyHorizon turns: basic attacks every turn, plus each spell
    // that is ready within the horizon once.
    static std::array<int, 13> splash{};
    auto& sp = splash[static_cast<int>(t)];
    if (sp == 0) sp = bc::champion{t}.passive() == bc::passive_type::splash_attack ? 2 : 1;
    float d = strength * (sp == 2 ? 1.5f : 1.f);
    const float h = static_cast<float>(kReadyHorizon);
    try {
        for (const auto& ab : c.abilities()) {
            const auto& info = ab.info();
            if (!info) continue;
            if (ab.cooldown() >= kReadyHorizon) continue;
            std::visit([&](const auto& p) {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, bc::ability_attack_reset>) d += strength / h;
                else if constexpr (std::is_same_v<T, bc::ability_damage> || std::is_same_v<T, bc::ability_cc>) d += focus * p.modifier / h;
                else if constexpr (std::is_same_v<T, bc::ability_aoe_damage> || std::is_same_v<T, bc::ability_dash>) d += focus * p.modifier * 1.5f / h;
            }, *info);
        }
    } catch (const std::exception&) {
        return dps_of(t, strength, focus);
    }
    return d;
}

bool Bot::power_play() const {
    return kPowerPlay && enemy_roster_.size() >= 2 && static_cast<int>(enemy_roster_.size()) - enemies_alive_ >= 2;
}

int Bot::hidden_grass(vec2 p) const {
    // Grass within 3 we cannot see into; standing in grass, its patch is in view.
    const auto& W = world();
    if (W.bushes.get(p)) return 0;
    int n = 0;
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            const vec2 q{p.x + dx, p.y + dy};
            if (W.in_bounds(q) && W.bushes.get(q) && !visible_.get(q)) ++n;
        }
    return n;
}

bool Bot::safe_wait_step(bc::champion& me, const Group& g, int keep) {
    if (!can_move_ || moved_ || !enemy_recent(kSafeWaitRecent)) return false;
    auto& W = world();
    // Hidden grass within 4 once, then each tile's count within 3 from that list.
    std::vector<vec2> hidden;
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            const vec2 q{pos_.x + dx, pos_.y + dy};
            if (W.in_bounds(q) && W.bushes.get(q) && !visible_.get(q)) hidden.push_back(q);
        }
    auto count = [&](vec2 p) {
        if (W.bushes.get(p)) return 0;
        int n = 0;
        for (auto q : hidden)
            if (cheb(p, q) <= 3) ++n;
        return n;
    };
    const int here = count(pos_);
    if (here == 0) return false;
    load_occupancy();
    vec2 best = pos_;
    int best_n = here;
    for (auto d : kDirs) {
        const vec2 n = pos_ + d;
        if (!free_tile(n) || W.group_id_at(n) == g.id || hazardous(n, g.id)) continue;
        int nearest = 1000;
        for (auto t : g.tiles) nearest = std::min(nearest, cheb(t, n));
        if (nearest > keep) continue;
        const int c = count(n);
        if (c < best_n) {
            best_n = c;
            best = n;
        }
    }
    if (best == pos_) return false;
    mark("safe-wait");
    return try_move(me, best - pos_);
}

bool Bot::crystal_escape(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats) {
    if (!can_act_crystal(me)) return false;
    auto& W = world();
    const auto* home = W.field(W.spawn_poi, 20000);
    if (!home) return false;
    const int here = W.dist_on(*home, pos_);
    if (here == kUnreached) return false;
    load_occupancy();
    vec2 best{-1, -1};
    int best_gain = kCrystalSave - 1;
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            const vec2 t{pos_.x + dx, pos_.y + dy};
            if (!free_tile(t)) continue;
            const int there = home->get(t);
            if (there == kUnreached) continue;
            int margin = 1000;
            for (const auto& [p, r] : threats) margin = std::min(margin, cheb(t, p) - r);
            if (margin < 1) continue;
            const int gain = here - there;
            if (gain > best_gain) {
                best_gain = gain;
                best = t;
            }
        }
    if (best.x < 0) return false;
    try {
        return me.consumables()[bc::consumable_type::tp_crystal].use(best);
    } catch (const std::exception&) {
        return false;
    }
}

bool Bot::can_act_crystal(bc::champion& me) const {
    try {
        return me.consumables()[bc::consumable_type::tp_crystal].count() > 0;
    } catch (const std::exception&) {
        return false;
    }
}

void Bot::scout_ward(bc::champion& me) {
    if (!kScoutWards || round_ < ward_ready_round_ || on_spawn()) return;
    if (cheb(ward_pos_, pos_) <= kScoutWardMove) return;  // our ward already watches this area
    for (const auto& e : enemies_)
        if (!harmless_on_spawn(e) && cheb(e.pos, pos_) <= 6) return;  // not in the middle of a fight
    auto& W = world();
    load_occupancy();
    vec2 best{-1, -1};
    int best_score = -1000;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const vec2 n{pos_.x + dx, pos_.y + dy};
            if (!free_tile(n) || W.ally_spawn.get(n)) continue;
            // A bush hides whoever stands in it; otherwise look toward the enemy.
            const int score = (W.bushes.get(n) ? 10 : 0) + cheb(pos_, W.enemy_centre) - cheb(n, W.enemy_centre);
            if (score > best_score) {
                best_score = score;
                best = n;
            }
        }
    if (best.x < 0) return;
    bool placed = false;
    try {
        placed = me.place_ward(best);
    } catch (const std::exception&) {
    }
    if (placed) {
        ward_ready_round_ = round_ + bc::constants::ward_cooldown + 1;
        ward_pos_ = best;
    } else {
        ward_ready_round_ = round_ + 3;
    }
}

bool Bot::altar_open(int index) const {
    if (altar_walls_open(index, false)) return true;
    // Every altar looks walled in, yet the enemy just banked: one is open and we did not see the
    // camp die (01boss-battle: they killed the monolith by the only altar at round 876 and banked
    // seven times while we carried 1,000+ gems). Camps not seen alive lately count as gone, so
    // we walk over and look.
    if (!kAltarRecheck || round_ - enemy_banked_round_ > 100) return false;
    for (int i = 0; i < static_cast<int>(world().altars.size()); ++i)
        if (altar_walls_open(i, false)) return false;
    return altar_walls_open(index, true);
}

bool Bot::altar_walls_open(int index, bool stale_ok) const {
    // Some altars are walled in by camps; they open only once those camps are dead.
    const auto& W = world();
    const vec2 a = W.altars[index];
    for (auto d : kDirs) {
        const vec2 n = a + d;
        if (!W.in_bounds(n) || W.blocked.get(n)) continue;
        const int gid = W.group_id_at(n);
        if (gid < 0) return true;
        const auto& g = W.groups[gid];
        bool camp_alive = false;
        for (const auto& c : g.camps)
            if (c.pos == n) camp_alive = g.alive_by(round_) && c.alive_known && !(stale_ok && round_ - c.seen_alive_round > 30);
        if (!camp_alive) return true;
    }
    return false;
}

int Bot::best_altar(int& distance) {
    auto& W = world();
    // Fields for the two closest altars as the crow flies keep the cost bounded.
    std::vector<std::pair<int, int>> order;
    // Skip altars we recently failed to walk to, unless no other altar is open.
    auto avoided = [&](int i) { return i < static_cast<int>(altar_avoid_until_.size()) && round_ < altar_avoid_until_[i]; };
    for (int i = 0; i < static_cast<int>(W.altars.size()); ++i)
        if (altar_open(i) && !avoided(i)) order.push_back({cheb(W.altars[i], pos_), i});
    if (order.empty())
        for (int i = 0; i < static_cast<int>(W.altars.size()); ++i)
            if (altar_open(i)) order.push_back({cheb(W.altars[i], pos_), i});
    std::sort(order.begin(), order.end());
    int best = -1;
    distance = kUnreached;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const int i = order[k].second;
        if (k >= 3 && !W.has_field(W.altar_poi[i])) continue;
        if (order[k].first >= distance) break;
        int d = W.dist(W.altar_poi[i], pos_, 15000);
        // Stick with the altar we were walking to unless another is clearly closer.
        if (i == altar_choice_ && d != kUnreached) d -= 3;
        // An enemy lock costs five attacks to clear: prefer another altar unless far.
        if (i < 32 && round_ - enemy_lock_round_[i] < 150 && d != kUnreached) d += 12;
        if (kAltarDanger && gems_ >= kEscortBag && d != kUnreached) d += altar_danger(W.altars[i]);
        if (kAltarExposure > 0 && gems_ >= kEscortBag && d != kUnreached) d += kAltarExposure * altar_exposure(W.altars[i]);
        if (d < distance) {
            distance = d;
            best = i;
        }
    }
    if (best == altar_choice_ && distance != kUnreached) distance += 3;
    altar_choice_ = best;
    return best;
}

bool Bot::go_home(bc::champion& me, bool urgent) {
    if (on_spawn()) return false;
    // A wounded champion alone is the one a hunting team picks off.
    const bool hurt = hp_ < max_hp_ * (pressured() ? 0.55f : 0.4f);
    // A trip home to shop only makes sense while the build still has something to buy;
    // otherwise a full purse sends us home every turn. Tavi shops wherever it stands.
    const bool can_spend = build_step_ < build_for(type_).size() && type_ != bc::champion_type::tavi;
    const bool rich = can_spend && (gold_ >= 250 || (gold_ >= 150 && hp_ < max_hp_ * 0.6f));
    if (!urgent && !hurt && !rich) return false;
    // Bank gems first when an altar is close.
    if (gems_ > 0 && hp_ > max_hp_ * 0.2f) {
        int d = 0;
        if (best_altar(d) >= 0 && d <= 8) return false;
    }
    set_status(Task::other, kHome);
    auto& W = world();
    const int home = W.dist(W.spawn_poi, pos_, 10000);
    if (home <= 6) return travel(me, W.spawn_poi);
    bool threatened = false;
    for (const auto& c : camps_)
        if (c.awake && c.strength > 0 && cheb(c.pos, pos_) <= c.range + 1) threatened = true;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= 7 && !harmless_on_spawn(e)) threatened = true;
    if (kHurtFlee && hurt && round_ - last_champ_hurt_round_ <= kFleeMemory) {
        // Get out of reach of whoever has been hitting us before trying to recall.
        std::vector<std::pair<vec2, int>> threats;
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const vec2 p{pos_.x + dx, pos_.y + dy};
                if (W.in_bounds(p) && W.bushes.get(p) && !visible_.get(p)) threats.push_back({p, 3});
            }
        for (const auto& s : sightings_)
            if (round_ - s.round <= kFleeMemory + 2 && cheb(s.pos, pos_) <= 8) threats.push_back({s.pos, 4});
        for (const auto& e : enemies_)
            if (!harmless_on_spawn(e)) threats.push_back({e.pos, e.range + 1});
        mark("hurt-flee");
        if (!threats.empty() && step_away(me, threats, kGrassRetreat)) return true;
        return travel(me, W.spawn_poi);
    }
    // Hit by someone we cannot see (a bush, the edge of our sight): the recall would only be cut.
    if (kRecallSafe > 0 && round_ - (kRecallSafe == 2 ? last_champ_hurt_round_ : last_hurt_round_) <= kRecallCalm) threatened = true;
    if (!threatened) {
        try {
            me.recall();
            return true;
        } catch (const std::exception&) {
        }
    }
    return travel(me, W.spawn_poi);
}

float Bot::enemy_scale() const {
    float k = 1.f;
    if (kLearnEnemy) k = std::max(k, (dmg_seen_ + kLearnPrior * kLearnBase) / (dmg_expected_ * kLearnBase + kLearnPrior * kLearnBase));
    if (kDeathScale) k = std::max(k, 1.f + kDeathStep * static_cast<float>(recent_team_deaths(kDeathWindow)));
    return std::min(k, kMaxEnemyScale);
}

float Bot::tribute_threat() const {
    // Damage per round from enemies that can be on us within two steps.
    float threat = 0;
    for (const auto& e : enemies_)
        if (!world().enemy_spawn.get(e.pos) && cheb(e.pos, pos_) <= e.range + 2) threat += e.dps;
    return threat * enemy_scale();
}

bool Bot::abandon_tribute(bc::champion& me) {
    const float threat = tribute_threat();
    if (threat <= 0) return false;
    int left = 0;
    try {
        left = me.remaining_channel_time();
    } catch (const std::exception&) {
        return false;
    }
    // Stay if the channel finishes before the enemies here can kill us, or if it wins the game.
    if (hp_ > threat * (left + 1.5f)) return false;
    if (static_cast<int>(bc::global_submitted_gems(team::ally)) + gems_ >= static_cast<int>(bc::constants::winning_gems)) return false;
    try {
        me.cancel_submit_gems();
    } catch (const std::exception&) {
        return false;
    }
    const auto state = me.state();
    can_move_ = state.contains(bc::champion_state::can_move);
    can_attack_ = state.contains(bc::champion_state::can_attack);
    can_cast_ = state.contains(bc::champion_state::can_cast);
    mark("tribute-abort");
    LOG("TABORT r" << round_ << " " << bc::to_string(type_) << " gems" << gems_ << " left" << left << " hp" << hp_ << " threat" << threat);
    task_ = Task::farm;
    target_ = -1;
    return true;
}

bool Bot::tribute(bc::champion& me) {
    if (gems_ <= 0 || breach_active()) return false;
    int d = 0;
    const int a = best_altar(d);
    if (a < 0 || d == kUnreached) return false;
    const int remaining = static_cast<int>(bc::constants::max_rounds) - round_;
    const int channel = (gems_ + 9) / 10;
    const bool endgame = remaining <= d + channel + 20;
    // Close to the finish every carried gem counts: teammates' bags together may win.
    const bool wins = bc::global_submitted_gems(team::ally) + gems_ + 150 >= bc::constants::winning_gems;
    // With small bags, enemies seen in the last few rounds count too: they are still about.
    const bool threatened = kSmallBags ? enemy_recent(kRecentEnemy) : !enemies_.empty();
    const bool want = gems_ >= carry_gems() || endgame || wins || d <= 6 || (threatened && gems_ >= kSmallBag) ||
                      task_ == Task::other;
    if (!want) {
        escort_wait_ = 0;
        return false;
    }
    const vec2 altar = world().altars[a];
    if (kSafeAltar && !endgame && !wins) {
        if (round_ < altar_wait_until_) return false;
        // Enemies hold the altar and we do not outnumber them: dying here hands them the bag.
        // Farm for a while and come back.
        if (cheb(pos_, altar) <= 10 && altar_guarded(altar)) {
            altar_wait_until_ = round_ + kAltarWait;
            if (task_ == Task::other) {
                task_ = Task::farm;
                target_ = -1;
            }
            mark("altar-guarded");
            return false;
        }
    }
    if (kBankEscort && !endgame && !wins && !(kCalmNoEscort && calm()) && gems_ >= (kTeamBank ? kEscortBag / 2 : kEscortBag) && escort_wait_ < 8 && cheb(pos_, altar) <= 9 &&
        cheb(pos_, altar) > bc::constants::submit_gems_range && altar_exposure(altar) > 0) {
        // An altar in the middle: let the teammate walking with us catch up before going in.
        bool with = false, coming = false;
        for (const auto& a : allies_seen_) {
            const int d = cheb(a.pos, pos_);
            if (d <= 3) with = true;
            else if (d <= kEscortRange + 3) coming = true;
        }
        if (!with && coming) {
            ++escort_wait_;
            task_ = Task::other;
            target_ = kTribute;
            set_status(Task::other, kTribute);
            mark("wait-escort");
            return true;
        }
    }
    task_ = Task::other;
    target_ = kTribute;
    set_status(Task::other, kTribute);
    if (cheb(pos_, altar) <= bc::constants::submit_gems_range) {
        // A channel the enemies here would cut short just hands them the bag.
        const float threat = tribute_threat();
        const bool final_bag = static_cast<int>(bc::global_submitted_gems(team::ally)) + gems_ >= static_cast<int>(bc::constants::winning_gems);
        if (threat > 0 && hp_ <= threat * (channel + 1.5f) && !final_bag) return false;
        if (kSafeChannel && !final_bag && channel >= 6 && altar_exposure(altar) > 0 && enemy_recent(15)) {
            bool escorted = false;
            for (const auto& a : allies_seen_)
                if (cheb(a.pos, pos_) <= 3) escorted = true;
            if (!escorted) return false;
        }
        try {
            me.submit_gems(altar);
            drop_ward(me);
            task_ = Task::farm;
            target_ = -1;
            escort_wait_ = 0;  // the next trip waits for its escort again
            return true;
        } catch (const std::exception&) {
            // Locked: attack the enemy's lock. Both teams can lock the same altar, and
            // altar_lock_at() then returns ours, so look for theirs explicitly.
            if (can_attack_)
                for (const auto& lock : bc::vision::altar_locks(team::enemy))
                    if (lock.position() == altar) {
                        try {
                            me.attack(lock);
                        } catch (const std::exception&) {
                        }
                        break;
                    }
            return true;
        }
    }
    travel(me, world().altar_poi[a]);
    // Walking to this altar has stalled for a long time: try another one for a while.
    if (stuck_rounds_ >= 12 && a < static_cast<int>(altar_avoid_until_.size())) altar_avoid_until_[a] = round_ + 60;
    return true;
}

bool Bot::capture(bc::champion& me) {
    auto& W = world();
    if (task_ == Task::capture && target_ >= 0) {
        const auto& g = W.groups[target_];
        // Drop the claim once the reward is gone, or when something pulled us far away or
        // the tiles cannot be reached: champions used to stand still "capturing" a reward
        // across the map for hundreds of rounds.
        const bool timed_out = kCaptureTimeout > 0 && capture_since_ >= 0 && round_ - capture_since_ > kCaptureTimeout;
        if (g.state != GroupState::reward || cheb(g.anchor, pos_) > 14 || capture_fail_ >= 6 || timed_out) {
            // Tiles we could not walk to (an island only a dash reaches, say): stop
            // re-claiming them every few turns.
            if (capture_fail_ >= 6 || timed_out) avoid_until_[target_ & 63] = round_ + 150;
            task_ = Task::farm;
            target_ = -1;
            capture_fail_ = 0;
        }
    }
    // Our Marcella is coming for this gem reward: leave it to her (rewards are split among
    // allies standing on the region) and get on with the next camp.
    if (task_ == Task::capture && target_ >= 0 && W.groups[target_].gems > 0 && banker_slot_ >= 0 &&
        comms_.slot(banker_slot_).task == Task::capture && comms_.slot(banker_slot_).target == target_ &&
        banker_free_near(W.groups[target_].anchor, kBankerReach, target_)) {
        task_ = Task::farm;
        target_ = -1;
        capture_fail_ = 0;
    }
    if (task_ != Task::capture) {
        // Claim a waiting reward nearby that nobody else is collecting.
        const bool banker = kMarcellaBank && type_ == bc::champion_type::marcella;
        int best = -1, best_d = 1000;
        for (auto& g : W.groups) {
            if (g.state != GroupState::reward || g.seen_round != round_) continue;
            if (opening_blocks(g, true)) continue;
            const int d = cheb(g.anchor, pos_);
            if (d > (banker && g.gems > 0 ? kBankerClaim : 10) || d >= best_d || g.id > kMaxTarget || round_ < avoid_until_[g.id & 63]) continue;
            // Marcella turns gems into gold: leave gem rewards to her when she is close and free.
            if (g.gems > 0 && banker_free_near(g.anchor, kBankerReach, g.id)) continue;
            // Rewards are split among allies standing on the region: with small bags, big gem
            // rewards are shared so no single champion walks off with a bag worth dying for.
            const int sharers = !(kSmallBags || kShareBig) ? 1 : g.gems >= 100 ? 5 : g.gems >= 40 ? 3 : 1;
            // Close by, Marcella takes a gem reward over (the claimant leaves it to her).
            const bool take_over = banker && g.gems > 0 && d <= kBankerReach;
            if (!take_over && comms_.claims_by_others(slot_, Task::capture, g.id, slots_) >= sharers) continue;
            best = g.id;
            best_d = d;
        }
        if (best < 0) return false;
        task_ = Task::capture;
        target_ = best;
        wait_rounds_ = 0;
        capture_fail_ = 0;
        capture_since_ = round_;
    }
    set_status(Task::capture, target_);
    const auto& g = W.groups[target_];
    for (auto t : g.tiles)
        if (t == pos_) {
            drop_ward(me);  // keep an eye on the group after we leave
            return true;    // stand still until the reward lands
        }
    load_occupancy();
    bc::bitboard goal;
    goal.clear();
    for (auto t : g.tiles)
        if (!occupied_.get(t)) goal.set(t);
    if (goal.is_empty()) return true;
    if (approach(me, goal)) capture_fail_ = 0;
    else ++capture_fail_;
    return true;
}

// ---------------------------------------------------------------------------
// Farming plan

Forecast Bot::forecast(const Group& g, float dps) const {
    dps = std::max(dps, 1.f);
    Forecast f{0, 0};
    if (g.harmless) {
        for (const auto& c : g.camps)
            if (c.alive_known) f.rounds += c.hp_known / dps;
        return f;
    }
    // Camps are pre-sorted strongest first: focus fire kills them in that order.
    float threat = 0;
    for (const auto& c : g.camps)
        if (c.alive_known) threat += c.strength;
    for (const int i : g.by_strength) {
        const auto& c = g.camps[i];
        if (!c.alive_known) continue;
        const float t = c.hp_known / dps;
        f.rounds += t;
        f.damage += threat * t;
        threat -= c.strength;
    }
    return f;
}

std::uint16_t Bot::dist_from_me(vec2 p) {
    auto& W = world();
    if (!W.in_bounds(p)) return kUnreached;
    if (me_field_round_ != round_ || me_field_pos_ != pos_ || !me_field_) {
        if (bc::fuel_remaining() < static_cast<std::uint64_t>(W.w) * W.h * 3 + 25000) return kUnreached;
        if (!me_field_) me_field_.emplace();
        bc::bitboard roots;
        roots.clear();
        roots.set(pos_);
        bc::bitboard visited;
        me_field_->compute(W.blocked_board, roots, visited);
        me_field_round_ = round_;
        me_field_pos_ = pos_;
    }
    return me_field_->get(p);
}

int Bot::dist_to_group(Group& g) {
    auto& W = world();
    auto& cached = group_dist_[g.id & 63];
    if (cached.round == round_ && cached.pos == pos_) return cached.dist;
    // The group's own field routes around every other camp. Without it (no fuel to build
    // it yet) a terrain-only search can lead straight through camps that wall it off.
    const bool exact = W.field(g.poi, kPlanFuel) != nullptr;
    int d = exact ? W.dist(g.poi, pos_) : dist_from_me(g.anchor);
    // On our spawn (round 1, after a respawn) the draft's spawn walk stands in for the field.
    if (kDraftHome && !exact && d == kUnreached && g.home >= 0 && W.ally_spawn.get(pos_)) d = g.home;
    // Terrain and camp tiles never move, so a group walled off from here stays that way.
    if (exact && d == kUnreached && W.group_id_at(pos_) < 0) {
        if (!g.unreachable) LOG("UNREACH r" << round_ << " " << bc::to_string(type_) << bc::to_string(pos_) << " g" << g.id << bc::to_string(g.anchor));
        g.unreachable = true;
    }
    cached = {d, round_, pos_};
    return d;
}

bool Bot::enemy_side(Group& g) {
    // Terrain distances never change: compute once per group.
    if (g.side < 0) {
        auto& W = world();
        const int mine = W.dist(W.spawn_poi, g.anchor, 30000);
        const int theirs = W.dist(W.enemy_spawn_poi, g.anchor, 30000);
        if (mine == kUnreached || theirs == kUnreached) return false;  // try again later
        g.side = theirs < mine ? 1 : 0;
        g.contested = std::abs(theirs - mine) <= std::max(6, (mine + theirs) / 10);
    }
    return g.side == 1;
}

void Bot::team_power(float& dps, float& hp, int& active, bool include_home, int party) const {
    dps = 0;
    hp = 0;
    active = 0;
    const bool split = party >= 0 && split_active();
    for (int i = 0; i < slots_; ++i) {
        if (split && party_of(i) != party) continue;
        const auto s = comms_.slot(i);
        // Dead, recalling or shopping teammates will not be at the fight soon.
        if (i != slot_ && s.task == Task::other && (s.target == kDead || (s.target == kHome && !include_home))) continue;
        if (i == slot_) {
            dps += dps_of(type_, strength_, focus_);
            hp += max_hp_;
        } else {
            dps += ally_dps_[i] * comms_.power();
            hp += ally_hp_[i] * 0.85f;
        }
        ++active;
    }
    if (slot_ < 0) {
        dps += dps_of(type_, strength_, focus_);
        hp += max_hp_;
        ++active;
    }
}

float Bot::group_rounds(const Group& g, float dps, int travel) const {
    int wait = 0;
    if (g.state == GroupState::respawning) wait = std::max(0, g.respawn_round - (round_ + travel));
    return travel + wait + forecast(g, dps).rounds + kCaptureRounds;
}

bool Bot::blocks_altar(const Group& g) const {
    for (auto a : world().altars)
        for (auto t : g.tiles)
            if (cheb(a, t) <= 1) return true;
    return false;
}

float Bot::heal_rounds(const Group& g, float dps, float hp) {
    // Rounds spent getting the damage back: a trip home and back per team's worth of health.
    if (g.harmless) return 0;
    auto& W = world();
    const int home = home_dist(g);
    if (home == kUnreached) return 0;
    return kHealCost * forecast(g, dps).damage / std::max(hp, 1.f) * (2.f * home + 10.f);
}

float Bot::siege_rounds(const Group& g, float team_dps, float team_hp, bool unlocks) {
    // Extra rounds spent going home and back between trips; negative when out of reach.
    const float damage = forecast(g, team_dps).damage;
    if (damage <= team_hp * (unlocks ? 0.8f : kSiegeShare)) return 0;
    // A young team besieging a monolith in contested ground loses the early game.
    if (!unlocks && round_ < kSiegeFrom && !early_siege_ok_) return -1;
    const float trips = damage / std::max(team_hp * kTripShare, 1.f);
    // With every altar walled in, opening one is the only way to score, however many trips it takes
    // (01boss-battle: the only altar sits behind a 20,000-health monolith; we never started on it
    // and turned in nothing all game while the other team banked from round 885).
    if (trips > (unlocks ? (kUnlockAnyTrips ? 1e9f : kMaxUnlockTrips) : kMaxTrips)) return -1;
    auto& W = world();
    const int home = home_dist(g);
    if (home == kUnreached) return -1;
    return (std::ceil(trips) - 1) * (2.f * home + 15.f);
}

bool Bot::any_altar_open() const {
    for (int i = 0; i < static_cast<int>(world().altars.size()); ++i)
        if (altar_open(i)) return true;
    return false;
}

int Bot::choose_squad_target(float team_dps, float team_hp, int exclude, bool allow_contested) {
    // Nothing can be banked until the way to an altar is open.
    if (breach_active()) return world().breach_group;
    // Wait for the camps beside us to respawn rather than walking across the map: on 01big-world
    // the squad walked 60-75 tiles to a far camp whenever the two by our spawn were respawning,
    // and cleared those half as often as the other team (6 against 12 times each).
    // In the opening, though, a respawning camp on our side must not keep the squad off a big
    // contested one (01concentrated-wealth: the 71-gem middle camp waited until round 369, not
    // 105): there the old 60-round wait applies, and contested camps stay a fallback.
    const int respawn_wait = kSafeOpening && round_ < kOpeningRounds ? (allow_contested ? 15 : 60) : kRespawnWait;
    auto& W = world();
    team_dps *= kFightEfficiency;
    const bool altar_reachable = any_altar_open();
    // Nearest first: the loop stops when fuel runs low, and in group order it used to stop
    // before reaching the camps beside us (01big-world: four island groups came first).
    const auto order = nearest_groups();
    int best = -1;
    float best_score = 0;
    int early_best = -1;  // kSiegeEarly: best camp needing trips home before kSiegeFrom
    float early_score = 0;
    struct Look {
        float score, value, rounds;
        int id;
    };
    std::array<Look, kLookTop> look{};
    int look_n = 0;
    // Per group, worked out once in the pass below and reused by the look ahead: fight, capture and
    // heal rounds (no walking), and whether it is a quick camp the squad would take.
    std::array<float, 64> base_rounds{};
    std::array<float, 64> worth{};        // value times the score adjustments below
    std::array<signed char, 64> quick{};  // 0 not worked out yet, 1 quick, -1 not
    for (const int gi : order) {
        auto& g = W.groups[gi];
        if (bc::fuel_remaining() < kSquadPlanFloor) break;
        if (g.unreachable || (g.harmless && !kSquadClusters) || g.id > kMaxTarget || g.id == exclude || g.state == GroupState::reward) continue;
        if (g.harmless && bc::fuel_remaining() < kClusterPlanFloor) continue;
        if (g.id == other_party_target_) continue;
        if (round_ < avoid_until_[g.id & 63]) continue;
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_)) continue;
        int d = dist_to_group(g);
        // When every altar is walled in by camps, the camps doing it hold the game hostage.
        const bool unlocks = !altar_reachable && blocks_altar(g);
        if (d == kUnreached) continue;
        if (kSquadTravel) d = std::max(d, squad_dist(g));
        // Camps walling in every altar stay a target: nothing can be banked until they fall.
        // In the opening a contested camp counts for less rather than nothing: with only poor camps
        // on our side, the squad used to walk 100 tiles for a 4-gem wolf (01concentrated-wealth)
        // while the 71-gem camp in the middle waited until round 369.
        bool contested_now = false;
        if (!unlocks && opening_blocks(g, allow_contested)) {
            if (!(kSoftOpening && g.contested)) continue;
            contested_now = true;
        }
        const float extra = siege_rounds(g, team_dps, team_hp, unlocks);
        if (kSiegeEarly && extra < 0 && g.gems >= 40 && !choosing_small_ && round_ < kSiegeFrom && g.alive_by(round_ + d + respawn_wait)) {
            // A camp needing trips home, before sieges are allowed: kept aside, taken only when
            // it beats everything else by kSiegeEarlyEdge.
            early_siege_ok_ = true;
            const float extra2 = siege_rounds(g, team_dps, team_hp, unlocks);
            early_siege_ok_ = false;
            if (extra2 >= 0) {
                const float fixed2 = forecast(g, team_dps).rounds + kCaptureRounds + heal_rounds(g, team_dps, team_hp);
                const int wait2 = g.state == GroupState::respawning ? std::max(0, g.respawn_round - (round_ + d)) : 0;
                const float score2 = g.value(kGoldWeight) / (d + wait2 + fixed2 + extra2) * squad_mods(g, contested_now);
                if (score2 > early_score) {
                    early_score = score2;
                    early_best = g.id;
                }
            }
        }
        if (extra < 0 || (choosing_small_ && extra > 0)) continue;
        if (!g.alive_by(round_ + d + respawn_wait)) continue;
        const float fixed = forecast(g, team_dps).rounds + kCaptureRounds + heal_rounds(g, team_dps, team_hp);
        if (kRoutePlan && W.plan.ok && g.id < 64) {
            base_rounds[g.id] = fixed;
            quick[g.id] = extra == 0 && !unlocks ? 1 : -1;
        }
        const float value = g.value(kGoldWeight) + (unlocks ? 1000.f : 0.f);
        const int wait_g = g.state == GroupState::respawning ? std::max(0, g.respawn_round - (round_ + d)) : 0;
        const float rounds = d + wait_g + fixed + extra;
        const float mods = squad_mods(g, contested_now);
        const float score = value / rounds * mods;
        if (kRoutePlan && W.plan.ok && g.id < 64) worth[g.id] = g.value(kGoldWeight) * mods;
        if (kRoutePlan && W.plan.ok) {
            // Keep the best few for the look ahead below.
            int at = std::min<int>(look_n, kLookTop - 1);
            if (look_n < kLookTop || score > look[at].score) {
                if (look_n < kLookTop) ++look_n;
                at = look_n - 1;
                while (at > 0 && look[at - 1].score < score) {
                    look[at] = look[at - 1];
                    --at;
                }
                look[at] = {score, value * mods, rounds, g.id};
            }
        }
        if (score > best_score) {
            best_score = score;
            best = g.id;
        }
    }
    if (kRoutePlan && W.plan.ok && look_n >= 2 && bc::fuel_remaining() > kSquadPlanFloor && (!kLookCalm || calm())) {
        // One camp ahead: with the walking distances measured in the draft, a camp that leads on to
        // a good next one beats a dead end of the same worth.
        float best_look = 0;
        for (int i = 0; i < look_n; ++i) {
            const auto& c = look[i];
            float combined = 0;
            const auto& next = W.plan.nearest(c.id, kLookNear);
            for (int h : next) {
                if (h >= 64 || h == c.id) continue;
                auto& gh = W.groups[h];
                if (gh.state == GroupState::reward) continue;
                if (quick[h] == 0) {
                    // Not scored above (respawning, or past the fuel cut-off): one forecast, here only.
                    quick[h] = -1;
                    const bool blocked_now = opening_blocks(gh, allow_contested);
                    if (!gh.unreachable && gh.id <= kMaxTarget && gh.id != exclude && gh.id != other_party_target_ &&
                        (!gh.harmless || kSquadClusters) && round_ >= avoid_until_[gh.id & 63] &&
                        !(blocked_now && !(kSoftOpening && gh.contested))) {
                        const auto fc = forecast(gh, team_dps);
                        if (gh.harmless || fc.damage <= team_hp * kSiegeShare) {
                            float heal = 0;
                            if (!gh.harmless) {
                                const int home = home_dist(gh);
                                if (home != kUnreached) heal = kHealCost * fc.damage / std::max(team_hp, 1.f) * (2.f * home + 10.f);
                            }
                            base_rounds[h] = fc.rounds + kCaptureRounds + heal;
                            worth[h] = gh.value(kGoldWeight) * squad_mods(gh, blocked_now);
                            quick[h] = 1;
                        }
                    }
                }
                if (quick[h] != 1) continue;
                if (kLookOwnSide && (enemy_side(gh) || gh.contested)) continue;
                const int walk = W.plan.between(c.id, h);
                if (walk == kUnreached) continue;
                const int arrive = static_cast<int>(c.rounds) + walk;
                if (!gh.alive_by(round_ + arrive + respawn_wait)) continue;
                const float wait = gh.state == GroupState::respawning ? std::max(0, gh.respawn_round - (round_ + arrive)) : 0.f;
                combined = std::max(combined, (c.value + worth[h]) / (c.rounds + walk + wait + base_rounds[h]));
            }
            // c.value already carries its adjustments, so c.score is c.value / c.rounds.
            const float rate = c.value / std::max(c.rounds, 1.f);
            const float factor = next.empty() ? 1.f : combined <= 0 ? kLookDeadEnd : std::clamp(combined / rate, 0.6f, 1.6f);
            if (c.score * factor > best_look) {
                best_look = c.score * factor;
                best = c.id;
            }
        }
    }
    if (kSiegeEarly && early_best >= 0 && early_score > kSiegeEarlyEdge * best_score) return early_best;
    if (best >= 0) return best;
    // Nothing dangerous is worth it (clusters only, or every camp respawning): the squad
    // farms clusters together rather than scattering, which leaves lone champions for
    // an enemy team to pick off.
    for (const int gi : order) {
        auto& g = W.groups[gi];
        if (bc::fuel_remaining() < kSquadPlanFloor) break;
        if (!g.harmless || g.unreachable || g.id > kMaxTarget || g.id == exclude || g.state == GroupState::reward) continue;
        if (g.id == other_party_target_) continue;
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_)) continue;
        bool soft = false;
        if (opening_blocks(g, allow_contested)) {
            if (!(kSoftClusters && g.contested)) continue;
            soft = true;
        }
        const int d = dist_to_group(g);
        if (d == kUnreached || !g.alive_by(round_ + d + 5)) continue;
        float score = g.value(kGoldWeight) / group_rounds(g, team_dps, d);
        if (soft) score *= kOpeningClusters;
        if (kAltarZoneBias > 0) score *= altar_zone(g);
        if (kFieldCamp && in_field(g.anchor)) score *= kFieldBonus;
        if (enemy_side(g)) score *= enemy_side_factor(false);
        score *= target_danger(g);
        if (score > best_score) {
            best_score = score;
            best = g.id;
        }
    }
    return best;
}

void Bot::local_power(float& dps, float& hp) const {
    dps = dps_of(type_, strength_, focus_);
    hp = hp_;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= 8) {
            dps += a.dps;
            hp += a.hp;
        }
}

void Bot::choose_task(bc::champion& me) {
    (void)me;
    auto& W = world();
    const int previous = target_;
    const bool split = split_active();
    if (split && party_of(slot_) == 1) {
        // Second squad: the leader keeps its target until it is gone; the partner follows it.
        float pdps = 0, php = 0;
        int members = 0;
        team_power(pdps, php, members, true, 1);
        int t = my_party_target();
        if (t >= static_cast<int>(W.groups.size())) t = -1;
        if (t >= 0) {
            auto& g = W.groups[t];
            const bool gone = g.state == GroupState::reward || (g.state == GroupState::respawning && g.respawn_round > round_ + 25);
            const bool too_strong = !g.harmless && siege_rounds(g, pdps * kFightEfficiency, php, false) != 0;
            const bool blocked = round_ < avoid_until_[t & 63] || (dist_to_group(g) == kUnreached && g.unreachable);
            const bool stalled_here = target_ == t && wait_rounds_ > 25 && !g.awake;
            if (stalled_here) avoid_until_[t & 63] = round_ + 80;
            if (gone || too_strong || blocked || stalled_here || t == comms_.squad_target()) t = -1;
        }
        if (t < 0 && bc::fuel_remaining() > 30000) {
            other_party_target_ = comms_.squad_target();
            choosing_small_ = true;
            t = choose_squad_target(pdps, php, comms_.squad_target(), round_ >= kOpeningRounds);
            choosing_small_ = false;
            other_party_target_ = -1;
        }
        party_target_ = t;
        task_ = Task::farm;
        target_ = t >= 0 ? t : comms_.squad_target();
        plan_round_ = round_;
        if (target_ != previous) wait_rounds_ = 0;
        return;
    }
    float team_dps = 0, team_hp = 0;
    int active = 0;
    team_power(team_dps, team_hp, active, false, split ? 0 : -1);
    // The second squad following ours (nothing of its own to do) is not "its" camp.
    const int b_target = split ? party_b_target() : -1;
    other_party_target_ = b_target != comms_.squad_target() ? b_target : -1;

    // Validate or replace the shared squad target. A squad that has waited too long
    // without enough people picks something the champions actually present can clear.
    int squad = comms_.squad_target();
    if (squad >= static_cast<int>(W.groups.size())) squad = -1;
    const bool breaching = squad >= 0 && squad == W.breach_group && breach_active();
    const bool stalled = !breaching && squad >= 0 && target_ == squad && task_ == Task::farm && wait_rounds_ > kStallRounds && !W.groups[squad].awake;
    // A squad that could not assemble enough strength here should not come straight back.
    if (stalled) avoid_until_[squad & 63] = round_ + 80;
    if (squad >= 0) {
        auto& g = W.groups[squad];
        // Only drop the target when it is gone, walled off from us (a teammate chose it
        // before it could tell) or too strong for the whole living team:
        // teammates popping home to shop must not send the squad across the map.
        float living_dps = 0, living_hp = 0;
        int living = 0;
        team_power(living_dps, living_hp, living, true, split ? 0 : -1);
        const bool unlocks = blocks_altar(g) && !any_altar_open();
        const bool walled = dist_to_group(g) == kUnreached && g.unreachable;
        early_siege_ok_ = kSiegeEarly;  // a siege chosen early is not stale for being early
        const bool stale = walled || g.state == GroupState::reward ||
                           (g.state == GroupState::respawning && g.respawn_round > round_ + 25) ||
                           siege_rounds(g, living_dps, living_hp * 1.25f, unlocks) < 0;
        early_siege_ok_ = false;
        // We could not reach it lately (farm() set the avoid timer): pick another.
        const bool blocked_for_me = round_ < avoid_until_[squad & 63];
        // We died beside it lately: teammates cannot know, so move the whole squad on rather
        // than walk back there one at a time.
        const bool deadly = kDangerMemory && !g.awake && !unlocks && danger_factor(g.anchor) < 0.35f;
        if (deadly) avoid_until_[squad & 63] = round_ + 60;
        if ((stale || stalled || blocked_for_me || deadly) && !breaching) squad = -1;
        if (split && squad == other_party_target_ && !breaching) squad = -1;
    }
    if (squad < 0 && bc::fuel_remaining() > 30000) {
        float dps = team_dps, hp = team_hp;
        if (stalled) local_power(dps, hp);
        squad = choose_squad_target(dps, hp, stalled ? comms_.squad_target() : -1);
        if (squad < 0 && kSafeOpening && round_ < kOpeningRounds) squad = choose_squad_target(dps, hp, stalled ? comms_.squad_target() : -1, true);
        if (squad >= 0 || !stalled) comms_.set_squad_target(squad);
        else squad = comms_.squad_target();
    }

    // Solo work is limited to harmless clusters nearby (and, with kSoloCamps, fighting camps
    // on our side we can clear alone), and to a third of the team.
    int solos = 0;
    for (int i = 0; i < slots_; ++i)
        if (i != slot_ && comms_.slot(i).task == Task::solo) ++solos;
    // Side jobs while the squad waits: with its target respawning for a while, anyone may clear
    // a nearby camp alone that it can finish and walk back from before the respawn. On 00rows 15%
    // of champion time went to standing beside a respawning camp.
    int squad_wait = 0;
    if (kWaitJobs && squad >= 0 && W.groups[squad].state == GroupState::respawning)
        squad_wait = W.groups[squad].respawn_round - round_ - dist_to_group(W.groups[squad]);
    const bool waiting = squad_wait > kWaitJobMin;
    const bool early = kEarlySolo && round_ < 300;
    bool straggler = false;
    if (kStragglerSolo && squad >= 0 && dist_to_group(W.groups[squad]) > kStragglerDist) {
        int there = 0;
        for (const auto& a : allies_seen_)
            if (cheb(a.pos, W.groups[squad].anchor) <= 4) ++there;
        straggler = there >= 2;
    }
    const int solo_cap = kClusterSolos ? std::max(1, active - 2) : std::max(1, early ? active / 2 : active / 3);
    const bool may_solo = waiting || straggler || solos < solo_cap || squad < 0;
    const float my_dps = dps_of(type_, strength_, focus_);
    const auto order = nearest_groups();
    int solo = -1;
    float solo_score = 0;
    for (const int gi : order) {
        auto& g = W.groups[gi];
        if (!may_solo) break;
        if (kSoloFuelFirst && bc::fuel_remaining() < kSoloPlanFloor) break;
        if (g.unreachable || g.id > kMaxTarget || g.state == GroupState::reward || g.id == squad) continue;
        if (!g.harmless && !kSoloCamps && !waiting) continue;
        if (round_ < avoid_until_[g.id & 63]) continue;
        bool soft = false;
        if (opening_blocks(g, false)) {
            if (!(kSoftClusters && g.harmless && g.contested)) continue;
            soft = true;
        }
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_) ||
            comms_.claimed_by_other(slot_, Task::capture, g.id, slots_))
            continue;
        if (bc::fuel_remaining() < kSoloPlanFloor) break;
        if (cheb(g.anchor, pos_) > 24) continue;
        if (kSoloSafe && unsafe_alone(g)) continue;
        if (kFieldCamp && field() && !in_field(g.anchor)) continue;
        // Fighting camps cost a distance field and a forecast each: only with fuel to spare.
        if (!g.harmless && (bc::fuel_remaining() < 35000 || !solo_camp_ok(g, my_dps))) continue;
        const int d = dist_to_group(g);
        if (d == kUnreached || d > (squad < 0 ? 60 : 20)) continue;
        if (!g.alive_by(round_ + d + 5)) continue;
        // A side job has to be done (there, clear, capture, back) before the squad's camp returns.
        if (waiting && 2 * d + forecast(g, my_dps).rounds + kCaptureRounds > squad_wait) continue;
        float score = g.value(kGoldWeight) / (group_rounds(g, my_dps, d) + heal_rounds(g, my_dps, hp_));
        if (soft) score *= kOpeningClusters;
        if (enemy_side(g)) score *= enemy_side_factor(true);
        score *= target_danger(g);
        if (score > solo_score) {
            solo_score = score;
            solo = g.id;
        }
    }

    siege_ = false;
    if (squad >= 0) {
        const auto& g = W.groups[squad];
        siege_ = !g.harmless && forecast(g, team_dps).damage > team_hp * 0.55f;
    }
    float squad_share = 0;
    if (squad >= 0) {
        auto& g = W.groups[squad];
        const int d = dist_to_group(g);
        if (d != kUnreached)
            squad_share = g.value(kGoldWeight) / std::max(active, 1) / (group_rounds(g, team_dps, d) + heal_rounds(g, team_dps, team_hp));
    }

    if (solo >= 0 && (squad < 0 || solo_score > squad_share * 1.25f)) {
        task_ = Task::solo;
        target_ = solo;
    } else if (squad >= 0) {
        task_ = Task::farm;
        target_ = squad;
    } else {
        task_ = Task::farm;
        target_ = -1;
    }
    plan_round_ = round_;
    if (target_ != previous) wait_rounds_ = 0;
}

bool Bot::farm(bc::champion& me) {
    auto& W = world();
    bool replan = target_ < 0 || task_ == Task::other || task_ == Task::capture || round_ - plan_round_ >= kReplanRounds;
    if (target_ >= 0 && task_ != Task::other && task_ != Task::capture) {
        const auto& g = W.groups[target_];
        if (!g.alive_by(round_ + 25) || g.state == GroupState::reward) replan = true;
        // Follow the squad when someone moved it.
        if (task_ == Task::farm && target_ != my_party_target()) replan = true;
        if (wait_rounds_ > kStallRounds && !g.awake) replan = true;
        // A solo target we have waited at this long without a fight is not working out
        // (unseen camps, an unreachable tile): try something else for a while.
        if (task_ == Task::solo && wait_rounds_ > 25 && !g.awake) avoid_until_[target_ & 63] = round_ + 60;
    }
    if (replan && bc::fuel_remaining() > 25000) choose_task(me);
    if (target_ < 0 || task_ == Task::other || task_ == Task::capture) return false;

    auto& g = W.groups[target_];
    set_status(task_, target_);
    // Close enough to the camps: wait for the party (fight_camps engages when ready).
    int nearest = 1000;
    for (auto t : g.tiles) nearest = std::min(nearest, cheb(t, pos_));
    const int standoff = ranged_ ? range_ : 1;
    // Near means near on foot: a wall of camps can make a close tile a long walk.
    const bool near = nearest <= standoff + 1 && W.dist(g.poi, pos_) <= standoff + 3;
    if (near && g.state != GroupState::respawning) {
        ++wait_rounds_;
        mark("farm-wait");
        bool any_camp = false;
        for (const auto& c : camps_)
            if (c.group == target_) any_camp = true;
        bool tiles_seen = false;
        for (auto t : g.tiles) tiles_seen |= visible_.get(t);
        // Camps we still believe alive but cannot see (behind a wall or in a bush).
        bool hidden = false;
        for (const auto& c : g.camps) hidden |= c.alive_known && !visible_.get(c.pos);
        if (!any_camp && g.state == GroupState::alive && tiles_seen && !hidden) {
            // Everything here is dead but the reward is not visible yet; replan next turn.
            plan_round_ = -1000;
        }
        if (!any_camp && (hidden || !tiles_seen)) {
            // Walk right up to the camps we cannot see so we can see (and hit) them. Waiting
            // for them to show up locked a champion in place for a whole game once: one tile
            // of its cluster was in view and dead, the other hidden behind a wall.
            bc::bitboard ring;
            ring.clear();
            for (const auto& c : g.camps)
                if (!tiles_seen || (c.alive_known && !visible_.get(c.pos))) ring.draw_chebyshev(c.pos, 1);
            approach(me, ring);
        } else if (hazardous(pos_, target_) && can_move_) {
            // Wait out of reach of the neighbouring groups; enemies may wake them.
            load_occupancy();
            vec2 best = pos_;
            int best_d = 1000;
            for (auto d : kDirs) {
                const vec2 n = pos_ + d;
                if (!free_tile(n) || hazardous(n, target_)) continue;
                int dn = 1000;
                for (auto t : g.tiles) dn = std::min(dn, cheb(t, n));
                if (dn < best_d) {
                    best_d = dn;
                    best = n;
                }
            }
            if (best != pos_) try_move(me, best - pos_);
        } else if (nearest > standoff) {
            for (const auto& c : camps_)
                if (c.group == target_) {
                    approach_unit(me, c.pos, standoff);
                    break;
                }
        }
        if (kSafeWait) safe_wait_step(me, g, standoff + 1);
        return true;
    }
    if (g.state == GroupState::respawning && nearest <= 3) {
        ++wait_rounds_;
        mark("farm-respawn-wait");
        // An occupied spawn tile holds back the whole group's respawn: wait beside it.
        if (W.group_id_at(pos_) == target_) {
            std::vector<std::pair<vec2, int>> tiles;
            for (auto t : g.tiles) tiles.push_back({t, 0});
            step_away(me, tiles);
        }
        if (kSafeWait) safe_wait_step(me, g, 3);
        return true;
    }
    // A target we have been unable to walk towards for a long time is out of reach for
    // now (walled in by other camps, say): give it up for a while.
    if (stuck_rounds_ >= 30 && !g.awake) {
        avoid_until_[target_ & 63] = round_ + 100;
        plan_round_ = -1000;
    }
    if (kRally && task_ == Task::farm && rally_hold(g)) {
        mark("rally");
        return true;
    }
    if (kRouteAhead && kRoutePlan && W.plan.ok && task_ == Task::farm && g.state == GroupState::alive) {
        const int walk = dist_to_group(g);
        float dps_there = 0;
        int there = 0;
        for (const auto& a : allies_seen_)
            if (cheb(a.pos, g.anchor) <= 4) {
                ++there;
                dps_there += a.dps;
            }
        if (there >= 2 && walk != kUnreached && walk > kAheadWalk &&
            walk > forecast(g, dps_there * kFightEfficiency).rounds + kCaptureRounds + kAheadMargin) {
            const int next = next_stop(g);
            if (next >= 0) {
                const auto& n = W.groups[next];
                if (cheb(pos_, n.anchor) <= n.max_range + n.extent + 2) {
                    mark("farm-ahead-wait");
                } else {
                    mark("farm-ahead");
                    travel(me, n.poi);
                }
                return true;
            }
        }
    }
    mark("farm-travel");
    travel(me, g.poi);
    return true;
}

bool Bot::rally_hold(Group& g) {
    // Walking in to a contested jackpot: wait for company rather than arrive alone.
    auto& W = world();
    const bool any = kRallyAll && !power_play();
    const bool jackpot = g.id == W.jackpot;
    const int d = jackpot || any ? dist_to_group(g) : kUnreached;
    if (d == kUnreached || d > kRallyDist + 2 || d < kRallyDist - 3) {
        rally_since_ = -1;
        return false;
    }
    bool contested = false;
    int most = 0;  // enemies seen there at once
    for (const auto& s : sightings_)
        if (s.count > 0 && round_ - s.round <= 60 && cheb(s.pos, g.anchor) <= 12) {
            contested = true;
            most = std::max(most, s.count);
        }
    int now = 0;
    for (const auto& e : enemies_)
        if (!harmless_on_spawn(e) && cheb(e.pos, g.anchor) <= 12) {
            contested = true;
            ++now;
        }
    most = std::max(most, now);
    if (any && !contested && recent_team_deaths(100) >= 2 && (g.contested || g.side == 1)) {
        contested = true;  // we keep dying out there: assume a group
        most = std::max(most, 3);
    }
    int with = 0, living = 0;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= 6 || cheb(a.pos, g.anchor) <= kRallyDist) ++with;
    for (int i = 0; i < slots_; ++i) {
        const auto st = comms_.slot(i);
        if (i != slot_ && !(st.task == Task::other && st.target == kDead)) ++living;
    }
    const int needed = any ? std::min(std::max(most - 1, jackpot ? 2 : 1), living) : std::min(2, living);
    if (!contested || with >= needed) {
        rally_since_ = -1;
        return false;
    }
    if (rally_since_ < 0) rally_since_ = round_;
    return round_ - rally_since_ <= (any ? kRallyAllMax : kRallyMax);
}

float Bot::squad_mods(Group& g, bool contested_now) {
    // Everything the squad's camp score multiplies by besides worth per round.
    auto& W = world();
    float m = 1.f;
    // Contested ground near the enemy spawn is riskier.
    if (enemy_side(g)) m *= enemy_side_factor(false);
    if (contested_now) m *= g.harmless ? kOpeningClusters : kOpeningContested;
    if (kHoldMiddle && held_altar_near(g.anchor)) m *= kHoldBonus;
    if (power_play() && (g.contested || g.side == 1)) m *= kPowerPlayBonus;
    if (kJackpotPush && g.id == W.jackpot) m *= kJackpotBonus;
    if (kAltarZoneBias > 0) m *= altar_zone(g);
    if (kFieldCamp && in_field(g.anchor)) m *= kFieldBonus;
    if (kMiddleBias > 0) {
        const vec2 mid{W.w / 2, W.h / 2};
        m *= 1.f + kMiddleBias * (1.f - static_cast<float>(cheb(g.anchor, mid)) / std::max(1, std::max(W.w, W.h) / 2));
    }
    return m * target_danger(g);
}

int Bot::next_stop(const Group& g) {
    // The camp the squad most likely takes after g: the best quick camp near it on the plan,
    // worked out the same way by every champion (and kept for 10 rounds).
    if (ahead_for_ == g.id && round_ - ahead_round_ < 10) return ahead_target_;
    auto& W = world();
    float team_dps = 0, team_hp = 0;
    int active = 0;
    team_power(team_dps, team_hp, active, false, -1);
    team_dps *= kFightEfficiency;
    int best = -1;
    float best_rate = 0;
    const int finish = static_cast<int>(forecast(g, team_dps).rounds) + kCaptureRounds;
    for (int h : W.plan.nearest(g.id, kLookNear)) {
        auto& gh = W.groups[h];
        if (gh.unreachable || gh.id > kMaxTarget || (gh.harmless && !kSquadClusters) || gh.state == GroupState::reward) continue;
        const int walk = W.plan.between(g.id, h);
        if (walk == kUnreached || !gh.alive_by(round_ + finish + walk + 15)) continue;
        if (opening_blocks(gh, false) && !(kSoftOpening && gh.contested)) continue;
        if (siege_rounds(gh, team_dps, team_hp, false) != 0) continue;
        const float wait = gh.state == GroupState::respawning ? std::max(0, gh.respawn_round - (round_ + finish + walk)) : 0.f;
        const float rate = gh.value(kGoldWeight) / (walk + wait + forecast(gh, team_dps).rounds + kCaptureRounds);
        if (rate > best_rate) {
            best_rate = rate;
            best = h;
        }
    }
    ahead_for_ = g.id;
    ahead_round_ = round_;
    ahead_target_ = best;
    return best;
}

// ---------------------------------------------------------------------------
// Shopping

void Bot::shop(bc::champion& me, bool consumables) {
    // An altar lock (10 gold) blocks enemy tributes, or cancels one in progress.
    if (consumables && !has_lock_ && gold_ >= 10) {
        try {
            me.buy(bc::consumable_type::altar_lock);
            gold_ -= 10;
        } catch (const std::exception&) {
        }
        has_lock_ = me.consumables()[bc::consumable_type::altar_lock].count() > 0;
    }
    if (kCrystalEscape && consumables && gold_ >= 60) {
        try {
            if (me.consumables()[bc::consumable_type::tp_crystal].count() == 0) {
                me.buy(bc::consumable_type::tp_crystal);
                gold_ -= 10;
            }
        } catch (const std::exception&) {
        }
    }
    const auto& build = build_for(type_);
    while (gold_ >= 30 && build_step_ < build.size()) {
        const auto& step = build[build_step_];
        const int cost = static_cast<int>(step.item.cost());
        if (cost <= 0 || gold_ < cost) break;
        const int n = std::min(step.count - build_bought_, gold_ / cost);
        if (n <= 0) break;
        try {
            me.buy(step.item, static_cast<std::uint32_t>(n));
        } catch (const bc::error& err) {
            // Only skip a step the shop will never sell us. A stun, a channel or being off
            // spawn passes: one stun used to burn through Tavi's whole build (Tavi shops in
            // the field every turn), and it never bought anything again that game.
            using ec = bc::error_code;
            const auto code = err.code();
            if (code != ec::invalid_action_buy_item_invalid_item && code != ec::invalid_action_buy_item_inventory_full &&
                code != ec::invalid_action_buy_item_stats_overflow)
                break;
            ++build_step_;
            build_bought_ = 0;
            continue;
        } catch (const std::exception&) {
            break;
        }
        gold_ -= n * cost;
        build_bought_ += n;
        if (build_bought_ >= step.count) {
            ++build_step_;
            build_bought_ = 0;
        }
    }
}
