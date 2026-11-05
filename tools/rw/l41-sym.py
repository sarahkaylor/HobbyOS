#!/usr/bin/env python3
"""l41-sym.py — name a guest VA as `<dso>+0x<off>` using the kernel's bt rows.

The kernel prints `addr@<exec-region-base>+<off-in-region>`.  A DSO's exec
region base is `dso_base + exec_vaddr`, so for every staged DSO and every
executable PT_LOAD this tool computes the DSO base, maps the region offset to a
file offset, and accepts the match only when the bytes immediately before that
file offset are a call (E8 / FF15 / FF /2) — the same test the kernel applies
to reject data pointers living in an R+E segment.

Usage:
  tools/rw/l41-sym.py '0x1084790c9a@0x108451e000+0x272c9a' ...
"""
import struct
import sys
import os

TREE = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
DSOS = [
    "/srv/nfs/rimworld/UnityPlayer.so",
    "/srv/nfs/rimworld/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so",
    "/srv/nfs/rimworld/RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmono-native.so",
    "/srv/nfs/rimworld/RimWorldLinux_Data/Plugins/libsteam_api.so",
    "/tmp/glibc231/lib/x86_64-linux-gnu/libc-2.31.so",
    "/tmp/glibc231/lib/x86_64-linux-gnu/libpthread-2.31.so",
    "/tmp/glibc231/lib/x86_64-linux-gnu/libm-2.31.so",
    "/tmp/glibc231/lib/x86_64-linux-gnu/libdl-2.31.so",
    "/tmp/glibc231/lib/x86_64-linux-gnu/librt-2.31.so",
    "/tmp/glibc231/lib/x86_64-linux-gnu/libgcc_s.so.1",
    os.path.join(TREE, "obj/xcompat/libX11.so.6"),
    os.path.join(TREE, "obj/xcompat/libXi.so.6"),
    os.path.join(TREE, "obj/mesa/lib/libGL.so.1"),
]


def loads(path):
    with open(path, "rb") as f:
        d = f.read(64)
        if d[:4] != b"\x7fELF":
            return []
        phoff, = struct.unpack_from("<Q", d, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
        f.seek(phoff)
        ph = f.read(phentsize * phnum)
    out = []
    for i in range(phnum):
        t, flags = struct.unpack_from("<II", ph, i * phentsize)
        off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", ph, i * phentsize + 8)
        if t == 1 and (flags & 1):   # PT_LOAD, executable
            out.append((va, off, fsz))
    return out


def call_kind(data, off):
    if off >= 5 and data[off - 5] == 0xE8:
        return "call rel32"
    if off >= 6 and data[off - 6] == 0xFF and data[off - 5] == 0x15:
        return "call [rip]"
    if off >= 2 and data[off - 2] == 0xFF and (data[off - 1] & 0x38) == 0x10:
        return "call r/m64"
    return None


def main():
    for arg in sys.argv[1:]:
        addr_s, rest = arg.split("@", 1)
        reg_s, off_s = rest.split("+", 1)
        addr, reg, roff = int(addr_s, 16), int(reg_s, 16), int(off_s, 16)
        found = []
        for p in DSOS:
            if not os.path.exists(p):
                continue
            try:
                with open(p, "rb") as f:
                    data = f.read()
            except Exception:
                continue
            for va, off, fsz in loads(p):
                if roff >= fsz:
                    continue
                foff = off + roff
                kind = call_kind(data, foff)
                if kind:
                    dso_base = reg - va
                    found.append((p, dso_base, addr - dso_base, kind))
        print(f"{addr:#x} @reg {reg:#x} + {roff:#x}")
        if not found:
            print("   no DSO: offset does not follow a call in any candidate")
        for p, base, o, kind in found:
            print(f"   -> {os.path.basename(p)}+{o:#x}  (base {base:#x}, {kind})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
