#!/usr/bin/env python3
"""Compact per-map view of the last tools/bench.py run (both sides on one line)."""
import glob
import json
import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
rows = {}
for f in sorted(glob.glob(str(ROOT / "matches" / "bench" / "*.json"))):
    m, side = os.path.basename(f)[:-5].rsplit("-", 1)
    d = json.load(open(f))
    me = "teamA" if side == "A" else "teamB"
    gems = {t["team"]: int(t["submittedGems"]) for t in d["teams"]}
    ids = {r["id"] for r in d["roster"]["champions"] if r["team"] == me}
    deaths = sum(int(c["metrics"]["deaths"]) for c in d["champions"] if c["id"] in ids)
    rows.setdefault(m, []).append(
        (side, d["match"]["outcome"] == me, gems[me], gems["teamB" if me == "teamA" else "teamA"], d["match"]["roundsCompleted"], deaths))
for m, v in rows.items():
    print(f"{m:24s}", "  ".join(f"{s}:{'W' if w else 'L'} {a:4d}-{b:<4d} r{r:4d} d{dd:2d}" for s, w, a, b, r, dd in v))
