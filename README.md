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
- **Squad farming:** the team agrees on one dangerous camp group through shared memory. Targets are scored by value over time, assuming the squad deals 60% of its paper damage (stragglers and retreats eat the rest), where time includes the trips home to heal the damage the fight will cost, so a young team clears cheap camps first and moves on to big ones as it grows. Champions gather and only engage once a fight forecast says the champions present can clear it safely. Gem and gold clusters (which never hit back) go to single champions.
- **Sieges:** camps never heal, so after round 500 a group too strong for one fight (a 20,000-health monolith, say) is worn down over up to two trips, with champions rotating home to heal in between. Camps that wall in every altar (the monolith pair on boss-battle) get up to eight trips, since nothing can be tributed until they fall. Champions retreat early enough to walk out of a monolith's five-tile reach.
- **Camp fights:** focus the strongest camp first. A champion that's taking the hits steps back before it could die, so a teammate becomes the nearest target. It flees camps it can't beat, including ones hitting it from out of sight.
- **Capture, tribute, shop:** one champion captures each reward. Gems are banked at the nearest open altar once a champion carries 40 (or passes close to an altar), so fewer trips interrupt farming. Champions recall when hurt or holding 250+ gold. Marcella stacks Power Focus after a Durability Health emblem (her range-4 spell deals 2x Focus, and she gains 15 Focus per shard, the most of any champion); ranged damage dealers, who collect most rewards, buy a Treasure Strength emblem (20% more gold) first; melee damage dealers go straight to Monster Strength, then Durability Health and Power Strength; tanks start with Jungle Health. Target choice counts each gold as 0.3 gems (up from 0.15), so the team grows stronger sooner.
- **Altar locks:** every champion buys a lock (10 gold) when home and places it on enemy-side altars it passes (on maps with one or two altars, on any altar), or wherever an enemy is mid-tribute, which cancels their channel. Our own locks never block us; altars with enemy locks are avoided when another is close.
- **Wards:** champions drop their free ward on groups they capture and at altars they tribute at, so they keep seeing respawns and enemies there after leaving.
- **PvP:** the team agrees on one enemy to focus through shared memory (byte 7), so everyone in reach hits the same champion. Damage spells and stuns (range 4) are fired at that enemy, or the weakest in spell range, before anyone walks, so casters use their reach instead of waiting for basic-attack range. It fights enemies nearby unless clearly outmatched, and retreats when personally in danger, except that a champion with two teammates beside it stays in a fight that is not lost (a champion running alone from its team's fight is the one that dies); and ignores enemies sitting on their own spawn.
- **Fuel-safe:** map knowledge lives in local bit grids and cached distance fields, so turns stay under budget.

**Results** (release mode, all 40 maps, both sides, 80 games each): 80/80 against the official starter bot (about 987 gems to 74), and a winning record against every earlier version. Against four ladder teams ranked 137-164 (20 scrims each on the same maps and sides), wins went from 10/20 to 12/20 with the last two fight changes.

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
