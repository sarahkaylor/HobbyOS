#!/usr/bin/env python3
"""tools/cp/p4/xprobe.py -- the X server half of Gate P4 without Wine (C4-x2).

One boot of a DESKTOP=icewm disk (a COPY of the tree's disk.img) with
HB_SECOND_SCREEN=0, the /CP/X11 runtime tree and glibc 2.31 on the FAT disk
(what `make cp-x11-disk` stages), and tools/cp/p4/xprobe.c as /XPROBE.ELF on
/XSTART.TXT.  It drives the probe over QMP:
  after "[XP] raw ready":   10 x rel(+7,+3), 5 x rel(-4,-9), 40 x rel(+100,+1)
                            (expected raw sums dx=4050 dy=25: the last batch
                            pins the cursor at the right edge)
  after "[XP] randr hold":  a screendump; the red 1280x720 window must fill
                            exactly the top-left 1280x720 of the glass, the
                            rest black.
  after "[XP] shm hold":    (C4-x3) a screendump; the 256x192 MIT-SHM pattern
                            the probe XShmPutImage'd at (64,64) must be on the
                            glass pixel for pixel.
PASS needs: screens=1 + XINERAMA + Composite, MIT-SHM listed and its rows
(XShmPutImage/XShmGetImage round trip, ShmCreatePixmap, window readback), the
raw sums from the relative slave, the RandR rows (refused offset, set,
restore) and both screendumps.

  bash tools/cp/qemu-slot.sh python3 tools/cp/p4/xprobe.py [--out DIR]
Env: CP_X11_WORK (default ~/cp/x11-build-x2), CP_SYSROOT (default
~/cp/sysroot-glibc231), GLIBC231_LIB (default ~/cp/wine-build/glibc231/lib/x86_64-linux-gnu).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p4_gate import Qmp, Serial, qemu_cmd, ppm, bbox_of  # noqa: E402

X11W = os.environ.get("CP_X11_WORK", os.path.expanduser("~/cp/x11-build-x2"))
SYS = os.environ.get("CP_SYSROOT", os.path.expanduser("~/cp/sysroot-glibc231"))
LDIR = os.environ.get("GLIBC231_LIB", os.path.expanduser("~/cp/wine-build/glibc231/lib/x86_64-linux-gnu"))


def log(m):
    print(f"[XPG] {m}", flush=True)


def mt(*a):
    subprocess.run(list(a), check=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "obj", "p4", "xprobe"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    dev = os.path.join(X11W, "destdir", "CP", "X11")
    elf = os.path.join(a.out, "xprobe.elf")
    subprocess.run(["clang", f"--sysroot={SYS}", "-O2", "-g", "-Wall", "-fPIE", "-pie",
                    "-I" + os.path.join(dev, "include"), "-o", elf,
                    os.path.join(REPO, "tools", "cp", "p4", "xprobe.c"),
                    "-L" + os.path.join(dev, "lib"), "-lXrandr", "-lXi", "-lXext", "-lX11",
                    "-Wl,-rpath-link," + os.path.join(dev, "lib"), "-Wl,--disable-new-dtags",
                    "-Wl,-rpath,/CP/X11/lib", "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2"],
                   check=True)
    disk = os.path.join(a.out, "disk.img")
    shutil.copyfile(os.path.join(REPO, "disk.img"), disk)
    for d in ("::/LIB64", "::/lib64", "::/CP"):
        subprocess.run(["mmd", "-i", disk, d], capture_output=True)
    for so in ("ld-linux-x86-64.so.2", "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2",
               "librt.so.1", "libgcc_s.so.1"):
        p = os.path.join(LDIR, so)
        if os.path.exists(p):
            mt("mcopy", "-o", "-i", disk, os.path.realpath(p), f"::/LIB64/{so}")
    mt("mcopy", "-o", "-i", disk, os.path.realpath(os.path.join(LDIR, "ld-linux-x86-64.so.2")),
       "::/lib64/ld-linux-x86-64.so.2")
    mt("mcopy", "-s", "-o", "-i", disk, os.path.join(X11W, "stage", "CP", "X11"), "::/CP/")
    mt("mcopy", "-o", "-i", disk, elf, "::XPROBE.ELF")
    st = os.path.join(a.out, "xstart.txt")
    open(st, "w").write("/XPROBE.ELF\n")
    mt("mcopy", "-o", "-i", disk, st, "::XSTART.TXT")
    env = os.path.join(a.out, "xsession.env")
    subprocess.run(["mcopy", "-o", "-i", disk, "::/X11/XSESSION.ENV", env], capture_output=True)
    keep = [ln for ln in (open(env).read().splitlines() if os.path.exists(env) else [])
            if not ln.startswith("HB_SECOND_SCREEN=")]
    open(env, "w").write("".join(ln + "\n" for ln in keep) + "HB_SECOND_SCREEN=0\n")
    mt("mcopy", "-o", "-i", disk, env, "::/X11/XSESSION.ENV")
    ovmf = os.path.join(a.out, "ovmf.fd")
    for c in (os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"), "/usr/share/OVMF/OVMF_CODE_4M.fd"):
        if os.path.exists(c):
            shutil.copy(c, ovmf)
            break
    cmd, qmp_path, serial = qemu_cmd(a.out, disk, ovmf, "4096M")
    for p in (qmp_path, serial):
        if os.path.exists(p):
            os.remove(p)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(os.path.join(a.out, "qemu.err"), "w"))
    log(f"qemu pid {proc.pid}")
    ser = Serial(serial, proc)
    fails = []
    try:
        q = Qmp(qmp_path)
        if ser.wait(r"\[XP\] shm hold|\[XP\] FAIL shm|\[XP\] raw ready", 300) and \
                re.search(r"\[XP\] shm hold", ser.text):
            time.sleep(1.0)
            p = os.path.join(a.out, "shm-pattern.ppm")
            q.shot(p)
            px, w, h = ppm(p)
            bad = 0
            if px:
                for yy in range(0, 192, 7):
                    for xx in range(0, 256, 5):
                        o = ((64 + yy) * w + 64 + xx) * 3
                        want = bytes((((xx * 5) & 0xff), ((yy * 3) & 0xff), (((xx ^ yy) * 7) & 0xff)))
                        bad += 1 if px[o:o + 3] != want else 0
            log(f"shm screendump: {bad} of {(192 // 7 + 1) * (256 // 5 + 1)} sampled pattern pixels differ")
            if not px or bad:
                fails.append(f"shm screendump: {bad} pattern pixels differ")
        if not ser.wait(r"\[XP\] raw ready", 300):
            fails.append("probe never reached raw ready")
            raise SystemExit
        time.sleep(1)
        for dx, dy, k in ((7, 3, 10), (-4, -9, 5), (100, 1, 40)):
            for _ in range(k):
                q.rel(dx, dy)
                time.sleep(0.05)
        if ser.wait(r"\[XP\] randr hold", 200):
            time.sleep(1.5)
            p = os.path.join(a.out, "randr-1280x720.ppm")
            q.shot(p)
            time.sleep(0.5)
            b = bbox_of(p, (255, 0, 0), step=1, min_count=1000)
            px, w, h = ppm(p)
            outside = 0
            if px:
                for (x, y) in ((1500, 100), (1900, 1000), (100, 900), (1279, 719)):
                    o = (y * w + x) * 3
                    outside += 1 if px[o:o + 3] != b"\x00\x00\x00" and not (x == 1279 and y == 719) else 0
            log(f"randr screendump: red bbox {b}; non-black samples outside the mode: {outside}")
            if not b or b[:4] != (0, 0, 1279, 719) or outside:
                fails.append(f"randr screendump: red bbox {b}, outside {outside}")
        ser.wait(r"\[XP\] done rc=\d+", 120)
    except SystemExit:
        pass
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    ser.poll()
    rows = [ln[ln.index("[XP]"):] for ln in ser.text.splitlines() if "[XP]" in ln]
    for r in rows:
        print("  " + r, flush=True)
    for ln in ser.text.splitlines():
        if "hobbyos: randr" in ln or "hobbyos: one screen" in ln or "[SHM]" in ln:
            print("  " + ln.strip(), flush=True)
    for need in ("[XP] MIT-SHM listed=yes", "[XP] shm roundtrip ok", "[XP] shm window ok"):
        if not any(r.startswith(need) for r in rows):
            fails.append("missing row " + need)
    tot = [r for r in rows if r.startswith("[XP] raw total")]
    if not any("dx=4050 dy=25" in r for r in tot):
        fails.append("raw sums != dx=4050 dy=25 on any source: " + "; ".join(tot))
    if not any(r.startswith("[XP] done rc=0") for r in rows):
        fails.append("probe rc != 0")
    for f in fails:
        log("FAIL: " + f)
    log("PASS" if not fails else "FAIL")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
