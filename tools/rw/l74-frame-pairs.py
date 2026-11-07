#!/usr/bin/env python3
"""tools/rw/l74-frame-pairs.py -- pick the same-phase pair of frames.

Two runs of the same binary on two rasterisers (virgl, softpipe) at the same
requested size, with the same screenshot cadence, do not present the same
*number* of frames: the software rasteriser is slower, so shot 04 of one run
is at a different point in the load than shot 04 of the other.  "Same phase"
therefore has to be *found*, not assumed.

This runs m3-frame-compare.py over every (virgl, softpipe) shot pair, parses
its 32x24 luminance-cell agreement and its bright-region boxes, and prints the
pairs ranked by agreement.  The best pair is the same-phase pair; if the best
agreement is low, the two runs never showed the same screen and the honest
answer is that no comparison can be made.

Usage:
  l74-frame-pairs.py <virgl-dir> <softpipe-dir> [--rect x,y,w,h] [--top 6]
"""
import argparse
import itertools
import os
import re
import subprocess
import sys

CMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "m3-frame-compare.py")
AGREE = re.compile(r"layout: (\d+)/(\d+) cells .*\(([\d.]+)% agreement\)")
BOXES = re.compile(r"bright regions: A=(\d+) B=(\d+) \(top: \(([-\d, ]+)\) vs \(([-\d, ]+)\)\)")
COLS = re.compile(r"colours=(\d+) nonblack=([\d.]+)%")


def shots(d):
    return sorted(f for f in os.listdir(d)
                  if f.endswith(".png") and not f.startswith("00_"))


def compare(a, b, rect):
    cmd = [sys.executable, CMP, a, b]
    if rect:
        cmd += ["--rect", rect]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout
    m = AGREE.search(out)
    bx = BOXES.search(out)
    ca = COLS.findall(out)
    return {
        "agree": float(m.group(3)) if m else 0.0,
        "boxes": (int(bx.group(1)), int(bx.group(2))) if bx else (0, 0),
        "top": (bx.group(3), bx.group(4)) if bx else ("", ""),
        "colours": [int(c[0]) for c in ca],
        "nonblack": [float(c[1]) for c in ca],
        "verdict": "STRUCTURALLY THE SAME FRAME" in out,
        "rc": r.returncode,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("virgl")
    ap.add_argument("softpipe")
    ap.add_argument("--rect", default="2,34,1280,720")
    ap.add_argument("--top", type=int, default=6)
    a = ap.parse_args()
    va, sb = shots(a.virgl), shots(a.softpipe)
    if not va or not sb:
        print(f"no shots: {a.virgl}={va} {a.softpipe}={sb}")
        return 1
    rows = []
    for x, y in itertools.product(va, sb):
        r = compare(os.path.join(a.virgl, x), os.path.join(a.softpipe, y), a.rect)
        rows.append((r["agree"], x, y, r))
    rows.sort(key=lambda t: -t[0])
    print(f"{'virgl':<16} {'softpipe':<16} {'agree':>7}  boxes  colours(v/s)  same?")
    for agree, x, y, r in rows[: a.top]:
        print(f"{x:<16} {y:<16} {agree:6.1f}%  {r['boxes'][0]}/{r['boxes'][1]}"
              f"  {r['colours']}  {r['verdict']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
