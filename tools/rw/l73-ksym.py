#!/usr/bin/env python3
"""l73-ksym.py — resolve kernel VAs (0x70xxxxxx) against a `hobbyos.elf`.

The kernel prints raw addresses: `[KPF-SYS] rip=0x...` and
`[KPF-STK] 0x... 0x...`.  The running image carries no symbol table, so the
only honest resolution is against the exact ELF the run booted.  Keep one:
`cp hobbyos.elf obj/l73/run<N>.elf` right after the build that staged the disk.

Usage:
  tools/rw/l73-ksym.py obj/l73/run1.elf 0x700AB808 0x700B7C2D ...
  tools/rw/l73-ksym.py --serial obj/rw-desktop/serial-desktop.log obj/l73/run1.elf

`--serial` pulls every [KPF-*] row out of a serial log first and resolves the
addresses in place, so the answer sits next to the row it belongs to.
"""
import bisect
import re
import subprocess
import sys


def load_syms(elf):
    out = subprocess.run(["nm", "-n", elf], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3:
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                continue
    syms.sort()
    if not syms:
        sys.exit(f"l73-ksym: no symbols in {elf}")
    return syms, [s[0] for s in syms]


def resolve(syms, addrs, va):
    if va < 0x70000000 or va >= 0x70200000:
        return "-"  # not kernel text
    i = bisect.bisect_right(addrs, va) - 1
    if i < 0:
        return "?"
    base, name = syms[i]
    return f"{name}+0x{va - base:x}"


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    serial = None
    if argv[1] == "--serial":
        serial = argv[2]
        argv = argv[1:]
        elf = argv[2]
        addrs_in = argv[3:]
    else:
        elf = argv[1]
        addrs_in = argv[2:]

    syms, addrs = load_syms(elf)

    if serial:
        rows = [l.rstrip("\n") for l in open(serial, errors="replace")
                if "[KPF-" in l or "Kernel-mode #PF" in l]
        for row in rows:
            print(row)
            for m in re.finditer(r"0x([0-9A-Fa-f]+)", row):
                va = int(m.group(1), 16)
                r = resolve(syms, addrs, va)
                if r != "-":
                    print(f"    {m.group(0):>18} = {r}")
            m = re.search(r"\brip=0x([0-9A-Fa-f]+)", row)
            if not m:
                m = re.search(r"\bcr2=0x([0-9A-Fa-f]+)", row)
        return

    for a in addrs_in:
        va = int(a, 16)
        print(f"{a:>18} = {resolve(syms, addrs, va)}")


if __name__ == "__main__":
    main(sys.argv)
