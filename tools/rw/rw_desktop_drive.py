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

    def click(self, x, y, w, h, hold=0.12):
        """Absolute left click through QMP's input-send-event (PS/2/vmmouse).

        `hold` is how long the button stays down.  IMGUI buttons process a
        queued press+release, so 0.12 s is enough for the front-end; the
        world map polls Input.GetMouseButtonDown once per frame, and at the
        0.7-1.0 fps this host measures a 0.12 s press falls between two
        frames and is never seen (measured: a land-tile click changed 235 px
        of highlight and selected nothing).  Site clicks hold for 2 s.
        """
        ax = int(x * 32767 / max(1, w - 1))
        ay = int(y * 32767 / max(1, h - 1))
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]})
        time.sleep(0.15)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}]})
        time.sleep(hold)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}]})

    def shot(self, path):
        self.cmd("screendump", {"filename": path})

    def pmem(self, pa, size, path):
        """Dump guest physical memory (M3 screenshots).

        QMP screendump answers "no surface" on the GL device
        (egl-headless, P9), but the desktop's framebuffer is ordinary guest
        RAM: the kernel's `framebuffer` BSS array, identity-mapped, so the
        ELF symbol address is its physical address.  pmemsave is a monitor
        command, reached through QMP's human-monitor-command."""
        # HMP parses its arguments as expressions unless the filename is
        # quoted: `pmemsave 0x.. N /tmp/x` answers "invalid char 't'".
        r = self.cmd("human-monitor-command",
                    {"command-line": f'pmemsave {pa} {size} "{path}"'})
        return r


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


def bgra_to_ppm(raw, w, h, out):
    """The kernel framebuffer is B8G8R8A8 (word 0xAARRGGBB), row 0 at the
    top; PPM wants RGB, so this is a per-pixel byte shuffle."""
    with open(raw, "rb") as f:
        data = f.read()
    if len(data) < w * h * 4:
        return False
    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        row = bytearray(w * 3)
        for y in range(h):
            base = y * w * 4
            for x in range(w):
                o = base + x * 4
                row[x * 3] = data[o + 2]
                row[x * 3 + 1] = data[o + 1]
                row[x * 3 + 2] = data[o]
            f.write(row)
    return True


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


def find_ignore_xy(png):
    """The Ignore button of RimWorld's Steam modal (same rule as
    tools/rw/click-ignore.py: a flat brown 106,81,46 band with two wide
    runs; the left run is Ignore).  Returns (x, y) or None."""
    try:
        from PIL import Image
    except Exception:
        return None
    BROWN, MIN_RUN, BAND_ROWS = (106, 81, 46), 220, 18
    try:
        im = Image.open(png).convert("RGB")
    except Exception:
        return None
    W, H = im.size
    if W < 100 or H < 100:
        return None
    px = im.load()
    band = {}
    for y in range(H // 2, H):
        xs = [x for x in range(W) if px[x, y] == BROWN]
        if len(xs) > 2 * MIN_RUN:
            band[y] = xs
    if not band:
        return None
    ys = sorted(band)
    groups, cur = [], [ys[0]]
    for y in ys[1:]:
        if y - cur[-1] <= 2:
            cur.append(y)
        else:
            groups.append(cur)
            cur = [y]
    groups.append(cur)
    g = max(groups, key=len)
    if len(g) < BAND_ROWS // 2:
        return None
    cols = [0] * W
    for y in g:
        for x in band[y]:
            cols[x] += 1
    runs, start = [], None
    for x in range(W):
        if cols[x] > len(g) // 3 and start is None:
            start = x
        elif cols[x] <= len(g) // 3 and start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, W - 1))
    runs = [r for r in runs if r[1] - r[0] >= MIN_RUN]
    if len(runs) < 2:
        return None
    left = runs[0]
    return ((left[0] + left[1]) // 2, (g[0] + g[-1]) // 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--qmp", required=True)
    ap.add_argument("--shot-dir", required=True)
    ap.add_argument("--app", default="RWLAUNCH.BIN")
    ap.add_argument("--boot-timeout", type=int, default=300)
    ap.add_argument("--run-timeout", type=int, default=300)
    ap.add_argument("--shot-every", type=int, default=8)
    ap.add_argument("--fb-phys", default="")
    ap.add_argument("--fb-w", type=int, default=1920)
    ap.add_argument("--fb-h", type=int, default=1080)
    args = ap.parse_args()

    os.makedirs(args.shot_dir, exist_ok=True)
    shots = []

    def grab(tag):
        p = os.path.join(args.shot_dir, tag + ".ppm")
        if args.fb_phys:
            raw = os.path.join(args.shot_dir, tag + ".raw")
            q.pmem(args.fb_phys, args.fb_w * args.fb_h * 4, raw)
            time.sleep(0.3)
            if not bgra_to_ppm(raw, args.fb_w, args.fb_h, p):
                print(f"[drive] shot {tag}: pmemsave failed", flush=True)
                return None
            os.remove(raw)
        else:
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

    # L79/L81: a scripted front-end flow (wait for the menu, click New colony,
    # follow the screens) takes over from the plain screenshot loop.
    if os.environ.get("RW_FLOW", "0") == "1":
        import rw_flow
        rc = rw_flow.run(q, grab, args.log, args, shots, find_ignore_xy)
        grab("99_final")
        text = read_log(args.log)
        print("\n[drive] --- flow final rows ---", flush=True)
        for line in text.splitlines()[-60:]:
            print("  " + line, flush=True)
        return rc

    # Poll until the launcher is done; screendump on a cadence so a frame (or
    # a hung engine) is captured while it is still on screen.
    end = time.time() + args.run_timeout
    last = time.time()
    i = 0
    milestones = {}
    # L78: RimWorld stops on a modal "Could not initialize Steam API" box
    # (Ignore / Quit) before it reaches the menu.  RW_CLICK_IGNORE_AFTER=<s>
    # clicks the brown Ignore button at that run time and re-checks every 30 s
    # (bounded to 6 attempts) so a re-drawn modal is dismissed too.
    click_after = float(os.environ.get("RW_CLICK_IGNORE_AFTER", "0") or 0)
    t_start = time.time()
    next_click = (t_start + click_after) if click_after > 0 else None
    clicks = 0
    checks = 0
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
        # M6/L63: the launcher exits when the GAME's process dies, so the
        # default ends the run there.  RW_KEEP_RUNNING=1 keeps the window open
        # to --run-timeout instead: a soak wants the rest of the window (the
        # desktop and the machine must still be alive after a guest fault).
        if "launcher-done" in milestones and os.environ.get(
                "RW_KEEP_RUNNING", "0") != "1":
            break
        if next_click is not None and time.time() >= next_click and checks < 12:
            checks += 1
            i += 1
            png = grab(f"{i:02d}_click{checks:02d}")
            xy = find_ignore_xy(png) if png else None
            if xy:
                print(f"[drive] click Ignore at {xy[0]},{xy[1]} "
                      f"(check {checks}, click {clicks + 1})", flush=True)
                q.click(xy[0], xy[1], args.fb_w, args.fb_h)
                clicks += 1
            else:
                print(f"[drive] check {checks}: no Steam modal on screen",
                      flush=True)
            next_click = time.time() + 30
            last = time.time()
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
