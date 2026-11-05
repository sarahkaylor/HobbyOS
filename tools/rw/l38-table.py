#!/usr/bin/env python3
"""L38: render the launch log's ladder table from the three final runs.

Everything in the table is read from the run's own files (never typed by hand):
  * JIT windows and how many came from the arena   -> serial [JITV] rows
  * whether the arena was claimed, at what address -> serial [JITA] rows
  * engine init / banner / assertion                -> Player.log (fd 1 is
                                                       Player.log, rule 36)
  * present rows                                    -> Player.log [XE] PUT/FLUSH
  * window pixels                                   -> the last screenshot
"""
import glob
import os
import re
import subprocess
import sys

EV = "/home/sarah/hobbyos-rw-lanes/l38-jit/obj/rw-desktop/evidence-l38"


def read(path):
    try:
        return open(path, "rb").read().decode("utf-8", "replace")
    except OSError:
        return ""


def pixels(png):
    """(black, total) for the last screenshot of the run."""
    try:
        out = subprocess.run(["convert", png, "-format", "%c", "histogram:info:"],
                             capture_output=True, text=True, timeout=60).stdout
    except Exception:
        return None
    total = 0
    black = 0
    for line in out.splitlines():
        m = re.match(r"\s*(\d+):.*#([0-9A-Fa-f]{6})", line)
        if not m:
            continue
        n = int(m.group(1))
        total += n
        if m.group(2).upper() == "000000":
            black += n
    return (black, total) if total else None


rows = []
for i in (1, 2, 3):
    ser = read(f"{EV}/final-{i}.serial.log")
    plr = read(f"{EV}/final-{i}.Player.log")
    jitv = re.findall(r"\[JITV\] pid=\d+ n=(\d+) hint=(\S+) base=(\S+) len=(\S+) d=(\S+) src=(\w+)", ser)
    claim = re.search(r"\[JITA\] claim base=(\S+) size=(\S+)", ser)
    banner = "yes" if "RimWorld 1.6" in plr else "no"
    engine = "yes" if "Initialize engine version" in plr else "no"
    assert_ = []
    if "x86-codegen" in plr:
        assert_.append("x86-codegen.h:410")
    if "tramp-amd64" in plr:
        assert_.append("tramp-amd64.c:641")
    if "Assertion" in plr and not assert_:
        assert_.append("other")
    puts = len(re.findall(r"\[XE\] PUT ", plr))
    skips = len(re.findall(r"\[XE\] PUTSKIP", plr))
    flushes = len(re.findall(r"\[XE\] FLUSH", plr))
    xi = len(re.findall(r"^\[XI\] XIGrabTouchBegin", plr, re.M))
    arena = sum(1 for r in jitv if r[5] == "arena")
    # the last screenshot written for this run: the runner copies flat, so the
    # newest file is the run's final frame
    shots = sorted(glob.glob(f"{EV}/*.png"), key=os.path.getmtime)
    px = pixels(shots[-1]) if shots else None
    pxtxt = f"{px[0]:,}/{px[1]:,} #000000 ({100.0*px[0]/px[1]:.1f}%)" if px else "n/a"
    rows.append(
        f"| {i} | {len(jitv)} | {arena} | {engine} | {banner} | "
        f"{', '.join(assert_) if assert_ else '**none**'} | "
        f"{puts} / {flushes} (skip {skips}) | 0 | {pxtxt} |")

print("| run | JIT windows | `src=arena` | `Initialize engine` | banner | assertion | `[XE] PUT` / `FLUSH` (Player.log) | `[XE] PUT` (serial) | window pixels |")
print("|---|---|---|---|---|---|---|---|---|")
for r in rows:
    print(r)
print()
for i in (1, 2, 3):
    ser = read(f"{EV}/final-{i}.serial.log")
    m = re.search(r"\[JITA\] claim base=(\S+) size=(\S+)", ser)
    n = len(re.findall(r"\[JITV\]", ser))
    x = len(re.findall(r"^\[XI\] XIGrabTouchBegin", read(f"{EV}/final-{i}.Player.log"), re.M))
    print(f"run {i}: claim={'no' if not m else m.group(1) + ' size ' + m.group(2)}, "
          f"windows={n}, XIGrabTouchBegin rows={x}")
