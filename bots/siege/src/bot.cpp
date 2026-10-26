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
constexpr bool kMarcellaBank = false;
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
// Breach. On 02the-pack the only altar sits inside a ring of 48 wolves: our siege rules never
// took the ring on, and neither side turned in a gem in 2,000 rounds. When no altar can be
// reached past live camps, the squad kills only the camps on the shortest walk in (see
// World::plan_breach), and nobody walks to the altar until the way is open.
constexpr bool kBreach = true;
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
    if (t == bc::champion_type::marcella) return caster;
    if (tank_type(t)) return tank;
    return bc::champion{t}.base_stats().base_attack_range > 1 ? earner : fighter;
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
        auto& cached = dps_cache_[static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)) & 31];
        const int refresh = kBurstModel && enemy ? 5 : 25;
        if (cached.round < 0 || round_ - cached.round > refresh) {
            const float strength = c.strength(), focus = c.focus();
            cached = {dps_of(t, strength, focus), burst_of(t, strength, focus), round_};
        }
        s.dps = cached.dps;
        s.burst = cached.burst;
        s.reach = threat_for(t).reach;
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
        if (g.state == GroupState::reward && round_ - g.seen_round > kRewardTimeout) {
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
    int guard = -1;
    for (auto t : kGuardOrder) {
        for (int i = 0; i < slots_ && i < static_cast<int>(ally_types_.size()); ++i)
            if (ally_types_[i] == t && !slot_dead_[i] && !(comms_.slot(i).task == Task::other && comms_.slot(i).target == kHome)) {
                guard = i;
                break;
            }
        if (guard >= 0) break;
    }
    if (guard != slot_) return false;
    mark("guard-altar");
    if (cheb(pos_, altar) > 3) approach_unit(me, altar, 2);
    return true;
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
    return true;
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
    if (damage_taken_ > 0) last_hurt_round_ = round_;
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
    if (at_spawn || type_ == bc::champion_type::tavi) shop(me, at_spawn);
    mark("locks");
    place_lock(me);
    scout_ward(me);

    mark("enemies");
    if (fight_enemies(me)) return;
    mark("flee");
    if (flee_camps(me)) return;
    mark("retreat");
    if (retreat_from_camps(me)) return;
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
        for (const auto& e : enemies_) {
            if (world().enemy_spawn.get(e.pos) || cheb(e.pos, pos_) > static_cast<int>(sp.range)) continue;
            const bool focused = slots_ <= 5 && enemy_id(e) == comms_.focus();
            if (!best || focused || (e.hp < best->hp && !(slots_ <= 5 && enemy_id(*best) == comms_.focus()))) best = &e;
        }
        if (best && spell_ready(i)) cast_on(sp, best->handle);
    }
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
    const float my_dps = dps_of(type_, strength_, focus_);
    float ally_dps = my_dps, ally_hp = hp_;
    // Count allies as far out as enemies: a shorter ally range made even fights look lost.
    const int ally_range = kStandAndFight ? 8 : 6;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= ally_range) {
            ally_dps += a.dps;
            ally_hp += a.hp;
        }
    float enemy_dps = 0, enemy_hp = 0, threat = 0, burst = 0;
    const SeenChamp* nearest = nullptr;
    int nearest_d = 1000;
    // With the burst model we also look at groups 8-10 tiles out, to leave before they arrive.
    const int sense = kBurstModel ? 10 : 8;
    for (const auto& e : enemies_) {
        if (harmless_on_spawn(e)) continue;
        const int d = cheb(e.pos, pos_);
        if (d <= sense) {
            enemy_dps += e.dps;
            enemy_hp += e.hp;
        }
        if (d <= e.range + 1) threat += e.dps;
        // What could land on us before our next turn if we stay here.
        if (d <= e.reach) burst += e.burst;
        if (d < nearest_d) {
            nearest_d = d;
            nearest = &e;
        }
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
    const bool clearly_losing = we_need > they_need * (pressured() ? 1.3f : 1.6f) || outnumbered;

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
        if (huntable(e) && cheb(e.pos, pos_) <= reach && (!target || e.hp < target->hp)) target = &e;
    // Everyone in reach of the team's chosen enemy hits it; otherwise ours becomes the choice.
    if (slots_ <= 5) {
        for (const auto& e : enemies_)
            if (enemy_id(e) == comms_.focus() && huntable(e) && cheb(e.pos, pos_) <= reach) target = &e;
        if (target) comms_.set_focus(enemy_id(*target));
    }
    const bool can_finish = target && target->hp <= strength_ * 1.05f;
    // One-turn burst decides whether we survive the next round; averaged damage hid spell
    // nukes and let enemies one-shot us.
    const bool in_danger = hp_ < threat * 2.5f + max_hp_ * 0.1f || (kBurstModel && burst >= hp_ * 0.7f);

    auto strike = [&](const SeenChamp& e) {
        bool acted = false;
        if (cheb(e.pos, pos_) > range_) acted = approach_unit(me, e.pos, range_);
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
    if (retreating) {
        // Shoot first if we cannot get away anyway, then run toward home.
        use_combat_spells(me, nullptr, target);
        set_status(Task::other, kHome);
        if (step_away(me, threats)) return true;
        if (target && cheb(target->pos, pos_) <= range_) {
            attack_target(me, target->handle, target->pos, target->hp);
            return true;
        }
        // No tile is safer and nothing is in range. If nothing has hurt us for a while,
        // the threat is across a wall or a hole: get on with the game rather than stare
        // at it (one champion stood like this for 200+ rounds).
        return round_ - last_hurt_round_ <= 8;
    }
    if (kJoinFights && !retreating && !in_danger) {
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
            const int d = focused ? -1 : cheb(e.pos, pos_);
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
    if (kStandAndFight && !losing && !outnumbered) {
        // Engage rather than keep farming while an enemy walks up and picks the moment.
        const SeenChamp* goal = nullptr;
        for (const auto& e : enemies_) {
            if (!huntable(e) || cheb(e.pos, pos_) > kEngageRange) continue;
            const bool focused = slots_ <= 5 && enemy_id(e) == comms_.focus();
            if (!goal || focused || (e.hp < goal->hp && !(slots_ <= 5 && enemy_id(*goal) == comms_.focus()))) goal = &e;
        }
        if (goal) {
            bool busy = false;  // a teammate is already on it
            for (const auto& a : allies_seen_)
                if (cheb(a.pos, goal->pos) <= 2) busy = true;
            const bool low = goal->hp <= strength_ * 2.f;
            // A ranged champion that steps in cannot attack that turn, and a ranged enemy at the
            // edge of its range shoots first: let it come to us instead.
            const bool under_fire = kCampAwareFights && incoming_camp_damage(pos_) > 0;
            if (ranged_ && !busy && !low && hold_turns_ < kMaxHold && !under_fire) {
                ++hold_turns_;
                mark("hold");
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
        float best_missing = focus_ * s.modifier * 0.6f;
        for (const auto& a : allies_seen_) {
            const float missing = a.max_hp - a.hp;
            if (cheb(a.pos, pos_) <= static_cast<int>(s.range) && missing > best_missing) {
                best = &a;
                best_missing = missing;
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
            if (enemy && cheb(enemy->pos, pos_) <= static_cast<int>(s.range)) {
                if (spell_ready(i)) cast_on(s, enemy->handle);
            } else if (camp && cheb(camp->pos, pos_) <= static_cast<int>(s.range)) {
                if (s.kind == bc::ability_kind::cc && (!camp->awake || camp->strength <= 0) && stun_only) break;
                if (camp->hp < strength_ && s.kind == bc::ability_kind::damage) break;  // the basic attack finishes it
                if (spell_ready(i)) cast_on(s, camp->handle);
            }
            break;
        }
        case bc::ability_kind::aoe_damage: {
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

bool Bot::step_away(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats) {
    if (!can_move_ || moved_) return false;
    load_occupancy();
    auto& W = world();
    auto margin = [&](vec2 p) {
        int worst = 1000;
        for (const auto& [t, r] : threats) worst = std::min(worst, cheb(p, t) - r);
        return worst;
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
        if (!channeling && !their_side && W.altars.size() > 2) continue;
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
            if (c.pos == n) camp_alive = g.alive_by(round_) && c.alive_known;
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
    if (!threatened) {
        try {
            me.recall();
            return true;
        } catch (const std::exception&) {
        }
    }
    return travel(me, W.spawn_poi);
}

float Bot::tribute_threat() const {
    // Damage per round from enemies that can be on us within two steps.
    float threat = 0;
    for (const auto& e : enemies_)
        if (!world().enemy_spawn.get(e.pos) && cheb(e.pos, pos_) <= e.range + 2) threat += e.dps;
    return threat;
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
    if (kBankEscort && !endgame && !wins && gems_ >= (kTeamBank ? kEscortBag / 2 : kEscortBag) && escort_wait_ < 8 && cheb(pos_, altar) <= 9 &&
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
        try {
            me.submit_gems(altar);
            drop_ward(me);
            task_ = Task::farm;
            target_ = -1;
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
        if (g.state != GroupState::reward || cheb(g.anchor, pos_) > 14 || capture_fail_ >= 6) {
            // Tiles we could not walk to (an island only a dash reaches, say): stop
            // re-claiming them every few turns.
            if (capture_fail_ >= 6) avoid_until_[target_ & 63] = round_ + 150;
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
            const int sharers = !kSmallBags ? 1 : g.gems >= 100 ? 5 : g.gems >= 40 ? 3 : 1;
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
    const int d = exact ? W.dist(g.poi, pos_) : dist_from_me(g.anchor);
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

void Bot::team_power(float& dps, float& hp, int& active, bool include_home) const {
    dps = 0;
    hp = 0;
    active = 0;
    for (int i = 0; i < slots_; ++i) {
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
    const int home = W.dist(W.spawn_poi, g.anchor, 20000);
    if (home == kUnreached) return 0;
    return kHealCost * forecast(g, dps).damage / std::max(hp, 1.f) * (2.f * home + 10.f);
}

float Bot::siege_rounds(const Group& g, float team_dps, float team_hp, bool unlocks) {
    // Extra rounds spent going home and back between trips; negative when out of reach.
    const float damage = forecast(g, team_dps).damage;
    if (damage <= team_hp * (unlocks ? 0.8f : kSiegeShare)) return 0;
    // A young team besieging a monolith in contested ground loses the early game.
    if (!unlocks && round_ < kSiegeFrom) return -1;
    const float trips = damage / std::max(team_hp * kTripShare, 1.f);
    if (trips > (unlocks ? kMaxUnlockTrips : kMaxTrips)) return -1;
    auto& W = world();
    const int home = W.dist(W.spawn_poi, g.anchor, 20000);
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
    for (const int gi : order) {
        auto& g = W.groups[gi];
        if (bc::fuel_remaining() < 20000) break;
        if (g.unreachable || g.harmless || g.id > kMaxTarget || g.id == exclude || g.state == GroupState::reward) continue;
        if (round_ < avoid_until_[g.id & 63]) continue;
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_)) continue;
        const int d = dist_to_group(g);
        // When every altar is walled in by camps, the camps doing it hold the game hostage.
        const bool unlocks = !altar_reachable && blocks_altar(g);
        if (d == kUnreached) continue;
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
        if (extra < 0) continue;
        if (!g.alive_by(round_ + d + respawn_wait)) continue;
        float score = (g.value(kGoldWeight) + (unlocks ? 1000.f : 0.f)) /
                      (group_rounds(g, team_dps, d) + extra + heal_rounds(g, team_dps, team_hp));
        // Contested ground near the enemy spawn is riskier.
        if (enemy_side(g)) score *= enemy_side_factor(false);
        if (contested_now) score *= kOpeningContested;
        score *= target_danger(g);
        if (score > best_score) {
            best_score = score;
            best = g.id;
        }
    }
    if (best >= 0) return best;
    // Nothing dangerous is worth it (clusters only, or every camp respawning): the squad
    // farms clusters together rather than scattering, which leaves lone champions for
    // an enemy team to pick off.
    for (const int gi : order) {
        auto& g = W.groups[gi];
        if (bc::fuel_remaining() < 20000) break;
        if (!g.harmless || g.unreachable || g.id > kMaxTarget || g.id == exclude || g.state == GroupState::reward) continue;
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_)) continue;
        if (opening_blocks(g, allow_contested)) continue;
        const int d = dist_to_group(g);
        if (d == kUnreached || !g.alive_by(round_ + d + 5)) continue;
        float score = g.value(kGoldWeight) / group_rounds(g, team_dps, d);
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
    float team_dps = 0, team_hp = 0;
    int active = 0;
    team_power(team_dps, team_hp, active);
    const int previous = target_;

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
        team_power(living_dps, living_hp, living, true);
        const bool unlocks = blocks_altar(g) && !any_altar_open();
        const bool walled = dist_to_group(g) == kUnreached && g.unreachable;
        const bool stale = walled || g.state == GroupState::reward ||
                           (g.state == GroupState::respawning && g.respawn_round > round_ + 25) ||
                           siege_rounds(g, living_dps, living_hp * 1.25f, unlocks) < 0;
        // We could not reach it lately (farm() set the avoid timer): pick another.
        const bool blocked_for_me = round_ < avoid_until_[squad & 63];
        // We died beside it lately: teammates cannot know, so move the whole squad on rather
        // than walk back there one at a time.
        const bool deadly = kDangerMemory && !g.awake && !unlocks && danger_factor(g.anchor) < 0.35f;
        if (deadly) avoid_until_[squad & 63] = round_ + 60;
        if ((stale || stalled || blocked_for_me || deadly) && !breaching) squad = -1;
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
    const bool may_solo = waiting || solos < std::max(1, active / 3) || squad < 0;
    const float my_dps = dps_of(type_, strength_, focus_);
    const auto order = nearest_groups();
    int solo = -1;
    float solo_score = 0;
    for (const int gi : order) {
        auto& g = W.groups[gi];
        if (!may_solo) break;
        if (g.unreachable || g.id > kMaxTarget || g.state == GroupState::reward || g.id == squad) continue;
        if (!g.harmless && !kSoloCamps && !waiting) continue;
        if (round_ < avoid_until_[g.id & 63]) continue;
        if (opening_blocks(g, false)) continue;
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_) ||
            comms_.claimed_by_other(slot_, Task::capture, g.id, slots_))
            continue;
        if (bc::fuel_remaining() < 12000) break;
        if (cheb(g.anchor, pos_) > 24) continue;
        // Fighting camps cost a distance field and a forecast each: only with fuel to spare.
        if (!g.harmless && (bc::fuel_remaining() < 35000 || !solo_camp_ok(g, my_dps))) continue;
        const int d = dist_to_group(g);
        if (d == kUnreached || d > (squad < 0 ? 60 : 20)) continue;
        if (!g.alive_by(round_ + d + 5)) continue;
        // A side job has to be done (there, clear, capture, back) before the squad's camp returns.
        if (waiting && 2 * d + forecast(g, my_dps).rounds + kCaptureRounds > squad_wait) continue;
        float score = g.value(kGoldWeight) / (group_rounds(g, my_dps, d) + heal_rounds(g, my_dps, hp_));
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
        if (task_ == Task::farm && target_ != comms_.squad_target()) replan = true;
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
        return true;
    }
    // A target we have been unable to walk towards for a long time is out of reach for
    // now (walled in by other camps, say): give it up for a while.
    if (stuck_rounds_ >= 30 && !g.awake) {
        avoid_until_[target_ & 63] = round_ + 100;
        plan_round_ = -1000;
    }
    mark("farm-travel");
    travel(me, g.poi);
    return true;
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
