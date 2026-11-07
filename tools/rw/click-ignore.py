#!/usr/bin/env python3
"""tools/rw/click-ignore.py — find the "Ignore" button of RimWorld's Steam modal.

RimWorld stops at start-up on a modal "Could not initialize Steam API" box with
two full-width buttons, Ignore (left) and Quit (right).  Enter does *not*
dismiss it (measured: `xdotool key Return` left the box up for a whole 50 s
run), so the harness has to click.  The buttons are a flat brown (106,81,46);
finding them by colour keeps the click valid if the dialog moves or the window
size changes — no hard-coded coordinates in the harness.

Usage: click-ignore.py <screenshot.png>
Prints "<x> <y>" (click target, left button) and exits 0, or exits 1 if the
dialog is not on screen.
"""
import sys
from PIL import Image

BROWN = (106, 81, 46)
MIN_RUN = 220      # modal buttons are ~280 px wide at 1280x720;
                   # the main menu's stacked buttons are ~160 px, so
                   # this threshold is what keeps the *verification*
                   # from mistaking the menu for the dialog
BAND_ROWS = 18     # rows in which the brown must be dense


def main():
    if len(sys.argv) < 2:
        print("usage: click-ignore.py <screenshot.png>", file=sys.stderr)
        return 2
    im = Image.open(sys.argv[1]).convert("RGB")
    W, H = im.size
    px = im.load()
    band = {}
    for y in range(H // 2, H):
        xs = [x for x in range(W) if px[x, y] == BROWN]
        if len(xs) > 2 * MIN_RUN:
            band[y] = xs
    if not band:
        return 1
    ys = sorted(band)
    # longest contiguous row group
    groups, cur = [], [ys[0]]
    for y in ys[1:]:
        if y - cur[-1] <= 2:
            cur.append(y)
        else:
            groups.append(cur)
            cur = [y]
    groups.append(cur)
    g = max(groups, key=len)
    if len(g) < BAND_ROWS // 2:
        return 1
    cols = [0] * W
    for y in g:
        for x in band[y]:
            cols[x] += 1
    runs, start = [], None
    for x in range(W):
        if cols[x] > len(g) // 3 and start is None:
            start = x
        elif cols[x] <= len(g) // 3 and start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, W - 1))
    runs = [r for r in runs if r[1] - r[0] >= MIN_RUN]
    if len(runs) < 2:
        return 1  # menu buttons stack vertically: one run per band
    left = runs[0]
    print(f"{(left[0] + left[1]) // 2} {(g[0] + g[-1]) // 2}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
