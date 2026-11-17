#!/usr/bin/env python3
"""tools/cp/p8/boot_p8.py -- boot + watch half of tools/cp/p8/run-p8.sh
(lane C8-game, docs/cp-p8.md).  Not run on its own: the bash wrapper builds,
owns the /etc/exports edits (exports.sh, never across this boot's lock) and
stages the disks, then calls this, which takes ONE QEMU slot
(tools/cp/qemu-slot.sh semantics) for the boot only.

--phase pfx   the prefix boot: wait for "[P8] done" (wineboot -i, reg add,
              sync), then stop QEMU.
--phase game  the game boot.  From "[P8] begin game" on it screendumps every
              --shot-every seconds; every shot that differs from the previous
              one is kept (PNG) and OCR'd (tesseract, if present).  Stops on:
                - "[P8] done" (the game exited / timed out and the launcher
                  cleaned up);
                - the main menu (OCR: two of NEW GAME / LOAD GAME / CONTINUE /
                  SETTINGS / CREDITS / QUIT GAME), after two more shots;
                - a first-run screen (OCR: the CDPR user agreement, the CDPR
                  account offer, the telemetry consent).  These are NEVER
                  answered (docs/cyberpunk.md §0.2 item 1): the run stops with
                  a screendump and says which screen it was;
                - --stall-s with no new serial byte and no screen change;
                - --timeout.
              The game's own title prompt "PRESS [SPACE] TO CONTINUE" (shown on
              every launch, not a first-run screen; P0's walker pressed Space
              there) gets one Space via QMP when --press-continue is set.
Writes OUT/timeline.txt (host wall time of every milestone).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools", "cp", "p5"))
from run_icewm_test import Qmp  # noqa: E402
from p5lib import acquire_slot, guests_running, ppm, read, release_slot  # noqa: E402

MENU_WORDS = ["NEW GAME", "LOAD GAME", "CONTINUE", "SETTINGS", "CREDITS", "QUIT GAME", "ADDITIONAL CONTENT"]
FIRST_RUN = [
    ("agreement", re.compile(r"USER\s+AGREEMENT|LICEN[CS]E\s+AGREEMENT|decline\s+or\s+accept", re.I)),
    # Not "CD PROJEKT RED ACCOUNT": the main menu itself has a corner button
    # with that label (run9-tmpcache stopped on it, a false positive).  The
    # offer is a modal with these phrases (docs/cp-p0-guest.md §5.1b).
    ("account offer", re.compile(r"SKIP\s+FOR\s+NOW|Unlock\s+Unique\s+Features|Cross-progression", re.I)),
    ("telemetry", re.compile(r"telemetry|CONSENT\s+TO\s+ALL|REJECT\s+ALL", re.I)),
]
PRESS_CONTINUE = re.compile(r"PRESS\b.{0,24}\bTO\s+CONTINUE", re.I | re.S)


def log(msg):
    print(f"[P8W] {msg}", flush=True)


def qemu_cmd(out, disk, cpdata, args):
    qmp = os.path.join(out, "qmp-sock")
    serial = os.path.join(out, "serial.log")
    fw = os.path.join(out, "ovmf.fd")
    if not os.path.exists(fw):
        shutil.copy(os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"), fw)
    # p5lib.qemu_cmd's device set + the ext2 data disk as NVMe namespace 2
    # (the Makefile's CPDATA line).
    cmd = ["qemu-system-x86_64", "-M", "q35", "-smp", str(args.smp), "-m", f"{args.mem}M",
           "-drive", f"if=pflash,format=raw,file={fw}",
           "-display", "none", "-serial", f"file:{serial}",
           "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
           "-drive", f"file={disk},format=raw,id=disk0,if=none",
           "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
           "-drive", f"file={cpdata},format=raw,id=cpdata,if=none",
           "-device", "nvme-ns,drive=cpdata,nsid=2",
           "-device", "edu", "-netdev", "user,id=net0",
           "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
           "-device", "virtio-gpu-pci", "-vga", "none",
           "-action", "shutdown=poweroff",
           "-qmp", f"unix:{qmp},server,nowait",
           "-enable-kvm", "-cpu", args.cpu]
    return cmd, qmp, serial


def unprefix(text):
    return "\n".join(ln[4:] if ln.startswith("[X] ") else ln for ln in text.splitlines())


def png(ppm_path, png_path):
    try:
        from PIL import Image
        Image.open(ppm_path).save(png_path)
        return True
    except Exception as e:  # noqa: BLE001
        log(f"png conversion failed: {e}")
        return False


def ocr(png_path):
    if not shutil.which("tesseract"):
        return None
    try:
        r = subprocess.run(["tesseract", png_path, "-", "--psm", "11"], capture_output=True, text=True, timeout=60)
        return r.stdout
    except subprocess.TimeoutExpired:
        return ""


def diff_frac(a, b):
    """fraction of sampled pixels (every 4th px of every 4th row) that differ by > 24"""
    wa, ha, pa = a
    wb, hb, pb = b
    if (wa, ha) != (wb, hb):
        return 1.0
    n = d = 0
    for y in range(0, ha, 4):
        row = y * wa * 3
        for x in range(0, wa, 4):
            o = row + x * 3
            n += 1
            if abs(pa[o] - pb[o]) + abs(pa[o + 1] - pb[o + 1]) + abs(pa[o + 2] - pb[o + 2]) > 24:
                d += 1
    return d / max(n, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--cpdata", required=True)
    ap.add_argument("--phase", choices=["pfx", "game"], required=True)
    ap.add_argument("--smp", type=int, default=8)
    ap.add_argument("--mem", type=int, default=12288)
    ap.add_argument("--cpu", default="host")
    ap.add_argument("--timeout", type=int, default=5400)
    ap.add_argument("--shot-every", type=int, default=15)
    ap.add_argument("--stall-s", type=int, default=1200)
    ap.add_argument("--press-continue", action="store_true")
    args = ap.parse_args()
    if args.mem > 12288:
        sys.exit("boot_p8: -m above 12 GiB needs the integrator's approval")
    shots_dir = os.path.join(args.out, "shots")
    os.makedirs(shots_dir, exist_ok=True)
    cmd, qmp_path, serial = qemu_cmd(args.out, args.disk, args.cpdata, args)
    for p in (serial, qmp_path):
        if os.path.exists(p):
            os.remove(p)
    tl_path = os.path.join(args.out, "timeline.txt")
    tl = open(tl_path, "w")
    t0 = None

    def mark(what):
        line = f"+{time.time() - t0:8.1f} s  {time.strftime('%H:%M:%S')}  {what}"
        tl.write(line + "\n")
        tl.flush()
        log(line)

    log("waiting for a QEMU slot (tools/cp/qemu-slot.sh semantics)")
    lockf, slot = acquire_slot()
    t0 = time.time()
    mark(f"slot {slot} acquired; boot -smp {args.smp} -m {args.mem}M -cpu {args.cpu} (KVM); "
         f"QEMU guests on the host before ours: {guests_running()}")
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    log(f"qemu pid {proc.pid}")
    verdict = "timeout"
    seen = set()
    try:
        qmp = Qmp(qmp_path, deadline_s=40)
        last_size, last_change = 0, time.time()
        prev_px = None
        baseline = None
        game_t = None
        next_shot = 0
        nshot = 0
        menu_hits = 0
        continue_presses = 0
        last_press = 0
        while time.time() - t0 < args.timeout and proc.poll() is None:
            raw = read(serial)
            # progress = launcher rows and the children's relayed output; the
            # kernel's idle/futex dumps ([IDLESTUCK], [ALIVE-SET]...) keep the
            # serial log growing while nothing happens, so they do not count.
            # (line starts only: kernel rows get spliced with "[X] " mid-line)
            prog = len(re.findall(r"(?m)^\[(?:X|P8)\] ", raw))
            if prog != last_size:
                last_size, last_change = prog, time.time()
            ser = unprefix(raw)
            for key, label in (("[P8] mount ", "NFS mount"), ("[P8] end wineboot ", "wineboot -i ended"),
                               ("[P8] warm-up: ", "warm-up result"), ("[P8] begin game ", "game launched"),
                               ("[P8] end game ", "game process ended"), ("[P8] sync done", "sync done"),
                               ("[P8] done", "launcher done")):
                i = ser.rfind(key)
                if i >= 0 and key not in seen:
                    seen.add(key)
                    mark(f"{label}: {ser[i:ser.find(chr(10), i)][:200]}")
                    if key == "[P8] begin game ":
                        game_t = time.time()
            if "[P8] done" in ser or "System halt" in ser:
                verdict = "launcher-done"
                time.sleep(3)
                break
            if args.phase == "game" and game_t and time.time() >= next_shot:
                next_shot = time.time() + args.shot_every
                path = os.path.join(shots_dir, "cur.ppm")
                if qmp.shot(path) and os.path.exists(path):
                    px = ppm(path)
                    if baseline is None:
                        baseline = px
                        shutil.copy(path, os.path.join(shots_dir, "baseline.ppm"))
                        png(path, os.path.join(shots_dir, "baseline.png"))
                        mark(f"baseline screendump {px[0]}x{px[1]} (desktop before the game's window)")
                    changed = prev_px is None or diff_frac(prev_px, px) > 0.01
                    if changed:
                        last_change = time.time()
                        nshot += 1
                        tag = f"s{nshot:03d}-t{time.time() - game_t:06.0f}"
                        keep = os.path.join(shots_dir, tag + ".ppm")
                        shutil.copy(path, keep)
                        pngp = keep[:-4] + ".png"
                        png(keep, pngp)
                        dfb = diff_frac(baseline, px)
                        if "first-window" not in seen and dfb > 0.02:
                            seen.add("first-window")
                            mark(f"first screen change after launch ({dfb:.1%} of the screen differs from the baseline): {tag}")
                        text = ocr(pngp)
                        if text is not None:
                            with open(keep[:-4] + ".ocr.txt", "w") as f:
                                f.write(text)
                            flat = " ".join(text.split())
                            log(f"shot {tag}: {dfb:.1%} vs baseline; OCR: {flat[:160]!r}")
                            hit = None
                            for name, rx in FIRST_RUN:
                                if rx.search(flat):
                                    hit = name
                                    break
                            if hit:
                                mark(f"FIRST-RUN SCREEN '{hit}' at {tag}: stopping without answering it")
                                verdict = f"first-run:{hit}"
                                break
                            words = [w for w in MENU_WORDS if w in flat.upper()]
                            if len(words) >= 2:
                                menu_hits += 1
                                mark(f"main menu OCR at {tag}: {words}")
                                if menu_hits >= 3:
                                    verdict = "main-menu"
                                    break
                                next_shot = time.time() + 5
                            elif (args.press_continue and PRESS_CONTINUE.search(flat)
                                  and continue_presses < 4 and time.time() - last_press > 20):
                                continue_presses += 1
                                last_press = time.time()
                                qmp.cmd("send-key", {"keys": [{"type": "qcode", "data": "spc"}]})
                                mark(f"title prompt 'PRESS ... TO CONTINUE' at {tag}: sent one Space (#{continue_presses})")
                        else:
                            log(f"shot {tag}: {dfb:.1%} vs baseline (no tesseract)")
                    prev_px = px
            if time.time() - last_change > args.stall_s:
                mark(f"STALL: no launcher/Wine row and no screen change for {args.stall_s} s")
                verdict = "stall"
                break
            time.sleep(1)
        if proc.poll() is not None and verdict == "timeout":
            verdict = f"qemu-exited rc={proc.returncode}"
        # final screendump (the furthest point)
        if proc.poll() is None:
            fin = os.path.join(shots_dir, "final.ppm")
            if qmp.shot(fin):
                time.sleep(1)
                png(fin, fin[:-4] + ".png")
                t = ocr(fin[:-4] + ".png")
                if t is not None:
                    open(fin[:-4] + ".ocr.txt", "w").write(t)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        release_slot(lockf)
        mark(f"verdict: {verdict}; QEMU stopped, slot released; guests on the host now: {guests_running()}")
        tl.close()
    open(os.path.join(args.out, "verdict.txt"), "w").write(verdict + "\n")
    return 0 if verdict in ("launcher-done", "main-menu") else 1


if __name__ == "__main__":
    sys.exit(main())
