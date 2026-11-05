#!/usr/bin/env python3
"""L38: read a run's [MM] trace and state the JIT placement facts.

Answers, from the request stream (not from guesses):
  * the exec-anonymous mmap sequence (len, hint, result);
  * the free-gap map at the instant the first JIT chunk is placed;
  * what the largest gap inside +-2 GiB of that anchor was, and how big the
    arena could have been;
  * what jumped in front of the chunk that left the +-2 GiB window;
  * how many exec mprotects (code-manager commits) happened and their sizes.

Usage: l38-layout.py <serial.log>
"""
import re
import sys

USER_VA_BASE = 0x1000000000
MMAP_BASE = USER_VA_BASE + 0x80000000
MMAP_LIMIT = MMAP_BASE + 0x480000000
PG = 0x1000

ROW = re.compile(r"\[MM\] #(\d+) (\w+) pid=(-?\d+) a=(\S+) b=(\S+) c=(\S+) "
                 r"d=(\S+) e=(\S+) f=(\S+) -> (\S+)")


def h(v):
    v = v.strip()
    if v.startswith("0x"):
        v = v[2:]
    return int(v, 16) if v else 0


def up(n, a=PG):
    return (n + a - 1) & ~(a - 1)


def main(path):
    rows = []
    for line in open(path, "rb"):
        m = ROW.search(line.decode("utf-8", "replace"))
        if m:
            rows.append((int(m.group(1)), m.group(2), int(m.group(3)),
                         [h(m.group(i)) for i in (4, 5, 6, 7, 8, 9)],
                         h(m.group(10))))
    print(f"{len(rows)} [MM] rows")

    live = []            # (base, end, seq, op, prot, flags)
    exec_mmaps = []
    exec_mprots = []
    snapshot = None
    first_exec_seq = None

    for seq, op, pid, a, ret in rows:
        if op == "mmap":
            ln = a[1]
            prot, flags = a[2], a[3]
            exec_anon = (prot & 4) and (flags & 0x20) and not (flags & 0x10)
            if ret > 0 and exec_anon:
                exec_mmaps.append((seq, a[0], ret, ln))
                if first_exec_seq is None:
                    first_exec_seq = seq
                    snapshot = list(live)
            if ret > 0:
                live.append((ret, ret + up(ln), seq, op, prot, flags))
        elif op == "munmap":
            base, ln = a[0], up(a[1])
            end = base + ln
            out = []
            for r in live:
                b, e = r[0], r[1]
                if e <= base or b >= end:
                    out.append(r)
                elif b >= base and e <= end:
                    continue
                else:
                    if b < base:
                        out.append((b, base) + r[2:])
                    if e > end:
                        out.append((end, e) + r[2:])
            live = out
        elif op == "mprotect":
            base, ln, prot = a[0], up(a[1]), a[2]
            if (prot & 4) and ret == 0:
                exec_mprots.append((seq, base, ln, a[0]))

    print(f"\nexec anon mmaps: {len(exec_mmaps)}; exec mprotects: {len(exec_mprots)}")
    print("\n[JIT chunks]")
    base0 = exec_mmaps[0][2] if exec_mmaps else 0
    for seq, hint, got, ln in exec_mmaps:
        print(f"  #{seq:5d} hint={hint:#014x} got={got:#014x} len={ln:#08x} "
              f"d0={(got - base0) / 2**30:+7.3f} GiB "
              f"hint_ok={'Y' if hint == got else 'n'}")

    if exec_mprots:
        print("\nfirst 12 exec mprotects (code-manager commits):")
        for seq, base, ln, raw in exec_mprots[:12]:
            print(f"  #{seq:5d} addr={raw:#014x} len={ln:#010x}")

    # ---- free-gap map at the first JIT chunk --------------------------------
    if snapshot is None:
        print("\nno exec anon mmap in this log")
        return
    live = sorted(snapshot)
    merged = []
    for b, e, *_ in live:
        if merged and b <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([b, e])
    anchor = exec_mmaps[0][2]
    lo = max(MMAP_BASE, anchor - 0x80000000)
    hi = min(MMAP_LIMIT, anchor + 0x80000000)
    print(f"\nfree gaps inside the +-2 GiB window of the first chunk "
          f"[{lo:#x}, {hi:#x}):")
    gaps = []
    cur = up(lo)
    for b, e in merged:
        if e <= cur:
            continue
        if b >= hi:
            break
        if b > cur:
            gaps.append((cur, min(b, hi)))
        cur = max(cur, up(e))
        if cur >= hi:
            break
    if cur < hi:
        gaps.append((cur, hi))
    gaps = [(b, e) for b, e in gaps if e > b]
    gaps.sort(key=lambda g: -(g[1] - g[0]))
    for b, e in gaps[:8]:
        print(f"  {b:#014x} .. {e:#014x}  {(e - b) / 2**20:9.2f} MiB  "
              f"d(anchor)={(b - anchor) / 2**30:+.3f} GiB")
    if gaps:
        b, e = gaps[0]
        print(f"  -> largest = {(e - b) / 2**20:.2f} MiB at {b:#x}")

    # ---- what jumped in front of the chunk that left the window ------------
    if len(exec_mmaps) >= 2:
        last_in, last_out = None, None
        for i, (seq, hint, got, ln) in enumerate(exec_mmaps):
            if abs(got - exec_mmaps[i - 1][2]) > 0x80000000 and i:
                last_in = exec_mmaps[i - 1]
                last_out = exec_mmaps[i]
                break
        if last_out is not None and last_in is not None:
            print(f"\nchunk #{last_out[0]} jumped {last_out[2] - last_in[2]:#x} "
                  f"bytes; rows in between:")
            for seq, op, pid, a, ret in rows:
                if last_in[0] < seq < last_out[0]:
                    print(f"  #{seq} {op} pid={pid} a={[hex(x) for x in a]} -> {ret:#x}")


main(sys.argv[1])
