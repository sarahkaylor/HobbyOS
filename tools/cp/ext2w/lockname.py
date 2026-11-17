#!/usr/bin/env python3
"""tools/cp/ext2w/lockname.py ELF ADDR... -- name kernel lock addresses.

Prints "<addr> <symbol>+<off>", and for an address inside the global file
table (gft_static) the slot and which ylock of struct file it is (the field
offsets come from the DWARF-free layout the kernel prints nowhere, so they
are passed in: LOCKNAME_FILE="<sizeof> <lock off> <pos_lock off>").
Used by reg_gate.sh to name [YLOCK-STALL] rows whose site is '?'.
"""
import os
import subprocess
import sys

elf = sys.argv[1]
out = subprocess.run(["llvm-nm", "-n", "--print-size", elf],
                     capture_output=True, text=True).stdout
syms = []
for line in out.splitlines():
    p = line.split()
    if len(p) >= 4:
        try:
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
        except ValueError:
            pass
lay = os.environ.get("LOCKNAME_FILE", "").split()
for a in sys.argv[2:]:
    v = int(a, 16)
    hit = [s for s in syms if s[0] <= v < s[0] + s[1]]
    if not hit:
        print(a, "?")
        continue
    base, size, name = hit[0]
    off = v - base
    extra = ""
    if name == "gft_static" and len(lay) == 3:
        sz, lk, pk = (int(x) for x in lay)
        slot, fo = divmod(off, sz)
        what = "f->lock" if fo == lk else "pos_lock" if fo == pk else "field+%d" % fo
        extra = " = global file slot %d %s" % (slot, what)
    print("%s %s+0x%x%s" % (a, name, off, extra))
