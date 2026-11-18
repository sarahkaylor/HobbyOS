#!/usr/bin/env python3
"""tools/cp/hunt/boot_p9_nn.py -- tools/cp/p9/boot_p9.py, unchanged, with one
experiment (lane H1-hunt round 2, cp-hunt H1-08b): a 1920x1080 game's shots
are scaled to the 1280x720 tables with NEAREST instead of PIL's default
(bicubic) resampling.  The save list's focus frame is a 1-2 px cyan line at
1080p; bicubic blends it with the dark background below boot_p9's
r<120/g>170/b>170 test, so list_focus() finds no frame or a 1-row sliver
(h2-fq1: target entry at canon y 509 read as 475..475, LISTSEEK FAILED).
Only game_view() is replaced; everything else is boot_p9.main()."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "p9"))
from PIL import Image  # noqa: E402
import boot_p9 as B  # noqa: E402

_orig = B.game_view


def game_view(im, xscreen, game_res="1280x720"):
    if game_res != "1280x720":
        gw, gh = (int(v) for v in game_res.split("x"))
        g = im if im.size == (gw, gh) else im.crop((0, 0, gw, gh))
        return g.resize((1280, 720), Image.NEAREST)
    return _orig(im, xscreen, game_res)


B.game_view = game_view

if __name__ == "__main__":
    sys.exit(B.main())
