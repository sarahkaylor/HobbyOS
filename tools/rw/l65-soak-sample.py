#!/usr/bin/env python3
"""tools/rw/l65-soak-sample.py -- 30 s timeline of a running soak.

Samples the live serial log and the live /RW_HOME/Player.log (read out of
disk.img with mcopy; the guest keeps writing it, so a copy is retried) and
appends one CSV row per interval:

  t_s        seconds since the launcher's `[RW] rwlaunch: start` row
  nfs_bytes  cumulative READ bytes from the last `[NFSRTT] periodic` row
  xsw_n      presents from the last `[XSW] n=` row in Player.log
  live       live threads from the last `[THRS] pid=4` row
  res_mib    resident MiB from the same row
  last_xo    last XML the game opened (`[XO]`), i.e. the load cursor
  phase      last Player.log phase marker, or `other`
  last_line  tail of Player.log (80 chars) so `other` is still named
  retr       `[NFSRTT]` rows with wait_max_us >= 500000 (retransmit/timeout)
  blkq       `[BLKQ]` stall rows
  nfspipe    `[NFSPIPE]` stall rows
  nfsrdfail  `[NFSRD] fail` rows
  txtzero    `[TXT] ZERO` refusals
  kvm        `KVM internal error` rows
  kernel     `[KERNEL]` memory-fault rows

Usage: l65-soak-sample.py --log SERIAL --out CSV [--interval 30]
       [--duration 2000] [--img disk.img] [--player-out /tmp/pl.log]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

PHASES = ("Loading defs", "Initializing", "Loading art", "Loading sound",
          "Loading main resources", "Initializing engine", "Loading language")


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("latin-1")
    except OSError:
        return ""


def mcopy_player(img, dst):
    """Copy ::/RW_HOME/Player.log out of the FAT image.  Returns True on a
    non-empty copy; the guest may be mid-write, so a failed read keeps the
    previous sample rather than inventing one."""
    tmp = dst + ".tmp"
    try:
        r = subprocess.run(["mcopy", "-o", "-i", img, "::/RW_HOME/Player.log", tmp],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=20)
        if r.returncode == 0 and os.path.getsize(tmp) > 0:
            os.replace(tmp, dst)
            return True
    except Exception:
        pass
    try:
        os.unlink(tmp)
    except OSError:
        pass
    return False


def phase_of(text):
    """Last phase marker in Player.log (not just the last line: the log
    interleaves thread noise after the marker)."""
    best = None
    for m in re.finditer("|".join(re.escape(p) for p in PHASES), text):
        best = (m.start(), m.group(0))
    if best:
        return best[1]
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    return "other:" + (lines[-1][:80] if lines else "-")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--img", default="disk.img")
    ap.add_argument("--player-out", default="/tmp/l65-player.log")
    ap.add_argument("--interval", type=int, default=30)
    ap.add_argument("--duration", type=int, default=2000)
    ap.add_argument("--marker-base", type=int, default=None,
                    help="launch-marker occurrences already in the log; a "
                         "mid-run start passes the current count")
    args = ap.parse_args()

    t0 = None
    start_wall = time.time()
    rows = 0
    next_at = 0.0
    # A stale serial log from the previous run still contains the launch
    # marker; only a marker beyond this count belongs to THIS run.
    boot_marks = (args.marker_base if args.marker_base is not None else
                  len(re.findall(r"\[RW\] rwlaunch: start", read(args.log))))
    started = args.marker_base is not None
    with open(args.out, "w") as out:
        out.write("t_s,nfs_bytes,xsw_n,live,res_mib,last_xo,phase,last_line,"
                  "retr,blkq,nfspipe,nfsrdfail,txtzero,kvm,kernel\n")
        while time.time() - start_wall < args.duration + 120:
            text = read(args.log)
            if t0 is None:
                if started or len(re.findall(r"\[RW\] rwlaunch: start", text)) > boot_marks:
                    t0 = time.time()
                    next_at = t0
                else:
                    time.sleep(2)
                    continue
            # Prefer the launcher's own clock: it is the same clock the
            # 5 s [THRS] and 1 MiB [NFSRTT] rows are stamped with, and a
            # mid-run start then lands on the run's real t, not the
            # sampler's.
            afters = re.findall(r"child still running after (\d+)s", text)
            t = (int(afters[-1]) + 1) if afters else int(time.time() - t0)
            if t > args.duration + 60:
                break
            got = mcopy_player(args.img, args.player_out)
            pl = read(args.player_out) if got else ""
            nfs = [int(m.group(1)) for m in
                   re.finditer(r"\[NFSRTT\] periodic .*?bytes=(\d+)", text)]
            thr = re.findall(r"\[THRS\] pid=4 .*?live=(\d+).*?res_mib=(\d+)", text)
            xsw = [int(m.group(1)) for m in re.finditer(r"\[XSW\] n=(\d+)", pl)] if pl else []
            xo = re.findall(r"\[XO\] (\S+)", text)
            phase = phase_of(pl) if pl else "(no copy)"
            last_line = ""
            if pl:
                ls = [l.strip() for l in pl.splitlines() if l.strip()]
                last_line = ls[-1][:80] if ls else ""
            retr = sum(1 for m in re.finditer(r"wait_max_us=(\d+)", text)
                       if int(m.group(1)) >= 500000)
            out.write(",".join([
                str(t),
                str(max(nfs) if nfs else 0),
                str(max(xsw) if xsw else 0),
                thr[-1][0] if thr else "",
                thr[-1][1] if thr else "",
                (xo[-1] if xo else "").replace(",", ";"),
                phase.replace(",", ";"),
                last_line.replace(",", ";"),
                str(retr),
                str(len(re.findall(r"\[BLKQ\] stall", text))),
                str(len(re.findall(r"\[NFSPIPE\] stall", text))),
                str(len(re.findall(r"\[NFSRD\] fail", text))),
                str(len(re.findall(r"\[TXT\] ZERO", text))),
                str(len(re.findall(r"KVM internal error", text))),
                str(len(re.findall(r"\[KERNEL\] pid=4 .*memory fault", text))),
            ]) + "\n")
            out.flush()
            rows += 1
            # keep the phase transitions visible on stdout too
            print(f"[sample] t={t:4d}s nfs={max(nfs) if nfs else 0:>10d} "
                  f"xsw={max(xsw) if xsw else 0:>4d} live={thr[-1][0] if thr else '?':>3} "
                  f"res={thr[-1][1] if thr else '?':>4}MiB phase={phase[:60]}",
                  flush=True)
            time.sleep(max(1, next_at + args.interval - time.time()))
            next_at += args.interval
    print(f"[sample] wrote {rows} rows to {args.out}", flush=True)


if __name__ == "__main__":
    main()
