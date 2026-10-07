#!/usr/bin/env python3
"""
rp_lib.py — shared building blocks for the render-pipeline probes
(tile_bleed_probe.py, resp_close_probe.py, run_rp.sh).

Covers: WebProcess ELF sha256 pinning, per-run FAT16 disk assembly (wk5
pattern), detached QEMU boot with saved PID, a QMP client (abs pointer /
keys / wheel / screendump), serial-marker polling, and Apps-menu launching.

Run discipline (from docs/browser/render-pipeline.md):
  - one QEMU per disk image; kill only your saved PID (never bare pkill)
  - instance-scoped /tmp sockets/serial (--instance + --port for locks)
  - every step bounded by a timeout; timeouts are recorded, not retried blind
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

# ---- display / WM geometry ground truth (src/include/display_mode.h,
# ---- src/user/graphics/window.c wm_pixel_content_rect, desktop.c) --------
SCREEN_W = 1920
SCREEN_H = 1080
TASKBAR_H = 26
TASKBAR_Y = SCREEN_H - TASKBAR_H          # 1054
APPS_BTN_X = 6
APPS_BTN_W = 64
START_MENU_W = 220
START_MENU_VISIBLE = 16

# content rect for a window: (x+2, y+34, w-4, h-36) — window.c:829
CHROME_L = 2
CHROME_T = 34
CHROME_R = 2
CHROME_B = 2


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for c in iter(lambda: fh.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def content_rect(win_x: int, win_y: int, win_w: int, win_h: int):
    """WM content rect for a window rect (window.c wm_pixel_content_rect)."""
    cx, cy = win_x + CHROME_L, win_y + CHROME_T
    cw, ch = win_w - CHROME_L - CHROME_R, win_h - CHROME_T - CHROME_B
    return (cx, cy, max(cw, 1), max(ch, 1))


def tile_rect(idx: int, num_windows: int):
    """WM tiling: window idx's rect for n windows (window.c update_layout)."""
    if num_windows == 0:
        return None
    cols, rows = 1, 1
    if num_windows == 2:
        cols, rows = 2, 1
    elif num_windows in (3, 4):
        cols, rows = 2, 2
    elif num_windows <= 6:
        cols, rows = 3, 2
    elif num_windows <= 9:
        cols, rows = 3, 3
    else:
        cols, rows = 4, 4
    w = SCREEN_W // cols
    h = (SCREEN_H - TASKBAR_H) // rows
    x = (idx % cols) * w
    y = (idx // cols) * h
    return (x, y, w, h)


# ------------------------------------------------------------------ disk --
def assemble_disk(workdir: str, os_tree: str, wp_elf: str, out: str,
                  need_console: bool = True, fixtures: dict | None = None,
                  bootloader_efi: str | None = None,
                  llvm_objcopy: str = "llvm-objcopy",
                  quiet: bool = True) -> str:
    """Assemble a bootable 128 MiB FAT16 disk (run_wk5.sh pattern).

    os_tree: a scratch copy of the OS tree with hobbyos.elf + obj/arm/*.bin
             already built.  wp_elf: the WebProcess ELF to vendor as
             WEBPROC.BIN (must exist; frozen-binary baselines pin its sha).
    fixtures: {DISK_NAME: host_path} e.g. {"HOME.HTM": ..., "TALL.HTM": ...}
    Returns out.  Raises RuntimeError on any failure.
    """
    if not os.path.isfile(wp_elf):
        raise RuntimeError(f"WebProcess ELF missing: {wp_elf}")
    os_tree = os.path.abspath(os_tree)
    if not os.path.isfile(os.path.join(os_tree, "hobbyos.elf")):
        raise RuntimeError(f"os_tree has no hobbyos.elf: {os_tree}")
    for need in ("obj/arm/desktop.bin",):
        if not os.path.isfile(os.path.join(os_tree, need)):
            raise RuntimeError(f"os_tree missing built {need}")
    if need_console and not os.path.isfile(os.path.join(os_tree, "obj/arm/console.bin")):
        raise RuntimeError("os_tree missing obj/arm/console.bin (CONSOLE.BIN "
                           "needed for the tile-bleed second-window step)")

    def sh(*a, **kw):
        if not quiet:
            print("+", " ".join(str(x) for x in a), flush=True)
        r = subprocess.run(a, capture_output=True, text=True, **kw)
        if r.returncode != 0:
            raise RuntimeError(f"cmd failed ({r.returncode}): "
                               f"{' '.join(str(x) for x in a)}\n"
                               f"{r.stderr[-2000:]}")
        return r.stdout

    objcopy = llvm_objcopy or shutil.which("llvm-objcopy")
    if not objcopy:
        # fall back to clang's bundled objcopy
        r = subprocess.run(["clang", "-print-prog-name=llvm-objcopy"],
                           capture_output=True, text=True)
        objcopy = r.stdout.strip() or "llvm-objcopy"

    if os.path.exists(out):
        os.unlink(out)
    shutil.rmtree(os.path.join(workdir, "rp-disk"), ignore_errors=True)
    staging = os.path.join(workdir, "rp-disk")
    os.makedirs(staging, exist_ok=True)

    # kernel flat image (ELF -> flat) + per-app flat images (OBJ_BIN targets
    # are ALREADY flat binaries from the Makefile's llvm-objcopy rules — copy
    # them verbatim; re-running objcopy on them fails "not a valid object")
    sh(objcopy, "-O", "binary",
       os.path.join(os_tree, "hobbyos.elf"),
       os.path.join(staging, "hobbyos.bin"))
    sh(objcopy, "-O", "binary", wp_elf, os.path.join(staging, "webproc.bin"))
    shutil.copy2(os.path.join(os_tree, "obj/arm/desktop.bin"),
                 os.path.join(staging, "desktop.bin"))
    if need_console:
        shutil.copy2(os.path.join(os_tree, "obj/arm/console.bin"),
                     os.path.join(staging, "console.bin"))

    # 128 MiB FAT16 disk, 8 KiB clusters (run_wk5.sh -s 16)
    sh("dd", "if=/dev/zero", f"of={out}", "bs=1M", "count=128", "status=none")
    sh("mkfs.fat", "-F", "16", "-s", "16", out)
    for d in ("/EFI", "/EFI/BOOT", "/boot"):
        sh("mmd", "-i", out, f"::{d}")

    efi = bootloader_efi or os.path.join(os_tree, "bootloader", "BOOTAA64.EFI")
    if not os.path.isfile(efi):
        raise RuntimeError(f"bootloader EFI missing: {efi}")
    sh("mcopy", "-i", out, efi, "::/EFI/BOOT/BOOTAA64.EFI")

    limine = ("timeout: 0\ndefault_entry: 1\n\n"
              "/HobbyOS (ARM AArch64)\nprotocol: linux\n"
              "path: boot():/boot/hobbyos.bin\n")
    limine_p = os.path.join(staging, "limine.conf")
    with open(limine_p, "w") as fh:
        fh.write(limine)
    sh("mcopy", "-i", out, limine_p, "::/boot/limine.conf")
    sh("mcopy", "-i", out, os.path.join(staging, "hobbyos.bin"),
       "::/boot/hobbyos.bin")
    sh("mcopy", "-i", out, os.path.join(staging, "desktop.bin"),
       "::/DESKTOP.BIN")
    sh("mcopy", "-i", out, os.path.join(staging, "webproc.bin"),
       "::/WEBPROC.BIN")
    if need_console:
        sh("mcopy", "-i", out, os.path.join(staging, "console.bin"),
           "::/CONSOLE.BIN")
    for name, src in (fixtures or {}).items():
        if not os.path.isfile(src):
            raise RuntimeError(f"fixture missing: {name} <- {src}")
        sh("mcopy", "-i", out, src, f"::/{name}")

    # read-back proof
    rb = subprocess.run(["mcopy", "-i", out, "::/WEBPROC.BIN", "-"],
                        capture_output=True)
    if rb.returncode != 0 or len(rb.stdout) == 0:
        raise RuntimeError("WEBPROC.BIN read-back from disk failed")
    return out


# ----------------------------------------------------------------- qemu --
def boot_qemu(disk: str, qmp_sock: str, serial_log: str, pidfile: str,
              fw: str | None = None, smp: int = 4, mem_mb: int = 4096,
              timeout: int = 1200, display: str = "none",
              extra_args: list | None = None) -> int:
    """Boot the disk headless under QEMU, detached, PID saved to pidfile.

    Returns the qemu PID (the saved one).  Caller must kill_exact() it.
    """
    if fw is None:
        for cand in ("/home/sarah/.local/share/AAVMF/AAVMF_CODE.fd",
                     "/usr/share/AAVMF/AAVMF_CODE.fd"):
            if os.path.isfile(cand):
                fw = cand
                break
    if not fw or not os.path.isfile(fw):
        raise RuntimeError("AAVMF firmware not found")
    for p in (qmp_sock, serial_log, pidfile):
        if os.path.exists(p):
            try:
                os.unlink(p)
            except OSError:
                pass
    cmd = [
        "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53",
        "-smp", str(smp), "-m", f"{mem_mb}M",
        "-accel", "tcg,thread=multi", "-bios", fw,
        "-display", display,
        "-device", "virtio-gpu-device",
        "-serial", f"file:{serial_log}",
        "-qmp", f"unix:{qmp_sock},server,nowait",
        "-semihosting",
        "-drive", f"if=none,file={disk},format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56",
        "-action", "shutdown=poweroff",
    ]
    if extra_args:
        cmd += list(extra_args)
    logf = open(os.path.join(os.path.dirname(serial_log) or ".",
                             "qemu-launch.log"), "a")
    logf.write("[" + time.strftime("%H:%M:%S") + "] " + " ".join(cmd) + "\n")
    logf.flush()
    proc = subprocess.Popen(cmd, stdout=logf, stderr=logf, start_new_session=True)
    with open(pidfile, "w") as fh:
        fh.write(str(proc.pid) + "\n")
    return proc.pid


def kill_exact(pidfile: str):
    """Kill exactly the PID recorded in pidfile (never pattern-kill)."""
    try:
        with open(pidfile) as fh:
            pid = int(fh.read().strip())
    except (OSError, ValueError):
        return
    try:
        os.kill(pid, 15)
        for _ in range(20):
            try:
                os.kill(pid, 0)
                time.sleep(0.25)
            except ProcessLookupError:
                return
        os.kill(pid, 9)
    except ProcessLookupError:
        pass


# ------------------------------------------------------------------ qmp --
class Qmp:
    def __init__(self, path: str, connect_timeout: float = 240.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        deadline = time.time() + connect_timeout
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError("QMP socket never appeared: " + path)
                time.sleep(0.4)
        self.sock.recv(1 << 20)
        self.cmd("qmp_capabilities")
        self.px, self.py = SCREEN_W // 2, SCREEN_H // 2

    def cmd(self, execute: str, arguments: dict | None = None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            buf += self.sock.recv(1 << 20)
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
            if len(buf) > (1 << 20):
                raise RuntimeError("QMP response too large")

    def move(self, x: int, y: int):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x",
                                     "value": int(x * 0x7FFF / SCREEN_W)}},
            {"type": "abs", "data": {"axis": "y",
                                     "value": int(y * 0x7FFF / SCREEN_H)}}]})
        self.px, self.py = x, y
        time.sleep(0.12)

    def click(self, x: int, y: int, button: str = "left", pause: float = 0.35):
        self.move(x, y)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": False}}]})
        time.sleep(pause)

    def key(self, qcode: str, pause: float = 0.4):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def wheel(self, direction: str, ticks: int = 1):
        button = "wheel-up" if direction == "up" else "wheel-down"
        for _ in range(ticks):
            self.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"button": button, "down": True}}]})
            time.sleep(0.05)
            self.cmd("input-send-event", {"events": [
                {"type": "btn", "data": {"button": button, "down": False}}]})
            time.sleep(0.18)

    def shot(self, path: str) -> bool:
        r = self.cmd("screendump", {"filename": path})
        return "return" in r


