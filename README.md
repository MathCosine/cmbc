# Cambridge Battlecode 2026a: apex

A `cambc` workspace with **apex**, a C++ bot for the cambc2026a game, plus tools to benchmark it.

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

## What apex does

- **Draft:** Vendetta, Vesper Vale, Varg, Willow, Marcella. It skips Cassi & Issac: one 600 HP body dying takes both down.
- **Squad farming:** the team agrees on one dangerous camp group through shared memory. Champions gather and only engage once a fight forecast says the champions present can clear it safely. Gem and gold clusters (which never hit back) go to single champions.
- **Camp fights:** focus the strongest camp first. A champion that's taking the hits steps back before it could die, so a teammate becomes the nearest target. It flees camps it can't beat, including ones hitting it from out of sight.
- **Capture, tribute, shop:** one champion captures each reward. Gems are banked at the nearest open altar. Champions recall when hurt or holding 400+ gold, then buy Monster then Power Strength shards (tanks get Jungle Health first).
- **PvP:** fights enemies nearby unless clearly outmatched, retreats when personally in danger, and ignores enemies sitting on their own spawn.
- **Fuel-safe:** map knowledge lives in local bit grids and cached distance fields, so turns stay under budget.

**Results** (release mode, all 40 maps, both sides, 80 games): it wins 79 of 80 against the official starter bot, averaging about 930 gems to about 130. Against its own first version in self-play it wins about 58 of 80.

## Setup

```bash
python3 -m pip install --upgrade cambc
cambc init --yes .      # refreshes SDK, toolchain and maps
cambc build apex
cambc run apex starter-cpp --map 00ambush          # add --view to watch it
python3 tools/bench.py apex starter-cpp             # all maps, both sides
python3 tools/table.py                              # per-map summary of that run
```

Debug builds (the default for `cambc run`) print status lines from `LOG(...)` and `debug_block` into the replay bundle. Release builds (`--release`, as on the platform) skip them.

## Uploading to the portal

**Command line:**

```bash
cambc login          # sign in with your platform account (session lasts 7 days)
cambc submit apex    # uploads bots/apex source to your team's submissions
```

**Or through the website:**

```bash
cambc bundle apex    # writes apex.zip in the current directory
```

Then upload `apex.zip` on the submissions page of https://2026a.battlecode.co.uk/.

The platform compiles the source itself in release mode, so only `bots/apex/src` is sent.

Once it's submitted, `cambc ladder` shows the ladder, `cambc matches` lists and downloads your matches, and `cambc match-groups` queues unranked scrims. All three need you to be logged in.
