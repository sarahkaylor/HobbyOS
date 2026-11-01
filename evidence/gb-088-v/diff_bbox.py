#!/usr/bin/env python3
"""diff_bbox.py — full-resolution before/after PPM diff for GB-088-V.

The harness's own diff() samples every 3rd row and every 2nd pixel; this compares
EVERY pixel and reports the count plus the bounding box, split above/below the
taskbar (y = H-26), which is what the card asks for.

Usage: python3 diff_bbox.py before.ppm after.ppm
Output:
  above_taskbar <count> bbox=(x0,y0)-(x1,y1)
  below_taskbar <count> [bbox=...]
"""
import sys


def parse_ppm(path):
    raw = open(path, "rb").read()
    vals, i = [], 0
    while len(vals) < 4:
        while raw[i:i + 1].isspace():
            i += 1
        if raw[i:i + 1] == b"#":
            while raw[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not raw[j:j + 1].isspace():
            j += 1
        vals.append(raw[i:j])
        i = j
    return raw, i + 1, int(vals[1]), int(vals[2])


def main():
    a, b = sys.argv[1], sys.argv[2]
    ra, oa, w, h = parse_ppm(a)
    rb, ob, wb, hb = parse_ppm(b)
    if (w, h) != (wb, hb):
        print(f"MISMATCH size {w}x{h} vs {wb}x{hb}")
        return 2
    taskbar_y = h - 26
    row = w * 3
    above = 0
    below = 0
    ax0 = ay0 = 10 ** 9
    ax1 = ay1 = -1
    for y in range(h):
        ra_row = ra[oa + y * row: oa + (y + 1) * row]
        rb_row = rb[ob + y * row: ob + (y + 1) * row]
        if ra_row == rb_row:
            continue
        for x in range(0, row, 3):
            if ra_row[x:x + 3] != rb_row[x:x + 3]:
                px = x // 3
                if y < taskbar_y:
                    above += 1
                    if px < ax0:
                        ax0 = px
                    if px > ax1:
                        ax1 = px
                    if y < ay0:
                        ay0 = y
                    if y > ay1:
                        ay1 = y
                else:
                    below += 1
    print(f"size {w}x{h} taskbar_y={taskbar_y}")
    print(f"above_taskbar {above} bbox=({ax0},{ay0})-({ax1},{ay1})")
    print(f"below_taskbar {below}")
    return 0


if __name__ == "__main__":
    sys.exit(main())