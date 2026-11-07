#!/usr/bin/env python3
"""l41-bt.py — turn a [FUTEX] bt row into module+offset.

The kernel prints raw user-stack words (only those in executable regions) and
the address-space map as [FUTEX] map rows.  This tool joins them: it merges the
r--/r-x segment pair of each DSO into one span, then reports every backtrace
word as `span_base+offset` and tests it against the staged DSOs / UnityPlayer so
the row reads `libpthread+0xf376` instead of `0x1083503376`.

Usage:
  tools/rw/l41-bt.py <serial.log> [pid] [--dso-dir DIR ...]
"""
import re
import struct
import subprocess
import sys
import os

MAP_RE = re.compile(r"\[FUTEX\] map base=(0x[0-9A-Fa-f]+) len=(0x[0-9A-Fa-f]+) "
                    r"kind=(\d+) prot=(\d+)")
ST_RE = re.compile(r"\[FUTEX\] st pid=(\d+) sp=(0x[0-9A-Fa-f]+)((?: 0x[0-9A-Fa-fa-f]+)+) word=(0x[0-9A-Fa-f]+)")
BT_RE = re.compile(r"\[FUTEX\] bt pid=(\d+)((?: 0x[0-9A-Fa-f]+)+)")
Q_RE = re.compile(r"\[FUTEX\] q slot=\d+ pid=(\d+) st=(\d+) as=(0x[0-9A-Fa-f]+) key=(0x[0-9A-Fa-f]+)[^ ]* bitset=[^ ]+ waits=(\d+) wakes=(\d+)(.*)$")


def parse_maps(lines):
    rows = []
    for l in lines:
        m = MAP_RE.search(l)
        if m:
            rows.append((int(m.group(1), 16), int(m.group(2), 16),
                         int(m.group(3)), int(m.group(4))))
    rows.sort()
    return rows


def spans(rows):
    """Merge [r--, r-x] pairs into one DSO span (base, end, textbase, textend)."""
    out = []
    i = 0
    while i < len(rows):
        b, n, k, p = rows[i]
        if p == 1 and i + 1 < len(rows):
            b2, n2, k2, p2 = rows[i + 1]
            if p2 == 5 and b2 == b + n and k2 == k:
                out.append((b, b + n + n2, b2, b2 + n2))
                i += 2
                continue
        if p == 5:
            out.append((b, b + n, b, b + n))
        i += 1
    return out


def find_span(sp, a):
    for s in sp:
        if s[0] <= a < s[1]:
            return s
    return None


def dso_text(path):
    """Return [(vaddr, filesz, flags)] for PT_LOADs of an ELF, or None."""
    try:
        with open(path, 'rb') as f:
            data = f.read(64)
            if data[:4] != b'\x7fELF' or data[4] != 2:
                return None
            e_phoff, = struct.unpack_from('<Q', data, 0x20)
            e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x36)
            f.seek(e_phoff)
            ph = f.read(e_phentsize * e_phnum)
        segs = []
        for i in range(e_phnum):
            p_type, p_flags = struct.unpack_from('<II', ph, i * e_phentsize)
            p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
                struct.unpack_from('<QQQQQQ', ph, i * e_phentsize + 8)
            if p_type == 1:  # PT_LOAD
                segs.append((p_vaddr, p_filesz, p_flags))
        return segs
    except Exception:
        return None


def nearest_sym(path, off):
    try:
        out = subprocess.run(['nm', '-D', '--defined-only', path],
                             capture_output=True, text=True, timeout=20).stdout
    except Exception:
        return None
    best = None
    for l in out.splitlines():
        p = l.split()
        if len(p) < 3:
            continue
        try:
            v = int(p[0], 16)
        except ValueError:
            continue
        if v <= off and (best is None or v > best[0]):
            best = (v, p[2])
    return best


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    log = sys.argv[1]
    pid = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2].isdigit() else '4'
    lines = open(log, 'rb').read().decode('utf-8', 'replace').splitlines()
    rows = parse_maps(lines)
    sp = spans(rows)
    for l in lines:
        m = ST_RE.search(l)
        if m and m.group(1) == pid:
            print(l.strip()[:200])
        m = BT_RE.search(l)
        if m and m.group(1) == pid:
            addrs = [int(x, 16) for x in m.group(2).split()]
            print(f"[bt pid={pid}]")
            for a in addrs:
                s = find_span(sp, a)
                if not s:
                    print(f"  {a:#x} -> no merged span")
                    continue
                print(f"  {a:#x} -> span {s[0]:#x} text {s[2]:#x} "
                      f"off_from_span {a - s[0]:#x} off_from_text {a - s[2]:#x}")
        m = Q_RE.search(l)
        if m and m.group(1) == pid:
            print("q:", l.strip()[:190])
    return 0


if __name__ == '__main__':
    sys.exit(main())
