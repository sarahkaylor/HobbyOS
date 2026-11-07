#!/usr/bin/env python3
"""Conservatively replay the [MM] rows of a serial log and flag an mmap whose
returned range overlaps a range that is still mapped.

Ownership only: mmap adds, munmap removes, mprotect/madvise/brk change nothing.
That is enough to catch the one condition that lets two owners share a VA
("mmap handed out memory that was never released"), independent of how the
kernel splits or merges region records.
"""
import re
import sys

ROW = re.compile(
    r"\[MM\] #(\d+) (\w+) pid=(-?\d+) a=(\S+) b=(\S+) c=(\S+) d=(\S+) "
    r"e=(\S+) f=(\S+) -> (\S+)")

def h(v):
    v = v.strip()
    if v.startswith("-"):
        return -int(v[1:], 0)
    return int(v, 16)

def main(path):
    rows = []
    for line in open(path, "rb"):
        line = line.decode("utf-8", "replace")
        m = ROW.search(line)
        if not m:
            continue
        rows.append((int(m.group(1)), m.group(2), int(m.group(3)),
                     [h(m.group(i)) for i in range(4, 10)], h(m.group(10))))
    print(f"{len(rows)} rows")
    live = []          # (base, end, seq, op)
    dup = 0
    for seq, op, pid, a, ret in rows:
        if op == "mmap":
            base, ln = ret, a[1]
            if base <= 0:
                continue
            end = base + ln
            if base & 0xFFF:
                print(f"  !! #{seq} mmap UNALIGNED base={base:#x}")
            for (b, e, s, o) in live:
                if base < e and b < end:
                    dup += 1
                    print(f"  !! #{seq} mmap {base:#x}+{ln:#x} overlaps "
                          f"#{s} {o} {b:#x}+{e-b:#x}")
            live.append((base, end, seq, f"{op} pid={pid}"))
        elif op == "munmap":
            base, ln = a[0], a[1]
            end = base + ln
            out = []
            for (b, e, s, o) in live:
                if b >= base and e <= end:
                    continue                     # fully removed
                if e <= base or b >= end:
                    out.append((b, e, s, o))     # untouched
                    continue
                if b < base:
                    out.append((b, base, s, o))
                if e > end:
                    out.append((end, e, s, o))
            live = out
    print(f"live ranges at end: {len(live)}; duplicate-owner events: {dup}")
    # anything live that covers the park word?
    for (b, e, s, o) in live:
        if b <= 0x10C00964A0 < e:
            print(f"  park word inside #{s} {o} {b:#x}+{e-b:#x}")
    print("highest live ranges:")
    for (b, e, s, o) in sorted(live)[-8:]:
        print(f"  {b:#x}+{e-b:#x} #{s} {o}")

main(sys.argv[1])
