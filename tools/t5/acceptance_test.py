#!/usr/bin/env python3
"""Host (T1) self-test for acceptance_run's liveness (GB-053)."""
import os, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from acceptance_run import ppm_live

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

def mkppm(path, w, h, pixel):
    hdr = b"P6\n%d %d\n255\n" % (w, h)
    with open(path, "wb") as f:
        f.write(hdr)
        for _ in range(w * h):
            f.write(pixel)

d = tempfile.mkdtemp()
black = os.path.join(d, "black.ppm")
mkppm(black, 640, 480, b"\x00\x00\x00")
check(ppm_live(black) is False, "a uniform black frame is NOT live")

var = os.path.join(d, "var.ppm")
hdr = b"P6\n4 4\n255\n"
px = b"\xff\xff\xff\x00\x00\x00" * 4   # alternating -> 2 distinct pixels
with open(var, "wb") as f:
    f.write(hdr + px)
check(ppm_live(var) is True, "a frame with distinct pixels IS live")

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
