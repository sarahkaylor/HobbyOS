#!/usr/bin/env python3
"""End-to-end acceptance: Apps menu -> Browser -> address bar -> GO ->
Google renders -> click the page search box with the mouse -> type text.

Drives a running HobbyOS ARM desktop over QMP (see tools/boot_arm.sh) and
reads the guest's own serial receipts ([WIN] geom / inputs / page-key) so
every click aims at a byte-truth rectangle instead of a guess.

Usage: python3 tools/run_browser_google.py [--url URL] [--text TEXT] [--shot-dir DIR]
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qmp_drive import Qmp  # noqa: E402

SERIAL = os.path.join(REPO, "serial.log")

APPS_BTN = (35, 1062)
BROWSER_ROW = (110, 760)     # Apps menu: row 1 (start menu rows are 20 px from y=726)
TASKBAR_Y = 1054


def serial_text():
    try:
        with open(SERIAL, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def wait_for(pattern, timeout=120, since=0, label=None):
    """Wait until `pattern` (regex) appears in the serial log after `since`."""
    rx = re.compile(pattern)
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = serial_text()[since:]
        m = rx.search(text)
        if m:
            return m
        time.sleep(0.5)
    raise SystemExit("timeout waiting for %s (%s)" % (label or pattern, pattern))


def serial_len():
    return len(serial_text())


def geom():
    """Last [WIN] geom x= y= w= h= line -> window rect."""
    gs = re.findall(r"\[WIN\] geom x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", serial_text())
    if not gs:
        raise SystemExit("no [WIN] geom line yet")
    x, y, w, h = (int(v) for v in gs[-1])
    return x, y, w, h


def inputs_census():
    """Last [WIN] inputs line -> [(name, x, y, w, h), ...] in viewport pixels.

    Format: "[WIN] inputs <count>;name@x,y:WxH|name@x,y:WxH|..."
    """
    ls = re.findall(r"\[WIN\] inputs (.*)", serial_text())
    if not ls:
        return []
    field = ls[-1].strip()
    if ";" in field:
        field = field.split(";", 1)[1]
    out = []
    for item in field.split("|"):
        if "@" not in item:
            continue
        name, rect = item.rsplit("@", 1)
        xy, wh = rect.split(":")
        x, y = xy.split(",")
        w, h = wh.split("x")
        out.append((name, int(x), int(y), int(w), int(h)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://www.google.com")
    ap.add_argument("--text", default="cat pictures")
    ap.add_argument("--shot-dir", default="/tmp/wk-google")
    args = ap.parse_args()
    os.makedirs(args.shot_dir, exist_ok=True)

    q = Qmp()

    print("1) boot: desktop is up")
    q.shot(os.path.join(args.shot_dir, "01-desktop.png"))

    print("2) Apps menu -> Browser")
    mark = serial_len()
    q.click(*APPS_BTN)
    time.sleep(1.0)
    q.shot(os.path.join(args.shot_dir, "02-apps-menu.png"))
    q.click(*BROWSER_ROW)
    wait_for(r"\[WIN\] created pref=", timeout=180, since=mark, label="browser window")
    wait_for(r"\[WIN\] load-ok .*HOME", timeout=300, since=mark, label="HOME.HTM load")
    time.sleep(2.0)
    q.shot(os.path.join(args.shot_dir, "03-browser-home.png"))
    x, y, w, h = geom()
    print("   window geom x=%d y=%d w=%d h=%d" % (x, y, w, h))

    # Chrome geometry (WK5Chrome.h): omnibox [94, w-38-(GO_W+GO_GAP)) rows 30..56;
    # GO button sits GO_GAP right of the omnibox, 34 px wide.
    omni_x0, omni_x1 = 94, w - 38 - 40
    omni = (x + (omni_x0 + omni_x1) // 2, y + 43)
    go = (x + omni_x1 + 6 + 17, y + 43)

    print("3) focus address bar, type %s, click GO" % args.url)
    mark = serial_len()
    q.click(*omni)
    wait_for(r"\[WIN\] addr click .*focus", timeout=30, since=mark, label="url focus")
    q.keys(args.url)
    time.sleep(0.5)
    q.shot(os.path.join(args.shot_dir, "04-url-typed.png"))
    mark = serial_len()
    q.click(*go)
    wait_for(r"\[WIN\] go-click url=", timeout=30, since=mark, label="GO click")

    print("4) wait for the page to render")
    wait_for(r"\[WIN\] (load-paint-early|load-ok)", timeout=600, since=mark, label="google rendered")
    wait_for(r"\[WIN\] frame view=", timeout=600, since=mark, label="first frame")
    time.sleep(3.0)
    q.shot(os.path.join(args.shot_dir, "05-google-rendered.png"))

    fields = inputs_census()
    print("   input census:", fields)
    if not fields:
        raise SystemExit("FAIL: no <input>/<textarea> in the rendered page")
    name, fx, fy, fw, fh = fields[0]
    # page viewport origin = window origin + chrome height (84)
    target = (x + fx + fw // 2, y + 84 + fy + fh // 2)
    print("5) click the search box %r at screen %s" % (name, target))
    mark = serial_len()
    q.click(*target)
    wait_for(r"\[WIN\] click x=\d+", timeout=30, since=mark, label="page click")

    print("6) type %r" % args.text)
    q.keys(args.text)
    time.sleep(1.5)
    q.shot(os.path.join(args.shot_dir, "06-typed.png"))

    # Guest-side truth: the page keys the driver forwarded + the field value.
    keys = re.findall(r"\[WIN\] page-key ch=0x([0-9a-f]{2})", serial_text()[mark:])
    typed = "".join(chr(int(k, 16)) for k in keys)
    print("   driver forwarded page keys: %r" % typed)
    if typed.strip() != args.text:
        print("FAIL: forwarded keys %r != %r" % (typed, args.text))
        sys.exit(1)
    print("OK: %r typed into the page search box" % args.text)


if __name__ == "__main__":
    main()
