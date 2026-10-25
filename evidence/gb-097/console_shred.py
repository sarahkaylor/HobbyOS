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


def is_subsequence(needle, hay):
    it = iter(hay)
    return all(ch in it for ch in needle)


def shredded(line):
    for ph in PHRASES:
        if ph in line:
            continue            # contiguous -> clean for this phrase
        if is_subsequence(ph, line):
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
