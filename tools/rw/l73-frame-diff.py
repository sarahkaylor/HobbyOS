#!/usr/bin/env python3
"""l73-frame-diff.py — is the screen CHANGING, and where?

A still screenshot proves a window exists; it does not prove the client is
drawing.  Two shots of the same phase do: a changed CRC plus a diff bounding
box that is not the desktop's clock face.

Reported per pair:
  DIFF   pixels differing above --thr, and the bounding box of the change
  CLOCK  the fraction of the change that lies inside the panel clock region
         (bottom-right, --clock x0,y0), so a "changed" frame that is only the
         clock is named as such
  OCR    tesseract text of the second frame (what the screen says)

Exit 0 when the pair differs by more than --min-frac outside the clock region,
1 otherwise (a static frame, or a change that is only the clock).

Usage:
  l73-frame-diff.py A.png B.png [--thr 8] [--clock 1780,1030] [--min-frac 0.002]
  l73-frame-diff.py A.png --ocr-only
"""
import sys

from PIL import Image, ImageChops


def load(p):
    return Image.open(p).convert("RGB")


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    thr, clock, min_frac = 8, (1780, 1030), 0.002
    args = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--thr":
            thr = int(argv[i + 1]); i += 2
        elif a == "--clock":
            clock = tuple(int(v) for v in argv[i + 1].split(",")); i += 2
        elif a == "--min-frac":
            min_frac = float(argv[i + 1]); i += 2
        elif a == "--ocr-only":
            args.append(a); i += 1
        else:
            args.append(a); i += 1

    if args and args[0] == "--ocr-only":
        import subprocess
        out = subprocess.run(["tesseract", args[1], "stdout"],
                             capture_output=True, text=True).stdout
        print("OCR:", " ".join(out.split())[:400] or "(no text)")
        return

    a, b = load(args[0]), load(args[1])
    if a.size != b.size:
        print(f"SIZE differs: {a.size} vs {b.size}")
    diff = ImageChops.difference(a, b).convert("L")
    px = diff.load()
    w, h = diff.size
    n = 0
    x0, y0, x1, y1 = w, h, -1, -1
    in_clock = 0
    cx, cy = clock
    for y in range(h):
        for x in range(w):
            if px[x, y] > thr:
                n += 1
                if x < x0: x0 = x
                if y < y0: y0 = y
                if x > x1: x1 = x
                if y > y1: y1 = y
                if x >= cx and y >= cy:
                    in_clock += 1
    total = w * h
    if n == 0:
        print(f"DIFF none above thr={thr} ({w}x{h})")
        return 1
    outside = n - in_clock
    print(f"DIFF pixels={n} ({100.0*n/total:.3f}% of {w}x{h}) thr={thr}")
    print(f"DIFF bbox=({x0},{y0})-({x1},{y1}) size={x1-x0+1}x{y1-y0+1}")
    print(f"CLOCK region x>={cx} y>={cy}: {in_clock} px "
          f"({100.0*in_clock/n:.1f}% of the change)")
    print(f"OUTSIDE-CLOCK fraction={outside/total:.5f} "
          f"(threshold {min_frac})")
    import subprocess
    out = subprocess.run(["tesseract", args[1], "stdout"],
                         capture_output=True, text=True).stdout
    print("OCR:", " ".join(out.split())[:400] or "(no text)")
    ok = outside / total >= min_frac
    print("VERDICT:", "CHANGING (not the clock)" if ok else "static or clock-only")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
