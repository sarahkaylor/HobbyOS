#!/usr/bin/env python3
"""Launch (if needed) + navigate the HobbyOS browser to a URL, then capture.

Usage: python3 tools/browser_nav.py <url> [--shot out.png] [--wait SECS]
Reuses the serial receipts for window geometry + input census.
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qmp_drive import Qmp  # noqa: E402

SERIAL = os.path.join(os.path.dirname(HERE), "serial.log")
APPS_BTN = (35, 1062)
BROWSER_ROW = (110, 760)


def serial():
    with open(SERIAL, "rb") as f:
        return f.read().decode("utf-8", "replace")


def wait(pattern, timeout=120, since=0, label=""):
    rx = re.compile(pattern)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if rx.search(serial()[since:]):
            return True
        time.sleep(0.5)
    print("TIMEOUT waiting for %s (%s)" % (label or pattern, pattern))
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--shot", default="/tmp/wk-nav.png")
    ap.add_argument("--wait", type=float, default=60)
    ap.add_argument("--launch", action="store_true")
    args = ap.parse_args()

    q = Qmp()
    mark = len(serial())
    if args.launch:
        q.click(*APPS_BTN)
        time.sleep(1.0)
        q.click(*BROWSER_ROW)
        wait(r"\[WIN\] load-ok .*HOME", timeout=240, since=mark, label="home load")
        time.sleep(2)
    gs = re.findall(r"\[WIN\] geom x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", serial())
    x, y, w, h = (int(v) for v in gs[-1])
    omni = (x + (94 + (w - 78)) // 2, y + 43)
    go = (x + (w - 78) + 6 + 17, y + 43)
    print("geom x=%d y=%d w=%d h=%d omni=%s go=%s" % (x, y, w, h, omni, go))

    mark = len(serial())
    q.click(*omni)
    wait(r"\[WIN\] addr click .*focus", 30, mark, "url focus")
    q.keys(args.url)
    time.sleep(0.4)
    q.click(*go)
    wait(r"\[WIN\] go-click", 30, mark, "go click")

    t0 = time.time()
    while time.time() - t0 < args.wait:
        tail = serial()[mark:]
        if "[WIN] frame view=" in tail:
            print("rendered: frame painted")
            break
        if "memory fault" in tail:
            print("CRASH: " + [l for l in tail.splitlines() if "memory fault" in l][0])
            break
        time.sleep(1.0)
    time.sleep(2)
    q.shot(args.shot)
    tail = serial()[mark:]
    for pat in ("load-paint-early", "load-ok", "paint-stage contents-done", "memory fault"):
        hits = [l for l in tail.splitlines() if pat in l]
        if hits:
            print("%-26s %s" % (pat, hits[-1][:160]))


if __name__ == "__main__":
    main()
