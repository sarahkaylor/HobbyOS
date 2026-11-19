#!/usr/bin/env python3
"""tools/cp/x11/smoke.py -- lane C4-x in-OS smoke (docs/cp-x11.md §4).

Boots the default DESKTOP=icewm disk (XSESSION -> XHOBBY.BIN + IceWM) with the
REAL X client libraries from tools/cp/x11/build.sh staged at /CP/X11, the
pinned glibc 2.31 runtime in /LIB64 (+ /lib64/ld-linux-x86-64.so.2), and
tools/cp/x11/xsmoke.c as /XSMOKE.ELF on /XSTART.TXT.  The client connects to
:0, prints XListExtensions + the XI2/RandR/Render/Fixes/Shm/Present/DRI3/SYNC/
XINERAMA/Composite/Damage queries, opens a 640x400 window, draws, holds, exits.

Reuses the existing harness pieces rather than forking them:
  * tools/doom_elf/run_doom_elf_test.py -- qemu_cmd (the q35 + nvme +
    virtio-gpu device set that reaches the X session), read_serial, wait_for
    and the XSTART/XSESSION.ENV staging recipe;
  * tools/gate/m3_x11_client.py (RimWorld M3) -- the glibc 2.31 /LIB64 staging
    set and ppm_colors (exact-pixel census of the window colours);
  * run_icewm_test.py -- Qmp.

Grade (all required): "[XS] XOpenDisplay ok", libX11 resolved from
/CP/X11/lib, the XListExtensions rows, "[XS] Expose -> window drawn",
"[XS] done rc=0", and >500 sampled pixels each of the window's pure green
(0,255,0) and blue (0,0,255) in the screendump.

Run on linuxci, ALWAYS under the shared lock:
  bash tools/cp/qemu-slot.sh python3 tools/cp/x11/smoke.py [--out DIR] [--build-disk]
Env: CP_X11_WORK (default ~/cp/x11-build), CP_SYSROOT (default
~/cp/wine-build/sysroot-glibc231).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools", "doom_elf"))
sys.path.insert(0, os.path.join(REPO, "tools", "gate"))
from run_icewm_test import Qmp  # noqa: E402
from run_doom_elf_test import qemu_cmd, read_serial, wait_for  # noqa: E402
from m3_x11_client import ppm_colors  # noqa: E402

WORK = os.environ.get("CP_X11_WORK", os.path.expanduser("~/cp/x11-build"))
SYS = os.environ.get("CP_SYSROOT", os.path.expanduser("~/cp/wine-build/sysroot-glibc231"))
STAGE = os.path.join(WORK, "stage", "CP", "X11")
CLIENT = os.path.join(WORK, "xsmoke", "xsmoke.elf")
# The M3 gate's /LIB64 set (tools/gate/m3_x11_client.py stage()).
GLIBC = {
    "ld-2.31.so": "ld-linux-x86-64.so.2",
    "libc-2.31.so": "libc.so.6",
    "libm-2.31.so": "libm.so.6",
    "libpthread-2.31.so": "libpthread.so.0",
    "libdl-2.31.so": "libdl.so.2",
    "librt-2.31.so": "librt.so.1",
    "libgcc_s.so.1": "libgcc_s.so.1",
}


def log(msg):
    print(f"[CPX11] {msg}", flush=True)


def mtools(*a, check=True):
    return subprocess.run(list(a), check=check, capture_output=True, text=True)


def stage(disk, hold_ms):
    ldir = os.path.join(SYS, "lib", "x86_64-linux-gnu")
    for d in ("::/LIB64", "::/lib64", "::/CP"):
        mtools("mmd", "-i", disk, d, check=False)
    for src, dst in GLIBC.items():
        mtools("mcopy", "-o", "-i", disk, os.path.join(ldir, src), f"::/LIB64/{dst}")
    mtools("mcopy", "-o", "-i", disk, os.path.join(ldir, "ld-2.31.so"), "::/lib64/ld-linux-x86-64.so.2")
    # The real-libs tree, exactly as staged (F4 proposal: /CP/X11).
    mtools("mcopy", "-s", "-o", "-i", disk, STAGE, "::/CP/")
    mtools("mcopy", "-o", "-i", disk, CLIENT, "::XSMOKE.ELF")
    tmp = os.path.join(os.path.dirname(disk), "tmp")
    os.makedirs(tmp, exist_ok=True)
    start = os.path.join(tmp, "XSTART.TXT")
    with open(start, "w") as f:
        f.write("# autostart (staged by tools/cp/x11/smoke.py)\n/XSMOKE.ELF\n")
    mtools("mcopy", "-o", "-i", disk, start, "::XSTART.TXT")
    env = os.path.join(tmp, "XSESSION.ENV")
    mtools("mcopy", "-o", "-i", disk, "::/X11/XSESSION.ENV", env, check=False)
    keep = open(env).read().splitlines() if os.path.exists(env) else []
    keep = [ln for ln in keep if not ln.startswith("XSMOKE_")]
    with open(env, "w") as f:
        for ln in keep:
            f.write(ln + "\n")
        f.write(f"XSMOKE_HOLD_MS={hold_ms}\n")
        if os.environ.get("XSMOKE_PREFAULT") == "0":
            f.write("XSMOKE_PREFAULT=0\n")  # reproduce the compat writev kernel hang
    mtools("mcopy", "-o", "-i", disk, env, "::/X11/XSESSION.ENV")
    log("staged: " + mtools("mdir", "-b", "-i", disk, "::/CP/X11/lib").stdout.count("\n").__str__()
        + " files in ::/CP/X11/lib, glibc 2.31 in ::/LIB64, ::XSMOKE.ELF, ::XSTART.TXT")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(WORK, "smoke-run"))
    ap.add_argument("--build-disk", action="store_true",
                    help="run `make ARCH=intel disk.img` first (default: use the existing disk.img)")
    ap.add_argument("--hold-ms", type=int, default=20000)
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    for p in (STAGE, CLIENT):
        if not os.path.exists(p):
            log(f"FAIL: {p} missing (bash tools/cp/x11/build.sh all)")
            return 2
    if args.build_disk:
        r = subprocess.run(["make", "-C", REPO, "ARCH=intel", "disk.img"],
                           env=dict(os.environ, TOOLS_SKIP=os.environ.get("TOOLS_SKIP", "1")),
                           capture_output=True, text=True)
        if r.returncode:
            log("FAIL: disk build\n" + (r.stdout + r.stderr)[-3000:])
            return 2
    os.makedirs(args.out, exist_ok=True)
    disk = os.path.join(args.out, "disk.img")
    shutil.copyfile(os.path.join(REPO, "disk.img"), disk)  # the repo image is never mutated
    stage(disk, args.hold_ms)

    cmd, qmp_path, serial = qemu_cmd(args.out, disk)
    if os.path.exists(serial):
        os.remove(serial)
    log("booting: " + " ".join(cmd[:8]) + " ...")
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    log(f"qemu pid {proc.pid}")
    shot = os.path.join(args.out, "xsmoke-window.ppm")
    fails, counts = [], {}
    try:
        qmp = Qmp(qmp_path, deadline_s=40)
        if not wait_for(serial, "session running", 150, proc):
            fails.append("session never reached 'session running'")
        if not wait_for(serial, "[XS] Expose -> window drawn", args.timeout, proc):
            fails.append("the client never drew its window")
        else:
            time.sleep(3)  # let the server's block handler present the damage
            if qmp.shot(shot):
                counts = ppm_colors(shot)
        wait_for(serial, "[XS] done rc=", args.hold_ms // 1000 + 30, proc)
        time.sleep(1)
        qmp.shot(os.path.join(args.out, "after-exit.ppm"))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    ser = read_serial(serial)
    rows = [ln[ln.index("[XS]"):] for ln in ser.splitlines() if "[XS]" in ln]
    green = counts.get((0, 255, 0), 0)
    blue = counts.get((0, 0, 255), 0)
    red = counts.get((255, 0, 0), 0)
    for need, what in (("[XS] XOpenDisplay ok", "XOpenDisplay"),
                       ("[XS] XListExtensions:", "XListExtensions"),
                       ("[XS] done rc=0", "clean exit rc=0")):
        if not any(r.startswith(need) for r in rows):
            fails.append(f"missing row: {what}")
    if not any(r.startswith("[XS] libX11=/CP/X11/lib/libX11.so.6") for r in rows):
        fails.append("libX11 was not resolved from /CP/X11/lib")
    if green <= 500 or blue <= 500:
        fails.append(f"window not in the screendump (green={green} blue={blue})")

    log("--- client rows (serial) ---")
    for r in rows:
        print(r, flush=True)
    log("--- session / loader lines ---")
    for ln in ser.splitlines():
        if any(k in ln for k in ("[XSESS]", "error while loading", "undefined symbol",
                                 "[LCOMPAT] ENOSYS", "cannot open shared")):
            print("  " + ln, flush=True)
    log(f"screendump {shot}: sampled pixels green={green} blue={blue} red={red}")
    if os.path.exists(shot) and shutil.which("convert"):
        png = os.path.join(args.out, "xsmoke-window.png")
        subprocess.run(["convert", shot, "-resize", "50%", png], check=False)
        log(f"png {png}")
    if fails:
        for f in fails:
            log("FAIL: " + f)
        return 1
    log("PASS: real libX11/libxcb client on XHOBBY.BIN :0 -- extensions listed, window drawn, rc=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
