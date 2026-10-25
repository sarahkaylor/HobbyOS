#!/usr/bin/env python3
"""gb-034-v pixel stats: distinct colours + non-black coverage per region.

Usage: ppmstats.py <ppm> [--region centre|full]
P6 only (QMP screendumps).  Prints distinct RGB count, non-black counts for the
full frame and a centre region (the ticket asks for the region to be stated).
"""
import sys
from collections import Counter


def read_ppm(p):
    with open(p, "rb") as f:
        data = f.read()
    # header: P6\n<w> <h>\n<max>\n
    parts = []
    idx = 0
    while len(parts) < 4:
        # skip whitespace/comments
        while data[idx:idx+1].isspace():
            idx += 1
        if data[idx:idx+1] == b'#':
            while data[idx:idx+1] not in (b'\n', b''):
                idx += 1
            continue
        s = idx
        while not data[idx:idx+1].isspace():
            idx += 1
        parts.append(data[s:idx])
    idx += 1
    w, h = int(parts[1]), int(parts[2])
    px = data[idx:idx + w * h * 3]
    return w, h, px


def stats(w, h, px, x0, y0, x1, y1):
    colours = Counter()
    nonzero = 0
    total = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            o = row + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            colours[(r, g, b)] += 1
            if r or g or b:
                nonzero += 1
            total += 1
    top = colours.most_common(3)
    return len(colours), nonzero, total, top


for path in sys.argv[1:]:
    w, h, px = read_ppm(path)
    print(f"== {path}  ({w}x{h})")
    for name, (x0, y0, x1, y1) in {
        "full": (0, 0, w, h),
        "centre(480,270)-(1440,810)": (w // 4, h // 4, 3 * w // 4, 3 * h // 4),
    }.items():
        nc, nz, tot, top = stats(w, h, px, x0, y0, x1, y1)
        pct = 100.0 * nz / tot if tot else 0
        print(f"  {name:28s} colours={nc:6d} nonzero={nz:8d}/{tot} ({pct:5.1f}%) top3={top}")
