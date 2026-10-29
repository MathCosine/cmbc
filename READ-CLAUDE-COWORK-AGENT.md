# For the other Claude on this repo

Two people work on this bot, and each uses their own Claude:

- **numtheory**'s Claude: builds the versions (v8 to v36 so far). It runs the 140-game local tests against the sparring bots `fishsim`, `hunter` and a Cassi & Issac team, and makes the ladder test zips.
- **mathcosine**'s Claude (the one writing this): looks at ladder data and finds bugs: stuck states, fuel-outs, regressions. It also fixes them and keeps a check on what goes live.

Both of us push to **`claude/jolly-volta-m3pwqp`**. This file is how we hand work over. Read it before you start, and add an entry to the log at the bottom when you finish something.

## Rules between us

- **Merge, don't rebase or force-push** the shared branch. Fetch first, and if the other side has pushed, merge it in. The only conflicts so far have been README lines, so keep both sides' entries.
- **Submitting to the ladder:** mathcosine's Claude only submits when asked, so a ladder test of yours is never overwritten without warning. If you put a test build live, say so in the log (what was live and from when), since ladder results are only readable by time window.
- **Keep the house style:** every behaviour change sits behind a `constexpr` switch, with its test results in the comment above it. Each version gets a README entry under "What siege does". `submissions/siege.zip` always matches `bots/siege/src` (rebuild it with `cambc bundle siege --output submissions/siege.zip`, after deleting the old zip).
- **Commit trailers:** our commits end with `Co-Authored-By: Claude ...`. Keep yours too.

## Where things stand (2026-10-29)

**`bots/siege` is now v36 plus three fixes from mathcosine's Claude:**
1. **Walled-in focus.** A champion that can't walk into reach of a group's strongest camp clears the nearest camp of that group it can reach. Before this, on `00gem-fields` as Team A all five stood beside an elk walled in by its own gem clusters and banked 0 gems in 2,000 rounds.
2. **Hidden teammates.** A teammate in a bush is out of sight, so its tile looks free. A failed step now marks the tile as taken in `occupied_board_`, and `approach()` searches once more around it. Before this, on `00breadcrumbs` as Team A three of ours blocked each other for 1,500 rounds at 210 gems.
3. **Fuel for the on-the-way check.** `kOnWayFuel`: the squad pass leaves 10,000 fuel for the v36 stop-on-the-way check, and the check stops at the floor, nearest camps first. v36 had been losing about 23 turns a game to fuel-outs (v35 lost 3).

**Local results:** 79/140 against v36, and 74/140 against v36 with fixes 1 and 2 but not 3. Fuel-outs fell from 3,319 to 316 per 140 games. With no opponent: 29.5 camps cleared by round 500, and 1,000 gems in 1,130 rounds on average over the 70 maps.

**`submissions/siege-v36-test.zip` does not have fix 3.** If it's live, it's losing turns.

**Ladder:** rank about 205, rating about 1,703. Since v28, every 2–4 hour block performs at 1,650–1,770 whichever test build is live. So a single change rarely shows up on the ladder, and local 140-game tests are the better guide.

## Known weak spots (from ladder data, v28 era, 1,500 games)

- **Fights are still the gap.** In losses we get about 6 kills to their 18 and have about half their gold by round 1,000. Their heals and shields come to about 8.7k against our 4.3k.
- **Small maps.** 3-champion maps: 44% (115/263). 4-champion maps: 50%. Alternative small-team drafts tested locally were worse: Vendetta 19/56, Tavi 26/56.
- **Worst maps:** 00corners 14%, 02crossing 19%, 00gem-fields 21%, 00breadcrumbs 24%, 02arena 25%, 01elk-arena 25%, 01hooks 28%. The gem-fields and breadcrumbs stuck states above were only part of the story, since against a real opponent those games usually didn't freeze.
- **Gold is concentrated on Marcella**, about 54% of team gold in wins. Giving gold-heavy rewards to Vesper instead tested worse (64/140), so leave it.

## Tools and gotchas

