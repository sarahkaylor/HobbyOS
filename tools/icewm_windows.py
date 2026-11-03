#!/usr/bin/env python3
"""Report the bounding boxes of the on-screen windows in a HobbyOS screenshot.

The IceWM wallpaper is a uniform dark slate blue, so every non-wallpaper pixel
belongs to a window frame or the taskbar. Connected components on a 4-px grid
give each window's rectangle (title bar included).
"""
import sys
from PIL import Image

WALL = None  # learned from the top-right corner of the screen


def near(c, ref, tol=10):
    return all(abs(c[i] - ref[i]) <= tol for i in range(3))


def main(path, taskbar_h=26):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    ref = px[w - 5, 5]
    step = 4
    gw, gh = w // step, h // step
    grid = [[0] * gw for _ in range(gh)]
    for gy in range(gh):
        for gx in range(gw):
            c = px[gx * step, gy * step]
            grid[gy][gx] = 0 if near(c, ref, 12) else 1
    seen = [[False] * gw for _ in range(gh)]
    boxes = []
    for gy in range(gh):
        for gx in range(gw):
            if grid[gy][gx] and not seen[gy][gx]:
                stack = [(gx, gy)]
                seen[gy][gx] = True
                x0 = x1 = gx
                y0 = y1 = gy
                n = 0
                while stack:
                    cx, cy = stack.pop()
                    n += 1
                    x0, x1 = min(x0, cx), max(x1, cx)
                    y0, y1 = min(y0, cy), max(y1, cy)
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < gw and 0 <= ny < gh and grid[ny][nx] and not seen[ny][nx]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
                boxes.append((n * step * step, x0 * step, y0 * step,
                              x1 * step + step, y1 * step + step))
    boxes.sort(reverse=True)
    print(f"wallpaper={ref} image={w}x{h}")
    for area, x0, y0, x1, y1 in boxes:
        kind = "TASKBAR" if y0 > h - taskbar_h - 8 else "window"
        if x1 - x0 > 40 and y1 - y0 > 20:
            print(f"{kind:8s} x={x0:4d}..{x1:4d} y={y0:4d}..{y1:4d}  "
                  f"{x1 - x0}x{y1 - y0}  ({area / 1000:.0f}k px)")


if __name__ == "__main__":
    main(sys.argv[1])
