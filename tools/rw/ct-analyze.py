#!/usr/bin/env python3
"""tools/rw/ct-analyze.py — read [CT] condvar rows and name a lost wake.

Rows (from tools/rw/mono-probe/condtrace.c), one per line, in program order:

  [CT] W tid=N cond=0x.. mutex=0x..          pthread_cond_wait entered
  [CT] T tid=N cond=0x.. mutex=0x.. to=MS    pthread_cond_timedwait entered
  [CT] w tid=N cond=0x.. mutex=0x.. ret=R    wait returned
  [CT] t tid=N cond=0x.. mutex=0x.. to=MS ret=R
  [CT] S tid=N cond=0x..                     pthread_cond_signal
  [CT] B tid=N cond=0x..                     pthread_cond_broadcast

For every wait that never returned, report whether its cond was signalled
after the wait began.  That single fact separates the two candidate layers:

  * signalled after the wait began, wait still parked -> the wake was lost at
    or below glibc (our kernel/futex), because the cond sequence number and
    the signalled state are both visible;
  * never signalled -> Mono never reached the cond with that waiter, i.e. the
    waiter was not on the list the pulser walked (monitor bookkeeping).
"""
import re
import sys
from collections import defaultdict

ROW = re.compile(r"\[CT\] (W|T|w|t|S|B) tid=(-?\d+)(?: cond=(0x[0-9a-f]+))?(?: mutex=(0x[0-9a-f]+))?(?: to=(-?\d+))?(?: ret=(-?\d+))?( main=1)?")
HEX = re.compile(r"\[CT\] (W|T|w|t|S|B) tid=(\d+) cond=(0x[0-9a-f]+)")


def parse(path):
    ev = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            if "[CT] " not in line:
                continue
            i = line.find("[CT] ")
            m = ROW.match(line, i)
            if not m:
                continue
            kind, tid, cond, mutex, to, ret, main = m.groups()
            ev.append((kind, int(tid), cond or "0x0", mutex or "0x0",
                       int(to) if to is not None else None,
                       int(ret) if ret is not None else None, bool(main)))
    return ev


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "obj/tp-probe/serial-plinq-ct1.log"
    tail = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    ev = parse(path)
    print(f"rows: {len(ev)}  file: {path}")
    if not ev:
        return

    pending = defaultdict(list)   # tid -> [(idx, cond, kind, to)]
    unmatched_ret = []
    for idx, (kind, tid, cond, mutex, to, ret, main) in enumerate(ev):
        if kind in ("W", "T"):
            pending[tid].append([idx, cond, kind, to])
        elif kind in ("w", "t"):
            st = pending[tid]
            if st and st[-1][1] == cond:
                st.pop()
            elif st:
                # popped out of order: find the matching entry
                for j in range(len(st) - 1, -1, -1):
                    if st[j][1] == cond:
                        st.pop(j)
                        break
                else:
                    unmatched_ret.append((idx, tid, cond))
            else:
                unmatched_ret.append((idx, tid, cond))

    stuck = [(tid, st) for tid, st in pending.items() if st]
    print(f"threads with an unreturned wait: {len(stuck)}")
    if unmatched_ret:
        print(f"returns with no matching wait: {len(unmatched_ret)} "
              f"(first: {unmatched_ret[:3]})")

    # per-cond signal timeline
    sigs = defaultdict(list)
    for idx, (kind, tid, cond, mutex, to, ret, main) in enumerate(ev):
        if kind in ("S", "B"):
            sigs[cond].append((idx, kind, tid))

    for tid, st in sorted(stuck):
        for (idx, cond, kind, to) in st:
            after = [s for s in sigs[cond] if s[0] > idx]
            print()
            print(f"== STUCK tid={tid} cond={cond} kind={kind} to={to} "
                  f"row#{idx}  signals-after={len(after)}")
            if after:
                print("   signals on that cond AFTER the wait began:")
                for (sidx, skind, stid) in after[:6]:
                    print(f"     row#{sidx} {skind} by tid={stid}")
            else:
                print("   NO signal/broadcast on that cond after the wait began")
                # who was signalled around that time, and by whom
                near = [(i, e) for i, e in enumerate(ev)
                        if abs(i - idx) <= 200 and e[0] in ("S", "B")]
                print(f"   {len(near)} signal rows within +/-200 rows; "
                      f"distinct conds: {len(set(e[2] for _, e in near))}")
                for (i, e) in near[:8]:
                    print(f"     row#{i} {e[0]} tid={e[1]} cond={e[2]}")
            print(f"   last {tail} rows up to the wait:")
            for i in range(max(0, idx - tail), idx + 1):
                k, t, c, mu, to2, r, mn = ev[i]
                extra = ""
                if to2 is not None:
                    extra += f" to={to2}"
                if r is not None:
                    extra += f" ret={r}"
                print(f"     {i:7d} {k} tid={t} cond={c}{extra}")
    print("\n== cond summary (top 12 by waits)")
    wc = defaultdict(int)
    for (kind, tid, cond, mutex, to, ret, main) in ev:
        if kind in ("W", "T"):
            wc[cond] += 1
    for cond, n in sorted(wc.items(), key=lambda kv: -kv[1])[:12]:
        print(f"   cond={cond} waits={n} signals={len(sigs[cond])}")


if __name__ == "__main__":
    main()
