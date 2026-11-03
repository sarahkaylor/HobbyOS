#!/usr/bin/env python3
"""Probe the live session: measure a window, drag it, measure again."""
import os
import sys
import time

sys.path.insert(0, "/home/sarah/Documents/GitHub/HobbyOS")
import capture_screenshots as C

WORK = "/tmp/live-probe"
os.makedirs(WORK, exist_ok=True)
q = C.Qmp("/tmp/icewm-live/qmp-sock")
NEUTRAL = (1700, 60)


def shot(name):
    p = os.path.join(WORK, name + ".ppm")
    q.shot(p)
    return p


def measure(ref):
    q.move(*NEUTRAL)
    time.sleep(0.6)
    cur = shot("cur")
    return C.diff_bbox(ref, cur)


if __name__ == "__main__":
    ref = "/tmp/live-probe/ref.ppm"
    q.move(*NEUTRAL)
    time.sleep(0.5)
    q.shot(ref)
    r = measure(ref)
    print("before drag:", r)
    if r:
        x0, y0, x1, y1 = r
        gx, gy = (x0 + x1) // 2, y0 + 9
        q.drag(gx, gy, gx + 400, gy + 300, steps=20)
        time.sleep(1.5)
        r2 = measure(ref)
        print("after drag (+400,+300):", r2)
        print("expected:",
              None if not r2 else (x0 + 400, y0 + 300, x1 + 400, y1 + 300))
