#!/usr/bin/env python3
"""Play one bot against another on every map, from both sides, and summarise.

Usage: python3 tools/bench.py BOT OPPONENT [--maps a,b,...] [--jobs N] [--rounds N]

Runs in release mode (as on the platform) through `cambc run-batch`, writes
replays and JSON summaries under matches/bench/, and prints one line per map
plus totals: wins, losses, average gems and average rounds of the matches won.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bot")
    parser.add_argument("opponent")
    parser.add_argument("--maps", default="")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--rounds", type=int, default=0)
    parser.add_argument("--out", default=str(ROOT / "matches" / "bench"))
    args = parser.parse_args()

    maps = sorted(p.stem for p in (ROOT / "maps").glob("*.map26a"))
    if args.maps:
        wanted = args.maps.split(",")
        maps = [m for m in maps if any(m == w or m.endswith(w) for w in wanted)]
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*"):
        old.unlink()

    bot = ROOT / "bots" / args.bot
    opp = ROOT / "bots" / args.opponent
    rows = []
    for m in maps:
        mp = ROOT / "maps" / f"{m}.map26a"
        for side in ("A", "B"):
            a, b = (bot, opp) if side == "A" else (opp, bot)
            stem = f"{m}-{side}"
            rows.append((m, side, f"{a} {b} {mp} {out / (stem + '.repl26a')} --statistics {out / (stem + '.json')}"))
    manifest = out / "manifest.txt"
    manifest.write_text("\n".join(r[2] for r in rows) + "\n")

    cmd = ["cambc", "run-batch", str(manifest), "--release", "--json", "--jobs", str(args.jobs), "--color", "never"]
    if args.rounds:
        cmd += ["--rounds", str(args.rounds)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout[-3000:], proc.stderr[-3000:], file=sys.stderr)

    wins = losses = 0
    won_rounds = []
    my_gems = []
    their_gems = []
    my_deaths = 0
    exhausted = 0
    lines = []
    for m, side, _ in rows:
        path = out / f"{m}-{side}.json"
        if not path.exists():
            lines.append(f"{m:24s} {side}  MISSING")
            losses += 1
            continue
        d = json.loads(path.read_text())
        me_team = "teamA" if side == "A" else "teamB"
        gems = {t["team"]: int(t["submittedGems"]) for t in d["teams"]}
        outcome = d["match"]["outcome"]
        rounds = d["match"]["roundsCompleted"]
        deaths = sum(int(c["metrics"]["deaths"]) for c in d["champions"] if c.get("team", "") == me_team)
        if not any("team" in c for c in d["champions"]):
            ids = {r["id"] for r in d["roster"]["champions"] if r["team"] == me_team}
            deaths = sum(int(c["metrics"]["deaths"]) for c in d["champions"] if c["id"] in ids)
        my_deaths += deaths
        fuel = sum(len(e["entries"]) for e in d["events"] if e["kind"] == "fuelExhausted" and e["owner"].get("team") == me_team)
        exhausted += fuel
        mine, theirs = gems.get(me_team, 0), gems.get("teamB" if me_team == "teamA" else "teamA", 0)
        my_gems.append(mine)
        their_gems.append(theirs)
        won = outcome == me_team
        wins += won
        losses += not won
        if won:
            won_rounds.append(rounds)
        lines.append(f"{m:24s} {side}  {'WIN ' if won else 'LOSS'} gems {mine:5d} vs {theirs:5d}  rounds {rounds:5d}  deaths {deaths:3d}  out-of-fuel {fuel}")
    print("\n".join(lines))
    n = len(rows)
    print(f"\n{args.bot} vs {args.opponent}: {wins}/{n} wins, {losses} losses")
    if my_gems:
        print(f"avg gems {sum(my_gems) / len(my_gems):.0f} vs {sum(their_gems) / len(their_gems):.0f}; deaths {my_deaths}; out-of-fuel turns {exhausted}")
    if won_rounds:
        print(f"avg rounds in wins {sum(won_rounds) / len(won_rounds):.0f}")


if __name__ == "__main__":
    main()
