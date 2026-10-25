#!/usr/bin/env python3
"""Pixel-diff two PPM screendumps: changed-pixel count over the whole frame and
over the game's centre region.  Used to decide "did this keypress change the
frame" without eyeballing.
"""
import sys


def read_ppm(p):
    data = open(p, "rb").read()
    parts, idx = [], 0
    while len(parts) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b'#':
            while data[idx:idx + 1] not in (b'\n', b''):
                idx += 1
            continue
        s = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        parts.append(data[s:idx])
    idx += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[idx:idx + w * h * 3]


a, b = sys.argv[1], sys.argv[2]
w, h, pa = read_ppm(a)
w2, h2, pb = read_ppm(b)
assert (w, h) == (w2, h2), "size mismatch"
diff = 0
maxd = 0
for i in range(0, w * h * 3, 3):
    d = abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1]) + abs(pa[i + 2] - pb[i + 2])
    if d:
        diff += 1
        if d > maxd:
            maxd = d
total = w * h
print(f"{a}\n  vs {b}\n  changed pixels: {diff}/{total} ({100.0 * diff / total:.3f}%)  max channel-sum delta: {maxd}")
