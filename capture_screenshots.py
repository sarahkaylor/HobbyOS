#!/usr/bin/env python3
"""Capture the README screenshots from a live HobbyOS desktop.

Boots the real desktop headless (arm) with a QMP socket, then stages four
scenes and screendumps each (raw .ppm -> .png via the screenshot skill's
ppm2png.py):

  1. 10_apps_menu     - the Apps (start) menu open on the taskbar
  2. 20_tiling        - the tiling WM running four apps (FILES, CLOCK, CALC
                        with a typed expression, SYSMON) in a 2x2 grid
  3. 30_pong_*        - PONG playing (burst; pick the liveliest frame)
  4. 40_milli_*       - MILLIPED playing (burst across the game's progression)

Every action is QMP input injection (absolute pointer + send-key); the
framebuffers land in /tmp/readme_shots/. Nothing is asserted here: the
scripts are the test suite's proven sequences (run_games_test.py,
run_desktop_apps_test.py); the operator picks the best frames afterwards.
"""
import json
import os
import re
import socket
import subprocess
import time

REPO = os.path.dirname(os.path.abspath(__file__))
os.chdir(REPO)

W, H = 1024, 768
TASKBAR_Y = H - 26
QMP_SOCK = "./qmp-readme"
SERIAL = "/tmp/readme_serial.log"
SHOT_DIR = "/tmp/readme_shots"
CONVERT = os.path.join(REPO, "skills/hobbyos-screenshot/scripts/ppm2png.py")


def log(msg):
    print(msg, flush=True)


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(15)
        for _ in range(160):
            try:
                self.s.connect(path)
                break
            except Exception:
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.s.recv(65536)
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.s.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            buf += self.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp

    def move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.1)

    def click(self, x, y):
        self.move(x, y)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.4)

    def key(self, qcode, pause=0.15):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def chord(self, qcodes, pause=0.2):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": q} for q in qcodes]})
        time.sleep(pause)

    def shot(self, name, settle=0.3):
        path = os.path.join(SHOT_DIR, name)
        self.cmd("screendump", {"filename": path})
        time.sleep(settle)
        return path


def serial_text():
    try:
        return open(SERIAL, errors="ignore").read()
    except OSError:
        return ""


