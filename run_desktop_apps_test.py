#!/usr/bin/env python3
"""
Boot the real HobbyOS desktop and launch every new application via the
taskbar start menu, capturing a screenshot of each and checking the app's
serial marker line.

Usage:  python3 run_desktop_apps_test.py   (or: make desktop_apps_test)
Result: exit 0 on success; screenshots in /tmp/hobbyos_apps/.
"""
import json
import os
import select
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.abspath(__file__))
os.chdir(REPO)

QMP_SOCK = "./qmp-sock-apps"
SERIAL_LOG = "/tmp/desktop_apps_serial.log"
SHOT_DIR = "/tmp/hobbyos_apps"
QEMU_BIN = "qemu-system-aarch64"

APPS = [
    ("FILES", "FILES.BIN"),
    ("CALC", "CALC.BIN"),
    ("CLOCK", "CLOCK.BIN"),
    ("SYSMON", "SYSMON.BIN"),
    ("HEX", "HEX.BIN"),
    ("TASKS", "TASKS.BIN"),
    ("FIND", "FIND.BIN"),
    ("DIFF", "DIFF.BIN"),
    ("NOTES", "NOTES.BIN"),
    ("UNIT", "UNIT.BIN"),
]

W, H = 1024, 768
TASKBAR_Y = H - 26


def log(msg):
    print(msg, flush=True)


def kill_qemu():
    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)


def dir_order():
    """Return the FAT-16 root directory entry order (matches the desktop's
    read_dir order, which is what the start menu shows)."""
    import re
    tools = os.path.expanduser("~/.local/share/hobbyos-tools/mdir")
    if not os.path.exists(tools):
        tools = "mdir"
    out = subprocess.run([tools, "-i", "disk.img", "-a", "-f", "::/"],
                         capture_output=True, text=True).stdout
    names = []
    for line in out.splitlines():
        if not line.strip() or "bytes free" in line or " files" in line:
            continue
        if line.lstrip().startswith("/"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        if parts[1] == "<DIR>":
            names.append(parts[0].upper())
        elif len(parts) >= 3 and parts[2].isdigit():
            names.append(f"{parts[0].upper()}.{parts[1].upper()}")
    return names


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        for _ in range(120):
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
        while True:
            buf += self.sock.recv(65536)
            text = buf.decode(errors="ignore")
            for part in text.strip().split("\n"):
                if not part:
                    continue
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp
            if len(buf) > 1 << 20:
                raise RuntimeError("QMP response too large")

    def move(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.15)

    def click(self, x, y, button="left"):
        self.move(x, y)
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.08)
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": button, "down": False}}]})
        time.sleep(0.25)

    def key(self, qcode, pause=0.3):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def shot(self, path):
        r = self.cmd("screendump", {"filename": path})
        return "return" in r


def ppm_has_content(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return False
    return any(data[100:])


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    kill_qemu()
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    order = dir_order()
    idx_of = {}
    for name, binname in APPS:
        if binname in order:
            idx_of[name] = order.index(binname)
        else:
            log(f"[ERR] {binname} not found on disk image!")
            return 1
    log(f"[INFO] root entries: {len(order)}; app menu indices: "
        + ", ".join(f"{n}={idx_of[n]}" for n in idx_of))

    log("[INFO] booting desktop...")
    serial = open(SERIAL_LOG, "w")
    proc = subprocess.Popen(
        ["stdbuf", "-oL", "-eL", "make", "run", "ARCH=arm",
         f"QEMU_ARGS=-display none -qmp unix:{QMP_SOCK},server,nowait"],
        stdout=serial, stderr=subprocess.STDOUT)

    results = []
    try:
        qmp = Qmp(QMP_SOCK)

        # Wait for the desktop to boot (serial marker) + settle.
        deadline = time.time() + 45
        booted = False
        while time.time() < deadline:
            try:
                if "Desktop starting" in open(SERIAL_LOG, errors="ignore").read():
                    booted = True
                    break
            except OSError:
                pass
            time.sleep(0.5)
        if not booted:
            log("[FAIL] desktop never booted")
            return 1
        time.sleep(3)

        qmp.shot(os.path.join(SHOT_DIR, "00_desktop.ppm"))
        log(f"[INFO] desktop screenshot: {'ok' if ppm_has_content(os.path.join(SHOT_DIR, '00_desktop.ppm')) else 'BLANK!'}")

        # Ground truth: the desktop dumps its exact start-menu mapping once at
        # startup ("[MENU] <idx>=<NAME>"). Wait for it, then compute real ups.
        menu_map = {}
        deadline = time.time() + 20
        while time.time() < deadline:
            text = open(SERIAL_LOG, errors="ignore").read()
            menu_map = {}
            for line in text.splitlines():
                if "[MENU]" in line and "=" in line:
                    seg = line.split("[MENU]", 1)[1].replace("[CONSOLE]", " ").strip()
                    if seg.startswith("count="):
                        continue
                    try:
                        k, v = seg.split("=", 1)
                        menu_map[int(k)] = v.strip()
                    except ValueError:
                        pass
            if menu_map:
                break
            time.sleep(0.5)
        if not menu_map:
            log("[FAIL] desktop never reported its menu mapping")
            return 1
        last_idx = max(menu_map.keys())
        log(f"[INFO] desktop menu: {len(menu_map)} entries, last idx {last_idx}")
        for name, binname in APPS:
            idx = None
            for k, v in menu_map.items():
                if v == binname:
                    idx = k
            if idx is None:
                log(f"[ERR] {binname} missing from desktop menu!")
                return 1
            idx_of[name] = idx
            ups = last_idx - idx
            marker = f"[APP] {name} started"
            ok_marker = False
            for attempt in (1, 2):
                # Ensure no start menu is lingering, then open it fresh.
                qmp.click(5, 5)
                time.sleep(0.3)
                qmp.click(38, TASKBAR_Y + 13)
                time.sleep(0.5)
                qmp.key("end")            # selection -> last item
                for _ in range(ups):      # then up to the target row
                    qmp.key("up")
                qmp.key("ret")

                deadline = time.time() + 15
                while time.time() < deadline:
                    text = open(SERIAL_LOG, errors="ignore").read()
                    if marker in text:
                        ok_marker = True
                        break
                    if "Unknown System Call" in text:
                        break
                    time.sleep(0.3)
                if ok_marker:
                    break
                log(f"[WARN] {name}: launch attempt {attempt} did not start; retrying")

            time.sleep(1.0)
            shot_path = os.path.join(SHOT_DIR, f"{idx:02d}_{name}.ppm")
            qmp.shot(shot_path)
            has_pixels = ppm_has_content(shot_path)
            log(f"[TEST] {name}: marker={'OK' if ok_marker else 'MISSING'} "
                f"screen={'ok' if has_pixels else 'BLANK'} (menu idx {idx})")
            results.append((name, ok_marker, has_pixels))

            # Close the focused window (F4) and let the WM settle.
            qmp.key("f4")
            time.sleep(1.5)
    finally:
        kill_qemu()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    failed = [n for (n, m, p) in results if not (m and p)]
    log("")
    log(f"=== Desktop apps acceptance: {len(results) - len(failed)}/{len(results)} passed ===")
    for name, m, p in results:
        log(f"  {name:7s} marker={'OK' if m else 'MISSING'} screen={'ok' if p else 'BLANK'}")
    if failed:
        log(f"FAILED: {', '.join(failed)}")
        return 1
    log("ALL DESKTOP APP TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
