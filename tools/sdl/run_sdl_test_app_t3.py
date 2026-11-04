#!/usr/bin/env python3
"""run_sdl_test_app_t3.py — GB-032's T3 integration leg, one boot.

Boots the intel guest in MODE=test with `SDL2TEST.BIN` as the only wave member
(a ::WAVE.CFG manifest), waits for the app's framebuffer line, sends one real
key over QMP, and requires the app to echo that `input_event` back on the
console before the machine halts.

This is the runtime half of the card's DoD ("draws + echoes input"): the host
suite (sdl_test_app_test_host) proves the ELF properties and the drawing math,
and this script proves the compat device path — open /dev/fb0 ->
FBIOGET_VSCREENINFO -> mmap, and read /dev/input/event0 — actually runs.

Usage:
    python3 tools/sdl/run_sdl_test_app_t3.py [--key a] [--timeout 240]

Exit 0 only when all three are present in the captured console:
    "sdl test app: fb /dev/fb0"          (the real node was opened and mapped)
    "sdl test app echo: KEY <code>=1"    (a QMP key became an input_event)
    "[EXITPC]" ... "code=0x0"            (the app exited cleanly)

The serial log, QMP replay and screendump stay under evidence/gb032/<utc>/.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(ROOT)

APP = "obj/sdl_test_app_pie"
QMP_SOCK = "/tmp/gb032-qmp.sock"


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(15)
        for _ in range(200):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.25)
        else:
            raise RuntimeError("QMP socket never appeared: " + path)
        self.sock.recv(65536)
        self.cmd("qmp_capabilities")
        self.replay = []

    def cmd(self, name, args=None):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                return None
            buf += chunk
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    reply = json.loads(part)
                except ValueError:
                    continue
                if "return" in reply or "error" in reply:
                    return reply

    def key(self, qcode):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        self.replay.append({"t": "key", "k": qcode})

    def screendump(self, path):
        self.cmd("screendump", {"filename": path})
        time.sleep(0.4)
        self.replay.append({"t": "screendump", "path": path})


def read_log(path):
    try:
        with open(path, errors="ignore") as f:
            return f.read()
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", default="a", help="QMP qcode to send (default: a)")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--no-build", action="store_true",
                    help="use the existing obj/sdl_test_app_pie and disk.img "
                         "as-is (deliberate A/B only: no rebuild, no freshness "
                         "check). The default path builds both.")
    ap.add_argument("--no-gpu", action="store_true",
                    help="boot without the virtio-gpu device. On the merged "
                         "kernel the BGA/std-VGA path presents too, so this is "
                         "a SECOND present path, not a negative control (the "
                         "negative control is the pre-GB-032 tree); it does not "
                         "make the screendump black.")
    args = ap.parse_args()

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    outdir = args.outdir or os.path.join("evidence", "gb032", stamp)
    os.makedirs(outdir, exist_ok=True)
    serial_path = os.path.join(outdir, "serial.log")
    shot_path = os.path.join(outdir, "draw.ppm")

    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]x86_64"],
                   stderr=subprocess.DEVNULL)
    for p in (QMP_SOCK, serial_path):
        if os.path.exists(p):
            os.remove(p)

    # A gate must grade THIS tree.  GB-033-V's 2026-11-04 RED was a stale
    # obj/sdl_test_app_pie: the old script built the app only when the file was
    # absent, so it staged the pre-PR device-free binary — no `fb /dev/fb0`
    # line, the literal `ABS 00=512` echo, an instant exit (hence the
    # screendump's Broken pipe) and blue=0.  GB-189: a worktree carrying a
    # pre-#351 disk.img was the same failure one layer down.
    #
    # The default path force-builds the app and then hobbyos.elf + disk.img.
    # Because disk.img depends on $(SDL_APP_PIE), force-building the app makes
    # the image out of date, so the image is regenerated on every default run:
    # a copied stale image cannot survive to the boot.  --no-build is the
    # deliberate A/B opt-out and skips both.
    env = dict(os.environ, TOOLS_SKIP="1")
    head = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    if args.no_build:
        missing = [p for p in (APP, "disk.img") if not os.path.exists(p)]
        if missing:
            print("run_sdl_test_app_t3: --no-build — missing "
                  + ", ".join(missing) + "; nothing to grade")
            return 2
        print(f"run_sdl_test_app_t3: --no-build A/B, tree {head or '?'}, "
              f"app {os.path.abspath(APP)} (no rebuild, no freshness check)",
              flush=True)
    else:
        build_log = os.path.join(outdir, "build.log")
        img_before = (os.path.getmtime("disk.img")
                      if os.path.exists("disk.img") else None)
        with open(build_log, "w") as bl:
            for cmdline in (["make", "-B", "ARCH=intel", "MODE=test",
                             "TOOLS_SKIP=1", APP],
                            ["make", "-j", str(os.cpu_count() or 4),
                             "ARCH=intel", "MODE=test", "TOOLS_SKIP=1",
                             "hobbyos.elf", "disk.img"]):
                r = subprocess.run(cmdline, env=env, stdout=bl,
                                   stderr=subprocess.STDOUT)
                if r.returncode != 0:
                    print("run_sdl_test_app_t3: BUILD FAILED — " +
                          " ".join(cmdline), flush=True)
                    print("".join(open(build_log, errors="ignore").read()
                                  .splitlines(True)[-30:]), flush=True)
                    return 2
        img_after = os.path.getmtime("disk.img")
        print(f"run_sdl_test_app_t3: tree {head or '?'}, "
              f"app {os.path.abspath(APP)}, disk.img "
              f"{'REBUILT' if img_after != img_before else 'up to date'} "
              f"(mtime {datetime.fromtimestamp(img_after, timezone.utc):%H:%M:%SZ}), "
              f"build log {build_log}", flush=True)
        with open(APP, "rb") as f:
            if b"/dev/fb0" not in f.read():
                print("run_sdl_test_app_t3: APPARATUS — the built app has no "
                      "/dev/fb0 path; refusing to grade a device-free binary")
                return 2

    cfg = os.path.join(outdir, "WAVE.CFG")
    with open(cfg, "w") as f:
        f.write("SDL2TEST.BIN\n")
    for src, dst in ((APP, "::SDL2TEST.BIN"), (cfg, "::WAVE.CFG")):
        subprocess.run(["mcopy", "-o", "-i", "disk.img", src, dst], check=True)

    # Boot QEMU directly rather than through `make run`: the make recipe forces
    # `-serial stdio`, and with stdout redirected QEMU's serial output arrives
    # in bursts (measured: the "drew" line was invisible for 20+ s, so the QMP
    # key missed the app's poll window).  `-serial file:` writes each chunk as
    # it happens.  The device set is the Makefile's intel line, unchanged.
    fw = os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd")
    cmd = ["qemu-system-x86_64", "-M", "q35,vmport=on", "-smp", "8",
           "-m", "12288M", "-pflash", fw, "-display", "none",
           "-serial", f"file:{serial_path}",
           "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
           "-drive", "file=disk.img,format=raw,id=disk0,if=none",
           "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
           "-device", "edu", "-netdev", "user,id=net0",
           "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
           "-action", "shutdown=poweroff",
           "-qmp", f"unix:{QMP_SOCK},server,nowait"]
    if not args.no_gpu:
        # lane-G2: with -vga none the virtio scanout is console 0, so QMP
        # screendump captures the surface the kernel draws into.
        cmd[-4:-4] = ["-device", "virtio-gpu-pci", "-vga", "none"]
    if os.path.exists("/dev/kvm"):
        cmd.append("-enable-kvm")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    q = QMP(QMP_SOCK)

    deadline = time.time() + args.timeout
    t0 = time.time()
    drew = False
    echoed = False
    halted = False
    sent = False
    shot = False
    while time.time() < deadline:
        log = read_log(serial_path)
        if not shot and "sdl test app: drew" in log:
            # Capture the presented frame BEFORE sending the key: the app
            # exits as soon as it sees the key, and QEMU powers off with it.
            print(f"[{time.time() - t0:.1f}s] draw line seen; screendump",
                  flush=True)
            time.sleep(0.3)
            try:
                q.screendump(shot_path)
            except OSError as e:
                print(f"  screendump failed: {e}", flush=True)
            shot = True
            time.sleep(0.2)
            print(f"[{time.time() - t0:.1f}s] sending key {args.key!r}",
                  flush=True)
            try:
                q.key(args.key)
            except OSError as e:
                print(f"  QMP key failed: {e}", flush=True)
            sent = True
        drew = drew or "sdl test app: drew" in log
        echoed = echoed or "sdl test app echo: KEY" in log
        halted = halted or "System halt" in log
        if halted and echoed:
            break
        if proc.poll() is not None and "System halt" in log:
            break
        time.sleep(0.5)

    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        subprocess.run(["pkill", "-9", "-f", "qemu-system[-]x86_64"],
                       stderr=subprocess.DEVNULL)
        proc.wait(timeout=10)

    log = read_log(serial_path)
    with open(os.path.join(outdir, "replay.json"), "w") as f:
        json.dump(q.replay, f, indent=1)

    # The screendump is part of the verdict, not decoration: "shows a window"
    # must be pixels.  The app clears its 64x48 window to blue and blits a
    # magenta/black checkered 8x8 tile at (10,10), so (0,0) is blue, (11,10)
    # is magenta, and the window contributes 64*48-64 blue pixels.
    blue = magenta = 0
    if os.path.exists(shot_path):
        raw = open(shot_path, "rb").read()
        head = raw.split(b"\n", 3)
        w, h = map(int, head[1].split())
        body = head[3]
        for y in range(0, min(h, 48)):
            base = y * w * 3
            for x in range(0, min(w, 64)):
                i = base + x * 3
                if body[i:i + 3] == b"\x00\x40\xff":
                    blue += 1
                elif body[i:i + 3] == b"\xff\x00\xff":
                    magenta += 1

    rows = [ln for ln in log.splitlines()
            if "sdl test app" in ln or "[EXITPC]" in ln
            or "[WAVE-TRY]" in ln or "[ARRIVED]" in ln or "System halt" in ln]
    print("--- sdl test app T3 rows ---")
    for ln in rows:
        print(ln)
    if "sdl test app: fb unavailable" in log:
        print("--- NOTE: the app ran the DEVICE-FREE fallback (no /dev/fb0). "
              "The staged binary or the guest image does not match this tree; "
              "that is not a draw result. ---")
    print(f"--- screendump: blue_window_px={blue} magenta_tile_px={magenta} "
          f"({shot_path}) ---")
    print(f"--- artifacts: {outdir} ---")

    ok = ("sdl test app: fb /dev/fb0" in log and drew and echoed
          and "code=0x0" in log and halted
          and blue >= (64 * 48 - 64) and magenta >= 24)
    print("run_sdl_test_app_t3: " + ("PASS" if ok else "FAIL"))
    if not ok:
        print(f"  fb_line={'sdl test app: fb /dev/fb0' in log} drew={drew} "
              f"echoed={echoed} exit0={'code=0x0' in log} halted={halted} "
              f"blue={blue} magenta={magenta}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
