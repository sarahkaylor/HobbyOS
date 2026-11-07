#!/usr/bin/env python3
"""l73-present-rate.py — presents per second from a desktop serial log.

The present path prints one `[XSW] n=<presents> swap=... crc=0x... changed=N`
row per flush (bounded by the shim's row budget), and the launcher prints
`[RW] rwlaunch: child still running after Ns` every ten seconds.  There is no
per-row timestamp, so the launcher's counter is the clock: interpolate each
[XSW] row against the nearest preceding `after Ns` marker.

Caveats this tool states rather than hides:
  * the launcher's counter is a loop counter, not wall time (~6 % behind on a
    loaded guest, docs/rimworld-launch-plan);
  * [XSW] rows are bounded, so `n` is the authority and the rows may stop
    while presents continue -- a frozen `n` is reported as such, never as 0 fps.

Usage:
  tools/rw/l73-present-rate.py obj/rw-desktop/serial-desktop.log [--window A:B]
"""
import re
import sys

XSW = re.compile(r"\[XSW\] n=(\d+)\b.*?changed=(\d+).*?w=(\d+) h=(\d+)")
CLK = re.compile(r"child still running after (\d+)s")


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    path = argv[1]
    lo, hi = 0.0, 1e18
    for a in argv[2:]:
        if a.startswith("--window"):
            lo, hi = [float(x) for x in a.split("=", 1)[1].split(":")]

    rows = []      # (n, changed, w, h)
    clocks = []    # (line_index, seconds)
    with open(path, "r", errors="replace") as f:
        for i, line in enumerate(f):
            m = CLK.search(line)
            if m:
                clocks.append((i, float(m.group(1))))
            m = XSW.search(line)
            if m:
                rows.append((i, int(m.group(1)), int(m.group(2)),
                             int(m.group(3)), int(m.group(4))))

    if not rows:
        print("PRESENTS: no [XSW] rows (the client never presented)")
        return

    def t_at(idx):
        best = None
        for ci, sec in clocks:
            if ci <= idx:
                best = sec
            else:
                break
        return best

    first_n = rows[0][1]
    last_n = rows[-1][1]
    t0 = t_at(rows[0][0])
    t1 = t_at(rows[-1][0])
    print(f"PRESENTS: n {first_n} -> {last_n}  (+{last_n - first_n})")
    print(f"PRESENT-ROWS: {len(rows)} (bounded budget; `n` is the authority)")
    print(f"WINDOW: t={t0}s .. t={t1}s (launcher clock, ~6% behind wall)")
    if t0 is not None and t1 is not None and t1 > t0:
        print(f"RATE: {(last_n - first_n) / (t1 - t0):.2f} presents/s "
              f"({last_n - first_n} frames / {t1 - t0:.0f} s)")
    last = rows[-1]
    print(f"LAST: n={last[1]} w={last[3]} h={last[4]} changed={last[2]}")

    # per-window rates, so a stall inside a long run is visible
    print("\nWINDOW RATES (consecutive [XSW] rows, >=3 s apart):")
    prev = None
    for idx, n, ch, w, h in rows:
        t = t_at(idx)
        if t is None:
            continue
        if lo <= t <= hi and prev and t - prev[0] >= 3.0:
            dt = t - prev[0]
            dn = n - prev[1]
            print(f"  t={prev[0]:>5.0f}->{t:<5.0f} dn={dn:<5d} "
                  f"{dn / dt:6.2f} p/s")
        prev = (t, n)


if __name__ == "__main__":
    main(sys.argv)
