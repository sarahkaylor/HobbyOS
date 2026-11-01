#!/usr/bin/env python3
"""Measure the window rectangle IceWM gives each launcher app."""
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath("capture_screenshots.py")))
import capture_screenshots as C

work = "/tmp/dbg-measure"
shutil.rmtree(work, ignore_errors=True)
s = C.Session(work)
NEUTRAL = (1700, 60)


def rect_of(name, before, after):
    s.qmp.move(*NEUTRAL)
    time.sleep(0.5)
    s.qmp.shot(after)
    r = C.diff_bbox(before, after)
    return r


try:
    s.wait_session()
    print("desktop up")
    for name in ("FILES", "CONSOLE", "CALC", "SYSMON", "CLOCK", "PONG",
                 "STARDUST", "XNIBBLER", "XCALC", "ANTFARM"):
        before = os.path.join(work, "b.ppm")
        after = os.path.join(work, "a.ppm")
        s.qmp.move(*NEUTRAL)
        time.sleep(0.4)
        s.qmp.shot(before)
        r = s.launch(name)
        r = rect_of(name, before, after)
        w = (r[2] - r[0]) if r else 0
        hh = (r[3] - r[1]) if r else 0
        print(f"{name:9s} rect={r} size={w}x{hh}")
        # close it again
        if r:
            s.qmp.click((r[0] + r[2]) // 2, r[1] + 9)
            time.sleep(1.0)
            s.qmp.key("f4")
            if s.wait_serial(f"[HBHOST] {name}.BIN exited", 20) < 0:
                print(f"  (warning: {name} did not report exit)")
            time.sleep(2)
finally:
    s.proc.terminate()
