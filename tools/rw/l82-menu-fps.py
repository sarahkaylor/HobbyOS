#!/usr/bin/env python3
"""tools/rw/l82-menu-fps.py -- fps over a window that starts WHERE YOU SAY.

`l74-sizes.py --window N` measures the first N seconds of guest time, which is
the loading phase; a menu number needs a window that starts after the menu is
up (L78: game t=533-549 s).  This tool takes the window as
`--from-t <guest seconds>` (or `--from-n <present number>`) and reports:

  * the fps over that window, timed by the guest's own CLOCK_MONOTONIC (the
    `t=` field every `[XSW]` row carries) -- no host-side log follower;
  * the *phase table* before the window, so a reader can see whether the
    window really is the menu;
  * the static-frame run: the earliest present whose CRC then never changes
    again (`changed=0` from there to the end).  L78 used exactly that to
    identify the menu, because a static frame with presents still advancing is
    a menu, not a frozen pipeline.

Input: a serial log with the compat Xlib's [XSW] rows (obj/l62/<name>/serial.log
or obj/rw-desktop/serial-desktop.log).  Exit 0 always: a slow number is a
measurement, not a failure.

Usage:
  l82-menu-fps.py <serial.log> [--from-t 560] [--window 120] [--bucket 30]
                  [--label NAME]
"""
import argparse
import re
import sys

ROW = re.compile(
    r"\[XSW\] n=(\d+).*? crc=(0x[0-9a-f]+) changed=(\d).*?"
    r" w=(-?\d+) h=(-?\d+) gx=(-?\d+) gy=(-?\d+).*?"
    r" t=(\d+) ready=(\d) nz=(\d+)/(\d+)")


def rows_from(path):
    out = []
    with open(path, errors="ignore") as f:
        for line in f:
            m = ROW.search(line)
            if not m:
                continue
            out.append(dict(
                n=int(m.group(1)), crc=m.group(2), changed=int(m.group(3)),
                w=int(m.group(4)), h=int(m.group(5)),
                gx=int(m.group(6)), gy=int(m.group(7)),
                t=int(m.group(8)), ready=int(m.group(9)),
                nz=int(m.group(10)), tot=int(m.group(11))))
    # The row goes to fd 2 AND to /dev/console (a polled UART), so a busy
    # console can deliver its copy of an early row late -- measured: a 20-row
    # prefix of the first presents reappeared at the END of a 6.6 MB log, and
    # an unsorted reader then reported "1..20 over 19 s" for a 4 200-present
    # run.  Sort by the guest clock and keep one row per present number.
    seen, uniq = set(), []
    for r in sorted(out, key=lambda r: (r["t"], r["n"])):
        if r["n"] in seen:
            continue
        seen.add(r["n"])
        uniq.append(r)
    # Pre-layout rows (ready=0) carry the deferred 1280x720 request, not the
    # surface the run measured; they also have t=0.
    return [r for r in uniq if r["ready"] == 1] or uniq


def fps_between(rows, lo, hi):
    """fps between two guest timestamps (seconds), from the rows inside."""
    win = [r for r in rows if lo <= r["t"] / 1000.0 <= hi]
    if len(win) < 2:
        return None, win
    dt = (win[-1]["t"] - win[0]["t"]) / 1000.0
    dn = win[-1]["n"] - win[0]["n"]
    return (dn / dt if dt > 0 else 0.0), win


def static_run(rows, min_rows=20):
    """Earliest row from which the CRC never changes again (the menu)."""
    last = rows[-1]["crc"]
    start = None
    for r in reversed(rows):
        if r["crc"] == last:
            start = r
        else:
            break
    if start is None:
        return None
    n_after = sum(1 for r in rows if r["n"] >= start["n"])
    return start if n_after >= min_rows else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--from-t", type=float, default=None,
                    help="window start, guest seconds since the first present")
    ap.add_argument("--from-n", type=int, default=None,
                    help="window start, present number")
    ap.add_argument("--window", type=float, default=120.0)
    ap.add_argument("--bucket", type=float, default=30.0)
    ap.add_argument("--label", default=None)
    a = ap.parse_args()

    rows = rows_from(a.path)
    label = a.label or a.path.rsplit("/", 1)[-1]
    if len(rows) < 3:
        print(f"{label}: only {len(rows)} [XSW] rows -- no measurement")
        return 0

    first, last = rows[0], rows[-1]
    span = (last["t"] - first["t"]) / 1000.0
    print(f"{label}")
    print(f"   surface        {last['w']}x{last['h']} "
          f"({last['w'] * last['h'] / 1e6:.3f} Mpx)  "
          f"fb0 rect {last['w']}x{last['h']}+{last['gx']}+{last['gy']}")
    print(f"   presents       {first['n']}..{last['n']} over {span:.1f}s "
          f"of guest time = {last['n'] / span:.2f} fps overall")

    # Which present is the static frame (the menu, if the run reached it)?
    st = static_run(rows)
    if st:
        after = [r for r in rows if r["n"] >= st["n"]]
        print(f"   static frame   from n={st['n']} t={st['t'] / 1000.0:.0f}s "
              f"(crc {st['crc']}, {len(after)} rows, "
              f"{(after[-1]['t'] - st['t']) / 1000.0:.0f}s)")
    else:
        print("   static frame   none (the CRC never settles)")

    # Phase table, 60 s buckets over the whole run.
    print(f"   phases         {'window':>10}  {'fps':>6}  {'ms/frame':>8}  crc")
    t0 = first["t"] / 1000.0
    t_end = last["t"] / 1000.0
    t = t0
    while t < t_end:
        f, win = fps_between(rows, t, min(t + 60.0, t_end + 0.001))
        if f is None:
            t += 60.0
            continue
        crc = win[-1]["crc"]
        print(f"                  {int(t):5d}-{int(t + 60):<5d} {f:6.2f}  "
              f"{1000.0 / f if f > 0 else 0:8.1f}  {crc}")
        t += 60.0

    # The measurement window.
    if a.from_n is not None:
        cand = [r for r in rows if r["n"] >= a.from_n]
        lo = cand[0]["t"] / 1000.0 if cand else 0.0
    else:
        lo = a.from_t if a.from_t is not None else t0
    hi = lo + a.window
    f, win = fps_between(rows, lo, hi)
    if f is None:
        print(f"   WINDOW {lo:.0f}-{hi:.0f}s: no rows (window too short)")
        return 0
    dt = (win[-1]["t"] - win[0]["t"]) / 1000.0
    dn = win[-1]["n"] - win[0]["n"]
    print(f"   WINDOW         {lo:.0f}->{win[-1]['t'] / 1000.0:.0f}s of guest "
          f"time ({dt:.1f}s), presents {win[0]['n']}->{win[-1]['n']} = {dn}")
    print(f"   fps            {f:.2f}   "
          f"({1000.0 / f:.1f} ms/frame)" if f > 0 else "   fps            0")
    print(f"   fill           {f * win[-1]['w'] * win[-1]['h'] / 1e6:.2f} Mpx/s")
    # 30 s buckets inside the window, so a phase change inside it is visible.
    nb = max(1, int(a.window // a.bucket))
    cells = []
    for i in range(nb):
        b0, b1 = lo + i * a.bucket, lo + (i + 1) * a.bucket
        bf, _ = fps_between(rows, b0, b1)
        cells.append(f"{bf:6.2f}" if bf is not None else "  0.00")
    print("   bucket fps     " + " ".join(cells) +
          "   (" + f"{int(a.bucket)}s buckets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
