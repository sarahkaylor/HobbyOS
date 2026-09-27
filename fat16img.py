#!/usr/bin/env python3
"""Tiny read-only FAT16 reader -- no dependencies, no mtools.

Why this exists: mtools is only half-installed here (mcopy/mmd but no
mdir/mtype), and the end-to-end test needs two things from the image:

  * the root directory *in on-disk order* -- that is the order the OS's
    read_dir() returns, which is what decides the Apps-menu indexes;
  * a file's bytes, read back after an app saved it.

Usage:
    python3 fat16img.py list disk.img
    python3 fat16img.py get  disk.img HELLO.TXT        # case-insensitive
"""

import struct
import sys


class Fat16:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.img = f.read()
        b = self.img
        self.bps = struct.unpack_from("<H", b, 0x0B)[0]          # bytes/sector
        self.spc = b[0x0D]                                       # sectors/cluster
        self.reserved = struct.unpack_from("<H", b, 0x0E)[0]
        self.nfats = b[0x10]
        self.root_entries = struct.unpack_from("<H", b, 0x11)[0]
        self.fat_size = struct.unpack_from("<H", b, 0x16)[0]     # sectors/FAT
        if not (self.bps and self.spc and self.fat_size):
            raise ValueError("not a FAT image (zeroed BPB)")
        self.cluster_size = self.bps * self.spc
        self.fat_start = self.reserved * self.bps
        self.root_start = (self.reserved + self.nfats * self.fat_size) * self.bps
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_start = self.root_start + self.root_sectors * self.bps

    # ------------------------------------------------------------ directory
    def root_raw(self):
        """Root directory entries in on-disk order: (name, is_dir, cluster,
        size).  Long-file-name fragments, deleted slots and the volume label
        are skipped; iteration stops at the 0x00 end marker."""
        out = []
        for i in range(self.root_entries):
            off = self.root_start + i * 32
            ent = self.img[off:off + 32]
            if len(ent) < 32 or ent[0] == 0x00:
                break
            if ent[0] == 0xE5:                                   # deleted
                continue
            attr = ent[0x0B]
            if attr == 0x0F or (attr & 0x08):                    # LFN / label
                continue
            base = ent[:8].decode("ascii", "replace").rstrip()
            ext = ent[8:11].decode("ascii", "replace").rstrip()
            name = base + ("." + ext if ext else "")
            cluster = struct.unpack_from("<H", ent, 0x1A)[0]
            size = struct.unpack_from("<I", ent, 0x1C)[0]
            out.append((name, bool(attr & 0x10), cluster, size))
        return out

    def find(self, name):
        want = name.upper()
        for ent in self.root_raw():
            if ent[0].upper() == want:
                return ent
        # 8.3 spelling with padding removed, e.g. "HELLO.TXT" -> "HELLO  TXT"
        want83 = want.split(".")
        for ent in self.root_raw():
            if (ent[0].upper().split(".") == want83):
                return ent
        return None

    # ----------------------------------------------------------------- file
    def _next_cluster(self, c):
        nxt = struct.unpack_from("<H", self.img, self.fat_start + c * 2)[0]
        return nxt if nxt < 0xFFF8 else None

    def _cluster_bytes(self, c):
        off = self.data_start + (c - 2) * self.cluster_size
        return self.img[off:off + self.cluster_size]

    def read_file(self, name):
        ent = self.find(name)
        if ent is None:
            raise FileNotFoundError(name)
        _, is_dir, cluster, size = ent
        if is_dir:
            raise IsADirectoryError(name)
        out = b""
        c = cluster
        while c and c >= 2 and len(out) < size:
            out += self._cluster_bytes(c)
            c = self._next_cluster(c)
        return out[:size]


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    img = Fat16(path)
    if cmd == "list":
        for i, (name, is_dir, cluster, size) in enumerate(img.root_raw()):
            kind = "dir " if is_dir else "file"
            print(f"{i:3d}  {kind}  {name:<14} {size:>7}  cluster {cluster}")
        print(f"({len(img.root_raw())} entries)")
        return 0
    if cmd == "get" and len(argv) >= 4:
        data = img.read_file(argv[3])
        sys.stdout.buffer.write(data)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
