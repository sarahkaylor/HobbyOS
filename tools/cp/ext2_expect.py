#!/usr/bin/env python3
"""tools/cp/ext2_expect.py OUTDIR PHASE — write the bytes linux_probe_ext2
must have left on the volume after PHASE (1 or 2), one file per regular file
of the probe's tree, plus MANIFEST (type/name/target lines).

Mirrors the generators in src/user/linux_probe_ext2.c (pat_a, pat_b, the
patch, the truncations).  tools/cp/ext2_gate.sh compares these against
`debugfs dump` of the image, so the oracle is e2fsprogs reading the disk,
never the driver that wrote it.
"""
import os
import sys

BIG_LEN = 1048576 + 333
PATCH_OFF = 300000
PATCH_LEN = 4096


def pat_a(i):
    return (i * 131 + (i >> 8) * 7 + 0x5A) & 0xFF


def pat_b(i):
    return (i * 17 + 0xC3) & 0xFF


def main():
    out, phase = sys.argv[1], int(sys.argv[2])
    os.makedirs(out, exist_ok=True)
    big = bytearray(pat_a(i) for i in range(BIG_LEN))
    for i in range(PATCH_LEN):
        big[PATCH_OFF + i] = pat_b(i)
    trunc = bytes(pat_a(i) for i in range(6000)) + bytes(8192 - 6000)
    trunc2 = bytes(pat_a(i) for i in range(100))
    hello = b"hello ext2\nappended\n" + (b"phase2\n" if phase == 2 else b"")
    slow = "sub2/" + "".join(chr(ord("a") + i % 26) for i in range(5, 120))
    files = {
        "lp/big.bin": bytes(big),
        "lp/hello.txt": hello,
        "lp/hello.hard": hello,
        "lp/trunc.bin": trunc,
        "lp/trunc2.bin": trunc2,
        "lp/victim.txt": b"replacement\n",
        "lp/sub2/r4.txt": b"r1\n",
        "lp/phase1.done": b"1\n",
    }
    if phase == 2:
        files["lp/phase2.done"] = b"2\n"
    for name, data in files.items():
        p = os.path.join(out, name.replace("/", "__"))
        with open(p, "wb") as f:
            f.write(data)
    with open(os.path.join(out, "MANIFEST"), "w") as m:
        for name in sorted(files):
            m.write("f %s %s\n" % (name, name.replace("/", "__")))
        for d in ("lp", "lp/sub", "lp/sub2", "lp/sub2/inner_moved", "lp/dosdevices"):
            m.write("d %s\n" % d)
        for link, tgt in (("lp/link_fast", "hello.txt"), ("lp/link_slow", slow),
                          ("lp/link_dir", "sub"), ("lp/dosdevices/z:", "/"),
                          ("lp/dosdevices/c:", "../sub")):
            m.write("l %s %s\n" % (link, tgt))
        for gone in ("lp/gone.txt", "lp/gonedir", "lp/tmp.bin", "lp/r1.txt",
                     "lp/r2.txt", "lp/repl.txt", "lp/sub/r3.txt", "lp/sub/inner"):
            m.write("x %s\n" % gone)


if __name__ == "__main__":
    main()
