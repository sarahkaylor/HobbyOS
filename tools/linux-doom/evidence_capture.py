#!/usr/bin/env python3
"""evidence_capture.py — run a Doom ELF against Xvfb, screenshot it, and
report objective frame statistics (no PIL required).

Why: "it renders" must be evidence, not a claim.  For each capture this
records the screenshot (PPM + PNG), the non-black pixel fraction and the
number of distinct colours, both for the whole root window and for the
game window's rectangle (found via xwininfo), plus the program's exit
status and its stderr/stdout tail.

Usage:
  python3 evidence_capture.py --binary /path/doom10.elf \
      --outdir evidence/doom10 --screen 640x480x8 \
      --capture 8:title --capture 45:attract --cwd /tmp/doomrun -- -iwad doom1.wad

The binary runs with cwd = --cwd (default: its own directory).  Xvfb is
started if the display is not already up, and stopped again if we started it.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def display_up(disp):
    return sh(["xdpyinfo", "-display", disp]).returncode == 0


def start_xvfb(disp, screen):
    num = disp.lstrip(":")
    proc = subprocess.Popen(
        ["Xvfb", disp, "-screen", "0", screen, "-nolisten", "tcp"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if display_up(disp):
            return proc
        if proc.poll() is not None:
            raise SystemExit(f"Xvfb {disp} exited with {proc.returncode}")
        time.sleep(0.1)
    raise SystemExit(f"Xvfb {disp} did not come up")


def read_ppm(path):
    """Minimal binary-P6 reader -> (w, h, bytes of RGB triples)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: not a P6 PPM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace after maxval
    w, h, maxval = fields
    px = data[pos:pos + w * h * 3 * (2 if maxval > 255 else 1)]
    if maxval > 255:  # ImageMagick emits 16-bit PPM for some visuals
        px = bytes(px[i] for i in range(0, len(px), 2))  # take the high byte
    if len(px) != w * h * 3:
        raise SystemExit(f"{path}: truncated pixel data")
    return w, h, px


def crop(w, h, px, x, y, cw, ch):
    x = max(0, min(x, w)); y = max(0, min(y, h))
    cw = max(1, min(cw, w - x)); ch = max(1, min(ch, h - y))
    out = bytearray()
    for row in range(y, y + ch):
        out += px[(row * w + x) * 3:(row * w + x + cw) * 3]
    return cw, ch, bytes(out)


def stats(w, h, px):
    n = w * h
    non_black = 0
    colors = {}
    step = 3
    for i in range(0, len(px), step):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r or g or b:
            non_black += 1
        key = (r << 16) | (g << 8) | b
        colors[key] = colors.get(key, 0) + 1
    top = sorted(colors.items(), key=lambda kv: -kv[1])[:8]
    return {
        "width": w, "height": h,
        "non_black_fraction": round(non_black / n, 4),
        "distinct_colors": len(colors),
        "top_colors": ["#%06x x%d" % (k, v) for k, v in top],
    }