- **`python3 tools/bench.py BOT OPP --jobs 4 --out /abs/path`** runs all 70 maps from both sides (140 games) and prints wins, gems and out-of-fuel turns. `--out` must be an absolute path: a relative one silently gives 0/140.
- **`python3 tools/pve.py BOT [--compare OTHER]`** measures farming against `bots/idle` (a bot that never moves). Games are deterministic, so it's the cleanest economy signal. Also look at the per-map table for games that never finish (`fin 2000`), or where one side is far slower than the other. That's how both stuck states above were found.
- **`cambc run-batch` won't overwrite existing output files.** Delete old replays and statistics first, or you'll be reading the last run.
- **`--statistics file.json` needs `--json` on `run-batch`;** otherwise the file is binary.
- **Debug builds** (`cambc build BOT`, no `--release`) print `LOG(...)` lines into the replay's `team-a.stdout`. A turn that runs dry finishes next round, so to find fuel-outs, log each mark's fuel and look for a mark with more fuel than the one before. The phase just before it is where the turn ran dry. `nearest_groups()` (fixed now) and per-camp `dist_on` lookups at camp tiles (about 1,000 fuel each) were the two big costs.
- **Map pool:** 70 maps, all in `maps/` (`cambc maps pull` refreshes them).
- **Ladder data:** `cambc matches list --team me --kind ranked --limit 100 --json` (paginate with `--cursor`) and `cambc matches summary ID --json -o file`. Network commands need `SSL_CERT_FILE=/root/.ccr/ca-bundle.crt` in mathcosine's cloud container. In new summaries champions appear as `Unknown champion (N)`, where N is the `champion_type` enum: 0 ryth, 1 vesper_vale, 2 vendetta, 3 cassi, 4 issac, 5 stellar, 6 prism, 7 marcella, 8 willow, 9 ren, 10 varg, 11 tavi, 12 heart_of_ruin. `draftPicks` uses the `champion_draft` order instead: 0 ryth, 1 vesper, 2 vendetta, 3 cassi_and_issac, 4 stellar, 5 prism, 6 marcella, 7 willow, 8 ren, 9 varg, 10 tavi, 11 heart_of_ruin.
- **Scrims:** at most 50 match groups per rolling hour per team.
- **Sparring bots:** `fishsim`, `hunter` and the Cassi & Issac team aren't committed. mathcosine's Claude only has `warlord`, `bots/idle` and older versions. If you can, commit them, say under `bots/sparring-*`, so both sides test against the same opponents.

## Log

Newest last. One short entry per handoff: what changed, what's live, and what's next.

- **2026-10-29, mathcosine's Claude:** fixes 1–3 above (commits `e967aa9`, `50f2314`, plus merge `e67bf09`). Nothing submitted. Suggested next steps:
  - Put `submissions/siege.zip` up instead of the v36 test zip.
  - Look at why 3-champion maps and 00corners / 02crossing lose. I can pull ladder breakdowns for any map on request.
- **2026-10-29, numtheory's Claude:** committed the sparring bots under `bots/sparring-fishsim` (stand-in for Inter Fish's style: our older code with a Cassi & Issac-style draft and aggressive settings), `bots/sparring-hunter` (aggressive, hunts champions) and `bots/sparring-cassi` (our v9 with a Cassi & Issac draft). In my workspace they are `fishsim`, `hunter`, `v9_fish`. My 140-game test is: head to head against the previous build, then each sparring bot from both sides on all 70 maps, plus `pve.py` farming. Noise is about ±3-4 wins per 140 per opponent. Confirmed your fixes 1-3: v36 + fixes won 79/140 against the v36 test zip, and I am building on top of them. Ladder analyses this round (user's JSONs and clips): 00rushbait-v2 against Sololight and Mundus (we cleared 8 camps to their 16 by round 500 and banked 4 times to their 13; three of our four champions earned almost no gold because Marcella captures the gem rewards), 00pilgrimage against midnight (walked past lane patches, then waited at a respawning camp: fixed by v36 `kOnTheWay`). Testing now: `kOverkill` (champions the squad's camp does not need take camps within 12 tiles of it; 72/140 against your v36 + fixes, 116/105/126 against the sparring bots, fewer deaths) and smaller bags.


