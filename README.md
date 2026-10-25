# Cambridge Battlecode 2026a: siege

A `cambc` workspace with **siege**, a C++ bot for the cambc2026a game, plus tools to benchmark it. Siege is the successor to apex: same squad farming, plus monolith sieges, a gold-first economy and a fix for maps whose altars are walled in by camps.

## The game in one minute

- **Goal:** tribute gems at altars. First team to 1,000 tributed gems wins; otherwise at round 2,000 the most gems wins (ties: gold earned, then kills, then Team B).
- **Draft:** pick 3 to 5 champions, one per round, at the same time as the opponent (they can't take yours). Cassi & Issac are one pick but two bodies that share a life.
- **Each turn** a champion can move one tile, make one basic attack (Strength damage; melee range 1, ranged range 3) and cast one of its two abilities (8-turn cooldowns, scale with Focus). Ranged champions can't move and attack in the same turn.
- **Camps** are monster groups. They're harmless until hit, then hit the *nearest* champion every round and don't chase. Kill the whole group, stand on one of its tiles for 5 rounds to capture it, and you get its gems and gold. It respawns 40–150 rounds later.
- **Gems** only count once tributed: stand next to an altar and channel for ceil(gems/10) turns. Carried gems are lost if a camp kills you, and stolen if a champion does.
- **Gold** buys shards on your spawn, or anywhere while dead. Five matching shards become an emblem.
- **Death:** 60 rounds out. **Recall:** 5 turns home, cancelled by damage. Standing on your spawn heals to full.
- **Vision** is per champion (8 tiles, blocked by walls and bushes). Teammates share only 8 bytes of memory, plus a 1,024-byte note written during the draft.
- **Fuel:** C++ gets about 100k WebAssembly instructions per turn (double on your first turn). Overrunning suspends your bot until next round.

The full rules are in `GameRules.md` and the C++ API in `BotAPICpp.md`, both from the competition handbook.

## What siege does

- **Draft:** Vendetta, Vesper Vale, Ryth, Tavi, Marcella. The stuns (Tavi, Ryth, Marcella) cut what camps and enemies deal back, and Vesper Vale earns a third more gold. It skips Cassi & Issac: one 600 HP body dying takes both down.
- **Squad farming:** the team agrees on one dangerous camp group through shared memory. Targets are scored by value over time, assuming the squad deals 60% of its paper damage (stragglers and retreats eat the rest), where time includes the trips home to heal the damage the fight will cost, so a young team clears cheap camps first and moves on to big ones as it grows. Champions gather and only engage once a fight forecast says the champions present can clear it safely. Gem and gold clusters (which never hit back) go to single champions, except when no dangerous group is worth the squad (cluster-only maps, or every camp respawning): then the squad farms clusters together instead of scattering, since lone champions are what enemy teams pick off.
- **Sieges:** camps never heal, so after round 500 a group too strong for one fight (a 20,000-health monolith, say) is worn down over up to four trips, with champions rotating home to heal in between. Camps that wall in every altar (the monolith pair on boss-battle) get up to eight trips, since nothing can be tributed until they fall. Champions retreat early enough to walk out of a monolith's five-tile reach.
- **Camp fights:** focus the strongest camp first. A champion that's taking the hits steps back before it could die, so a teammate becomes the nearest target. It flees camps it can't beat, including ones hitting it from out of sight.
- **Capture, tribute, shop:** one champion captures each reward. Gems are banked at the nearest open altar once a champion carries 40 (or passes close to an altar), so fewer trips interrupt farming. Champions recall when hurt, or when holding 250+ gold while their build still has something to buy (before, a finished build sent a rich champion home every turn; Tavi, who shops anywhere, never recalls just to shop). A shop error that passes (stunned, channelling, off spawn) only pauses shopping: before, one stun made Tavi skip every remaining step of its build and never buy again that game. Marcella stacks Bruiser Focus after a Durability Health emblem (her range-4 spell deals 2x Focus, and she gains 15 Focus per shard, the most of any champion); ranged damage dealers, who collect most rewards, buy a Treasure Strength emblem (20% more gold) first; melee damage dealers go straight to Monster Strength; both then take Jungle Health (30% less damage from camps, which deal most of the damage a farming champion takes), Durability Health and then Bruiser Strength for good; tanks start with Jungle Health. The repeat step is Bruiser rather than Power: 5 shards of the chosen stat plus 1 of each other is 7 shards' worth for 150 gold where Power gives 6, and emblem passives don't stack. Nearly every strong ladder team builds Bruiser; against seven local sparring bots the switch took siege from 421 to 454 wins out of 560, and it won 45/80 head to head against the Power build. Target choice counts each gold as 0.15 gems: worth chasing early for items, but not at the cost of the gem camps.
- **Map knowledge:** every champion keeps its own picture of the camps (vision isn't shared). A group last seen dead and waiting to be captured is assumed captured eight rounds later and put on its respawn timer, so champions who walked away after the kill count it again when it comes back. Enemies standing on their own spawn (which heals them every round) are ignored unless they can hit us from there; before, champions near an enemy spawn kept stepping toward and away from them. A group with no walking route past the other camps (behind boss-battle's monoliths, say, or boxed in by holes) is marked unreachable once a champion has the fuel to check, and the squad drops a target any member finds walled off: before, a champion short of fuel could pick one by straight-line terrain distance and the whole team stood still for the rest of the game (boss-battle as Team B tributed nothing, even against a bot that never moves). Teammates' health is counted as the most each has shown, not their starting health, so a team that has bought Health items no longer rates sieges and heal trips as twice as costly as they are.
- **Getting unstuck:** champions used to freeze for hundreds of rounds in real games; each case now has a way out.
  - **Hidden camps:** a champion walks up to camps it believes alive but can't see. One camp in view and dead, the other behind a wall, kept Vesper Vale still from round 29 to 2000 on great-wall. A solo target with no fight after 25 rounds of waiting is dropped for a while.
  - **Corridors:** two teammates meeting head-on in a one-tile corridor used to wait for each other for good (three champions froze for about 900 rounds on shortcut). The lower slot goes first and the other steps aside; after six rounds either one does.
  - **Unreachable targets:** a target we have not moved toward in 30 rounds, or an altar in 12, is avoided for a while, and the squad drops a target its member has had to avoid.
  - **Capture claims:** a claim lapses when the reward is gone, we are more than 14 tiles away, or six steps toward it fail; before, champions stood still "capturing" rewards across the map.
  - **Retreats:** a champion "retreating" on our own spawn fights back and heals, then goes back to work, instead of standing there for the rest of the game (973 rounds on rushbait-v2). One that can't step further away and hasn't been hit for 8 rounds stops retreating, since the threat is across a wall or a hole.
- **Safe tributes:** a tribute only banks when its channel completes (a turn per 10 gems), and dying mid-channel hands the whole bag to the killer. A champion doesn't start a channel that the enemies in reach would kill it before finishing, and cancels one in progress when they turn up, unless the bag wins the game. On boss-battle this turned a 632-1163 loss into a 907-828 win.
- **Careful mode:** every champion counts team deaths from the shared slot statuses. After 3 deaths in 150 rounds the team plays safe until they stop: gems are banked at 10 instead of 40, camps nearer the enemy spawn score x0.15 for the squad and x0.1 solo (x0.7 and x0.6 otherwise), champions go home to heal at 55% health instead of 40%, and even fights count as lost. Built against teams that keep killing us (61 deaths on hooks against kaizen, 20 a game against brakerence). Locally it took siege from 454 to 469 wins out of 560 against seven sparring bots and won 42/80 head to head, mostly against the aggressive ones (69 vs 62 against the hunter).
- **Escort after respawn:** after two team deaths in 120 rounds, a champion that has just respawned waits on spawn (up to 40 rounds) for a teammate instead of walking back out alone: in 35 ladder games with stand-and-fight, 78 deaths came within 25 rounds of a respawn. Locally neutral (39/80 against the version without it, same farming speed); on for a ladder test.
- **Other survival switches, off:** `bot.cpp` keeps five more ideas behind switches, each one line to turn on: avoiding targets near our last death or a seen enemy group (danger memory), scoring camps equidistant from both spawns lower (contested), banking at 20 gems and sharing big rewards (small bags), judging fights by one-turn burst instead of averaged damage (burst model), and scoring the enemy half lower (shun enemy side). Each cut deaths but cost more farming than it saved against the local sparring bots: 428-451 wins out of 560 alone, and all seven together farmed 10% slower even with no enemy around. They may still be worth a ladder scrim, since the sparring bots don't spawn-camp the way kaizen does.
- **Altar locks:** every champion buys a lock (10 gold) when home and places it on enemy-side altars it passes (on maps with one or two altars, on any altar), or wherever an enemy is mid-tribute, which cancels their channel. Our own locks never block us; altars with enemy locks are avoided when another is close. Both teams can lock the same altar, and the lookup then returns ours, so a champion whose tribute fails searches the enemy's locks and attacks theirs.
- **Wards:** champions drop their free ward on groups they capture and at altars they tribute at, so they keep seeing respawns and enemies there after leaving.
- **PvP:** the team agrees on one enemy to focus through shared memory (byte 7), so everyone in reach hits the same champion. Damage spells and stuns (range 4) are fired at that enemy, or the weakest in spell range, before anyone walks, so casters use their reach instead of waiting for basic-attack range. It fights enemies nearby unless clearly outmatched, and retreats when personally in danger, except that a champion with two teammates beside it stays in a fight that is not lost (a champion running alone from its team's fight is the one that dies); and ignores enemies sitting on their own spawn.
- **Stand and fight:** ladder games showed our champions landing a hit in under half of their fight rounds against 55-79% for the teams beating us, and losing even fights 24 to 146. Debug logs showed why: with an enemy within 7 tiles a champion kept farming 43% of the time, stepped away 21% (median health 58%) and chased 16%, and a ranged champion that moves cannot attack. Now allies are counted as far out as enemies (8 tiles), champions retreat only when in danger or clearly losing (1.6x, 1.3x under careful mode), hit back instead of fleeing an adjacent melee champion (it walks and hits every turn), engage enemies within 5 tiles, and ranged champions let enemies step into their range rather than step into theirs, for up to 6 turns, unless a teammate is on the enemy or it is nearly dead. On the ladder (14 games) this doubled kills per game (3.1 to 6.4), evened champion damage (17k-35k a game before, 26.5k-28k after) and turned fights where we outnumbered the enemy from 19-62 into 27-18. Locally it looked worse, because the sparring bots share our fight code. Fights where we were outnumbered were still 7-71 (97 of our deaths against 20), so with more enemies than allies within 6 tiles champions no longer engage and leave when they come close; this last rule has not been played on the ladder yet.
- **Fuel-safe:** map knowledge lives in local bit grids and cached distance fields, so turns stay under budget.

**Results** (release mode, all 40 maps, both sides, 80 games each): 80/80 against the official starter bot (about 987 gems to 74), and a winning record against every earlier version. Against four ladder teams ranked 137-164 (20 scrims each on the same maps and sides), wins went from 10/20 to 12/20 with the last two fight changes. On a 110-game benchmark against 22 ladder teams ranked 100-205 (every map, both sides), the tribute guard took siege from 51 to 57 wins and was ahead on each of the three opponent sets.

### Farming speed

`python3 tools/pve.py BOT [--compare OTHER]` plays a bot against `bots/idle` (which never moves) on every map from both sides and reports how fast it farms with nobody contesting it: rounds to tribute 1,000 gems, gems at rounds 300/500/1000, gold and clears by round 500. The games are deterministic, so economy changes show up directly instead of drowning in the noise of real matches (one changed decision flips about a quarter of real games either way). The two map-knowledge fixes, the gold weight and the trip limit took the average from 1,360 rounds to 1,180; in real scrims siege then banked 5-26% more gems by round 500 against the same opponents. Buying Jungle before Durability took it to 1,157 rounds and, against the same 22 teams on the same day, from 61 to 69 wins out of 110 (58 for the version before these changes). The walled-target and teammate-health fixes took it to 1,136 rounds; in a same-evening run against the 22 teams (several had updated their bots since the earlier one, so only same-evening totals compare), the walled-target fix alone scored 64/110, both fixes 66/110, and the same two builds with Vendetta's Monster emblem in Health instead of Strength 58/110 each. The getting-unstuck fixes took it to 1,122 rounds, with half the deaths (great-wall 1,353 to 1,056, lost-world 1,056 to 716; boss-battle got slower, 836 to 1,032, and no single fix explains it). Against seven local sparring bots (560 games) they scored 421 wins to 423, won 45/80 head to head against the version without them, and cut stretches of 300+ rounds without activity from 19 to 6. They have not been scrimmed against the ladder yet.

## Setup

```bash
python3 -m pip install --upgrade cambc
cambc init --yes .      # refreshes SDK, toolchain and maps
cambc build siege
cambc run siege starter-cpp --map 00ambush          # add --view to watch it
python3 tools/bench.py siege starter-cpp             # all maps, both sides
python3 tools/table.py                              # per-map summary of that run
```

Debug builds (the default for `cambc run`) print status lines from `LOG(...)` and `debug_block` into the replay bundle. Release builds (`--release`, as on the platform) skip them.

## Uploading to the portal

**Command line:**

```bash
cambc login          # sign in with your platform account (session lasts 7 days)
cambc submit siege   # uploads bots/siege source to your team's submissions
```

**Or through the website:**

```bash
cambc bundle siege   # writes siege.zip in the current directory
```

Then upload `siege.zip` on the submissions page of https://2026a.battlecode.co.uk/. A ready-made copy is kept at `submissions/siege.zip`; `submissions/apex.zip` is the previous version, in case you want to roll back.

The platform compiles the source itself in release mode, so only `bots/siege/src` is sent.

Once it's submitted, `cambc ladder` shows the ladder, `cambc matches` lists and downloads your matches, and `cambc match-groups` queues unranked scrims. All three need you to be logged in.
