#!/usr/bin/env python3
"""Summarise a RimWorld serial log's GC handshake + end state.

Evidence rules this encodes (docs/rimworld-launch-log.md section 23):
  * a suspend counts only when [SIGD] sig=30 (SIGPWR) was planted for a pid,
  * it is matched when a [SIGD] sig=24 (SIGXCPU) for the same pid follows and
    both handlers returned ([RT]),
  * "GC works" needs repeated matched pairs, not one.
"""
import re
import sys


def main(path):
    sigd = []          # (line, pid, sig)
    rt = []            # (line, pid, rip)
    fwk = []
    lines = open(path, "rb").read().decode("utf-8", "replace").splitlines()
    for i, ln in enumerate(lines, 1):
        m = re.search(r"\[SIGD\] pid=(\d+) sig=(\d+)", ln)
        if m:
            sigd.append((i, int(m.group(1)), int(m.group(2))))
        m = re.search(r"\[RT\] pid=(\d+) rip=0x([0-9A-Fa-f]+)", ln)
        if m:
            rt.append((i, int(m.group(1))))
        m = re.search(r"\[FWK\] tid=(\d+) sig=(\d+) why=(\S+)", ln)
        if m:
            fwk.append((i, int(m.group(1)), int(m.group(2)), m.group(3)))

    sus = [x for x in sigd if x[2] == 30]
    res = [x for x in sigd if x[2] == 24]
    # Per-pid counts: a matched handshake needs one SIGXCPU for each SIGPWR.
    pids = sorted({p for _, p, _ in sus} | {p for _, p, _ in res})
    print(f"file            : {path}")
    print(f"SIGPWR plants   : {len(sus)}   pids={sorted({p for _, p, _ in sus})}")
    print(f"SIGXCPU plants  : {len(res)}")
    print(f"[RT] returns    : {len(rt)}")
    for pid in pids:
        a = sum(1 for _, p, _ in sus if p == pid)
        b = sum(1 for _, p, _ in res if p == pid)
        print(f"  pid {pid:>3}: {a} suspend / {b} resume"
              f"{'  MATCHED' if a and a == b else ''}")
    # The 8th suspend (the old wall) and whether it was resumed.
    if len(sus) >= 8:
        ln, pid, _ = sus[7]
        later = [r for r in res if r[0] > ln and r[1] == pid]
        print(f"8th SIGPWR      : line {ln} pid={pid} -> "
              f"{'RESUMED at line %d' % later[0][0] if later else 'NEVER RESUMED'}")
    print(f"[FWK] wake rows : {len(fwk)}  "
          f"reasons={sorted({r[3] for r in fwk})}")
    # Last signal row and last 3 non-noise lines.
    if sigd:
        print(f"last signal row : line {sigd[-1][0]}: pid={sigd[-1][1]} sig={sigd[-1][2]}")
    noise = ("glX", "submit_io_cmd", "[FUTEX]", "[COW]", "[MPROT]", "[RW]",
             "[SIGDUMP] stk", "  ", "0x")
    tail = [ln for ln in lines[-400:] if ln and not ln.startswith(noise)]
    print("last 6 notable lines:")
    for ln in tail[-6:]:
        print("   ", ln[:160])


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
        print()
