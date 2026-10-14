#!/usr/bin/env python3
"""Convert QEMU PPM screendumps to PNG (for vision/archival)."""
import struct
import sys
import zlib


def ppm_to_png(src, dst):
    raw = open(src, "rb").read()
    i = 0

    def token():
        nonlocal i
        while raw[i:i + 1].isspace():
            i += 1
        j = i
        while not raw[j:j + 1].isspace():
            j += 1
        t = raw[i:j]
        i = j
        return t

    magic = token()
    w = int(token())
    h = int(token())
    mx = int(token())
    assert magic == b"P6" and mx == 255, f"unexpected PPM header {magic} {mx}"
    # Header token scan leaves i at the line ending after the maxval; skip
    # LF/CR only — the pixel data may legitimately begin with a byte Python's
    # isspace() also classifies as whitespace (e.g. 0x0c form-feed), and a
    # CRLF header must not leave a stray \n in the data.
    while raw[i:i + 1] in (b"\n", b"\r"):
        i += 1
    want = w * h * 3
    data = raw[i:i + want]
    assert len(data) == want, f"{src}: data {len(data)} != {want}"
    stride = w * 3
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        row = data[y * stride:(y + 1) * stride]
        out.append(0)
        for x in range(stride):
            out.append((row[x] - prev[x]) & 0xFF)
        prev = row

    def chunk(typ, payload):
        c = struct.pack(">I", len(payload)) + typ + payload
        c += struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF)
        return c

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(out), 1))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print(f"{src} -> {dst} ({w}x{h})")


if __name__ == "__main__":
    for src in sys.argv[1:]:
        ppm_to_png(src, src.rsplit(".", 1)[0] + ".png")
