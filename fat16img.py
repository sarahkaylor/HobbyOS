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

    # ------------------------------------------------------------- deep paths
    def _listing(self, entries):
        """Normalize raw 32-byte directory entries into (name, is_dir,
        cluster, size, long_name) tuples, mirroring root_raw() plus the
        reassembled VFAT long name (None when the entry is 8.3-only).
        Long-name 0x0F entries precede their 8.3 entry in reverse chunk
        order, so the chunks are re-sorted by sequence before joining."""
        out = []
        pending = []
        for ent in entries:
            if len(ent) < 32 or ent[0] == 0x00:   # end-of-directory marker
                break
            attr = ent[0x0B]
            if attr == 0x0F:                      # VFAT long-name fragment
                seq = ent[0] & 0x1F
                chunk = (ent[1:11] + ent[14:26] + ent[28:32]).decode(
                    "utf-16-le", "replace")
                pending.append((seq, chunk))
                continue
            if ent[0] == 0xE5:                    # deleted
                pending = []
                continue
            if attr & 0x08:                       # volume label
                continue
            base = ent[:8].decode("ascii", "replace").rstrip()
            ext = ent[8:11].decode("ascii", "replace").rstrip()
            name = base + ("." + ext if ext else "")
            cluster = struct.unpack_from("<H", ent, 0x1A)[0]
            size = struct.unpack_from("<I", ent, 0x1C)[0]
            long_name = None
            if pending:
                pending.sort(key=lambda x: x[0])
                # VFAT pads the final chunk with 0x0000 then 0xFFFF (which
                # decodes as '\uffff'); strip both.
                long_name = "".join(c for _, c in pending).rstrip("\x00\uffff")
                pending = []
            out.append((name, bool(attr & 0x10), cluster, size, long_name))
        return out

    def _dir_raw(self, cluster):
        """Parse the entries of a subdirectory given its first cluster."""
        entries = []
        c = cluster
        while c and c >= 2:
            off = self.data_start + (c - 2) * self.cluster_size
            chunk = self.img[off:off + self.cluster_size]
            for i in range(0, self.cluster_size, 32):
                entries.append(chunk[i:i + 32])
            c = self._next_cluster(c)
        return self._listing(entries)

    def _match(self, ent, part):
        if ent[0].upper() == part:
            return True
        if ent[0].upper().split(".") == part.split("."):
            return True
        if len(ent) > 4 and ent[4]:
            if ent[4].upper() == part or ent[4].upper().replace(" ", "") == part:
                return True
        return False

    def resolve(self, path):
        """Resolve a possibly-deep path ("SRC/OUT.LOG") to (entry, parent).
        Case-, 8.3- and VFAT-long-name-insensitive; parent is None for root
        entries."""
        parts = [p for p in path.split("/") if p]
        if not parts:
            return None, None
        listing = self.root_raw()
        parent = None
        for i, part in enumerate(parts):
            want = part.upper()
            ent = None
            for e in listing:
                if self._match(e, want):
                    ent = e
                    break
            if ent is None:
                return None, parent
            if i == len(parts) - 1:
                return ent, parent
            if not ent[1]:
                return None, parent
            parent = ent
            listing = self._dir_raw(ent[2])
        return None, parent

    # ----------------------------------------------------------------- file
    def _next_cluster(self, c):
        nxt = struct.unpack_from("<H", self.img, self.fat_start + c * 2)[0]
        return nxt if nxt < 0xFFF8 else None

    def _cluster_bytes(self, c):
        off = self.data_start + (c - 2) * self.cluster_size
        return self.img[off:off + self.cluster_size]

    def read_file(self, name):
        # root lookup first (backward compatible); then deep paths
        ent = self.find(name)
        if ent is None:
            ent, parent = self.resolve(name)
        else:
            parent = None
        if ent is None:
            raise FileNotFoundError(name)
        _, is_dir, cluster, size = ent[:4]  # subdir entries carry +long_name
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
