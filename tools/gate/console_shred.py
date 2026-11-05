#!/usr/bin/env python3
"""console_shred.py — detect BYTE-interleaved console records (GB-097).

Why not t4_glued.sh: that gate counts record FUSIONS (two tags on one line).
GB-097's defect is different in kind — two writers alternate *bytes inside one
record*, so the line carries no second tag at all:

    [KERNEL] FATAL: Exception in Kve2r nelloa Mdoerd:e!  Vimeagctoer : m14 ...

The signal is precise: a phrase the writer emits as one contiguous string
("Exception in Kernel Mode", "Error Code:") survives only as a *subsequence*
when another writer interleaves characters. So:

    shredded  := the line contains the phrase as a subsequence but NOT as a
                 contiguous substring.

A clean line contains it contiguously and is never flagged; a line with the
phrase absent is not flagged (it is a different record, not a shredded one).

Usage:  console_shred.py <serial>       exit 0 = no shredded lines, 1 = found
        console_shred.py --self-test    exit 0 = detector fires on the known
                                        specimen and stays quiet on clean text
"""
import sys

# Phrases emitted contiguously by their writer. Keep them short enough that a
# foreign-writer interleave is likely to break them, long enough to be specific.
PHRASES = [
    "Exception in Kernel Mode",
    "Error Code:",
    "halted this CPU",
    "console silent",
    "stack trace",
    "freeing root=",
    "still translates through it",
    "owner_cpu=",
    "stale_ms=",
]

SPECIMEN = ("[KERNEL] FATAL: Exception in Kve2r nelloa Mdoerd:e!  Vimeagctoer : m14 "
            "aEprrpoerd C od(el: 0azx0y00)0, 00b00yt00e00s000=26")
CLEAN = ("[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 "
         "Error Code: 0x0000000000000002")

# GB-210 specimens from the GB-097-V2 serial (5c88129d).  The two FALSE lines
# are ordinary records the unbounded detector flagged; the two TRUE lines are
# byte-interleaved records it must keep flagging.
FALSE_STALL = ("=== T3 guest sample: SKIPPED — no monitor attached (T3_MONITOR not set; "
               "first stall marker in the serial) ===")
FALSE_DEADCLAIM = ("[DEADCLAIM] slot=18 pid=18 name= claimed only by halted cpus -- excluded "
                   "from the wave-end predicate (no CPU can run it; the resume path halted that CPU)")
TRUE_ERROR = ("0000[KERNEL] FATAL: Exception in Kernel Mode! Vector0: 0140 Er0ror Code07: "
              "0x09 0x0000000000000000000000000004E 02")
TRUE_EXC = ("v2 loader: image mapped (lazy), bytes=111240 pages[KERNEL]= FATAL: "
            "Exception in Kernel28  fModirst=in ele! Vector: 1a4p=8ms")


# GB-210: how many foreign bytes may be interleaved between the phrase's own
# characters.  Unbounded subsequence matching flags ordinary prose on a long
# line (measured false positives at slack 19, 37, 45, 65, 82, 88, 95, 200);
# every measured genuine interleave sits at slack <= 12.  16 separates the two
# measured sets: a byte-interleaved writer inserts bytes inside a record, it
# does not scatter a 24-char phrase across a 20k-char line.
MATCH_SLACK = 16


def bounded_subsequence(needle, hay, slack=MATCH_SLACK):
    """True iff needle occurs in hay as a subsequence within len(needle)+slack chars."""
    m = len(needle)
    n = len(hay)
    if m > n:
        return False
    limit = m + slack          # span == last_index - first_index must be <= limit
    for i in range(n):
        if hay[i] != needle[0]:
            continue
        j = 1
        k = i + 1
        end = min(n, i + limit + 1)
        while j < m and k < end:
            if hay[k] == needle[j]:
                j += 1
            k += 1
        if j == m:
            return True
    return False


def shredded(line):
    for ph in PHRASES:
        if ph in line:
            continue            # contiguous -> clean for this phrase
        if bounded_subsequence(ph, line):
            return ph           # present only interleaved -> shredded
    return None


TOOL_PREFIXES = ("ld.lld", "qemu-system", "clang", "make", "mkdir", "cp ", "mcopy",
                 "python3", "bash", "echo ", "gcc", "llvm-objcopy", "truncate",
                 "dd ", "mkfs", "rm ", "git ", "install ")


def scan(path):
    hits = []
    with open(path, "r", errors="replace") as f:
        for n, line in enumerate(f, 1):
            s = line.lstrip()
            if s.startswith(TOOL_PREFIXES):
                continue          # build/QEMU command echo, not a console record
            ph = shredded(line)
            if ph:
                hits.append((n, ph, line.rstrip("\r\n")))
    return hits


def self_test():
    ok = True
    if shredded(SPECIMEN) is None:
        print("  FAIL: detector did NOT fire on the known shredded specimen")
        ok = False
    else:
        print(f"  PASS: fires on specimen (phrase '{shredded(SPECIMEN)}')")
    if shredded(CLEAN) is not None:
        print(f"  FAIL: clean line flagged (phrase '{shredded(CLEAN)}')")
        ok = False
    else:
        print("  PASS: clean contiguous record not flagged")
    if shredded("[SEDTEST] PASS amp (stdout)") is not None:
        print("  FAIL: unrelated record flagged")
        ok = False
    else:
        print("  PASS: unrelated record not flagged")
    # GB-210: measured V2 false positives (long clean lines) must stay quiet.
    for name, spec in (("stall", FALSE_STALL), ("DEADCLAIM", FALSE_DEADCLAIM)):
        ph = shredded(spec)
        if ph is not None:
            print(f"  FAIL: clean long {name} line flagged (phrase '{ph}')")
            ok = False
        else:
            print(f"  PASS: clean long {name} line not flagged (GB-210)")
    # GB-210: measured V2 genuine interleaves must still fire.
    for want, spec in (("Error Code:", TRUE_ERROR), ("Exception in Kernel Mode", TRUE_EXC)):
        ph = shredded(spec)
        if ph != want:
            print(f"  FAIL: genuine interleave not flagged as '{want}' (got {ph!r})")
            ok = False
        else:
            print(f"  PASS: genuine interleave flagged as '{want}' (GB-210)")
    print("self-test:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] == "--self-test":
        sys.exit(self_test())
    hits = scan(sys.argv[1])
    print(f"console_shred: {len(hits)} byte-interleaved record(s) in '{sys.argv[1]}'")
    for n, ph, line in hits[:10]:
        print(f"  L{n} [breaks '{ph}'] {line[:150]}")
    sys.exit(1 if hits else 0)
