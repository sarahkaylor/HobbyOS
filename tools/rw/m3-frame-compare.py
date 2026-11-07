#!/usr/bin/env python3
"""tools/rw/m3-frame-compare.py -- is this frame the game? (M3 evidence)

A virgl frame cannot be pixel-exact against a softpipe frame: two rasterisers,
two rounding paths for the same geometry.  What *can* be compared is structure
and content, and that is what this tool reports for two screenshots of the same
phase (same scene state, different driver):

  colour     distinct RGB triples, and the fraction of pixels that are not
             black -- a driver that draws nothing is 1 colour / 0 % non-black
  layout     a 32x24 grid of cell mean luminance, compared cell by cell
             (agreement = cells within a tolerance), plus the bounding box of
             every "bright" connected region (luma >= --bright) and whether
             the boxes match within --tol pixels
  text       tesseract OCR of both images (and the text both share)

Usage:
  m3-frame-compare.py A.png B.png [--rect x,y,w,h] [--tol 8] [--bright 200]
  m3-frame-compare.py A.png --ocr-only

Exit: 0 if the two frames agree structurally (>= --min-cells agreement and the
bright region count within 1), 1 otherwise.  On a single image it just prints
the rows and exits 0.
"""
import argparse
import subprocess
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    print("need Pillow (python3 -m pip install pillow)", file=sys.stderr)
    sys.exit(2)

CELLS_X, CELLS_Y = 32, 24
try:                      # Pillow >= 9.1 moves the filters under Resampling
    _BOX = Image.Resampling.BOX
except AttributeError:    # pragma: no cover
    _BOX = Image.BOX


def load(path, rect=None):
    im = Image.open(path).convert("RGB")
    if rect:
        x, y, w, h = rect
        im = im.crop((x, y, x + w, y + h))
    return im


def colour_stats(im):
    px = im.load()
    w, h = im.size
    colours = set()
    nonblack = 0
    total = 0
    # A stride keeps this fast on 2 Mpx screenshots without changing the shape
    # of the answer (colours are quantised by the actual palette).
    step = max(1, min(w, h) // 400)
    for y in range(0, h, step):
        for x in range(0, w, step):
            r, g, b = px[x, y]
            colours.add((r, g, b))
            if r | g | b:
                nonblack += 1
            total += 1
    return len(colours), nonblack / max(1, total), total


def cell_luma(im):
    small = im.resize((CELLS_X, CELLS_Y), _BOX)
    px = small.load()
    return [[0.299 * px[x, y][0] + 0.587 * px[x, y][1] + 0.114 * px[x, y][2]
             for x in range(CELLS_X)] for y in range(CELLS_Y)]


def bright_boxes(im, thresh):
    """Bounding boxes of 8-connected bright regions, coarse (one cell grid)."""
    lum = cell_luma(im)
    seen = [[False] * CELLS_X for _ in range(CELLS_Y)]
    boxes = []
    for y in range(CELLS_Y):
        for x in range(CELLS_X):
            if seen[y][x] or lum[y][x] < thresh:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            x0 = x1 = x
            y0 = y1 = y
            n = 0
            while stack:
                cx, cy = stack.pop()
                n += 1
                x0, x1 = min(x0, cx), max(x1, cx)
                y0, y1 = min(y0, cy), max(y1, cy)
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < CELLS_X and 0 <= ny < CELLS_Y and \
                                not seen[ny][nx] and lum[ny][nx] >= thresh:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
            if n >= 2:
                boxes.append((x0, y0, x1, y1, n))
    boxes.sort(key=lambda b: -b[4])
    return boxes[:12]


def ocr(path, rect=None):
    cmd = ["tesseract", path, "-", "--psm", "11"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    text = r.stdout
    if rect:
        return text
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--rect", default="")
    ap.add_argument("--tol", type=int, default=8)
    ap.add_argument("--bright", type=float, default=200.0)
    ap.add_argument("--min-cells", type=float, default=0.90)
    ap.add_argument("--ocr-only", action="store_true")
    a = ap.parse_args()

    rect = None
    if a.rect:
        rect = tuple(int(v) for v in a.rect.split(","))
        if len(rect) != 4:
            print("--rect wants x,y,w,h", file=sys.stderr)
            return 2

    ia = load(a.a, rect)
    nca, nba, _ = colour_stats(ia)
    print(f"[CMP] {a.a}: {ia.size[0]}x{ia.size[1]} colours={nca} "
          f"nonblack={100.0 * nba:.1f}%")
    ta = ocr(a.a, rect)
    la = [l.strip() for l in ta.splitlines() if l.strip()]
    if la:
        print(f"[CMP] {a.a} ocr({len(la)} lines): " + " | ".join(la[:8]))

    if not a.b or a.ocr_only:
        return 0

    ib = load(a.b, rect)
    ncb, nbb, _ = colour_stats(ib)
    print(f"[CMP] {a.b}: {ib.size[0]}x{ib.size[1]} colours={ncb} "
          f"nonblack={100.0 * nbb:.1f}%")
    tb = ocr(a.b, rect)
    lb = [l.strip() for l in tb.splitlines() if l.strip()]
    if lb:
        print(f"[CMP] {a.b} ocr({len(lb)} lines): " + " | ".join(lb[:8]))

    la_l, lb_l = cell_luma(ia), cell_luma(ib)
    agree = 0
    total = CELLS_X * CELLS_Y
    for y in range(CELLS_Y):
        for x in range(CELLS_X):
            if abs(la_l[y][x] - lb_l[y][x]) <= a.tol:
                agree += 1
    frac = agree / total
    print(f"[CMP] layout: {agree}/{total} cells within {a.tol} luma "
          f"({100.0 * frac:.1f}% agreement)")

    ba = bright_boxes(ia, a.bright)
    bb = bright_boxes(ib, a.bright)
    print(f"[CMP] bright regions: A={len(ba)} B={len(bb)} (top: "
          f"{ba[0][:4] if ba else '-'} vs {bb[0][:4] if bb else '-'})")
    matched = 0
    for x0, y0, x1, y1, _ in ba:
        for X0, Y0, X1, Y1, _ in bb:
            if abs(x0 - X0) <= a.tol and abs(y0 - Y0) <= a.tol and \
                    abs(x1 - X1) <= a.tol and abs(y1 - Y1) <= a.tol:
                matched += 1
                break
    print(f"[CMP] bright regions matched within {a.tol} cells: "
          f"{matched}/{max(1, len(ba))}")

    words_a = set(w.lower().strip(".,:;!?") for w in ta.split())
    words_b = set(w.lower().strip(".,:;!?") for w in tb.split())
    shared = sorted(w for w in (words_a & words_b) if len(w) > 2)
    print(f"[CMP] shared words: {len(shared)} " + (" ".join(shared[:12])))

    ok = frac >= a.min_cells and abs(len(ba) - len(bb)) <= 1 and \
        (not words_a or bool(shared))
    print(f"[CMP] verdict: {'STRUCTURALLY THE SAME FRAME' if ok else 'DIFFERENT'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
