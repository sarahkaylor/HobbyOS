#!/usr/bin/env python3
"""tools/rw/l59-summary.py -- one row per 600 s run for the L59 ladder.

Reads the desktop serial log and Player.log and prints the measures the launch
log's §46 table needs:

  NFS bytes            the last [NFSRTT] periodic ... bytes= value
  [XSW] max            the frame counter the present path prints
  fps                  [XSW] max / run seconds (the load's own frame rate)
  kernel fault rows    [KERNEL] ... memory fault / Kernel-mode #PF
  SIGX / VMRT / VMK    the L59 delivery and fault-branch rows
  THRS curve           first/last/high-water live threads, slots, regions
  Player.log           bytes, NUL runs >= 32, last non-empty line

Usage: l59-summary.py <serial.log> [Player.log]
"""
import re
import sys


def read(path):
    try:
        return open(path, "rb").read().decode("latin-1")
    except OSError:
        return ""


def main():
    serial = read(sys.argv[1])
    plog = read(sys.argv[2]) if len(sys.argv) > 2 else ""
    t = serial

    nfs = 0
    for m in re.finditer(r"\[NFSRTT\] periodic .*?bytes=(\d+)", t):
        nfs = max(nfs, int(m.group(1)))

    xsw = 0
    for m in re.finditer(r"\[XSW\][^\n]*?\bn=(\d+)", t):
        xsw = max(xsw, int(m.group(1)))

    faults = re.findall(r"\[KERNEL\][^\n]*(?:memory fault|Kernel-mode #PF)[^\n]*", t)
    sigx = re.findall(r"\[SIGX\][^\n]*", t)
    vmrt = re.findall(r"\[VMRT\][^\n]*", t)
    vmk = re.findall(r"\[VMK\][^\n]*", t)

    thrs = []
    for m in re.finditer(
        r"\[THRS\] pid=(\d+) live=(\d+) hw=(\d+) slots=(\d+)/(\d+) regs=(\d+)", t
    ):
        thrs.append(tuple(int(x) for x in m.groups()))

    # run seconds: the harness prints nothing per second; use the highest
    # [THRS] sample index (5 s cadence) as the wall clock of the load.
    secs = 5 * len(thrs) if thrs else 0

    nul_runs = 0
    if plog:
        run = 0
        for ch in plog:
            if ch == "\0":
                run += 1
                if run == 32:
                    nul_runs += 1
                    run = 0
            else:
                run = 0

    last_line = ""
    for line in plog.splitlines():
        if line.strip():
            last_line = line.strip()

    print(f"nfs_bytes={nfs} ({nfs / 1048576.0:.2f} MiB)")
    print(f"xsw_max={xsw} fps={(xsw / secs if secs else 0):.2f} (over {secs}s)")
    print(f"kernel_fault_rows={len(faults)}")
    print(f"sigx={len(sigx)} vmrt={len(vmrt)} vmk={len(vmk)}")
    if thrs:
        live = [x[1] for x in thrs]
        print(
            "thrs first=%d last=%d hw=%d slots_last=%d/%d regs_last=%d"
            % (live[0], live[-1], max(x[2] for x in thrs), thrs[-1][3],
               thrs[-1][4], thrs[-1][5])
        )
    print(f"player_log_bytes={len(plog)} nul_runs32={nul_runs}")
    print(f"player_last_line={last_line[:110]!r}")
    for row in faults[:6]:
        print("  FAULT " + row[:220])
    for row in sigx[:4]:
        print("  SIGX  " + row[:200])
    for row in vmrt[:6]:
        print("  VMRT  " + row[:200])
    for row in vmk[:4]:
        print("  VMK   " + row[:200])


if __name__ == "__main__":
    main()
