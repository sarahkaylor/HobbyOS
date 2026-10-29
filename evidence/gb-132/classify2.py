"""GB-132 banner classifier v2.

Splits each fatal banner into three states, because the three have different
meanings and conflating them is how "impossible vector" got argued from
shredded lines:

  TORN      the line was interleaved while it was written (the fatal printer
            uses uart_puts_raw, no uart_lock -- GB-097).  Every field on that
            line is untrustworthy, including a "vector=20000".
  NONSENSE  the line is a single clean record, the vector is a real CPU
            vector, but rip is not a plausible code address (rip < 0x1000, or
            rip inside a stack window).  This is a real read of a bad frame.
  BADVEC    the line is a single clean record and the vector is not a CPU
            vector.  Real frame garbage, not a printing artifact.
  OK        clean record, plausible vector and rip.

Usage: python3 classify2.py <log>...
"""
import re
import sys

BANNER = re.compile(
    r"FATAL kernel-mode fault, halting this CPU: vector=(-?\d+)\s+"
    r"rip=0x([0-9A-Fa-f]+)\s+cr2=0x([0-9A-Fa-f]+)\s+pid=(\d+)\s+err=(\d+)\s+"
    r"cr3=0x([0-9A-Fa-f]+)")
ANY_FATAL = re.compile(
    r"(FATAL kernel-mode fault, halting this CPU|FATAL: Exception in Kernel Mode)")
CPU_VECTORS = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 16, 17, 18, 19,
               20, 21, 28, 29, 30}
FOREIGN = re.compile(r"(\.BIN|Process \d|PASS|FAIL|\[TORNFRAME|"
                     r"\[EXITPC\]|\[ARRIVED\])")


def is_stackish(rip):
    return (rip >> 40) in range(0x17, 0x18) or 0x7F00000000 <= rip < 0x8000000000


def classify(path):
    res = {"TORN": [], "NONSENSE": [], "BADVEC": [], "OK": [], "OLDD": 0}
    with open(path, "r", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if not ANY_FATAL.search(line):
                continue
            if "FATAL: Exception in Kernel Mode" in line:
                res["OLDD"] += 1
                continue
            m = BANNER.search(line)
            if not m:
                res["TORN"].append((n, line.rstrip()[:120]))
                continue
            vec = int(m.group(1))
            rip = int(m.group(2), 16)
            cr2 = int(m.group(3), 16)
            pid = int(m.group(4))
            err = int(m.group(5))
            cr3 = int(m.group(6), 16)
            torn = bool(FOREIGN.search(line.split("cr3=")[0]))
            if torn:
                res["TORN"].append((n, line.rstrip()[:120]))
            elif vec not in CPU_VECTORS:
                res["BADVEC"].append((vec, rip, cr2, pid, err, cr3))
            elif rip < 0x1000 or is_stackish(rip):
                res["NONSENSE"].append((vec, rip, cr2, pid, err, cr3))
            else:
                res["OK"].append((vec, rip, cr2, pid, err, cr3))
    return res


def main(paths):
    tot = {k: 0 for k in ("TORN", "NONSENSE", "BADVEC", "OK", "OLDD")}
    for p in paths:
        r = classify(p)
        name = p.rsplit("/", 1)[-1]
        print(f"== {name}")
        print(f"   old-format #PF dumps={r['OLDD']}  banners: OK={len(r['OK'])} "
              f"NONSENSE={len(r['NONSENSE'])} BADVEC={len(r['BADVEC'])} "
              f"TORN={len(r['TORN'])}")
        for cat in ("BADVEC", "NONSENSE", "OK"):
            for row in r[cat]:
                vec, rip, cr2, pid, err, cr3 = row
                print(f"   {cat:8s} vector={vec} rip=0x{rip:X} cr2=0x{cr2:X} "
                      f"pid={pid} err={err} cr3=0x{cr3:X}")
        for k in tot:
            tot[k] += len(r[k]) if k != "OLDD" else r[k]
    print("\nTOTAL", tot)


if __name__ == "__main__":
    main(sys.argv[1:])