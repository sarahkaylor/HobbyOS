#!/usr/bin/env python3
"""Summarise one L57 RimWorld desktop run: the EBADF/language wall, the fd
flight-recorder dumps, the NFS socket/port telemetry and the present ladder.

Usage: tools/rw/l57-summary.py <serial.log> <Player.log> [final.png]

Both files are searched: the compat X11 shim and the launcher write to fd 2,
which Unity dup2()s onto Player.log after -logFile, so [XSW]/[XE] rows can be
in either.  Every number is counted; nothing is typed in.
"""
import re
import sys


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("latin1", "replace")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ser, pj = read(sys.argv[1]), read(sys.argv[2])
    both = ser + "\n" + pj
    print("== L57 ladder")
    print(f"  Invalid handle           {both.count('Invalid handle')}")
    print(f"  Exception loading lang   {both.count('Exception loading language')}")
    print(f"  Could not open file      {both.count('Could not open file')}")
    print(f"  Could not map file       {both.count('Could not map file')}")
    print(f"  NFS no reply             {both.count('no reply from server')}")
    print(f"  SIGSEGV dumps            {ser.count('SIGDUMP] SIGSEGV')}")
    print(f"  memory fault rows        {ser.count('memory fault')}")
    print(f"  [FDR] dumps              {ser.count('[FDR] fail')}")
    print(f"  [LFRE]/readfail          {ser.count('[FDR] readfail')}")
    print(f"  [NFSRD] fail rows        {ser.count('[NFSRD] fail')}")
    print(f"  [NFSNOSOCK] rows         {ser.count('[NFSNOSOCK]')}")
    print(f"  [NSPORT] skipped rows    {ser.count('[NSPORT]')}")
    print(f"  [NFSCHK] rows            {ser.count('[NFSCHK]')}")
    for line in [l for l in ser.splitlines() if "[NFSCHK]" in l][:2]:
        print("   " + line.strip()[:200])
    print(f"  [NFSLS] rows             {ser.count('[NFSLS]')}")
    last_nsock = [l for l in ser.splitlines() if "[NSOCK]" in l]
    if last_nsock:
        print("   " + last_nsock[-1].strip()[:200])

    bytes_seen = 0
    for m in re.finditer(r"\[NFSIO\] bytes=0x0x([0-9A-Fa-f]+)", ser):
        bytes_seen = int(m.group(1), 16)
    print(f"  NFS bytes                {bytes_seen/1048576:.2f} MiB")

    sw = re.findall(r"\[XSW\] n=(\d+) swap=(\d+) put=(\d+) putok=(\d+)", both)
    if sw:
        n = [int(a) for a, _, _, _ in sw]
        swaps = [int(b) for _, b, _, _ in sw]
        print(f"  [XSW] rows {len(sw)}  n max {max(n)}  swap max {max(swaps)}")
        for row in [l for l in both.splitlines() if "[XSW]" in l][-1:]:
            print("   " + row.strip()[:200])
    else:
        print("  [XSW] rows 0 (the client never presented)")
    print(f"  [NMAP] ok rows           {both.count('[NMAP] ok')}")
    for pat in ("RimWorld 1.6", "Loading defs", "Tried to use an uninitialized DefOf"):
        print(f"  {pat:<32} {'yes' if pat in both else 'NO'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
