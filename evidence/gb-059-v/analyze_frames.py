#!/usr/bin/env python3
"""Frame-difference analysis: where does the screen change between samples?

A screendump digest that differs proves *something* changed, not that a game is
rendering: the bare desktop changes too (taskbar clock).  So the control run
measured 3/3 distinct digests with nothing launched, which kills the naive
"distinct digest == live frame" test.

This script localises the change instead: per frame pair it reports the number
of differing pixels and their bounding box, both for the whole screen and for
the client area (everything above the bottom taskbar strip).  A game frame
animating in its window changes thousands of pixels in the client area; the
clock changes a few dozen inside the taskbar.

usage: analyze_frames.py <dir> [--taskbar 60]
"""
import argparse, glob, os, sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    magic, dims, maxv, px = parts[0], parts[1], parts[2], parts[3]
    w, h = (int(x) for x in dims.split())
    assert magic == b"P6", magic
    return w, h, px


def diff(a, b, w, h, y0, y1, x0=0, x1=None):
    x1 = w if x1 is None else x1
    n = 0
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1):
            i = (row + x) * 3
            if a[i:i + 3] != b[i:i + 3]:
                n += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    bbox = None if n == 0 else (minx, miny, maxx, maxy)
    return n, bbox


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--taskbar", type=int, default=60, help="bottom strip excluded from client area")
    a = ap.parse_args()
    frames = sorted(glob.glob(os.path.join(a.dir, "frame*.ppm")))
    if len(frames) < 2:
        sys.exit(f"need >=2 frames in {a.dir}")
    w, h, first = read_ppm(frames[0])
    print(f"{a.dir}: {len(frames)} frames {w}x{h}; taskbar strip = bottom {a.taskbar}px; "
          f"client area = rows 0..{h - a.taskbar - 1}")
    imgs = [read_ppm(f)[2] for f in frames]
    for i in range(1, len(imgs)):
        cn, cbox = diff(imgs[0], imgs[i], w, h, 0, h - a.taskbar)
        tn, tbox = diff(imgs[0], imgs[i], w, h, h - a.taskbar, h)
        print(f"  frame0 vs frame{i}: client_changed={cn:6d}px bbox={cbox}   "
              f"taskbar_changed={tn:5d}px bbox={tbox}")


if __name__ == "__main__":
    main()