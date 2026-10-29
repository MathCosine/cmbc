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