def ppm_has_content(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return False
    return any(data[100:])


def wait_serial(pattern, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if re.search(pattern, serial_text()):
            return True
        time.sleep(0.4)
    return False


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
    for f in (SERIAL, QMP_SOCK):
        if os.path.exists(f):
            os.remove(f)

    log("[CAP] booting desktop...")
    subprocess.Popen(
        ["stdbuf", "-oL", "make", "run", "ARCH=arm",
         f"QEMU_ARGS=-display none -qmp unix:{QMP_SOCK},server,nowait"],
        stdout=open(SERIAL, "w"), stderr=subprocess.STDOUT)
    q = Qmp(QMP_SOCK)
    if not wait_serial(r"Desktop starting", 120):
        raise SystemExit("[CAP] desktop never booted")
    time.sleep(3)

    # Resolve menu indices from the desktop's [MENU] dump (never hardcode).
    txt = serial_text()
    entries = dict((int(a), b) for a, b in re.findall(
        r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)(?:\[CONSOLE\]\s*)*=(?:\[CONSOLE\]\s*)*([A-Za-z0-9_.]+)", txt))
    want = ["FILES.BIN", "CLOCK.BIN", "CALC.BIN", "SYSMON.BIN", "PONG.BIN", "MILLIPED.BIN"]
    idx_of = {}
    for name in want:
        idx = next((i for i, n in entries.items() if n == name), None)
        if idx is None:
            raise SystemExit(f"[CAP] {name} missing from menu dump")
        idx_of[name] = idx
    log(f"[CAP] menu indices: {idx_of}")

    def launch(idx):
        q.click(5, 5)                      # dismiss any open menu / defocus
        q.click(38, TASKBAR_Y + 13)        # Apps button
        time.sleep(0.8)
        q.key("home", 0.3)
        for _ in range(idx):
            q.key("down", 0.06)
        q.key("ret", 0.3)

    # ---- 1. Apps menu open ------------------------------------------------
    q.click(38, TASKBAR_Y + 13)
    time.sleep(1.0)
    m = q.shot("10_apps_menu.ppm")
    log(f"[CAP] apps menu: {'ok' if ppm_has_content(m) else 'BLANK!'}")
    q.click(5, 5)
    time.sleep(0.5)

    # ---- 2. Tiling WM: FILES, CLOCK, CALC (typed), SYSMON ----------------
    launch(idx_of["FILES.BIN"]); time.sleep(1.5)
    launch(idx_of["CLOCK.BIN"]); time.sleep(1.5)
    launch(idx_of["CALC.BIN"]); time.sleep(1.0)
    for k in ("1", "2"):
        q.key(k, 0.12)
    q.chord(["shift", "equal"], 0.15)      # '+'
    for k in ("3", "4"):
        q.key(k, 0.12)
    q.key("ret", 0.3)                      # evaluate -> "> 46"
    time.sleep(1.0)
    launch(idx_of["SYSMON.BIN"]); time.sleep(2.5)
    t = q.shot("20_tiling.ppm")
    log(f"[CAP] tiling: {'ok' if ppm_has_content(t) else 'BLANK!'} "
        f"(launched={serial_text().count('[LAUNCH]')})")

    # ---- close all four apps (F4 = keyboard X button) ---------------------
    for _ in range(4):
        q.key("f4", 0.9)
    time.sleep(2.0)
    q.click(5, 5)

    # ---- 3. PONG (reset-burst; the game is fast, 'r' restarts at 0:0) -----
    launch(idx_of["PONG.BIN"])
    time.sleep(1.5)                        # game up and drawing
    p = None
    for n in range(1, 7):
        q.key("r", 0.1)                    # reset: 0:0, ball back to centre
        p = q.shot(f"31_pong_r{n}.ppm", settle=0.12)
    log(f"[CAP] pong reset-burst done, last {'ok' if ppm_has_content(p) else 'BLANK!'}")
    # quit: 'q' is forwarded to the game's stdin; retry, then F4 fallback
    for n in range(1, 6):
        q.key("q", 0.5)
        if re.search(r"Process \d+: PONG\.BIN exited", serial_text()):
            log(f"[CAP] pong exited on q x{n}")
            break
    else:
        for n in range(1, 5):
            q.key("f4", 0.6)
            if re.search(r"Process \d+: PONG\.BIN exited", serial_text()):
                log(f"[CAP] pong needed f4 x{n}")
                break
    time.sleep(2.0)

    # ---- 4. MILLIPED (burst across progression) ---------------------------
    launch(idx_of["MILLIPED.BIN"])
    time.sleep(1.1)
    q.shot("41_milli_a.ppm")               # just started, creature near centre
    time.sleep(1.0)
    q.shot("41_milli_b.ppm")
    time.sleep(2.0)
    q.shot("41_milli_c.ppm")
    time.sleep(6.0)
    q.shot("41_milli_d.ppm")
    time.sleep(6.0)
    q.shot("41_milli_e.ppm")
    log("[CAP] milli burst done")
    for n in range(1, 6):
        q.key("q", 0.5)
        if re.search(r"Process \d+: MILLIPED\.BIN exited", serial_text()):
            log(f"[CAP] milli exited on q x{n}")
            break
    else:
        for n in range(1, 5):
            q.key("f4", 0.6)
            if re.search(r"Process \d+: MILLIPED\.BIN exited", serial_text()):
                log(f"[CAP] milli needed f4 x{n}")
                break
    time.sleep(2.0)
    q.shot("50_restored.ppm")

    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]aarch64"], stderr=subprocess.DEVNULL)
    time.sleep(1)

    # ---- convert every ppm to png -----------------------------------------
    for f in sorted(os.listdir(SHOT_DIR)):
        if f.endswith(".ppm"):
            subprocess.run(["python3", CONVERT, os.path.join(SHOT_DIR, f),
                            os.path.join(SHOT_DIR, f[:-4] + ".png")],
                           check=True, capture_output=True)
    log("[CAP] all shots converted:")
    for f in sorted(os.listdir(SHOT_DIR)):
        if f.endswith(".png"):
            log(f"  {os.path.join(SHOT_DIR, f)}")


if __name__ == "__main__":
    main()
