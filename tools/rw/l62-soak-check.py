#!/usr/bin/env python3
"""tools/rw/l62-soak-check.py -- the L62 soak tripwire table.

One row per assertion, each counted from the serial log (and Player.log for the
FAT16 NUL runs).  The rule of this lane: a tripwire that fires is reported with
its tuple, never rounded into "0-ish"; a tripwire that never fires is reported
with the row that would have carried it (so an absent instrument cannot read as
a pass).

Usage: l62-soak-check.py <serial.log> [Player.log]
"""
import re
import sys


def read(path):
    try:
        return open(path, "rb").read().decode("latin-1")
    except OSError:
        return ""


def count(pat, t):
    return len(re.findall(pat, t))


def rows(pat, t, limit=6, width=240):
    return [m.group(0)[:width] for m in re.finditer(pat, t)][:limit]


def main():
    serial = read(sys.argv[1])
    plog = read(sys.argv[2]) if len(sys.argv) > 2 else ""
    s = serial

    txt_summ = re.findall(r"\[TXT\] n=(\d+) bad=(\d+)", s)
    txt_n, txt_bad = (int(txt_summ[-1][0]), int(txt_summ[-1][1])) if txt_summ else (0, 0)

    xsw = [int(m.group(1)) for m in re.finditer(r"\[XSW\] n=(\d+)", s)]
    nfs = [int(m.group(1)) for m in re.finditer(r"\[NFSRTT\] periodic .*?bytes=(\d+)", s)]
    thrs = re.findall(r"\[THRS\] pid=(\d+) live=(\d+) gt=\d+ hw=(\d+) slots=(\d+)/(\d+) regs=(\d+)", s)

    nul32 = nul128 = 0
    run = 0
    for ch in plog:
        if ch == "\0":
            run += 1
            if run == 32:
                nul32 += 1
            if run == 128:
                nul128 += 1
        else:
            run = 0
    # a NUL run at the file END is the chunk in flight when the process died
    # (measured §45.8), not a mid-file hole: report both.
    tail_nul = len(plog) - len(plog.rstrip("\0"))

    last_line = ""
    for line in plog.splitlines():
        if line.strip():
            last_line = line.strip()

    checks = [
        ("zero-pages published to exec mappings",
         count(r"\[TXT\] ZERO ", s) + count(r"\[TXT\] UNVERIFIED ", s)
         + count(r"\[TXT\] IMG UNVERIFIED ", s),
         r"\[TXT\] (?:ZERO|UNVERIFIED|IMG UNVERIFIED)[^\n]*"),
        ("short page reads refused", count(r"\[ZPFILL\]", s), r"\[ZPFILL\][^\n]*"),
        ("block-queue stalls", count(r"\[BLKQ\] stall", s), r"\[BLKQ\] stall[^\n]*"),
        ("block-queue controller resets", count(r"\[BLKQ\] controller reset", s),
         r"\[BLKQ\] controller reset[^\n]*"),
        ("block-queue give-ups", count(r"\[BLKQ\] giving up|\[BLKQ\] reinit failed", s),
         r"\[BLKQ\] (?:giving up|reinit failed)[^\n]*"),
        ("STILL SPINNING rows", count(r"STILL SPINNING", s), r"STILL SPINNING[^\n]*"),
        ("slow block requests (>250 ms)", count(r"\[BLKSLOW\]", s), r"\[BLKSLOW\][^\n]*"),
        ("stale CQEs drained", count(r"\[BLKQ\] drain stale cqe", s),
         r"\[BLKQ\] drain stale cqe[^\n]*"),
        ("present-leaf retries [VMRT]", count(r"\[VMRT\]", s), r"\[VMRT\][^\n]*"),
        ("retry-budget kills [VMK]", count(r"\[VMK\]", s), r"\[VMK\][^\n]*"),
        ("in-place leaf repairs [VMFIX]", count(r"\[VMFIX\]", s), r"\[VMFIX\][^\n]*"),
        ("delivered SIGSEGV/BUS/ILL/ABRT [SIGX]", count(r"\[SIGX\]", s), r"\[SIGX\][^\n]*"),
        ("kernel memory-fault rows", count(r"\[KERNEL\][^\n]*(?:memory fault|Kernel-mode #PF)", s),
         r"\[KERNEL\][^\n]*(?:memory fault|Kernel-mode #PF)[^\n]*"),
        ("non-#PF user exceptions (vector rows)",
         count(r"\[KERNEL\] User process fault! Vector:", s),
         r"\[KERNEL\] User process fault![^\n]*"),
        ("fault-pc rows", count(r"\[KERNEL\] fault-pc", s), r"\[KERNEL\] fault-pc[^\n]*"),
        ("Player.log mid-file NUL runs >=32 B", nul32, r"n/a"),
        ("Player.log NUL runs >=128 B", nul128, r"n/a"),
    ]

    print("== L62 soak tripwire table")
    print(f"   serial log        {len(s)} bytes")
    print(f"   frames            [XSW] max n={max(xsw) if xsw else 0} "
          f"({len(xsw)} rows)")
    print(f"   NFS bytes         {max(nfs) if nfs else 0} "
          f"({(max(nfs) if nfs else 0)/1048576.0:.2f} MiB)")
    print(f"   [TXT] exec pages  n={txt_n} bad={txt_bad}"
          + ("" if txt_summ else "  (NO [TXT] SUMMARY ROW -- gate never ran or died early)"))
    if thrs:
        live = [int(x[1]) for x in thrs]
        print(f"   [THRS]            samples={len(thrs)} first={live[0]} last={live[-1]} "
              f"hw={max(int(x[2]) for x in thrs)} slots_last={thrs[-1][3]}/{thrs[-1][4]} "
              f"regs_last={thrs[-1][5]}")
    else:
        print("   [THRS]            NO SAMPLES")
    print(f"   Player.log        {len(plog)} bytes, tail_nul={tail_nul}, "
          f"last_line={last_line[:100]!r}")
    print("")
    print(f"   {'assertion':<44} {'count':>6}")
    print("   " + "-" * 52)
    fired = []
    for name, n, pat in checks:
        mark = "" if n == 0 else "   <-- FIRED"
        print(f"   {name:<44} {n:>6}{mark}")
        if n:
            fired.append((name, pat))
    if fired:
        print("")
        print("== fired: tuples")
        for name, pat in fired:
            print(f"-- {name}")
            for r in rows(pat, s):
                print("   " + r)
    print("")
    for pats, label in (
            ((r"\[BLKSLOW\][^\n]*", r"\[BLKQ\][^\n]*"), "block-layer rows (last 6)"),
            ((r"\[TXT\][^\n]*", r"\[ZPFILL\][^\n]*"), "page-in verification rows (last 6)"),
            ((r"\[VM[KRFT]\][^\n]*", r"\[VMFIX\][^\n]*"), "fault-branch rows (last 6)")):
        found = []
        for p in pats:
            found += re.findall(p, s)
        print(f"-- {label}: {len(found)}")
        for r in found[-6:]:
            print("   " + r[:240])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
