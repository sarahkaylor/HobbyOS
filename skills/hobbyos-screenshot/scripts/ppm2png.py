#!/usr/bin/env python3
"""Dependency-free PPM (P6 binary) -> PNG converter.

Usage: ppm2png.py <input.ppm> <output.png>

QEMU's `screendump` emits a binary P6 PPM. This converts it to PNG using only
the Python stdlib (struct + zlib), so it runs anywhere without Pillow.
Also prints a blank/black warning to stderr, matching run_desktop_test.py.
"""
import sys
import struct
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        # Header: "P6\n<width> <height>\n<maxval>\n" (whitespace/comments possible).
        magic = f.read(2)
        if magic != b"P6":
            raise ValueError(f"not a binary PPM (P6): got {magic!r}")
        vals = []
        token = b""
        while len(vals) < 3:
            c = f.read(1)
            if not c:
                raise ValueError("unexpected EOF in PPM header")
            if c == b"#":  # comment to end of line
                while f.read(1) not in (b"\n", b""):
                    pass
                continue
            if c.isspace():
                if token:
                    vals.append(int(token))
                    token = b""
            else:
                token += c
        width, height, _maxval = vals
        data = f.read(width * height * 3)
        return width, height, data


def write_png(path, width, height, rgb):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    # Prepend the mandatory per-scanline filter byte (0 = none).
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * stride:(y + 1) * stride])

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        print("usage: ppm2png.py <input.ppm> <output.png>", file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb)
    if not any(rgb):
        print(f"WARNING: {src} is completely black — the framebuffer did not render.",
              file=sys.stderr)
    print(f"wrote {dst} ({w}x{h})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