def find_window(disp, hint_re):
    """Return the game window: prefer one whose title matches hint_re,
    otherwise the largest mapped child of root.

    The largest-child fallback matters on 8-bit screens: Doom 1.10 never calls
    XStoreName, so its window is unnamed, and a root-window grab would map its
    pixels through the *root* colormap (Xvfb does not colour-translate child
    windows on GetImage), producing a handful of 6x6x6-cube colours.  Grabbing
    the window itself makes the server hand back pixels plus that window's
    colormap, which is the real DOOM palette.
    """
    out = sh(["xwininfo", "-display", disp, "-root", "-tree"]).stdout
    kids = []
    for line in out.splitlines():
        # xwininfo prints either 0xID "title": (class)  or  0xID (has no name):
        m = re.search(r'(0x[0-9a-f]+)\s+(?:"([^"]*)"|\(has no name\)):.*?'
                      r'(\d+)x(\d+)\+(-?\d+)\+(-?\d+)\s+[+-]\d+[+-]\d+', line)
        if not m:
            continue
        kids.append({"id": m.group(1), "name": m.group(2),
                     "x": int(m.group(5)), "y": int(m.group(6)),
                     "w": int(m.group(3)), "h": int(m.group(4))})
    if not kids:
        return None
    for k in kids:
        if k["name"] and re.search(hint_re, k["name"], re.I):
            return k
    return max(kids, key=lambda k: k["w"] * k["h"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--screen", default="640x480x8")
    ap.add_argument("--display", default=":99")
    ap.add_argument("--cwd", default=None)
    ap.add_argument("--capture", action="append", default=[],
                    help="SECONDS:LABEL")
    ap.add_argument("--window-hint", default=r"doom", help="regex for the game window title")
    ap.add_argument("--settle", type=float, default=0.3)
    ap.add_argument("cmdline", nargs=argparse.REMAINDER)
    a = ap.parse_args()

    args = a.cmdline[1:] if a.cmdline[:1] == ["--"] else a.cmdline
    os.makedirs(a.outdir, exist_ok=True)
    cwd = a.cwd or os.path.dirname(os.path.abspath(a.binary))

    xvfb = None
    if not display_up(a.display):
        xvfb = start_xvfb(a.display, a.screen)
        print(f"[ev] started Xvfb {a.display} {a.screen}")
    depth = sh(["xdpyinfo", "-display", a.display]).stdout
    m = re.search(r"depth of root window:\s+(\d+)", depth)
    print(f"[ev] display {a.display} root depth = {m.group(1) if m else '?'}")

    env = dict(os.environ, DISPLAY=a.display)
    log = open(os.path.join(a.outdir, "runtime.log"), "wb")
    t0 = time.time()
    proc = subprocess.Popen([os.path.abspath(a.binary)] + args, cwd=cwd,
                            env=env, stdout=log, stderr=subprocess.STDOUT)

    results, alive = [], True
    for spec in a.capture:
        secs_s, _, label = spec.partition(":")
        secs = float(secs_s)
        wait = secs - (time.time() - t0)
        if wait > 0:
            time.sleep(wait)
        if proc.poll() is not None:
            alive = False
            print(f"[ev] process exited early: {proc.returncode}")
            break
        win = find_window(a.display, a.window_hint)
        ppm = os.path.join(a.outdir, f"{label}.ppm")
        if win:
            # Grab the game window: correct colours on a PseudoColor screen.
            sh(["import", "-display", a.display, "-depth", "8",
                "-window", win["id"], ppm], timeout=30)
        else:
            sh(["import", "-display", a.display, "-depth", "8",
                "-window", "root", ppm], timeout=30)
        time.sleep(a.settle)
        w, h, px = read_ppm(ppm)
        entry = {"label": label, "seconds": round(time.time() - t0, 1),
                 "screenshot": ppm, "root": stats(w, h, px),
                 "geometry": win}
        if win and (win["w"], win["h"]) != (w, h):
            cw, ch, cpx = crop(w, h, px, 0, 0, win["w"], win["h"])
            cppm = os.path.join(a.outdir, f"{label}-crop.ppm")
            with open(cppm, "wb") as f:
                f.write(b"P6\n%d %d\n255\n" % (cw, ch) + cpx)
            entry["window"] = dict(stats(cw, ch, cpx), screenshot=cppm)
        sh(["convert", ppm, os.path.join(a.outdir, f"{label}.png")])
        results.append(entry)
        print(f"[ev] {label}: {entry['root']['width']}x{entry['root']['height']} "
              f"{float(entry['root']['non_black_fraction'])*100:.1f}% non-black, "
              f"{entry['root']['distinct_colors']} colours")

    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait()
    log.close()
    if xvfb is not None:
        xvfb.terminate()

    tail = ""
    with open(os.path.join(a.outdir, "runtime.log"), "rb") as f:
        tail = f.read().decode("utf-8", "replace")[-1500:]
    summary = {"binary": os.path.abspath(a.binary), "args": args,
               "display": a.display, "screen": a.screen, "cwd": cwd,
               "exit_code": proc.returncode, "captures": results,
               "log_tail": tail}
    with open(os.path.join(a.outdir, "stats.json"), "w") as f:
        json.dump(summary, f, indent=2, default=str)
    print(f"[ev] wrote {os.path.join(a.outdir, 'stats.json')}")
    return 0 if results else 1


if __name__ == "__main__":
    sys.exit(main())
