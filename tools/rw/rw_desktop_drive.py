#!/usr/bin/env python3
"""tools/rw/rw_desktop_drive.py -- drive a MODE=desktop HobbyOS boot that
tools/rw/run-rimworld-desktop.sh started, launch RWLAUNCH.BIN from the Apps
menu with real key events, and record what the game does.

This is the QMP half of tools/gate/m3_x11_client.py lifted into the RimWorld
track: the boot itself, the [MENU] mapping, F1/End/Up*N/Enter through the
PS/2 -> evdev path, periodic screendumps while the app runs, and a raw serial
ladder at the end.  It deliberately does not grade a verdict: the ladder is
the deliverable and a frame claim needs the screenshot.

The launcher must be started *from the desktop* (not the boot wave) because
the desktop is what owns the window protocol: it creates a window for the
child it spawns and hands it the two pipes that src/user/xcompat/libX11.so.6
speaks ESC ] X / ESC ] G over.  rwlaunch keeps fd 0/1 (the pipes) and maps the
game's fd 2 to /dev/console, so [RWMAIN]/Unity stderr rows reach this log
while stdin/stdout stay the window protocol.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time


class Qmp:
    def __init__(self, path, timeout=120):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        end = time.time() + timeout
        while time.time() < end:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.sock.recv(65536)
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        end = time.time() + 30
        while time.time() < end:
            buf += self.sock.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                if not part:
                    continue
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp
        return {}

    def key(self, qcode, pause=0.35):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def shot(self, path):
        self.cmd("screendump", {"filename": path})


def read_log(path):
    try:
        return open(path, errors="ignore").read()
    except OSError:
        return ""


def wait_for(path, needle, timeout, start=0):
    """Wait until `needle` appears in the file beyond offset `start`."""
    end = time.time() + timeout
    while time.time() < end:
        t = read_log(path)
        if needle in t[start:]:
            return True, len(t)
        time.sleep(0.4)
    return False, len(read_log(path))


def menu_map(text):
    menu = {}
    for line in text.splitlines():
        m = re.search(r"\[MENU\]\s*(\d+)=(\S+)", line)
        if m:
            menu[int(m.group(1))] = m.group(2)
    return menu


def ppm_has_colors(path):
    """Coarse census so the harness can tell a painted frame from a blank one
    without an image library: count non-black pixels and distinct RGB values."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return (0, 0, 0, 0)
    if d[:2] != b"P6":
        return (0, 0, 0, 0)
    i, parts = 2, []
    while len(parts) < 3:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        parts.append(int(d[i:j]))
        i = j
    i += 1
    w, h = parts[0], parts[1]
    px = d[i:]
    nonblack = 0
    colors = set()
    for y in range(0, h, 4):
        row = y * w * 3
        for x in range(0, w, 4):
            o = row + x * 3
            c = (px[o], px[o + 1], px[o + 2])
            colors.add(c)
            if c != (0, 0, 0):
                nonblack += 1
    return (w, h, nonblack, len(colors))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--qmp", required=True)
    ap.add_argument("--shot-dir", required=True)
    ap.add_argument("--app", default="RWLAUNCH.BIN")
    ap.add_argument("--boot-timeout", type=int, default=300)
    ap.add_argument("--run-timeout", type=int, default=300)
    ap.add_argument("--shot-every", type=int, default=8)
    args = ap.parse_args()

    os.makedirs(args.shot_dir, exist_ok=True)
    shots = []

    def grab(tag):
        p = os.path.join(args.shot_dir, tag + ".ppm")
        q.shot(p)
        time.sleep(0.4)
        png = p[:-4] + ".png"
        subprocess.run(["convert", p, png], stderr=subprocess.DEVNULL)
        shots.append(png)
        w, h, nb, nc = ppm_has_colors(p)
        print(f"[drive] shot {tag}: {png} ({w}x{h}, nonblack={nb}, colors={nc})",
              flush=True)
        return png

    ok, _ = wait_for(args.log, "[MENU] count=", args.boot_timeout)
    if not ok:
        print("[drive] FAIL: desktop never printed [MENU] count= (boot failed?)",
              flush=True)
        txt = read_log(args.log)
        for line in txt.splitlines()[-20:]:
            print("  " + line)
        return 2
    time.sleep(3)  # [MENU] is dumped before the taskbar is painted
    q = Qmp(args.qmp)
    grab("00_desktop")

    text = read_log(args.log)
    menu = menu_map(text)
    app = args.app.upper()
    idx = next((i for i, n in menu.items()
                if n.upper().split(".")[0] == app.split(".")[0]), None)
    if idx is None:
        print(f"[drive] FAIL: {app} is not in the desktop menu "
              f"({len(menu)} entries)", flush=True)
        for i in sorted(menu):
            print(f"   {i}={menu[i]}")
        return 2
    ups = max(menu) - idx
    print(f"[drive] {app} at menu index {idx} of {len(menu)} "
          f"({ups} ups from the end)", flush=True)

    # F1 opens the Apps menu; End jumps to the last row; Up*ups walks to ours;
    # Enter launches.  Real key events through PS/2 -> evdev, the same path
    # tools/gate/m3_x11_client.py uses.
    q.key("f1")
    time.sleep(0.6)
    q.key("end")
    for _ in range(ups):
        q.key("up", pause=0.12)
    q.key("ret")

    launched, n0 = wait_for(args.log, "[RW] rwlaunch: start", 60)
    print("[drive] menu launch marker: " + ("seen" if launched else "MISSING"),
          flush=True)
    if not launched:
        grab("01_no_launch")
        return 2

    # Poll until the launcher is done; screendump on a cadence so a frame (or
    # a hung engine) is captured while it is still on screen.
    end = time.time() + args.run_timeout
    last = time.time()
    i = 0
    milestones = {}
    while time.time() < end:
        t = read_log(args.log)
        for needle, name in (
                ("[RWMAIN] PlayerMain=", "playermain"),
                ("Initialize engine version", "engine-banner"),
                ("GfxDevice:", "gfxdevice"),
                ("GL_VERSION", "gl-version"),
                ("[RW] rwlaunch: done", "launcher-done")):
            if name not in milestones and needle in t:
                milestones[name] = time.time()
                print(f"[drive] milestone {name}", flush=True)
        if "launcher-done" in milestones:
            break
        if time.time() - last >= args.shot_every:
            i += 1
            grab(f"{i:02d}_running")
            last = time.time()
        time.sleep(1.0)

    grab("99_final")

    text = read_log(args.log)
    print("\n[drive] --- launcher rows ---", flush=True)
    for line in text.splitlines():
        if line.startswith("[RW]") or line.startswith("[RWMAIN]"):
            print(line, flush=True)
    print("\n[drive] --- compat loader rows ---", flush=True)
    for line in text.splitlines():
        if "[LCOMPAT]" in line:
            print(line, flush=True)
    print("\n[drive] --- engine + DSO rows ---", flush=True)
    pat = re.compile(r"Initialize engine version|GfxDevice|GL_VERSION|GL_RENDERER|"
                     r"Mono path|Mono config|error while loading|"
                     r"cannot open shared|dlopen|undefined symbol|"
                     r"Video subsystem|Unable to load|Failed to load|"
                     r"mono library|Steam|Audio|ALSA|pulse|X11|DISPLAY")
    for line in text.splitlines():
        if pat.search(line):
            print(line, flush=True)
    print("\n[drive] shots: " + " ".join(shots), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
