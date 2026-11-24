#!/usr/bin/env python3
"""raw BGRA (1920x1080 stride 7680) -> PNG, stdlib only.

QMP pmemsave of the kernel framebuffer at 0x72000000 gives 1920x1080 BGRA.
vision_analyze wants an image it can decode, and there is no PIL on the build
host, so this writes a PNG by hand (zlib + struct).
"""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 1920
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 1080
    step = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    data = open(src, "rb").read()
    need = w * h * 4
    if len(data) < need:
        raise SystemExit(f"{src}: {len(data)} bytes, need {need}")
    ow, oh = w // step, h // step
    rows = bytearray()
    for y in range(0, h, step):
        off = y * w * 4
        rows.append(0)                       # filter: none
        rgb = bytearray(ow * 3)
        for x in range(0, w, step):
            o = off + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            d = (x // step) * 3
            rgb[d] = r
            rgb[d + 1] = g
            rgb[d + 2] = b
        rows += rgb
    w, h = ow, oh

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print(f"{dst}: {w}x{h}")


if __name__ == "__main__":
    main()
