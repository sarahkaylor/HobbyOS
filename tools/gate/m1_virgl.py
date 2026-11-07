#!/usr/bin/env python3
"""tools/gate/m1_virgl.py -- the M1 virgl gate (docs/gpu-abi.md §6).

Two boots of the REAL desktop kernel, one tier at a time:

  A. 3D (the deliverable): `-device virtio-gpu-gl-pci -vga none` with
     `-display egl-headless,gl=on`.  The kernel must negotiate
     VIRTIO_GPU_F_VIRGL, run the kernel-only probe (no Mesa) -- CTX_CREATE,
     RESOURCE_CREATE_3D 64x64, ATTACH_BACKING, TRANSFER_TO_HOST_3D,
     fenced SUBMIT_3D of a virgl CLEAR_TEXTURE, fence wait,
     TRANSFER_FROM_HOST_3D -- and assert all 4096 readback pixels.  Graded
     rows: every `[VIRGL3D]` step with its device response type, the
     `match=4096/4096` pixel assertion, `RESULT PASS`, and QEMU's
     `virtio_gpu_features virgl 1` trace (the host's view of the same
     negotiation).  The desktop must still come up afterwards.

  B. 2D-only regression (hard gate): `-device virtio-gpu-pci -vga none` with
     `-display none`.  The same kernel/image must print `VirtIO GPU:
     virtio-pci`, start the desktop, present through the 2D path
     (`virtio_gpu_cmd_res_flush` trace > 0), produce a QMP screendump with
     visible content, and log **no** `[VIRGL3D]` row at all -- proof the 3D
     code is behind the negotiated capability.

Build first (`TOOLS_SKIP=1`, the tools stage is already pinned) unless
M1_SKIP_BUILD=1.  Never run this concurrently with the unit tier: both own
disk.img and the console.

Exit 0 only when every enabled check passed.  Evidence is the serial logs,
the QEMU traces and the 2D screenshot under obj/m1_virgl/.
"""
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARCH = os.environ.get("M1_ARCH", "intel")
IMG = os.path.join(ROOT, "disk.img")
OUT = os.environ.get("M1_OUT", os.path.join(ROOT, "obj", "m1_virgl"))
QEMU = os.environ.get("M1_QEMU", "qemu-system-x86_64")
OVMF = os.environ.get("M1_OVMF", os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"))
BOOT_TIMEOUT = int(os.environ.get("M1_BOOT_TIMEOUT", "120"))
SKIP_BUILD = os.environ.get("M1_SKIP_BUILD", "0") == "1"
SKIP_3D = os.environ.get("M1_SKIP_3D", "0") == "1"
SKIP_2D = os.environ.get("M1_SKIP_2D", "0") == "1"

results = []


def log(msg):
    print(msg, flush=True)


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    log(("  PASS  " if ok else "  FAIL  ") + name + (("  [" + detail + "]") if detail else ""))
    return ok


def run(cmd, **kw):
    return subprocess.run(cmd, **kw)


def build():
    if SKIP_BUILD:
        log("m1: M1_SKIP_BUILD=1, using the existing kernel + disk image")
        return True
    log("m1: building kernel + desktop disk image (TOOLS_SKIP=1)...")
    env = dict(os.environ, TOOLS_SKIP="1")
    r = run(["make", "-j" + str(os.cpu_count() or 4), "ARCH=" + ARCH, "MODE=desktop",
             "DESKTOP=hobby", "hobbyos.elf", "disk.img"],
            cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        log(r.stdout[-4000:] if r.stdout else "(no build output)")
        return False
    return True


def qemu_common(tag, display, device, extra=None):
    args = [
        QEMU, "-M", "q35,vmport=on", "-smp", "8", "-m", "12288M",
        "-pflash", OVMF,
        "-display", display,
        "-serial", "file:" + os.path.join(OUT, tag + ".serial.log"),
        "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
        "-drive", "file=" + IMG + ",format=raw,id=disk0,if=none",
        "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1", "-device", "edu",
        "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
        "-device", device, "-vga", "none", "-enable-kvm",
        "-D", os.path.join(OUT, tag + ".trace.log"),
        "-trace", "enable=virtio_gpu_features",
        "-trace", "enable=virtio_gpu_cmd_res_flush",
        "-action", "shutdown=poweroff",
    ]
    if extra:
        args += extra
    return args


def wait_for(path, needle, timeout, proc=None, also=None):
    """Poll `path` for `needle`; returns the file text (or None on timeout).
    `also` is an optional second needle that also satisfies the wait."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "rb") as f:
                data = f.read().decode("utf-8", "replace")
        except FileNotFoundError:
            data = ""
        if needle in data or (also and also in data):
            return data
        if proc is not None and proc.poll() is not None:
            return data
        time.sleep(0.5)
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return None


def stop(proc):
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass


def boot_3d():
    tag = "gl"
    serial = os.path.join(OUT, tag + ".serial.log")
    trace = os.path.join(OUT, tag + ".trace.log")
    for f in (serial, trace):
        try:
            os.remove(f)
        except FileNotFoundError:
            pass
    log("m1: boot A -- virtio-gpu-gl-pci + egl-headless,gl=on")
    proc = subprocess.Popen(qemu_common(tag, "egl-headless,gl=on", "virtio-gpu-gl-pci"),
                            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    text = wait_for(serial, "[VIRGL3D] RESULT", BOOT_TIMEOUT, proc)
    if text is None:
        text = ""
    # The desktop must still come up on the GL device (2D path untouched).
    if "[VIRGL3D] RESULT" in text and "Desktop starting..." not in text:
        text = wait_for(serial, "Desktop starting...", 45, proc) or text
    stop(proc)
    log("m1: boot A stopped")

    def has(pat):
        return re.search(pat, text) is not None

    check("3d: VIRTIO_GPU_F_VIRGL negotiated (guest row)", has(r"\[VIRGL3D\] feat virgl=1 ctx_init=0"))
    check("3d: GET_CAPSET_INFO answered (0x1102)", has(r"capset_info id=1 .*resp=0x0*1102"))
    check("3d: GET_CAPSET answered (0x1103)", has(r"capset id=1 .*resp=0x0*1103"))
    check("3d: CTX_CREATE ok (0x1100)", has(r"ctx_create ctx=\d+ .*resp=0x0*1100"))
    check("3d: RESOURCE_CREATE_3D ok (0x1100)", has(r"res_create res=\d+ 64x64 .*resp=0x0*1100"))
    check("3d: ATTACH_BACKING ok (0x1100, 4 pages)", has(r"attach res=\d+ pages=4 .*resp=0x0*1100"))
    check("3d: CTX_ATTACH_RESOURCE ok (0x1100)", has(r"ctx_attach ctx=\d+ res=\d+ resp=0x0*1100"))
    check("3d: SUBMIT_3D ok (0x1100, fenced)", has(r"submit ctx=\d+ bytes=52 fence=1 echoed=1 resp=0x0*1100"))
    check("3d: fence retired", has(r"fence id=1 retired=1"))
    check("3d: readback pixels == clear colour (4096/4096)",
          has(r"readback px00=0x0*FF00FF00 .*match=4096/4096"))
    check("3d: RESULT PASS", has(r"\[VIRGL3D\] RESULT PASS"))
    check("3d: desktop still starts on the GL device", "Desktop starting..." in text)

    ttext = ""
    try:
        ttext = open(trace, "r", errors="replace").read()
    except FileNotFoundError:
        pass
    check("3d: QEMU trace says virgl 1", re.search(r"virtio_gpu_features virgl 1\b", ttext) is not None,
          "" if ttext else "trace file missing")

    # The timing rows are evidence, not a gate: print them for the report.
    for line in text.splitlines():
        if "timing" in line:
            log("    " + line.strip())


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(path)
        self.f = self.sock.makefile("rwb")
        self._read()

    def _read(self):
        line = self.f.readline()
        if not line:
            raise RuntimeError("QMP closed")
        return json.loads(line.decode())

    def cmd(self, name, **args):
        self.f.write((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
        self.f.flush()
        while True:
            msg = self._read()
            if "return" in msg or "error" in msg:
                return msg

    def close(self):
        try:
            self.f.close()
            self.sock.close()
        except Exception:
            pass


def boot_2d():
    tag = "2d"
    serial = os.path.join(OUT, tag + ".serial.log")
    trace = os.path.join(OUT, tag + ".trace.log")
    shot = os.path.join(OUT, tag + ".ppm")
    qmp_path = os.path.join(OUT, "qmp-" + tag + ".sock")
    for f in (serial, trace, shot):
        try:
            os.remove(f)
        except FileNotFoundError:
            pass
    try:
        os.remove(qmp_path)
    except FileNotFoundError:
        pass

    log("m1: boot B -- virtio-gpu-pci (2D-only) + -display none")
    proc = subprocess.Popen(
        qemu_common(tag, "none", "virtio-gpu-pci",
                    ["-qmp", "unix:" + qmp_path + ",server,nowait"]),
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    text = wait_for(serial, "Desktop starting...", BOOT_TIMEOUT, proc) or ""
    # Let the desktop paint and present a frame before the screendump.
    time.sleep(6)
    shot_ok = False
    qmp_err = ""
    try:
        q = QMP(qmp_path)
        q.cmd("qmp_capabilities")
        r = q.cmd("screendump", filename=shot, format="ppm")
        shot_ok = "return" in r
        if not shot_ok:
            qmp_err = json.dumps(r)
        q.close()
    except Exception as e:  # noqa: BLE001 - the gate reports, never crashes
        qmp_err = str(e)
    stop(proc)
    log("m1: boot B stopped")

    check("2d: driver bound the 2D device", "VirtIO GPU: virtio-pci" in text)
    check("2d: desktop started", "Desktop starting..." in text)
    ttext = ""
    try:
        ttext = open(trace, "r", errors="replace").read()
    except FileNotFoundError:
        pass
    flushes = len(re.findall(r"virtio_gpu_cmd_res_flush", ttext))
    check("2d: /dev/fb0 scanout presents (RESOURCE_FLUSH trace > 0)", flushes > 0,
          "flushes=%d" % flushes)
    check("2d: no [VIRGL3D] row (3D stays behind the capability)",
          "[VIRGL3D]" not in text)
    check("2d: QEMU trace says virgl 0",
          re.search(r"virtio_gpu_features virgl 0\b", ttext) is not None)

    colors = 0
    if shot_ok and os.path.exists(shot):
        try:
            from PIL import Image
            im = Image.open(shot).convert("RGB")
            colors = len(im.getcolors(maxcolors=1 << 24) or [])
        except Exception as e:  # noqa: BLE001
            qmp_err = "PIL: " + str(e)
    check("2d: screendump shows a painted desktop (>= 8 colours)", colors >= 8,
          "colours=%d %s" % (colors, qmp_err))
    return colors


def main():
    os.makedirs(OUT, exist_ok=True)
    log("m1: virgl gate; worktree " + ROOT)
    if not os.path.exists(IMG):
        log("m1: disk.img missing -- run the build first")
        return 2
    if not build():
        log("m1: BUILD FAILED")
        return 2
    if not SKIP_3D:
        boot_3d()
    if not SKIP_2D:
        boot_2d()
    passed = sum(1 for _, ok, _ in results if ok)
    failed = len(results) - passed
    log("m1: %d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
