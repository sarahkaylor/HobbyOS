#!/usr/bin/env python3
"""lp_check_kstack.py -- console-side verdict for linux_probe_kstack (C6-p7b).

The probe reads the kernel's own counters through a test syscall, but a CPU
that faults while two CPUs share a kernel task's stack can leave the probe
alive (the probe runs on other CPUs) -- so the console is graded too:

  FAIL on any [KSTACK-SHARE] row (a pick resumed a kernel task whose stack
       another CPU was still on), any kernel-mode fault row, any
       [UNOWNED-FREED] / [STOLEN] / [STALLBREAK] lock-strand row, or a
       [KSTACK-ST] done row with share/canary_bad != 0;
  FAIL if fewer [KSTACK-ST] done rows than [KSTACK-ST] start rows.

Usage: lp_check_kstack.py <serial.log>      Exit 0 = PASS, 1 = FAIL.
       lp_check_kstack.py --self-test
"""
import re
import sys

BAD = [
    ("kstack-share", re.compile(r"\[KSTACK-SHARE\]")),
    ("kernel-fault", re.compile(r"FATAL: Exception in Kernel Mode|FATAL kernel-mode fault")),
    ("lock-strand", re.compile(r"\[(UNOWNED-FREED|STOLEN|STALLBREAK)\]")),
]
START = re.compile(r"\[KSTACK-ST\] start run=(\d+) tasks=(\d+)")
DONE = re.compile(r"\[KSTACK-ST\] done run=(\d+) yields=(\d+) canary_bad=(\d+) share=(\d+)")


def check(text):
    fails = []
    for name, rx in BAD:
        hits = [ln for ln in text.splitlines() if rx.search(ln)]
        if hits:
            fails.append("%s: %d row(s), first: %s" % (name, len(hits), hits[0].strip()[:200]))
    starts = START.findall(text)
    dones = DONE.findall(text)
    for run, yields, bad, share in dones:
        print("lp_check_kstack: run %s yields=%s canary_bad=%s share=%s" % (run, yields, bad, share))
        if int(bad) or int(share):
            fails.append("run %s: canary_bad=%s share=%s" % (run, bad, share))
    if not starts:
        fails.append("no [KSTACK-ST] start row (the stress never ran)")
    if len(dones) < len(starts):
        fails.append("%d start row(s) but %d done row(s)" % (len(starts), len(dones)))
    return fails


def self_test():
    ok = "[KSTACK-ST] start run=1 tasks=6/6 ms=4000\n[KSTACK-ST] done run=1 yields=9000 canary_bad=0 share=0\n"
    assert check(ok) == [], check(ok)
    assert check(ok + "[KSTACK-SHARE] site=idle cpu=3 slot=40 ? still on its stack: cpu=1 n=1\n")
    assert check(ok + "[KERNEL] FATAL: Exception in Kernel Mode! Vector: 13\n")
    assert check(ok.replace("share=0", "share=2"))
    assert check("[KSTACK-ST] start run=1 tasks=6/6 ms=4000\n")
    assert check("")
    print("lp_check_kstack: self-test PASS")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        text = f.read().decode("utf-8", "replace")
    fails = check(text)
    for f in fails:
        print("lp_check_kstack: FAIL " + f)
    if fails:
        return 1
    print("lp_check_kstack: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
