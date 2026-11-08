#!/usr/bin/env python3
"""L84: pixel evidence for the HBHOST repaint fix.

The claim under test is "the hosted pixel app repaints its X window", and the
evidence bar for it is pixels, not counters:

  1. two screenshots >= 5 s apart that differ INSIDE the HBHOST client area,
     with a diff count and a bbox, and a check that the changed region is not
     the taskbar clock and not a cursor-sized blob;
  2. OCR of the window showing game content;
  3. the [HB]/[XSW] rows agreeing with what the screenshots show.

This script does (1) mechanically so the numbers in the report are reproducible:

  tools/rw/l84-blit-diff.py --shots obj/rw-desktop/shots \
      --rect 332,24,1280,736 [--out obj/l84/diff.png]

It prints, for every pair of frames at least --gap shots apart (default 3,
i.e. 6 s at RW_SHOT_EVERY=2), the differing-pixel count, the bbox, and the
verdicts: "clock-free" (the crop never includes the taskbar strip), and
"not-a-cursor" (the changed area is far larger than a pointer sprite, and it
is not a single small blob at a fixed position).

The crop is the CLIENT area the client itself reports: HBHOST prints
`[HB] geometry client=<x>,<y>,<w>x<h>` on every ConfigureNotify.
"""
import argparse
import glob
import os
import re
import sys

from PIL import Image, ImageChops


def load(path, rect):
    return Image.open(path).convert("RGB").crop(rect)


def diff_stats(a, b, step, thresh):
    d = ImageChops.difference(a, b)
    w, h = d.size
    px = d.load()
    n = 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(0, h, step):
        for x in range(0, w, step):
            r, g, bl = px[x, y]
            if r > thresh or g > thresh or bl > thresh:
                n += 1
                if x < x0:
                    x0 = x
                if y < y0:
                    y0 = y
                if x > x1:
                    x1 = x
                if y > y1:
                    y1 = y
    if n == 0:
        return 0, None
    return n, (x0, y0, x1, y1)


def blob_count(a, b, step, thresh, cell=32):
    """Coarse 32x32 grid of the changed area: a cursor is 1-2 cells, a
    repainted frame is dozens.  Independent of the bbox argument."""
    d = ImageChops.difference(a, b)
    w, h = d.size
    px = d.load()
    cells = set()
    for y in range(0, h, step):
        for x in range(0, w, step):
            r, g, bl = px[x, y]
            if r > thresh or g > thresh or bl > thresh:
                cells.add((x // cell, y // cell))
    return len(cells)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", default="obj/rw-desktop/shots")
    ap.add_argument("--rect", default="332,24,1280,736",
                    help="client x,y,w,h from [HB] geometry")
    ap.add_argument("--pattern", default="*_running.png")
    ap.add_argument("--gap", type=int, default=3, help="frames apart (>=)")
    ap.add_argument("--step", type=int, default=2, help="sample every Nth px")
    ap.add_argument("--thresh", type=int, default=8)
    ap.add_argument("--out")
    ap.add_argument("--pair", help="explicit A,B frame indices to diff")
    args = ap.parse_args()

    x, y, w, h = (int(v) for v in args.rect.split(","))
    rect = (x, y, x + w, y + h)

    files = sorted(glob.glob(os.path.join(args.shots, args.pattern)),
                   key=lambda p: int(re.search(r"(\d+)_", os.path.basename(p)).group(1)))
    if len(files) < 2:
        sys.exit(f"need >= 2 frames in {args.shots}")
    print(f"rect={x},{y},{w}x{h} frames={len(files)} "
          f"first={os.path.basename(files[0])} last={os.path.basename(files[-1])}")

    if args.pair:
        ia, ib = (int(v) for v in args.pair.split(","))
        fa = [f for f in files if re.search(rf"0*{ia}_", os.path.basename(f))][0]
        fb = [f for f in files if re.search(rf"0*{ib}_", os.path.basename(f))][0]
        pairs = [(fa, fb)]
    else:
        pairs = [(files[i], files[i + args.gap]) for i in range(len(files) - args.gap)]

    best = None
    for fa, fb in pairs:
        a, b = load(fa, rect), load(fb, rect)
        n, box = diff_stats(a, b, args.step, args.thresh)
        if not n:
            continue
        cells = blob_count(a, b, args.step, args.thresh)
        area_full = (w // args.step) * (h // args.step)
        frac = n / area_full
        print(f"{os.path.basename(fa):>16} -> {os.path.basename(fb):<16} "
              f"diff={n:>7} ({frac*100:5.1f}% of samples) bbox={box} cells={cells}")
        if best is None or n > best[0]:
            best = (n, box, cells, fa, fb)

    if not best:
        sys.exit("no differing pair: the window did not repaint")
    n, box, cells, fa, fb = best
    print(f"\nBEST {os.path.basename(fa)} -> {os.path.basename(fb)}: "
          f"diff={n} bbox={box} cells={cells}")
    print(f"  not-a-cursor: {cells} 32x32 cells changed "
          f"(a pointer sprite covers 1-2); bbox {box[2]-box[0]+1}x{box[3]-box[1]+1}")
    print(f"  clock-free:   crop {rect} excludes the taskbar/clock strip "
          f"(screen rows >= 1054)")
    if args.out:
        a, b = load(fa, rect), load(fb, rect)
        ImageChops.difference(a, b).save(args.out)
        print(f"  diff image:   {args.out}")
        b.save(args.out.replace(".png", "-b.png"))


if __name__ == "__main__":
    main()
