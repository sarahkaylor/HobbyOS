#!/usr/bin/env python3
"""Repro: Google loads -> click the page search box -> type -> click the page's
own Search BUTTON (not Enter).  Regression target for the submit-click crash
(WebCore NavigationScheduler [NS] trace dereferencing m_redirect after cancel).

Usage: python3 tools/run_browser_click_search.py [--shot-dir DIR] [--text TEXT]
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qmp_drive import Qmp  # noqa: E402
from run_browser_google import (APPS_BTN, BROWSER_ROW, geom, inputs_census,  # noqa: E402
                                serial_len, serial_text)


def wait(pattern, timeout=120, since=0, label=""):
    rx = re.compile(pattern)
    t0 = time.time()
    while time.time() - t0 < timeout:
        m = rx.search(serial_text()[since:])
        if m:
            return m
        time.sleep(0.4)
    print("   TIMEOUT waiting for %s" % (label or pattern))
    return None


def parse_rects(name):
    ls = re.findall(r"\[WIN\] %s (.*)" % name, serial_text())
    if not ls:
        return []
    field = ls[-1].strip()
    if ";" in field:
        field = field.split(";", 1)[1]
    out = []
    for item in field.split("|"):
        if "@" not in item:
            continue
        label, rect = item.rsplit("@", 1)
        xy, wh = rect.split(":")
        x, y = xy.split(",")
        w, h = wh.split("x")
        out.append((label, int(x), int(y), int(w), int(h)))
    return out


def click_page(q, gx, gy, px, py, tries=3):
    """Click page-viewport point (px,py).  The WM reports mouse coordinates in
    window-content space, but the FIRST click after a window gains focus arrives
    in screen space — so calibrate from the driver's own echo and retry."""
    for _ in range(tries):
        mark = serial_len()
        q.click(gx + px, gy + 84 + py)
        m = wait(r"\[WIN\] click x=(-?\d+) y=(-?\d+) px=(-?\d+) py=(-?\d+)", 20, mark, "click echo")
        if not m:
            return False
        ex, ey, epx, epy = (int(v) for v in m.groups())
        dx, dy = px - epx, py - epy
        if abs(dx) <= 2 and abs(dy) <= 2:
            return True
        print("   click landed at page %d,%d (wanted %d,%d) — correcting by %d,%d"
              % (epx, epy, px, py, dx, dy))
        px, py = px + dx, py + dy
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", default="cat pictures")
    ap.add_argument("--url", default="http://www.google.com")
    ap.add_argument("--shot-dir", default="/tmp/wk-click")
    args = ap.parse_args()
    os.makedirs(args.shot_dir, exist_ok=True)
    q = Qmp()

    print("1) launch browser + navigate to", args.url)
    mark = serial_len()
    q.click(*APPS_BTN)
    time.sleep(1.0)
    q.click(*BROWSER_ROW)
    wait(r"\[WIN\] load-ok .*HOME", 240, mark, "home")
    time.sleep(2)
    gx, gy, w, h = geom()
    q.click(gx + (94 + (w - 78)) // 2, gy + 43)
    wait(r"\[WIN\] addr click .*focus", 30, serial_len() - 4000, "focus")
    q.keys(args.url)
    q.click(gx + (w - 78) + 6 + 17, gy + 43)
    wait(r"\[WIN\] (load-paint-early|load-ok)", 600, mark, "render")
    wait(r"\[WIN\] inputs \d+;", 60, mark, "input census")
    time.sleep(2)
    gx, gy, w, h = geom()
    print("   geom x=%d y=%d w=%d h=%d" % (gx, gy, w, h))
    print("   inputs:", inputs_census())
    print("   buttons:", parse_rects("buttons"))

    fields = inputs_census()
    if not fields:
        raise SystemExit("no search field")
    name, fx, fy, fw, fh = fields[0]
    print("2) click search field %r" % name)
    if not click_page(q, gx, gy, fx + fw // 2, fy + fh // 2):
        raise SystemExit("could not land on the search field")
    print("3) type %r" % args.text)
    q.keys(args.text)
    time.sleep(1.0)
    q.shot(os.path.join(args.shot_dir, "01-typed.png"))

    btns = parse_rects("buttons")
    if not btns:
        raise SystemExit("no submit button in census")
    label, bx, by, bw, bh = btns[0]
    print("4) click the page button %r" % label)
    mark = serial_len()
    if not click_page(q, gx, gy, bx + bw // 2, by + bh // 2):
        raise SystemExit("could not land on the button")
    time.sleep(8)
    q.shot(os.path.join(args.shot_dir, "02-after-click.png"))

    tail = serial_text()[mark:]
    if "memory fault" in tail:
        hit = [l for l in tail.splitlines() if "memory fault" in l][0]
        print("RESULT: CRASH -> " + hit.strip())
        sys.exit(2)
    for pat in ("[WIN] link ->", "[NS] schedule", "[WIN] go-click", "load-ok url=",
                "load-paint-early", "doc title=", "wk5EngineNav"):
        hits = [l.strip() for l in tail.splitlines() if pat in l]
        if hits:
            print("   %-20s %s" % (pat, hits[-1][:150]))
    print("RESULT: no crash")


if __name__ == "__main__":
    main()