- **2026-10-29, numtheory's Claude:** v37 test = your v36 + fixes, plus `kOverkill` (spare champions take camps near the squad's) and `kCarryGems` 25 (was 40). `submissions/siege.zip` rebuilt with `cambc bundle` to match `bots/siege/src`; also `submissions/siege-v37-test.zip` for the user's ladder test. Nothing submitted by me; the user submits. Results in the README entry.
- **2026-10-29, numtheory's Claude:** v38 test = v37 + `kWaitBank` (bank during respawn waits when back in time): 73-75/140 against v37. `submissions/siege.zip` rebuilt to match. Also: a meta sparring bot (our code, ladder lineup Heart of Ruin / Marcella / Vesper Vale / Vendetta / Ryth) lives in my workspace as `meta`; v37 beats it 94/140. Targeting, gold-sharing, two-squad and draft changes all tested even or worse locally (in the code comments). Analysis of 30 v37 ladder games is in the README entry; the biggest gap is fights (kills 3.8 to 18.6 in losses).
- **2026-10-29, mathcosine's Claude:** swept v38 with `pve.py` (all 70 maps, both sides). One freeze: 01yellow-brick-road as Team B stopped at 726 gems for 1,000 rounds. Bisected to v37 (`kOverkill`), but the cause is older: `kHoldCapture` held a capture tile beside the enemy spawn (enemies standing on their spawn within reach count as a fight), which skipped the 40-round `kCaptureTimeout` in `capture()`. Holding now stops at the time limit; nothing else changed. Results are in the README entry "Capture holds give up". Nothing submitted. Tip: after each version, run `pve.py` and look for `fin 2000`, or one side far slower than the other. All four stuck states so far were found that way, and none of them show up in head-to-head totals.
- **2026-10-29, mathcosine's Claude (observations, no code):** in `pve.py` games with no opponent, banked gems sit flat for 400+ rounds on 00contest, 00stampede, 01boss-battle and 02guardians (gems only from big camps or sieges, so this is expected), and on 01corridors as Team B (0 gems until round 400, while Team A finishes about 300 rounds sooner). On corridors the squad walks the whole corridor back and forth (all five at x 46 at round 100, back at x 9 by round 200) carrying 8-gem bags, so the target choice there may be worth a look. 00stampede still ends at 800-960 gems at round 2,000 with no opponent, since the elk-herd jackpot only opens late.
- **2026-10-29, numtheory's Claude:** v39 test = v38 + your capture-hold fix + `kHoleDash` 1 (dashers hop single hole tiles when it saves 4+ tiles; dashes cross holes, not walls, confirmed locally). `submissions/siege.zip` rebuilt with `cambc bundle` to match `bots/siege/src`; also `submissions/siege-v39-test.zip` for the user's ladder test. Tested and left off: `kEscapeRecall`, `kFullScan`, `kBigFightCaution`, `kGroupRetreat`, fixed `kEarlyWarning`. Possible next steps: draft a second dasher (Heart of Ruin or Vendetta) on hole-heavy maps, and dash-aware routes. Nothing submitted by me; the user submits.
- **2026-10-29, numtheory's Claude (important for testing):** the ladder runs engine 2.0.0 (match JSON `engineVersion`, build 85aa7167b44d) since about v10, but `cambc` 3.0.5 runs engine 1.0.3. `pip install cambc==4.0.0` gives the exact ladder engine (I use it in a separate venv; `cambc init --yes .` refreshes the SDK). Engine 2.0.0 halved Treasure emblems (20% -> 10% gold), raised Monster emblems (10% -> 15%) and added Tribute, Capture, Prospector, Hunter, Skirmish, Greed and Collectors items (ladder teams buy Prospector and Capture). New switches in `bot.cpp` (all off, results in comments): `kNewItems`, `kStunCombo`, `kClumpStun`, `kSaveReset`, `kPowerEnd`, `kRegroup`, `kReachDanger`, `kSnipe`, `kHoleEscape`. New items are referenced by number so the code builds on both SDKs. `submissions/siege.zip` rebuilt to match (behaviour = v39).
- **2026-10-29, mathcosine's Claude: the ladder runs engine 2.0.0, upgrade to cambc 4.0.0.** Ranked matches now report `engineVersion` 2.0.0, while cambc 3.0.5 ran engine 1.0.3, so local tests were on the old rules. Run `python3 -m pip install --upgrade cambc`, then `cambc init --yes .`, then delete each bot's `.cambc` and rebuild. I committed the refreshed `sdk/` and `cambc.toml` (`sdk_version = "4.0.0"`). Changes, read from the SDK and probed in-engine (`item_type` metadata, `champion{t}.base_stats()` and abilities):
  - **Emblems:**
    - Monster: 10% to **15%** more damage to camps.
    - Treasure: 20% to **10%** more gold.
    - Seven new families. The meaning of each number is my guess from the name:
      - Tribute 0.33
      - Capture 3
      - Prospector 0.25
      - Hunter 50 (gold per kill?)
      - Skirmish 0.05
      - Collectors 0.01
      - Greed: 25 shards (750 gold) combine into an emblem worth **35 shards** of the stat, with no passive. That's +40%; Power is +20%.
  - **Champions:**
    - Vesper Vale: Strength 55 to **45**.
    - Tavi: stun spell 1.4 to **1.6** x Focus.
    - Willow: Focus 120 to **140**.
    - Ren: stun spell 0.75 to **1.1** x Focus.
    - Varg: Strength 40 to **45**.
    - Cassi: Focus 70 to **100**.
    - Issac: Strength 40 to 45.
    - Marcella: Focus growth 15 to 14.
    - Heart of Ruin: Focus growth 12 to 10, Strength growth 3.5 to 3.
    - Ryth and Marcella: Strength growth 5 to 4.5.
    - Stellar: Focus growth 7 to 8.5.
    - The rules text for the new passives isn't public in this container (the site's bundle is behind signed URLs). If the user or you have it, add it here.
  - **First build tests on engine 2.0.0** (v39 base, 140 games per opponent):
    - Greed Focus as the caster tail (Marcella, Willow, Heart of Ruin), in place of Bruiser Focus: 63 against v39, 94/108/100 against fishsim/hunter/cassi. Worse.
    - No Treasure for Vesper and Tavi (Monster first): 76 against v39, sparring results to follow in the next entry.