# --------------------------------------------------------------- serial --
def serial_text(path: str) -> str:
    try:
        with open(path, errors="ignore") as fh:
            return fh.read()
    except OSError:
        return ""


def wait_marker(path: str, needle: str, timeout: float,
                poll: float = 0.4) -> tuple[bool, int]:
    """Wait for needle to appear in the serial log. Returns (found, len)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        t = serial_text(path)
        if needle in t:
            return True, len(t)
        time.sleep(poll)
    return False, len(serial_text(path))


def menu_map(serial_log: str, timeout: float = 90.0) -> dict:
    out = {}
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in serial_text(serial_log).splitlines():
            if "[MENU]" not in line or "=" not in line:
                continue
            seg = line.split("[MENU]", 1)[1]
            if "count=" in seg:
                continue
            for tok in seg.replace("[CONSOLE]", "").split():
                if "=" in tok:
                    try:
                        k, v = tok.split("=", 1)
                        out[int(k)] = v.strip()
                    except ValueError:
                        pass
        if out:
            return out
        time.sleep(0.5)
    return out


def launch_from_menu(qmp: Qmp, menu: dict, name: str) -> bool:
    """Open the Apps menu and launch `name` (run_wk5_drive pattern)."""
    idx = None
    for k, v in menu.items():
        if v == name:
            idx = k
            break
    if idx is None:
        return False
    top = max(menu, default=0)
    ups = top - idx
    qmp.click(APPS_BTN_X + APPS_BTN_W // 2, TASKBAR_Y + 13)  # Apps button
    time.sleep(0.6)
    qmp.key("end", pause=0.5)
    for _ in range(ups):
        qmp.key("up", pause=0.3)
    qmp.key("ret", pause=0.5)
    return True


def parse_win_log(serial_log: str) -> dict:
    """Thin [WIN] line extractor for probes (perf_receipts.py is the
    authoritative parser; this returns just what probes need to know)."""
    t = serial_text(serial_log)
    geoms = []
    for m in re.finditer(r"\[WIN\] geom x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)",
                         t):
        geoms.append((int(m.group(1)), int(m.group(2)),
                      int(m.group(3)), int(m.group(4))))
    return {
        "geoms": geoms,
        "geom": geoms[-1] if geoms else None,
        "load_ok": len(re.findall(r"\[WIN\] load-ok url=", t)),
        "close_ok": "[WIN] close ok" in t,
        "exit_rc0": "[WIN] exit rc=0" in t,
        "frames": len(re.findall(r"\[WIN\] frame view=", t)),
        "frame_times": len(re.findall(r"\[WIN\] frame-times render=", t)),
        "reblits": len(re.findall(r"\[WIN\] reblit cached", t)),
        "viewport_apply": len(re.findall(r"\[WIN\] viewport apply", t)),
    }


# ------------------------------------------------------------- PPM I/O --
def read_ppm(path: str) -> tuple[int, int, bytes]:
    """Read a P6 PPM -> (w, h, pixel bytes RGB, 3*B/px).  Raises on bad."""
    with open(path, "rb") as fh:
        head = fh.readline().strip()
        if head not in (b"P6", b"P6\n"):
            raise ValueError(f"not a P6 ppm: {head!r} in {path}")
        while True:
            ln = fh.readline()
            if ln.strip().startswith(b"#"):
                continue
            parts = ln.split()
            if len(parts) >= 2:
                break
        w, h = int(parts[0]), int(parts[1])
        maxv = int(fh.readline().split()[0])
        if maxv != 255:
            raise ValueError(f"maxval {maxv} != 255 in {path}")
        data = fh.read()
    if len(data) != w * h * 3:
        raise ValueError(f"{path}: expected {w * h * 3} bytes, got {len(data)} "
                         f"(is this a 1920x1080 RGB PPM?)")
    return w, h, data


def diff_pixels(a: bytes, b: bytes) -> list:
    """Return list of (idx, rgb_a, rgb_b) where pixel(idx) differs.
    idx = row*W + col (caller maps to x,y with its own W).  Uses
    struct.iter_unpack (C-speed) so a full 2 MPix frame diffs in ~0.4 s."""
    import struct
    if len(a) != len(b):
        raise ValueError("frame size mismatch")
    n = len(a) // 3
    ita = struct.iter_unpack("<BBB", a)
    itb = struct.iter_unpack("<BBB", b)
    out = []
    j = 0
    for pa, pb in zip(ita, itb):
        if pa != pb:
            out.append((j, pa, pb))
        j += 1
        if len(out) >= 5_000_000:
            break
    return out
