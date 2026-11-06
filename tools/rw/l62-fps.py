#!/usr/bin/env python3
"""tools/rw/l62-fps.py -- frames/second over a fixed window from a present timeline.

Input is the jsonl written by tools/rw/l62-xsw-follow.py (one row per [XSW]
present row: host epoch-ms, the guest's own frame counter n=, and the surface
size w=/h= the shim presented).  Output is the number the ladder quotes:

  fps       (n at t0+WINDOW - n at t0) / WINDOW, t0 = the first timestamped row
  Mpix/s    fps * w * h / 1e6 -- the fill rate the present path actually moved
  px        the presented surface, i.e. what the WM granted (not what was asked)

Usage: l62-fps.py <timeline.jsonl> [--window 120] [--label NAME] [--buckets 30]
"""
import argparse
import json
import statistics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timeline")
    ap.add_argument("--window", type=float, default=120.0)
    ap.add_argument("--bucket", type=float, default=30.0)
    ap.add_argument("--label", default="run")
    a = ap.parse_args()

    rows = []
    with open(a.timeline) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("t") is None:
                continue
            rows.append(r)
    if len(rows) < 2:
        print(f"{a.label}: no timestamped [XSW] rows ({len(rows)}) -- MISSING")
        return 0

    t0 = rows[0]["t"]
    n0 = rows[0]["n"]
    last = rows[-1]
    span = (last["t"] - t0) / 1000.0
    # The window ends at the last sample inside it; a run that ends early is
    # reported with its real dt rather than a rate over time that did not run.
    win = [r for r in rows if (r["t"] - t0) / 1000.0 <= a.window]
    tend, nend = win[-1]["t"], win[-1]["n"]
    dt = (tend - t0) / 1000.0
    fps = (nend - n0) / dt if dt > 0 else 0.0
    w, h = win[-1]["w"], win[-1]["h"]
    px = w * h

    # Inter-frame interval inside the window: a bursty loader (a few frames
    # then a long stall) has a mean that hides it, so also report p50/p90.
    gaps = [(win[i]["t"] - win[i - 1]["t"]) / 1000.0 for i in range(1, len(win))]
    gaps = [g for g in gaps if g > 0]

    print(f"== {a.label}")
    print(f"   surface        {w}x{h} ({px/1e6:.3f} Mpx)   rows={len(rows)}")
    print(f"   window         {dt:.1f}s (requested {a.window:.0f}s), "
          f"frames {n0}->{nend} = {nend-n0}")
    print(f"   fps            {fps:.3f}")
    print(f"   fill           {fps*px/1e6:.3f} Mpx/s")
    if gaps:
        print(f"   frame gap      p50={statistics.median(gaps):.2f}s "
              f"min={min(gaps):.2f}s max={max(gaps):.2f}s")
    print(f"   whole run      {last['n']-rows[0]['n']} frames in {span:.1f}s "
          f"= {(last['n']-rows[0]['n'])/span if span else 0:.3f} fps")
    # Buckets: is the rate steady, or is one phase carrying it?
    print("   buckets        " + "  ".join(
        f"{i*a.bucket:.0f}-{(i+1)*a.bucket:.0f}s"
        for i in range(max(1, int(a.window // a.bucket)))))
    out = []
    for i in range(max(1, int(a.window // a.bucket))):
        lo, hi = t0 + i * a.bucket * 1000, t0 + (i + 1) * a.bucket * 1000
        b = [r for r in win if lo <= r["t"] < hi]
        out.append(f"{(b[-1]['n']-b[0]['n'])/a.bucket:6.2f}" if len(b) > 1 else "  0.00")
    print("   bucket fps     " + " ".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