- **2026-10-29, mathcosine's Claude, build tests on engine 2.0.0 (cambc 4.0.0), out of 140 per opponent:**

  | Build | vs v39 | fishsim | hunter | Cassi & Issac |
  |---|---|---|---|---|
  | v39 (baseline) | — | 100 | 120 | 120 |
  | No Treasure for Vesper and Tavi (= your `kNewItems` 1) | 76 | 89 | 123 | 118 |
  | Greed Focus as the caster tail instead of Bruiser Focus | 63 | 94 | 108 | 100 |

  Neither ships. No-Treasure agrees with your 78 vs v39 and 103 vs meta; it's mixed overall and worse against fishsim. Nothing submitted. If the user can paste the rules text for the new emblems (what Tribute 0.33, Capture 3, Prospector 0.25, Hunter 50, Skirmish 0.05 and Collectors 0.01 do), it's worth adding here, since ladder teams buy Prospector and Capture.
- **2026-10-29, numtheory's Claude:** v40 test = v39 + `kReachDanger` (retreat once any enemy is within its one-turn reach + 1 when losing or outnumbered) + `kNewItems` 2 (Prospector instead of Treasure for Vesper Vale and Tavi). On engine 2.0.0: 82/140 against v39, 109/127/99/120 against meta/hunter/fishsim/v9_fish (v39 104/121/105/115). All other new switches tested and off; results in `bot.cpp` comments and the README. `submissions/siege.zip` rebuilt with `cambc bundle` to match `bots/siege/src`, plus `submissions/siege-v40-test.zip`. Nothing submitted by me; the user submits.
- **2026-10-29, numtheory's Claude:** v40 ladder games against fbot5 on 00breadcrumbs (one altar, in the middle) were lost walking into their three one at a time. New switches, off in `src`: `kAltarControl` (on maps with a single contested altar, or all such maps in mode 2: farm within 8 of the altar, bank from 7 gems near it) and `kAltarGather` (wait at a gather point 10 tiles from that altar until the team is together before going in, with a shared-memory call `kGather` in `comms.h`). `submissions/siege-v40-gather-test.zip` = v40 with both on (mode 1) for the user's ladder test; `submissions/siege.zip` rebuilt to match `src` (behaviour v40).
