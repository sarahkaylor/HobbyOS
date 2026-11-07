#!/usr/bin/env python3
"""L39: render the present-ladder table from the final runs.

Everything is read from the run's own files (never typed by hand):
  * banner / engine init                  -> Player.log
  * [XSW] series (swap/put/crc)           -> Player.log + serial
  * [XHB] heartbeats (pumps/wins/heals)   -> serial (pre-dup2 rows land there)
  * [XI] row counts                       -> both (bounded now)
  * window pixels                         -> the run's last screenshot
"""
import glob
import os
import re
import subprocess

EV = "/home/sarah/hobbyos-rw-lanes/l39-present/obj/rw-desktop/evidence-l39"


def read(path):
    try:
        return open(path, "rb").read().decode("utf-8", "replace")
    except OSError:
        return ""


def pixels(png):
    """(black, total) for one screenshot."""
    try:
        out = subprocess.run(["convert", png, "-format", "%c", "histogram:info:"],
                             capture_output=True, text=True, timeout=120).stdout
    except Exception:
        return None
    total = black = 0
    for line in out.splitlines():
        m = re.match(r"\s*(\d+):.*#([0-9A-Fa-f]{6})", line)
        if not m:
            continue
        n = int(m.group(1))
        total += n
        if m.group(2).upper() == "000000":
            black += n
    return (black, total) if total else None


def rows(tag, n=3):
    out = []
    for i in range(1, n + 1):
        ser = read(f"{EV}/{tag}-{i}.serial.log")
        plr = read(f"{EV}/{tag}-{i}.Player.log")
        both = ser + plr
        banner = "yes" if "RimWorld 1.6" in plr else "no"
        xsw = re.findall(r"\[XSW\] n=(\d+) swap=(\d+) put=(\d+) putok=(\d+) "
                         r"flush=(\d+) crc=(0x[0-9a-f]+) changed=(\d)", both)
        xhb = re.findall(r"\[XHB\] pumps=(\d+) swaps=(\d+) puts=(\d+) putok=(\d+) "
                         r"flush=(\d+) heals=(\d+) wins=(\d+) maps=(\d+) fs=(\d+) "
                         r"grant=(\d+) ready=(\d)", both)
        xi = len(re.findall(r"^\[XI\]", both, re.M))
        grab = len(re.findall(r"^\[XI\] XIGrabTouchBegin", both, re.M))
        last = xsw[-1] if xsw else None
        lastn = xhb[-1] if xhb else None
        shots = sorted(glob.glob(f"{EV}/{tag}-*-shot*.png") +
                       glob.glob(f"{EV}/*.png"), key=os.path.getmtime)
        px = pixels(shots[-1]) if shots else None
        pxtxt = (f"{px[0]:,}/{px[1]:,} #000000 ({100.0*px[0]/px[1]:.1f}%)"
                 if px else "n/a")
        out.append((i, banner, len(xsw), last, len(xhb), lastn, xi, grab, pxtxt))
    return out


def main():
    print("| run | banner | `[XSW]` rows | last `[XSW]` (swap/put/putok/crc/changed) "
          "| `[XHB]` rows | last pump row | `[XI]` rows | window pixels |")
    print("|---|---|---|---|---|---|---|---|")
    for (i, banner, nx, last, nh, lastn, xi, grab, pxtxt) in rows("final"):
        l = (f"`swap={last[1]} put={last[2]} putok={last[3]} "
             f"crc={last[5]} changed={last[6]}`" if last else "none")
        h = (f"pumps={lastn[0]} swaps={lastn[1]} puts={lastn[2]} "
             f"flush={lastn[4]} heals={lastn[5]} wins={lastn[6]}" if lastn else "none")
        print(f"| {i} | {banner} | {nx} | {l} | {nh} | {h} | {xi} (grab {grab}) | {pxtxt} |")


if __name__ == "__main__":
    main()
