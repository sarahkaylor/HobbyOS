#!/usr/bin/env python3
"""l76-msample.py — what each thread was doing, from the /TPLATE late sampler.

`[TPWA] tid=<n> sp=0x.. words=<n>` is followed on the same line by
`bt=<Class>.<Method> <- <Class>.<Method> ...` (rwmain.c tp_bt_words, which
resolves only addresses that mono_jit_info_table_find knows: managed frames).
A thread with `bt=(no JIT frame in N words)` is in native code at that instant.

The question this answers for L76 is not "who is parked" (the futex ring does
that) but "what is the LongEventHandler event thread executing" -- the splash
says `Initializing`, which PlayDataLoader sets right before KeyPrefs.Init and
ShortHashGiver.GiveAllShortHashes, so the top frame at each sample names the
step that never returns.

Usage: tools/rw/l76-msample.py obj/rw-desktop/l72.log [--tid N]
"""
import argparse
import collections
import re
import sys

RE_SAMPLE = re.compile(r"^\[TPWA\] tid=(\d+) sp=0x[0-9a-f]+ words=(\d+) bt=(.*)$")
RE_T = re.compile(r"\[TPWT\] late t=(\d+)s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--tid", type=int, default=0)
    ap.add_argument("--top", type=int, default=18)
    a = ap.parse_args()

    per_tid = collections.defaultdict(collections.Counter)
    tid_at = {}
    t = None
    cur_tid = None
    with open(a.log, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = RE_T.search(line)
            if m:
                t = int(m.group(1))
                continue
            m = RE_SAMPLE.match(line)
            if m:
                cur_tid = int(m.group(1))
                if t is not None and cur_tid not in tid_at:
                    tid_at[cur_tid] = t
                bt = m.group(3)
                if bt.startswith("(no JIT frame"):
                    per_tid[cur_tid]["<native>"] += 1
                else:
                    # innermost first
                    frames = [x.strip() for x in bt.split("<-")]
                    per_tid[cur_tid]["TOP " + frames[0]] += 1
                    for fr in frames[1:4]:
                        per_tid[cur_tid]["     " + fr] += 1
                continue

    if not per_tid:
        print("no [TPWA] samples in %s" % a.log)
        return 1

    print("== managed stacks per thread (%d threads sampled)" % len(per_tid))
    for tid in sorted(per_tid):
        if a.tid and tid != a.tid:
            continue
        print("\n-- tid=%d (first sampled t=%ss), samples=%d"
              % (tid, tid_at.get(tid, "?"),
                 sum(v for k, v in per_tid[tid].items() if k.startswith("TOP"))))
        for name, n in per_tid[tid].most_common(a.top):
            print("   %4d  %s" % (n, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
