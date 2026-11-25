#!/usr/bin/env python3
"""vdesc_scan.py -- locate every self-consistent FALCON_UCODE_DESC in the ROM.

A descriptor header is valid when bit 0 (VERSION_AVAILABLE) is set, the version
byte is 2 or 3, and the size field equals the version's body size (60 / 44).
Scanning finds where the descriptors actually live, which is what tells us how
the table's DescPtr field resolves (or that the table and the descriptors are in
different address spaces, as the 0xE0 partition would imply).
"""
import struct
import sys

rom = open(sys.argv[1] if len(sys.argv) > 1 else "evidence/vbios.bin", "rb").read()
hits = []
for o in range(0, len(rom) - 8, 4):
    v = struct.unpack_from("<I", rom, o)[0]
    ver = (v >> 8) & 0xFF
    size = (v >> 16) & 0xFFFF
    if (v & 1) and ((ver == 2 and size == 60) or (ver == 3 and size == 44)):
        stored = struct.unpack_from("<I", rom, o + 4)[0]
        if ver == 3:
            engmask = struct.unpack_from("<H", rom, o + 0x24)[0]
            uid = rom[o + 0x26]
            sigc = rom[o + 0x27]
            sigv = struct.unpack_from("<H", rom, o + 0x28)[0]
            extra = "engmask=0x%04x ucode_id=%d sigcnt=%d sigvers=0x%04x" % (
                engmask, uid, sigc, sigv)
        else:
            extra = "(v2 body)"
        hits.append((o, ver, size, stored, extra, v))
print("%d descriptor headers" % len(hits))
for o, ver, size, stored, extra, v in hits:
    print("  0x%06x v%d/%2dB vDesc=0x%08x stored=%-9d %s" % (o, ver, size, v, stored, extra))
