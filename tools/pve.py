#!/usr/bin/env python3
"""Measure how fast a bot farms with nobody contesting it.

Usage: python3 tools/pve.py BOT [--maps a,b,...] [--jobs N] [--compare OTHER]

Plays BOT against bots/idle (which drafts and never moves) on every map from both
sides, in release mode, and prints per map (averaged over the two sides): rounds to
tribute 1,000 gems (2000 if never),
tributed gems at rounds 300/500/1000, gold earned and camps cleared by round 500.
The games are deterministic, so small changes to farming show up directly instead
of being drowned in the noise of real matches. With --compare, OTHER is run too
and the two are printed side by side.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MARKS = (300, 500, 1000)


def at(runs, r):
    """Value of a run-length timeline at round r."""
    cur, v = 0, 0.0
    for run in runs:
        if cur > r:
            break
        v = float(run["value"])
        cur += run["duration"]
    return v


def play(bot, maps, jobs, out):
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*"):
        old.unlink()
    idle = ROOT / "bots" / "idle"
    lines = []
    for m in maps:
        mp = ROOT / "maps" / f"{m}.map26a"
        lines.append(f"{ROOT / 'bots' / bot} {idle} {mp} {out / (m + '-A.repl26a')} --statistics {out / (m + '-A.json')}")
        lines.append(f"{idle} {ROOT / 'bots' / bot} {mp} {out / (m + '-B.repl26a')} --statistics {out / (m + '-B.json')}")
    manifest = out / "manifest.txt"
    manifest.write_text("\n".join(lines) + "\n")
    cmd = ["cambc", "run-batch", str(manifest), "--release", "--json", "--jobs", str(jobs), "--color", "never"]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout[-2000:], proc.stderr[-2000:], file=sys.stderr)
    rows = {}
    for m in maps:
        sides = []
        for side, team in (("A", "teamA"), ("B", "teamB")):
            path = out / f"{m}-{side}.json"
            if not path.exists():
                continue
            d = json.loads(path.read_text())
            gems = {t["team"]: int(t["submittedGems"]) for t in d["teams"]}
            finish = d["match"]["roundsCompleted"] if gems.get(team, 0) >= 1000 else 2000
            sub = [sum(at(t["runs"], r) for t in d["teamTimelines"] if t["metric"] == "submittedGems" and t["owner"] == team) for r in MARKS]
            gold = sum(at(t["runs"], 500) for t in d["championTimelines"] if t["metric"] == "earnedGold" and t["owner"].get("team") == team)
            clears = sum(1 for e in d["events"] if e["kind"] == "campClear" and e["owner"].get("team") == team for en in e["entries"] if en["timestamp"]["round"] <= 500)
            deaths = sum(int(c["metrics"]["deaths"]) for c in d["champions"] if c.get("team") == team)
            sides.append((finish, *sub, gold, clears, deaths))
        if sides:
            rows[m] = tuple(sum(v[i] for v in sides) / len(sides) for i in range(7))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bot")
    parser.add_argument("--maps", default="")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--compare", default="")
    args = parser.parse_args()
    maps = sorted(p.stem for p in (ROOT / "maps").glob("*.map26a"))
    if args.maps:
        wanted = args.maps.split(",")
        maps = [m for m in maps if any(m == w or m.endswith(w) for w in wanted)]
    bots = [args.bot] + ([args.compare] if args.compare else [])
    results = [play(b, maps, args.jobs, ROOT / "matches" / f"pve-{b}") for b in bots]
    head = "finish  g300  g500 g1000 gold500 clr500 dead"
    print(f"{'map':24s} " + " | ".join(f"{b[:40]:^44s}" for b in bots))
    print(f"{'':24s} " + " | ".join(head for _ in bots))
    for m in maps:
        cells = []
        for r in results:
            v = r.get(m)
            cells.append(f"{v[0]:6.0f} {v[1]:5.0f} {v[2]:5.0f} {v[3]:5.0f} {v[4]:7.0f} {v[5]:6.1f} {v[6]:4.1f}" if v else f"{'missing':44s}")
        print(f"{m:24s} " + " | ".join(cells))
    for b, r in zip(bots, results):
        n = max(len(r), 1)
        avg = [sum(v[i] for v in r.values()) / n for i in range(7)]
        print(f"{b}: avg finish {avg[0]:.0f} | gems r300 {avg[1]:.0f} r500 {avg[2]:.0f} r1000 {avg[3]:.0f} | gold r500 {avg[4]:.0f} | clears r500 {avg[5]:.1f} | deaths {avg[6]:.1f} | maps {len(r)}")


if __name__ == "__main__":
    main()
