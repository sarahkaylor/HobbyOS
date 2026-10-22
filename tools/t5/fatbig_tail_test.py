#!/usr/bin/env python3
"""Host (T5) regression: FATBIG tail-checksum logic round-trips on the host
(GB-062).  Reproduces the exact byte pattern + tail checksum of
src/user/fatbig_test.c on the host filesystem.  The FAT16 + user-test logic is
arch-agnostic and passes here, so an intel FATBIG failure (got bad=0x800 on a
2 MiB tail) is proven to be the intel NVMe block-device layer (virtio_blk.c
__x86_64__ branch), not FAT logic — see docs/fatbig-intel-delta.md.
"""
import os, sys, tempfile

TARGET = 2 * 1024 * 1024
CHUNK = 4096


def pat_byte(p):
    return ((p >> 8) ^ (p & 0xFF)) & 0xFF   # src/user/fatbig_test.c casts to (char)


def run():
    d = tempfile.mkdtemp()
    path = os.path.join(d, "BIGFILE.BIN")
    # write 2 MiB in 4 KiB chunks, byte pattern identical to the user test
    with open(path, "wb") as f:
        written = 0
        while written < TARGET:
            n = min(CHUNK, TARGET - written)
            chunk = bytes(pat_byte(p) for p in range(written, written + n))
            f.write(chunk)
            written += n
    # reopen, size, read tail, checksum
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        sz = f.tell()
        f.seek(TARGET - 4096, os.SEEK_SET)
        tail = f.read(4096)
    bad = sum(1 for i, b in enumerate(tail)
              if (b & 0xFF) != pat_byte(TARGET - 4096 + i))
    print(f"  size={sz} tail_bytes={len(tail)} bad={bad}")
    assert sz == TARGET, "reopen size != 2 MiB"
    assert len(tail) == 4096, "tail read != 4096"
    assert bad == 0, f"host tail checksum not clean (bad={bad}) — FAT logic broken"
    print("Tests: ALL PASSED  (FATBIG tail pattern + checksum clean on host)")
    return 0


if __name__ == "__main__":
    sys.exit(run())
