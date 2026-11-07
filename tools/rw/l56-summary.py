#!/usr/bin/env python3
"""Summarise one L56 RimWorld desktop run: the mmap-race counters, the NFS
ladder, the present ladder and the screenshot's own pixel statistics.

Usage: tools/rw/l56-summary.py <serial.log> <Player.log> [final.png]

Both files are searched: the compat X11 shim and the launcher write to fd 2,
which Unity dup2()s onto Player.log after -logFile, so [XSW]/[XE] rows can be
in either.  Every number is counted; nothing is typed in.
"""
import re
import subprocess
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
    print("== L56 ladder")
    print(f"  Could not open file      {both.count('Could not open file')}")
    print(f"  Could not map file       {both.count('Could not map file')}")
    print(f"  NFS no reply             {both.count('no reply from server')}")
    print(f"  mmap insert FAIL rows    {ser.count('[MR] FAIL')}")
    print(f"  [MRACE] same-base pairs  {ser.count('[MRACE] same-base')}")
    print(f"  SIGSEGV dumps            {ser.count('SIGDUMP] SIGSEGV')}")
    print(f"  blk stalls               {both.count('[BLKQ] stall')}")

    bytes_seen = 0
    for m in re.finditer(r"\[NFSIO\] bytes=0x0x([0-9A-Fa-f]+)", ser):
        bytes_seen = int(m.group(1), 16)
    print(f"  NFS bytes                {bytes_seen/1048576:.2f} MiB")

    sw = re.findall(r"\[XSW\] n=(\d+) swap=(\d+) put=(\d+) putok=(\d+)", both)
    if sw:
        n = [int(a) for a, _, _, _ in sw]
        swaps = [int(b) for _, b, _, _ in sw]
        putok = [int(d) for _, _, _, d in sw]
        print(f"  [XSW] rows {len(sw)}  n max {max(n)}  swap max {max(swaps)}"
              f"  putok max {max(putok)}")
        for row in [l for l in both.splitlines() if "[XSW]" in l][-2:]:
            print("   " + row.strip()[:200])
    else:
        print("  [XSW] rows 0 (the client never presented)")
    print(f"  [XE] PUT rows            {both.count('[XE] PUT ')}")
    print(f"  [NMAP] ok rows           {both.count('[NMAP] ok')}")
    for pat in ("Initialize engine version", "RimWorld 1.6", "Loaded All Assemblies",
                "Finished resetting the current domain", "Loading."):
        print(f"  {pat:<32} {'yes' if pat in both else 'NO'}")

    if len(sys.argv) > 3:
        png = sys.argv[3]
        try:
            out = subprocess.run(
                ["convert", png, "-format", "%w %h %[fx:int(mean*255)] %k",
                 "info:"], capture_output=True, text=True, check=True).stdout.split()
            print(f"  screenshot {out[0]}x{out[1]} mean={out[2]} colors={out[3]}")
        except Exception as e:  # pragma: no cover - diagnostic only
            print(f"  screenshot stats failed: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
