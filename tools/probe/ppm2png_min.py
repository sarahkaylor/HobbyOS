#!/usr/bin/env python3
"""ppm2png_min.py <in.ppm> <out.png> - P6 PPM to PNG with nothing but zlib."""
import struct
import sys
import zlib


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    if data[:2] != b"P6":
        print("not a P6 ppm")
        return 1
    i, toks = 2, []
    while len(toks) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        toks.append(data[i:j])
        i = j
    i += 1
    w, h, maxv = (int(t) for t in toks)
    assert maxv == 255
    px = data[i:i + w * h * 3]

    raw = bytearray()
    for y in range(h):
        raw.append(0)                                   # filter: none
        raw += px[y * w * 3:(y + 1) * w * 3]

    def chunk(tag: bytes, body: bytes) -> bytes:
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
           + chunk(b"IEND", b""))
    open(dst, "wb").write(png)
    print(f"{dst}: {w}x{h}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
