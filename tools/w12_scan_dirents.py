#!/usr/bin/env python3
"""scan_dirents.py — w12-rcut evidence helper: raw /SRC/obj/arm dirent slot
scan incl. LFN + deleted records, used to analyze the attempt-1 S2 -j8
dirent loss (net_rdma_test.o's entry absent entirely; torn LFN fragment at
slot 47).  PRINTS all slots that mention net/test/tmp plus the summary.

Usage: python3 scan_dirents.py <image>
"""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "disk.img"
with open(path, "rb") as f:
    img = f.read()
bs = img[0:512]
bps = struct.unpack_from("<H", bs, 11)[0]
spc = bs[13]
nfat = bs[16]
root_entries = struct.unpack_from("<H", bs, 17)[0]
spf = struct.unpack_from("<H", bs, 22)[0]
reserved = struct.unpack_from("<H", bs, 14)[0]
fat_start = reserved * bps
root_start = fat_start + nfat * spf * bps
root_sectors = (root_entries * 32 + 511) // 512
data_start = root_start + root_sectors * bps
cl_size = bps * spc


def nxt(c):
    n = struct.unpack_from("<H", img, fat_start + c * 2)[0]
    return n if n < 0xFFF8 else None


def dirchain(c):
    out = []
    if c == 0:  # root dir: fixed location, not in the data area
        for i in range(root_sectors):
            sec = img[root_start + i * bps:root_start + (i + 1) * bps]
            for j in range(len(sec) // 32):
                out.append(sec[j * 32:(j + 1) * 32])
        return out
    while c:
        base = data_start + (c - 2) * cl_size
        for i in range(spc):
            sec = img[base + i * bps:base + (i + 1) * bps]
            for j in range(len(sec) // 32):
                out.append(sec[j * 32:(j + 1) * 32])
        c = nxt(c)
    return out


def resolve(p):
    parts = [x for x in p.strip("/").upper().split("/") if x]
    c = 0
    for part in parts:
        found = None
        for e in dirchain(c):
            if e[0] in (0x00, 0xE5) or e[11] == 0x0F:
                continue
            if e[0:11].decode("latin1").strip() == part:
                found = e
                break
        if found is None:
            raise SystemExit("dir not found: %s" % p)
        c = struct.unpack_from("<H", found, 26)[0]
    return c, dirchain(c)


c, recs = resolve("/SRC/obj/arm")
print("cluster", c, "raw slots:", len(recs))
for i, e in enumerate(recs):
    b0 = e[0]
    if b0 == 0x00:
        continue
    if b0 == 0xE5:
        label, name = "DEL", "DELETED"
    elif e[11] == 0x0F:
        label = "LFN"
        name = "".join(chr(x) for x in e[1:11] + e[14:26] + e[28:32] if 0x20 <= x < 0x7f)
    else:
        label = "SHORT"
        name = e[0:11].decode("latin1")
    if ("NET" in name) or ("RDMA" in name) or ("TEST" in name) or ("TMP" in name) or ("~1" in name):
        print(i, label, repr(name))
tot = sum(1 for e in recs if e[0] not in (0, 0xE5) and e[11] != 0x0F)
nlf = sum(1 for e in recs if e[11] == 0x0F)
nde = sum(1 for e in recs if e[0] == 0xE5)
print("short entries:", tot, "lfn records:", nlf, "deleted slots:", nde)
