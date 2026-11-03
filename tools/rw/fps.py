#!/usr/bin/env python3
"""tools/rw/fps.py — GALLIUM_HUD_DUMP_DIR -> fps statistics.

GALLIUM_HUD_DUMP_DIR writes one plain-text file per HUD variable, one value per
line, no timestamp (measured: a single file named `fps`, 39 lines for a 25 s
run).  This turns that into a table: n, min, p50, p90, max, and a steady-state
subset that drops the first 25% of samples (Unity's startup / level-load
transient shows up as 16 fps and a single sub-1 fps stall).

Usage: fps.py <hud-dir> [--json]
Exit 0 always: a missing dump is reported as `MISSING`, not as a crash.
"""
import json
import os
import statistics
import sys


def load(d):
    out = {}
    if not os.path.isdir(d):
        return out
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if not os.path.isfile(p):
            continue
        vals = []
        with open(p, "r", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    vals.append(float(line.split(",")[-1].split()[0]))
                except ValueError:
                    pass
        if vals:
            out[name] = vals
    return out


def stats(v):
    if not v:
        return None
    s = sorted(v)
    return {
        "n": len(v),
        "min": round(min(v), 2),
        "p50": round(statistics.median(v), 2),
        "p90": round(s[min(len(s) - 1, int(0.9 * len(s)))], 2),
        "max": round(max(v), 2),
        "mean": round(statistics.fmean(v), 2),
    }


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "out/hud"
    data = load(d)
    if not data:
        print(f"fps.py: no HUD dump under {d} (MISSING)")
        return 0
    rows = {}
    for name, vals in data.items():
        rows[name] = stats(vals) or {}
        steady = vals[len(vals) // 4:]
        st = stats(steady) or {}
        rows[name]["steady_p50"] = st.get("p50")
        tail = vals[-40:]
        tt = stats(tail) or {}
        rows[name]["tail_p50"] = tt.get("p50")
        rows[name]["tail_min"] = tt.get("min")
        rows[name]["tail_max"] = tt.get("max")
    if "--json" in sys.argv:
        print(json.dumps(rows, indent=2, sort_keys=True))
        return 0
    hdr = (f"{'variable':<10} {'n':>4} {'min':>7} {'p50':>7} {'p90':>7} {'max':>7} "
           f"{'mean':>7} {'tail_p50':>9} {'tail_min':>9}")
    print(hdr)
    print("-" * len(hdr))
    for name, r in rows.items():
        print(f"{name:<10} {r['n']:>4} {r['min']:>7} {r['p50']:>7} {r['p90']:>7} "
              f"{r['max']:>7} {r['mean']:>7} {str(r['tail_p50']):>9} {str(r['tail_min']):>9}")
    print("\n(tail_* = last 40 samples, i.e. after the Steam modal is dismissed "
          "and the main menu has settled)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
