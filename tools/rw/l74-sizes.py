#!/usr/bin/env python3
"""tools/rw/l74-sizes.py -- fps vs surface size, timed by the GUEST's clock.

Input is a saved serial log containing the compat Xlib's `[XSW]` present rows
(or a whole run directory).  Each row carries

    n=<present count>  w=<surface w> h=<surface h>  gx=<fb0 x> gy=<fb0 y>
    t=<ms since this process's first present, SYS_GETTIME/CLOCK_MONOTONIC>

The rate is therefore (n2-n1)/((t2-t1)/1000) between two rows the *guest*
wrote -- no host-side follower, no dependence on when the host read the file.

Why that matters (measured, L74): the old ladder timed rows with a host
follower polling the serial log, and a 200 s run produced all its rows inside
a 69.6 s window (QEMU writes the log in bursts; a truncate-and-grow between
two polls makes the follower read from a stale offset).  That is a 2x error in
the number the ladder quotes.  The guest clock cannot drift like that.

Rows are printed every 10th present (plus the first three), so `n` is exact
and `t` is quantised to whatever the guest's clock resolution is (1 ms).

Usage:
  l74-sizes.py <serial.log | run-dir> [...] [--window 120] [--bucket 30]
               [--label NAME]

Prints, per input: the surface size, the fb0 rect it presented into, frames,
fps, Mpx/s, ms/frame, and 30 s buckets.  Exit 0 always: a slow number is a
measurement, not a failure.
"""
import argparse
import os
import re
import sys

ROW = re.compile(
    r"\[XSW\] n=(\d+).*? w=(-?\d+) h=(-?\d+) gx=(-?\d+) gy=(-?\d+)"
    r".*? t=(\d+) .*?ready=(\d)")


def rows_from(path):
    out = []
    with open(path, errors="ignore") as f:
        for line in f:
            m = ROW.search(line)
            if not m:
                continue
            out.append({
                "n": int(m.group(1)),
                "w": int(m.group(2)), "h": int(m.group(3)),
                "gx": int(m.group(4)), "gy": int(m.group(5)),
                "t": int(m.group(6)), "ready": int(m.group(7)),
            })
    return out


def report(label, rows, window, bucket):
    if len(rows) < 3:
        print(f"{label}: no timestamped [XSW] rows ({len(rows)}) -- MISSING")
        return
    # A row with t=0 is either the first present or a failed SYS_GETTIME.  The
    # last one is the useful anchor (the first three rows can precede layout).
    t0 = rows[0]["t"]
    n0 = rows[0]["n"]
    win = [r for r in rows if (r["t"] - t0) / 1000.0 <= window]
    if len(win) < 2:
        print(f"{label}: window too short ({len(win)} rows)")
        return
    last = win[-1]
    dt = (last["t"] - t0) / 1000.0
    dn = last["n"] - n0
    fps = dn / dt if dt > 0 else 0.0
    w, h = last["w"], last["h"]
    mpx = fps * w * h / 1e6
    print(f"{label}")
    print(f"   surface        {w}x{h} ({w*h/1e6:.3f} Mpx)  "
          f"fb0 rect {w}x{h}+{last['gx']}+{last['gy']}")
    print(f"   window         {dt:.1f}s of guest time, frames {n0}->{last['n']} "
          f"= {dn}")
    print(f"   fps            {fps:.2f}   ({1000.0/fps:.1f} ms/frame)"
          if fps > 0 else "   fps            0")
    print(f"   fill           {mpx:.2f} Mpx/s")
    # Buckets, from the same rows: an average hides a load phase.
    nb = max(1, int(window // bucket))
    cells = []
    for i in range(nb):
        lo, hi = t0 + i * bucket * 1000, t0 + (i + 1) * bucket * 1000
        b = [r for r in rows if lo <= r["t"] < hi]
        cells.append(f"{(b[-1]['n']-b[0]['n'])/bucket:6.2f}" if len(b) > 1 else "  0.00")
    print("   buckets        " + "  ".join(
        f"{int(i*bucket)}-{int((i+1)*bucket)}s" for i in range(nb)))
    print("   bucket fps     " + " ".join(cells))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="+")
    ap.add_argument("--window", type=float, default=120.0)
    ap.add_argument("--bucket", type=float, default=30.0)
    ap.add_argument("--label", default=None)
    a = ap.parse_args()
    for p in a.path:
        if os.path.isdir(p):
            cand = [os.path.join(p, f) for f in ("serial.log", "serial-desktop.log")
                    if os.path.exists(os.path.join(p, f))]
            if not cand:
                print(f"{p}: no serial.log")
                continue
            label = a.label or os.path.basename(p.rstrip("/"))
            report(label, rows_from(cand[0]), a.window, a.bucket)
        else:
            report(a.label or os.path.basename(p), rows_from(p),
                   a.window, a.bucket)
    return 0


if __name__ == "__main__":
    sys.exit(main())
