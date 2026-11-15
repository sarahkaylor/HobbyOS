#!/usr/bin/env python3
"""cp/p0guest: MangoHud CSV -> fps median/p90 (as frame-time percentiles).

    mango-stats.py <log.csv> [skip_seconds]

Rows whose elapsed time (from MangoHud's log start) is below skip_seconds are
dropped. "fps p90" here is the 90th-percentile-slow frame: 1000 / p90(frametime).
"""
import csv
import statistics
import sys


def main():
    path = sys.argv[1]
    skip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    with open(path, newline="") as fh:
        lines = fh.read().splitlines()
    # MangoHud writes a system-info header block, then the per-frame header.
    start = next(i for i, l in enumerate(lines) if l.startswith("fps,"))
    rows = list(csv.DictReader(lines[start:]))
    ft = []
    for r in rows:
        el = float(r.get("elapsed", 0)) / 1e9  # ns since logging started
        if el >= skip and r.get("frametime"):
            ft.append(float(r["frametime"]))  # ms
    if not ft:
        print(f"mango file={path} frames=0 (skip={skip}s)")
        return
    ft.sort()
    med = statistics.median(ft)
    p90 = ft[int(len(ft) * 0.9)]
    p99 = ft[int(len(ft) * 0.99)]
    span = sum(ft) / 1000
    print(f"mango file={path} skip_s={skip:.1f} frames={len(ft)} span_s={span:.1f} avg_fps={len(ft)/span:.1f} "
          f"median_ms={med:.2f} fps_median={1000/med:.1f} p90_ms={p90:.2f} fps_p90={1000/p90:.1f} p99_ms={p99:.2f}")


if __name__ == "__main__":
    main()
