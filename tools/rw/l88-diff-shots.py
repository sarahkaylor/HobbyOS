#!/usr/bin/env python3
"""l88-diff-shots.py -- pixel compare of the same screen from two runs.

Two runs of the same driver flow reach the same static pages; this compares
them inside the game window's client area only (never the taskbar or its
clock).  A static page must print `0 / N`, or the optimisation changed what is
on screen.

  python3 tools/rw/l88-diff-shots.py RUN_A_SHOT_DIR RUN_B_SHOT_DIR [--crop x0,y0,x1,y1]

The default crop is the client area L84/L88 both measured:
`[HB] geometry client=332,24,1280x736 screen=1920x1080`.

Pages compared (tags are the driver's own shot tags):
  adv_scenario_probe, adv_storyteller_probe, storyteller_settle,
  adv_worldopts_probe

`adv_worldopts_probe` is expected to differ: that page animates a rotating
globe preview and carries a random seed.  It is printed for contrast, not as a
failure.
"""
import argparse
import glob
import os
import sys

CROP = (332, 24, 1612, 760)
TAGS = [
    ("adv_scenario_probe", "scenario page", True),
    ("adv_storyteller_probe", "storyteller page (arrives)", True),
    ("storyteller_settle", "storyteller page (settled)", True),
    ("adv_worldopts_probe", "world options page (animated)", False),
]


def find(d, tag):
    g = sorted(glob.glob(os.path.join(d, "*" + tag + ".png")))
    return g[0] if g else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--crop", default=None,
                    help="x0,y0,x1,y1 in screen coords")
    args = ap.parse_args()
    crop = CROP
    if args.crop:
        crop = tuple(int(v) for v in args.crop.split(","))  # type: ignore
    try:
        from PIL import Image
        import numpy as np
    except ImportError as e:
        print("need pillow + numpy: %s" % e, file=sys.stderr)
        return 2

    rc = 0
    for tag, label, must_match in TAGS:
        pa, pb = find(args.a, tag), find(args.b, tag)
        if not pa or not pb:
            print("%-30s %-32s MISSING (a=%s b=%s)"
                  % (tag, label, bool(pa), bool(pb)))
            rc = 1
            continue
        a = np.asarray(Image.open(pa).convert("RGB").crop(crop)).astype(np.int16)
        b = np.asarray(Image.open(pb).convert("RGB").crop(crop)).astype(np.int16)
        if a.shape != b.shape:
            print("%-30s %-32s SHAPE %s vs %s" % (tag, label, a.shape, b.shape))
            rc = 1
            continue
        d = np.abs(a - b).max(axis=2)
        n = int((d > 0).sum())
        verdict = ""
        if must_match:
            verdict = "IDENTICAL" if n == 0 else "DIFFERS"
            if n:
                rc = 1
        else:
            verdict = "(animated page -- differs by design)"
        print("%-30s %-32s differing=%7d / %d  %s"
              % (tag, label, n, d.size, verdict))
    return rc


if __name__ == "__main__":
    sys.exit(main())
