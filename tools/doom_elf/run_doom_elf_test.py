#!/usr/bin/env python3
"""tools/doom_elf/run_doom_elf_test.py — boot HobbyOS with X11 and run a Linux
Doom ELF through the compat layer, then prove it with screendumps.

The chain under test (docs/doom-linux-elf-plan.md §2):

    kernel -> XSESSION.BIN -> XHOBBY.BIN (real X.Org, HobbyOS DDX)
           -> /XSTART.TXT -> DOOM.ELF   (static-PIE musl x86_64, Linux ABI)
           -> X11 protocol on /tmp/.X11-unix/X0 -> framebuffer -> QMP screendump

Why a dedicated harness and not the IceWM menu: the failure modes we must tell
apart are "the session did not start", "the ELF did not load", "the ELF loaded
and died in a syscall", and "the game runs but draws nothing".  A menu click
conflates all four.  So: a staged disk image (a COPY of disk.img, never the
repo's), an autostart line, the serial console, and PPM screendumps.

Evidence produced (in --out, default /tmp/hobbyos_doom_elf/):
  serial.log        the guest console (what the kernel and the game printed)
  shot-*.ppm        screendumps at known times
  REPORT.txt        the verdict, the frame statistics and the ENOSYS list

Exit 0 only when: the session reached "session running", the autostart line was
spawned, no FALLBACK happened, and at least one screendump looks like Doom
(many colours + a large non-black area).  Every wait is bounded.

GRADING NOTE (learned the hard way): Doom animates through SCREEN-MELT frames
whose content is jagged blue/black bands with as few as 4-11 distinct colours —
identical on a stock Xvfb, so they are not a rendering fault.  Grade the BEST
frame of a >=30 s run (the title/demo frames reach 40-200 colours), never a
single early screenshot, and never call a low-colour frame "garbled" without
comparing it against the same binary on a host X server.
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from run_icewm_test import Qmp, ppm_pixels, screen_stats  # noqa: E402  (repo helpers)

MK = ["make", "-C", REPO, "ARCH=intel", "DESKTOP=icewm"]


def log(msg):
    print(f"[DOOMELF] {msg}", flush=True)


def read_serial(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def wait_for(serial, needle, deadline_s, proc=None):
    """Poll the serial log for a substring; return True if it appears."""
    end = time.time() + deadline_s
    while time.time() < end:
        if needle in read_serial(serial):
            return True
        if proc is not None and proc.poll() is not None:
            return needle in read_serial(serial)
        time.sleep(0.5)
    return needle in read_serial(serial)


def qemu_cmd(work, disk):
    qmp = os.path.join(work, "qmp-sock")
    serial = os.path.join(work, "serial.log")
    fw_src = os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd")
    fw = os.path.join(work, "ovmf.fd")
    if not os.path.exists(fw):
        shutil.copy(fw_src, fw)
    # Same device set as run_icewm_test.py (a plain q35 std-VGA config does not
    # reach the kernel; the virtio-gpu + nvme + edu set does).
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
    return cmd, qmp, serial


def stage(disk, elf, wad, start_line):
    """mcopy the game and its launch config onto a COPY of the disk image."""
    pairs = []

    def put(src, dst):
        pairs.append(src)
        subprocess.run(["mcopy", "-o", "-i", disk, src, dst], check=True)

    tmp = os.path.join(os.path.dirname(disk), "tmp")
    os.makedirs(tmp, exist_ok=True)
    if elf:
        put(elf, "::DOOM.ELF")
    if wad and os.path.exists(wad):
        put(wad, "::DOOM1.WAD")
    start = os.path.join(tmp, "XSTART.TXT")
    with open(start, "w") as f:
        f.write("# autostart (staged by run_doom_elf_test.py)\n")
        f.write(start_line + "\n")
    put(start, "::XSTART.TXT")
    # /X11/XSESSION.ENV already exists on an icewm disk: read it out, append our
    # switches, and put it back (mcopy -o would otherwise drop the existing
    # session rows).
    env = os.path.join(tmp, "XSESSION.ENV")
    subprocess.run(["mcopy", "-o", "-i", disk, "::/X11/XSESSION.ENV", env],
                   check=False, capture_output=True)
    existing = ""
    if os.path.exists(env):
        existing = open(env).read()
    keep = [ln for ln in existing.splitlines()
            if not ln.startswith("XSESS_") and not ln.startswith("HB_XDEPTH")
            and not ln.startswith("DOOMWADDIR")]
    with open(env, "w") as f:
        for ln in keep:
            f.write(ln + "\n")
        f.write("DOOMWADDIR=/\n")
        f.write("HB_XDEPTH=%s\n" % os.environ.get("DOOM_XDEPTH", "24"))
        if os.environ.get("DOOM_NO_WM", "1") == "1":
            f.write("XSESS_NO_WM=1\n")
    put(env, "::/X11/XSESSION.ENV")
    return pairs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=os.path.join(REPO, "obj/doom-linux/doom10.elf"))
    ap.add_argument("--wad", default=os.environ.get("DOOM_WAD",
                                                   os.path.join(REPO, "obj/doom-linux/DOOM1.WAD")))
    ap.add_argument("--out", default="/tmp/hobbyos_doom_elf")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--seconds", type=int, default=70)
    ap.add_argument("--start-line", default=None,
                    help="exact /XSTART.TXT line (default: /DOOM.ELF)")
    ap.add_argument("--keys", default="ret,f1",
                    help="comma-separated qcode names pressed after the session is up")
    ap.add_argument("--skip-frame-check", action="store_true",
                    help="transport/session probes draw no game frame; grade only the chain")
    # A Doom window is 320x200 or 640x400 inside a 1920x1080 framebuffer: the
    # frame is 100% covered but only ~3-12% of the SCREEN.  These floors
    # discriminate a game frame from a blank/fallback desktop while tolerating
    # both window sizes; the garbled depth-8 run (10 colours, 1%) fails them.
    ap.add_argument("--min-nonblack", type=float, default=0.02)
    ap.add_argument("--min-colours", type=int, default=120)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    work = args.out
    disk = os.path.join(work, "disk.img")
    report = os.path.join(work, "REPORT.txt")
    fails = []

    # Upstream linuxdoom-1.10 has NO -iwad option (verified in d_main.c
    # IdentifyVersion): it looks for doom1.wad in $DOOMWADDIR (default "."), so
    # the WAD sits at the disk root and the env carries DOOMWADDIR=/.
    start_line = args.start_line or "/DOOM.ELF"
    stage_elf = args.start_line is None or os.path.exists(args.elf)
    if args.start_line is None and not os.path.exists(args.elf):
        log(f"FAIL: no ELF at {args.elf}")
        return 2

    if not args.no_build:
        log("building DESKTOP=icewm disk (make ... disk.img)")
        env = dict(os.environ, TOOLS_SKIP=os.environ.get("TOOLS_SKIP", "1"))
        r = subprocess.run(MK + ["disk.img"], cwd=REPO, capture_output=True,
                           text=True, env=env)
        if r.returncode != 0:
            log("FAIL: disk build failed")
            print(r.stdout[-3000:], r.stderr[-3000:])
            return 2
    log("copying disk.img -> work image (repo image is never mutated)")
    shutil.copyfile(os.path.join(REPO, "disk.img"), disk)
    stage(disk, args.elf if stage_elf else None, args.wad, start_line)

    cmd, qmp_path, serial = qemu_cmd(work, disk)
    log("booting: " + " ".join(cmd[:8]) + " ...")
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    shots = []
    try:
        qmp = Qmp(qmp_path, deadline_s=40)
        if not wait_for(serial, "session running", 150, proc):
            fails.append("session never reached 'session running'")
        else:
            log("session running")
        # Let the client map its window before the first shot.
        time.sleep(3)
        for k in [k for k in args.keys.split(",") if k]:
            try:
                qmp.cmd("send-key", {"keys": [{"type": "qcode", "data": k}]})
            except Exception as e:  # noqa: BLE001 - a missing key is not fatal
                log(f"send-key {k}: {e}")
            time.sleep(1.0)
        end = time.time() + args.seconds
        i = 0
        while time.time() < end and proc.poll() is None:
            p = os.path.join(work, f"shot-{i:02d}.ppm")
            if qmp.shot(p):
                shots.append(p)
                log(f"shot-{i:02d}.ppm")
            i += 1
            time.sleep(max(2, args.seconds // 12))
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    ser = read_serial(serial)
    if "FALLBACK" in ser:
        fails.append("session FALLBACK (the X stack or a client failed)")
    # XSESSION logs the line AFTER tokenising it in place, so only argv[0]
    # survives in the log line; match that token, not the whole line.
    if f"XSTART: {start_line.split()[0]}" not in ser:
        fails.append(f"the staged /XSTART.TXT line was never spawned: {start_line}")
    ensys = sorted(set(re.findall(r"\[LCOMPAT\] ENOSYS nr=(\d+) name=(\S+)", ser)))
    if not shots:
        fails.append("no screendump was produced")

    best = None
    for p in shots:
        nb, colours, _ = screen_stats(p)
        if best is None or (nb, colours) > best[1:]:
            best = (p, nb, colours)
    if not args.skip_frame_check and best and (
            best[1] < args.min_nonblack or best[2] < args.min_colours):
        fails.append(
            f"no Doom-like frame: best {best[0]} nonblack={best[1]:.3f} colours={best[2]}"
            f" (want >= {args.min_nonblack} / {args.min_colours})")

    with open(report, "w") as f:
        f.write("run_doom_elf_test report\n")
        f.write(f"elf={args.elf}\nwad={args.wad}\n")
        f.write(f"shots={len(shots)}\n")
        if best:
            f.write(f"best={best[0]} nonblack={best[1]:.3f} colours={best[2]}\n")
        f.write("fails=" + json.dumps(fails) + "\n")
        f.write("unhandled_syscalls=" + json.dumps(ensys) + "\n\n")
        f.write(ser[-200000:])

    log("--- serial tail ---")
    for line in ser.splitlines()[-40:]:
        log("  " + line)
    # The client's own words, when it has any: XSESSION relays a child's stderr
    # to the console as "[X] ...", and these are the strings that tell the
    # failure modes apart (no display / no IWAD / X error) instead of leaving
    # "black screen" as the only symptom.
    said = [ln for ln in ser.splitlines()
            if re.search(r"\[X\] |I_Error|Could not open|No IWAD|Game mode|"
                         r"XIO|BadWindow|BadMatch|XPutImage|MITSHM|undefined symbol", ln)]
    if said:
        log("--- what the client said ---")
        for line in said[-25:]:
            log("  " + line)
    if ensys:
        log("unhandled Linux syscalls seen: " + ", ".join(f"{n}({nm})" for n, nm in ensys[:30]))
    if fails:
        for x in fails:
            log("FAIL: " + x)
        log("report: " + report)
        return 1
    if args.skip_frame_check:
        log("PASS: the chain completed (frame check skipped)")
    else:
        log("PASS: Doom ELF ran under X and produced a frame")
    log("report: " + report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
