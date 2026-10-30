#!/usr/bin/env python3
"""
Headless boot test for the IceWM secondary desktop (icewm.md §4.5, M5/M6).

Builds a DESKTOP=icewm disk, boots it in QEMU (intel: KVM when /dev/kvm
exists), follows the XSESSION.BIN serial log ("[XSESS] ..." lines) and takes
QMP screenshots.  Every wait is bounded; only the QEMU this script started is
ever killed (never pkill — other VMs may be running).

Scenarios:
  --stubs  (M5) X binaries are the stub programs from `make icewm-stubs`
           (src/user/x11sess/xstub.c): the stub "server" lives 4 s and exits
           3, so the run must show the whole session path AND the fallback:
             kernel launches XSESSION.BIN -> env + /tmp/.X11-unix -> spawns
             XHOBBY.BIN (args/env visible in the stub's log) -> spawns
             ICEWMBG.BIN + ICEWM.BIN -> server dies -> WM stopped ->
             "[XSESS] FALLBACK" -> DESKTOP.BIN draws the HobbyOS desktop.
  default  (M6) the real X stack from X11_BUILD_DIR (default
           third_party/x11/build/<arch>): the server must become ready, the
           session must stay up (no FALLBACK) for --settle seconds and the
           screen must not be black.  IceWM-specific pixel checks (taskbar,
           start menu) belong to the integ lane once IceWM draws.

Usage:
  python3 run_icewm_test.py --stubs            # or: make icewm_test ICEWM_TEST_ARGS=--stubs
  python3 run_icewm_test.py [--arch arm] [--x11-build-dir DIR] [--no-build]
Result: exit 0 on success; screenshots + serial log in --out (default
/tmp/hobbyos_icewm_<arch>/).
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.abspath(__file__))
W, H = 1920, 1080
TASKBAR_H = 26


def log(msg):
    print(msg, flush=True)


# ------------------------------------------------------------------ QMP ---
class Qmp:
    def __init__(self, path, deadline_s=30):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        end = time.time() + deadline_s
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > end:
                    raise RuntimeError("QMP socket never appeared")
                time.sleep(0.25)
        self.buf = b""
        self._line()  # greeting
        self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            resp = self._line()
            if "return" in resp or "error" in resp:
                return resp

    def shot(self, path):
        return "return" in self.cmd("screendump", {"filename": path})


def ppm_pixels(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        return None, 0, 0
    fields, i = [], 2
    while len(fields) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    return data[i + 1:], fields[0], fields[1]


def screen_stats(path):
    """(non-black fraction, distinct colours in a sample, bottom strip differs
    from the screen middle)."""
    px, w, h = ppm_pixels(path)
    if not px or w * h * 3 > len(px):
        return 0.0, 0, False
    step = 7
    nonblack = total = 0
    colours = set()
    for off in range(0, w * h * 3, 3 * step):
        p = px[off:off + 3]
        total += 1
        if p != b"\x00\x00\x00":
            nonblack += 1
        if len(colours) < 4096:
            colours.add(p)
    row = lambda y: px[(y * w) * 3:(y * w + w) * 3]
    bottom = row(h - TASKBAR_H // 2)
    middle = row(h // 2)
    return nonblack / max(total, 1), len(colours), bottom != middle


def save_png(ppm, png):
    if shutil.which("convert"):
        subprocess.run(["convert", ppm, png], stderr=subprocess.DEVNULL)


# ---------------------------------------------------------------- build ---
def build(args):
    mk = ["make", f"ARCH={args.arch}", f"-j{args.jobs}"]
    if args.stubs:
        log("[TEST] building stub X binaries (make icewm-stubs)")
        subprocess.run(mk + ["icewm-stubs"], cwd=REPO, check=True)
    extra = [f"X11_BUILD_DIR={args.x11_build_dir}"] if args.x11_build_dir else []
    extra += args.make_arg
    log(f"[TEST] building DESKTOP=icewm disk.img {' '.join(extra)}")
    r = subprocess.run(mk + ["DESKTOP=icewm"] + extra + ["disk.img"], cwd=REPO)
    if r.returncode != 0:
        log("[TEST] FAIL: DESKTOP=icewm disk.img build failed")
        sys.exit(2)


def qemu_cmd(args, work, disk):
    qmp = os.path.join(work, "qmp-sock")
    serial = os.path.join(work, "serial.log")
    if args.arch == "intel":
        fw_src = os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd")
        fw = os.path.join(work, "ovmf.fd")
        shutil.copy(fw_src, fw)
        # Same device set as the lane VM helper (tools/boot.sh): the default
        # std-VGA q35 config without edu/virtio-gpu did not reach the kernel.
        cmd = ["qemu-system-x86_64", "-M", "q35", "-smp", "4", "-m", "8192M",
               "-drive", f"if=pflash,format=raw,file={fw}",
               "-display", "none", "-serial", f"file:{serial}",
               "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
               "-drive", f"file={disk},format=raw,id=disk0,if=none",
               "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
               "-device", "edu", "-netdev", "user,id=net0",
               "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
               "-device", "virtio-gpu-pci", "-vga", "none",
               "-action", "shutdown=poweroff",
               "-qmp", f"unix:{qmp},server,nowait"]
        if os.path.exists("/dev/kvm") and not os.environ.get("SKIP_KVM"):
            cmd.append("-enable-kvm")
    else:
        fw = os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd")
        cmd = ["qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53", "-smp", "4",
               "-m", "8192M", "-accel", "tcg,thread=multi", "-bios", fw,
               "-display", "none", "-serial", f"file:{serial}",
               "-drive", f"if=none,file={disk},format=raw,id=hd0",
               "-device", "virtio-blk-device,drive=hd0",
               "-device", "virtio-gpu-device", "-device", "virtio-keyboard-device",
               "-device", "virtio-tablet-device",
               "-netdev", "user,id=net0",
               "-device", "virtio-net-device,netdev=net0,mac=52:54:00:12:34:56",
               "-semihosting", "-action", "shutdown=poweroff",
               "-qmp", f"unix:{qmp},server,nowait"]
    return cmd, qmp, serial


class Serial:
    def __init__(self, path):
        self.path = path

    def text(self):
        try:
            return open(self.path, "rb").read().decode(errors="replace")
        except OSError:
            return ""

    def wait(self, needle, deadline, proc, start=0):
        """Index just past `needle` (searched from `start`), or -1 when the
        deadline passes / QEMU exits first."""
        while time.time() < deadline:
            t = self.text()
            i = t.find(needle, start)
            if i >= 0:
                return i + len(needle)
            if proc.poll() is not None:
                return -1
            time.sleep(0.2)
        return -1


# ----------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--arch", default="intel", choices=["intel", "arm"])
    ap.add_argument("--stubs", action="store_true", help="M5 stub scenario (see docstring)")
    ap.add_argument("--x11-build-dir", default=None)
    ap.add_argument("--no-build", action="store_true", help="boot the existing disk.img as is")
    ap.add_argument("--make-arg", action="append", default=[],
                    help="extra make argument (repeatable), e.g. STAGE_TOOLS=0")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=0,
                    help="boot deadline in seconds (default 120 intel / 900 arm)")
    ap.add_argument("--settle", type=int, default=20,
                    help="real scenario: seconds the session must stay up")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    if args.stubs and not args.x11_build_dir:
        args.x11_build_dir = f"obj/{args.arch}/x11stub"
    boot_to = args.timeout or (120 if args.arch == "intel" else 900)
    out = args.out or f"/tmp/hobbyos_icewm_{args.arch}"
    os.makedirs(out, exist_ok=True)

    if not args.no_build:
        build(args)

    work = tempfile.mkdtemp(prefix="icewm_test_")
    disk = os.path.join(work, "disk.img")
    shutil.copy(os.path.join(REPO, "disk.img"), disk)
    cmd, qmp_path, serial_path = qemu_cmd(args, work, disk)
    log("[TEST] " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(work, "qemu.err"), "w"))
    failures = []
    shots = []
    try:
        qmp = Qmp(qmp_path)
        ser = Serial(serial_path)
        t0 = time.time()
        deadline = t0 + boot_to

        def expect(needle, what, start=0, dl=None):
            i = ser.wait(needle, dl or deadline, proc, start)
            if i < 0:
                failures.append(f"missing serial marker: {what!r} ({needle!r})")
                log(f"[TEST] FAIL {what}")
            else:
                log(f"[TEST] ok   {what}  (+{time.time() - t0:.1f}s)")
            return max(i, start)

        def shot(name):
            ppm = os.path.join(out, name + ".ppm")
            if qmp.shot(ppm):
                time.sleep(0.5)
                save_png(ppm, os.path.join(out, name + ".png"))
                shots.append(ppm)
                frac, ncol, strip = screen_stats(ppm)
                log(f"[TEST] shot {name}: non-black {frac:.2f}, colours {ncol}, "
                    f"bottom-strip-differs {strip}")
                return frac, ncol, strip
            failures.append(f"screendump {name} failed")
            return 0.0, 0, False

        pos = expect("Mode: DESKTOP (icewm)", "kernel built with DESKTOP=icewm")
        pos = expect("[XSESS] IceWM session launcher starting", "XSESSION.BIN started", pos)
        pos = expect("[XSESS] spawned /XHOBBY.BIN", "X server spawned", pos)
        if args.stubs:
            expect("[XSTUB] role=server argc=6 argv: XHOBBY.BIN :0 -nolisten tcp",
                   "server stub got the argv table", pos)
            expect("[XSTUB]   DISPLAY=:0", "server stub sees DISPLAY=:0", pos)
            expect("[XSTUB]   FONTCONFIG_FILE=/X11/FONTS.CNF", "server stub sees FONTCONFIG_FILE", pos)
            pos = expect("[XSESS] spawned /ICEWM.BIN", "window manager spawned", pos)
            expect("[XSTUB] role=wm", "wm stub ran", pos)
            shot("1-session")
            pos = expect("[XSESS] X server pid=", "server exit reaped", pos)
            expect("exited status=3", "server exit status 3 reported", pos)
            pos = expect("[XSESS] FALLBACK", "fallback to DESKTOP.BIN", pos)
            pos = expect("Desktop starting...", "HobbyOS desktop started", pos)
            # Let the desktop paint, then check the screen is the desktop.
            time.sleep(5 if args.arch == "intel" else 30)
            frac, ncol, strip = shot("2-fallback-desktop")
            if frac < 0.5 or ncol < 3 or not strip:
                failures.append("fallback desktop screenshot does not look like the "
                                "HobbyOS desktop (black/flat screen or no taskbar)")
            if "role=wm" in ser.text() and "window manager pid=" not in ser.text():
                failures.append("wm stub was not stopped/reaped before the fallback")
        else:
            pos = expect("[XSESS] server ready", "X server accepted on /tmp/.X11-unix/X0", pos)
            pos = expect("[XSESS] session running", "IceWM session running", pos)
            end = time.time() + args.settle
            while time.time() < end and proc.poll() is None:
                if "[XSESS] FALLBACK" in ser.text():
                    break
                time.sleep(0.5)
            if "[XSESS] FALLBACK" in ser.text():
                failures.append("session fell back to DESKTOP.BIN (see serial log)")
            frac, ncol, strip = shot("1-icewm")
            if frac < 0.05 or ncol < 2:
                failures.append("IceWM session screenshot is black/flat")
        if proc.poll() is not None:
            failures.append(f"QEMU exited early (rc={proc.returncode})")
    except Exception as e:  # bounded harness: report, never hang
        failures.append(f"harness error: {e}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
        out_serial = os.path.join(out, "serial.log")
        try:
            shutil.copy(serial_path, out_serial)
        except OSError:
            open(out_serial, "w").close()
        try:
            err = open(os.path.join(work, "qemu.err"), errors="replace").read().strip()
        except OSError:
            err = ""
        if err:
            log("[TEST] qemu stderr: " + err[-600:])
        shutil.rmtree(work, ignore_errors=True)

    log("[TEST] serial log: " + out_serial)
    for line in open(out_serial, errors="replace"):
        if line.startswith(("[XSESS]", "[XSTUB]")):
            log("   | " + line.rstrip())
    if failures:
        for f in failures:
            log("[TEST] FAIL: " + f)
        log("[TEST] RESULT: FAIL")
        return 1
    log("[TEST] RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
