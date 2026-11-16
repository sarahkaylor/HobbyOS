#!/usr/bin/env python3
"""tools/cp/p5/run_vkcubex.py -- Gate P5, Linux row: vkcube through Mesa's X11
WSI in the guest's X session for >= 60 s (lane C5-vk2, docs/cp-vk.md §8).

Boots the default DESKTOP=icewm disk (XSESSION -> XHOBBY.BIN + IceWM) with:
  * the glibc 2.31 runtime in /LIB64 + /lib64/ld-linux-x86-64.so.2
    (tools/cp/x11/smoke.py's set);
  * C4-x's real X client libraries at /CP/X11 (CP_X11_WORK/stage/CP/X11);
  * the x11 variant of the Vulkan tree at /CP/VK (CP_VK_WORK/stage-x11/CP/VK:
    libvulkan.so.1 as a real file -- FAT has no symlinks --, lavapipe, the ICD
    JSON) and /VKCUBEX.ELF on /XSTART.TXT with its arguments.

It screendumps twice while the cube spins (progress rows t=20s and t=50s), then
waits for the RESULT row.  Every boot holds one QEMU slot (tools/cp/qemu-slot.sh semantics) for the
boot only.

Grade (all required):
  G1 "[VK] RESULT frames=N wall=W" with W >= 60000 ms and N > 0, and
     "[VK] done rc=0";
  G2 no fault line in the serial log (memory fault / page fault / FATAL /
     a non-zero [EXITPC] for the probe's pid / "error while loading");
  G3 both screendumps show the swapchain image: >= 20000 pixels of the clear
     colour (15,18,28)+-3 and >= 2000 saturated "cube" pixels, and the two
     shots differ inside the window (the cube moved).

Usage (strix; MAKEFLAGS=-j16):
  python3 tools/cp/p5/run_vkcubex.py [--secs 65] [--size 640x480] [--present fifo]
                                     [--out DIR] [--no-build] [--smp 8] [--mem 12288]
                                     [--cpu host] [--timeout 600]
Env: CP_VK_WORK (default ~/cp/vk-build), CP_X11_WORK (default ~/cp/x11-build),
     CP_SYSROOT (glibc 2.31 sysroot; default $CP_VK_WORK/sysroot-glibc231).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_icewm_test import Qmp  # noqa: E402
from p5lib import acquire_slot, census, guests_running, qemu_cmd, read, release_slot, wait_for  # noqa: E402

VKW = os.environ.get("CP_VK_WORK", os.path.expanduser("~/cp/vk-build"))
X11W = os.environ.get("CP_X11_WORK", os.path.expanduser("~/cp/x11-build"))
SYS = os.environ.get("CP_SYSROOT", os.path.join(VKW, "sysroot-glibc231"))
VK = os.path.join(VKW, "stage-x11", "CP", "VK")
X11 = os.path.join(X11W, "stage", "CP", "X11")
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
    print(f"[P5X] {msg}", flush=True)


def m(*a, check=True):
    return subprocess.run(list(a), check=check, capture_output=True, text=True)


def stage(disk, args, tmp):
    ldir = os.path.join(SYS, "lib", "x86_64-linux-gnu")
    for d in ("::/LIB64", "::/lib64", "::/CP", "::/CP/VK", "::/CP/VK/lib", "::/CP/VK/bin",
              "::/CP/VK/share", "::/CP/VK/share/vulkan", "::/CP/VK/share/vulkan/icd.d"):
        m("mmd", "-i", disk, d, check=False)
    for src, dst in GLIBC.items():
        m("mcopy", "-o", "-i", disk, os.path.join(ldir, src), f"::/LIB64/{dst}")
    m("mcopy", "-o", "-i", disk, os.path.join(ldir, "ld-2.31.so"), "::/lib64/ld-linux-x86-64.so.2")
    m("mcopy", "-s", "-o", "-i", disk, X11, "::/CP/")
    lib = os.path.join(VK, "lib")
    m("mcopy", "-o", "-i", disk, os.path.realpath(os.path.join(lib, "libvulkan.so.1")), "::/CP/VK/lib/libvulkan.so.1")
    m("mcopy", "-o", "-i", disk, os.path.join(lib, "libvulkan_lvp.so"), "::/CP/VK/lib/libvulkan_lvp.so")
    m("mcopy", "-o", "-i", disk, os.path.join(VK, "share/vulkan/icd.d/lvp_icd.x86_64.json"),
      "::/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json")
    m("mcopy", "-o", "-i", disk, os.path.join(VK, "bin/vkcubex"), "::/CP/VK/bin/vkcubex")
    m("mcopy", "-o", "-i", disk, os.path.join(VK, "bin/vkcubex"), "::/VKCUBEX.ELF")
    w, h = args.size.split("x")
    start = os.path.join(tmp, "XSTART.TXT")
    with open(start, "w") as f:
        f.write("# autostart (staged by tools/cp/p5/run_vkcubex.py)\n")
        f.write(f"/VKCUBEX.ELF {args.secs} {w} {h} {args.present}\n")
    m("mcopy", "-o", "-i", disk, start, "::XSTART.TXT")
    log("staged: /LIB64 glibc 2.31, /CP/X11 (%d libs), /CP/VK (x11 variant), /VKCUBEX.ELF; XSTART: %s"
        % (len(os.listdir(os.path.join(X11, "lib"))), open(start).read().splitlines()[1]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=65)
    ap.add_argument("--size", default="640x480")
    ap.add_argument("--present", default="fifo")
    ap.add_argument("--out", default=os.path.join(REPO, "obj", "cp-p5", "vkcubex"))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--smp", type=int, default=8)
    ap.add_argument("--mem", type=int, default=12288)
    ap.add_argument("--cpu", default="host")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    for p in (os.path.join(VK, "bin/vkcubex"), os.path.join(VK, "lib/libvulkan_lvp.so"), X11):
        if not os.path.exists(p):
            log(f"FAIL: {p} missing (CP_VK_WSI=x11 tools/cp/vk/build.sh all; tools/cp/x11/build.sh all)")
            return 2
    if not args.no_build:
        log("build: make ARCH=intel hobbyos.elf disk.img (default MODE=desktop, DESKTOP=icewm)")
        r = subprocess.run(["make", "-C", REPO, "ARCH=intel", "hobbyos.elf", "disk.img"],
                           env=dict(os.environ, TOOLS_SKIP=os.environ.get("TOOLS_SKIP", "1")),
                           capture_output=True, text=True)
        if r.returncode:
            log("FAIL: build\n" + (r.stdout + r.stderr)[-3000:])
            return 2
    os.makedirs(args.out, exist_ok=True)
    tmp = os.path.join(args.out, "tmp")
    os.makedirs(tmp, exist_ok=True)
    disk = os.path.join(args.out, "disk.img")
    shutil.copyfile(os.path.join(REPO, "disk.img"), disk)  # the repo image is never mutated
    stage(disk, args, tmp)

    cmd, qmp_path, serial = qemu_cmd(args.out, disk, args)
    if os.path.exists(serial):
        os.remove(serial)
    shots, fails = [], []
    log("waiting for a QEMU slot (tools/cp/qemu-slot.sh semantics)")
    lockf, slot = acquire_slot()
    log(f"slot {slot} acquired {time.strftime('%H:%M:%S')}; boot: -smp {args.smp} -m {args.mem}M -cpu {args.cpu} (KVM)")
    t_boot = time.time()
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    log(f"qemu pid {proc.pid}")
    try:
        qmp = Qmp(qmp_path, deadline_s=40)
        if not wait_for(serial, "session running", 180, proc):
            fails.append("X session never reached 'session running'")
        elif not wait_for(serial, "[VK] render loop", 240, proc):
            fails.append("vkcubex never reached its render loop")
        else:
            for tag in ("t=20s", "t=50s"):
                if wait_for(serial, f"[VK] {tag} ", 120, proc):
                    p = os.path.join(args.out, f"vkcubex-{tag.replace('=', '')}.ppm")
                    if qmp.shot(p):
                        shots.append(p)
                    log(f"{tag}: screendump taken; QEMU guests on the host now (incl. this one): {guests_running()}")
                else:
                    fails.append(f"no progress row {tag}")
                    break
            wait_for(serial, "[VK] done rc=", args.secs + 60, proc)
            time.sleep(2)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        release_slot(lockf)
        log(f"guests on the host at the end of the run: {guests_running()} (incl. none of ours)")
        log(f"boot finished after {time.time() - t_boot:.0f} s; lock released")

    ser = read(serial)
    rows = [ln[ln.index("[VK]"):] for ln in ser.splitlines() if "[VK]" in ln]
    res = [r for r in rows if r.startswith("[VK] RESULT ")]
    if not res:
        fails.append("no RESULT row")
    else:
        kv = dict(t.split("=", 1) for t in res[-1].split() if "=" in t and t.count("=") == 1)
        wall = float(kv.get("wall", "0"))
        frames = int(kv.get("frames", "0"))
        if wall < 60000 or frames <= 0:
            fails.append(f"RESULT wall={wall} frames={frames} (< 60 s or no frames)")
    if "[VK] done rc=0" not in rows:
        fails.append("no '[VK] done rc=0'")
    bad = [ln for ln in ser.splitlines()
           if any(k in ln for k in ("memory fault", "page fault", "FATAL", "error while loading",
                                    "cannot open shared", "undefined symbol"))]
    if bad:
        fails.append(f"{len(bad)} fault/loader line(s)")
    cs = []
    for p in shots:
        c = census(p)
        cs.append(c)
        log(f"screendump {os.path.basename(p)}: clear={c['clear']} cube={c['sat']} bbox={c['bbox']} hash={c['hash']}")
        if c["clear"] < 20000 or c["sat"] < 2000:
            fails.append(f"{os.path.basename(p)}: swapchain image not recognisable")
        png = p[:-4] + ".png"
        subprocess.run([sys.executable, os.path.join(REPO, "skills/hobbyos-screenshot/scripts/ppm2png.py"), p, png],
                       capture_output=True)
    if len(cs) == 2 and cs[0]["hash"] == cs[1]["hash"]:
        fails.append("the two screendumps are identical inside the window (cube not moving)")
    if len(cs) < 2:
        fails.append(f"only {len(cs)} screendump(s)")

    log("--- [VK] rows (serial) ---")
    for r in rows:
        print(r, flush=True)
    log("--- session / loader / fault lines ---")
    for ln in ser.splitlines():
        if any(k in ln for k in ("[XSESS]", "error while loading", "undefined symbol", "[LCOMPAT] ENOSYS",
                                 "cannot open shared", "memory fault", "page fault", "FATAL", "[EXITPC]")):
            print("  " + ln, flush=True)
    if fails:
        for f in fails:
            log("FAIL: " + f)
        return 1
    log("PASS: Linux vkcube on Mesa's X11 WSI (lavapipe) in the X session for >= 60 s, two distinct screendumps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
