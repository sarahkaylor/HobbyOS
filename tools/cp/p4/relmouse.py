#!/usr/bin/env python3
"""tools/cp/p4/relmouse.py -- relative pointer motion, kernel half, no X
(lane C4-x2).

Boots the MODE=test kernel + disk of this tree with tools/cp/p4/relprobe.c as
the only wave member (/REL.ELF, a musl static PIE reading /dev/input/event0)
and drives it over QMP: after "[REL] ready" it injects
    10 x rel(+7,+3), 5 x rel(-4,-9), 40 x rel(+100,+1)
The last batch drives the kernel's integrated ABS position into the right
screen edge.  PASS needs the probe's REL sums to equal the injected sums
exactly (rel_x=4050, rel_y=25: evdev +y is down, as QMP's) and ABS_X to sit
at the edge (px 1919 of 1920).

Run from the tree root, under the shared lock:
  flock /tmp/cp-qemu.lock python3 tools/cp/p4/relmouse.py [--out DIR] [--no-build]
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
from p4_gate import Qmp, Serial, qemu_cmd  # noqa: E402

EXPECT_X = 10 * 7 + 5 * -4 + 40 * 100
EXPECT_Y = 10 * 3 + 5 * -9 + 40 * 1


def log(m):
    print(f"[RELM] {m}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "obj", "p4", "relmouse"))
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    env = dict(os.environ, TOOLS_SKIP=os.environ.get("TOOLS_SKIP", "1"),
               MAKEFLAGS=os.environ.get("MAKEFLAGS", "-j16"))
    if not a.no_build:
        r = subprocess.run(["make", "-C", REPO, "ARCH=intel", "MODE=test", "hobbyos.elf", "disk.img"],
                           env=env, capture_output=True, text=True)
        open(os.path.join(a.out, "build.log"), "w").write(r.stdout + r.stderr)
        if r.returncode:
            log("FAIL: build (see build.log)")
            return 2
    elf = os.path.join(a.out, "rel.elf")
    r = subprocess.run(["musl-gcc", "-static-pie", "-fPIE", "-O1", "-Wall", "-o", elf,
                        os.path.join(REPO, "tools", "cp", "p4", "relprobe.c")], capture_output=True, text=True)
    if r.returncode:
        log("FAIL: relprobe build\n" + r.stderr)
        return 2
    disk = os.path.join(a.out, "disk.img")
    shutil.copyfile(os.path.join(REPO, "disk.img"), disk)
    cfg = os.path.join(a.out, "wave.cfg")
    open(cfg, "w").write("REL.ELF\n")
    for src, dst in ((elf, "::REL.ELF"), (cfg, "::WAVE.CFG")):
        subprocess.run(["mcopy", "-o", "-i", disk, src, dst], check=True)
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
    ok = False
    try:
        q = Qmp(qmp_path)
        if not ser.wait(r"\[REL\] ready", 240):
            log("FAIL: the probe never printed [REL] ready")
        else:
            time.sleep(1)
            for _ in range(10):
                q.rel(7, 3)
                time.sleep(0.05)
            for _ in range(5):
                q.rel(-4, -9)
                time.sleep(0.05)
            for _ in range(40):
                q.rel(100, 1)
                time.sleep(0.05)
            m = ser.wait(r"\[REL\] total[^\n]*", 120)
            if m:
                row = m.group(0)
                g = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(-?\d+)", row))
                edge = (1919 * 32767) // 1920
                log("row: " + row)
                log(f"expect rel_x={EXPECT_X} rel_y={EXPECT_Y} abs_x={edge} (px 1919)")
                ok = (g.get("rel_x") == EXPECT_X and g.get("rel_y") == EXPECT_Y
                      and g.get("abs_x") == edge)
            ser.wait(r"\[REL\] done", 30)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    ser.poll()
    for ln in ser.text.splitlines():
        if "[REL]" in ln:
            print("  " + ln[ln.index("[REL]"):], flush=True)
    log("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
