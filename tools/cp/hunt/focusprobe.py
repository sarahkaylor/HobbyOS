#!/usr/bin/env python3
"""tools/cp/hunt/focusprobe.py SHOT.png [SHOT.png ...] [--menu main|pause]
        [--xscreen WxH] [--game-res WxH] [--diff A.png B.png]

The highlighted row of a main / pause menu in a P9 screendump, read with
boot_p9.py's own focus_in() / canon_image() (so it sees what `seek` sees),
without a tap and without ending the boot (a `seek` that finds nothing
returns a verdict; lane H1-hunt round 2, cp-hunt H1-12).  Prints numbers
only (the shots are game content and stay on strix).

--diff A B: where two shots differ (bounding box of changed pixels and its
size): finds the mouse cursor after a relative move, or a focus change.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p9"))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p8"))
from PIL import Image, ImageChops  # noqa: E402
import boot_p9 as B  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shots", nargs="*")
    ap.add_argument("--menu", default="main", choices=list(B.MENUS))
    ap.add_argument("--xscreen", default="1280x720")
    ap.add_argument("--game-res", default="1280x720")
    ap.add_argument("--diff", nargs=2)
    a = ap.parse_args()
    for p in a.shots:
        im = Image.open(p).convert("RGB")
        label, yc, rows = B.focus_in(B.canon_image(im, a.xscreen, a.game_res), a.menu)
        print(f"focusprobe {os.path.basename(p)} {im.size[0]}x{im.size[1]}: "
              + (f"rows {rows[0]}..{rows[1]} centre {yc:.0f} -> {label}" if rows else "no highlighted row"))
    if a.diff:
        x, y = (Image.open(p).convert("RGB") for p in a.diff)
        d = ImageChops.difference(x, y).convert("L").point(lambda v: 255 if v > 40 else 0)
        bb = d.getbbox()
        n = sum(1 for v in d.getdata() if v)
        print(f"focusprobe diff: bbox {bb} changed_px {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
