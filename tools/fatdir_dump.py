#!/usr/bin/env python3
"""fatdir_dump.py — dump raw FAT16 directory entries including the date/time
fields, for w10-fstime timestamp evidence (host side, read-only).

Reads the raw dirent 32-byte records of a directory and prints, per entry:
  name | attr | date (raw) | time (raw) | decoded UTC | size | cluster

Usage:
  python3 tools/fatdir_dump.py <image> <dir-path>   (e.g. ::/W10 or /W10)
"""
import datetime
import struct
import sys

SECTOR = 512
ENTRY = 32


class Fat16:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.img = f.read()
        bs = self.img[0:512]
        if bs[510:512] != b"\x55\xaa":
            raise SystemExit(f"{path}: no 0x55AA boot signature")
        self.bps = struct.unpack_from("<H", bs, 11)[0]
        self.spc = bs[13]
        self.nfat = bs[16]
        self.root_entries = struct.unpack_from("<H", bs, 17)[0]
        self.spf = struct.unpack_from("<H", bs, 22)[0]
        self.reserved = struct.unpack_from("<H", bs, 14)[0]
        self.fat_start = self.reserved * self.bps
        self.root_start = self.fat_start + self.nfat * self.spf * self.bps
        self.root_sectors = (self.root_entries * ENTRY + SECTOR - 1) // SECTOR
        self.data_start = self.root_start + self.root_sectors * self.bps
        self.cluster_size = self.bps * self.spc

    def _dir_sectors(self, cluster):
        if cluster == 0:
            base, n = self.root_start, self.root_sectors
        else:
            base = self.data_start + (cluster - 2) * self.cluster_size
            n = self.spc
        return [self.img[base + i * SECTOR: base + (i + 1) * SECTOR]
                for i in range(n)]

    def _next(self, c):
        n = struct.unpack_from("<H", self.img, self.fat_start + c * 2)[0]
        return n if n < 0xFFF8 else None

    def _dir_entries(self, cluster):
        out = []
        if cluster == 0:
            for sec in self._dir_sectors(0):
                for i in range(len(sec) // ENTRY):
                    e = sec[i * ENTRY:(i + 1) * ENTRY]
                    if e[0] in (0x00, 0xE5):
                        continue
                    if e[11] == 0x0F:
                        continue  # LFN record
                    out.append(e)
            return out
        c = cluster
        while c:
            for sec in self._dir_sectors(c):
                for i in range(len(sec) // ENTRY):
                    e = sec[i * ENTRY:(i + 1) * ENTRY]
                    if e[0] in (0x00, 0xE5):
                        continue
                    if e[11] == 0x0F:
                        continue  # LFN record
                    out.append(e)
            c = self._next(c)
        return out

    def resolve_cluster(self, path):
        parts = [p for p in path.strip("/").upper().split("/") if p]
        cluster = 0
        for part in parts:
            found = None
            for e in self._dir_entries(cluster):
                name = e[0:11].decode("latin1").strip()
                if name == part:
                    found = e
                    break
            if found is None:
                raise SystemExit(f"dir not found: {path}")
            if not (found[11] & 0x10):
                raise SystemExit(f"not a directory: {path}")
            cluster = struct.unpack_from("<H", found, 26)[0]
        return cluster


def decode(date, time):
    year = ((date >> 9) & 0x7F) + 1980
    month = (date >> 5) & 0x0F
    day = date & 0x1F
    hour = (time >> 11) & 0x1F
    minute = (time >> 5) & 0x3F
    sec = (time & 0x1F) * 2
    if year < 1980 or month < 1 or month > 12 or day < 1 or day > 31:
        return "INVALID"
    try:
        return datetime.datetime(year, month, day, hour, minute, sec).strftime(
            "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return "INVALID"


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    img = Fat16(argv[1])
    c = img.resolve_cluster(argv[2])
    print(f"# dirent dump of ::{argv[2].strip('/').upper()} "
          f"(cluster {c}) — date/time are the raw FAT16 fields")
    entries = img._dir_entries(c)
    print(f"{'name':<12} {'attr':>4} {'date':>5} {'time':>5}  "
          f"{'UTC':<20} {'size':>7} {'clu':>5}")
    for e in entries:
        name = e[0:11].decode("latin1")
        attr = e[11]
        date = struct.unpack_from("<H", e, 24)[0]
        time = struct.unpack_from("<H", e, 22)[0]
        size = struct.unpack_from("<I", e, 28)[0]
        clu = struct.unpack_from("<H", e, 26)[0]
        print(f"{name:<12} {attr:>4} {date:>5} {time:>5}  "
              f"{decode(date, time):<20} {size:>7} {clu:>5}")
    print(f"({len(entries)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
