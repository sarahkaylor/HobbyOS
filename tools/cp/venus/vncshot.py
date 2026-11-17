#!/usr/bin/env python3
"""tools/cp/venus/vncshot.py -- a screendump of what QEMU's display really shows
(lane C6-p7, docs/cp-p7.md §4).

QMP `screendump` cannot see a GL scanout on QEMU 10.2.1 (qemu_console_surface()
is NULL unless the scanout is a plain surface: "no surface"), and C6-venus'
pmemsave fallback reads the kernel framebuffer -- the X desktop -- which is
exactly what a blob scanout is NOT showing.  egl-headless does read every
flushed GL scanout (texture or dma-buf) back into the console's DisplaySurface
(egl_scanout_flush -> egl_fb_read) and announces it with dpy_gfx_update, so a
VNC listener on that console sees the pixels the guest put on the glass.

host-venus.sh adds `-vnc unix:WORK/vnc-sock` when VENUS_VNC=1 (a unix socket:
nothing listens on the network, nothing opens on the desktop).  This is a
minimal RFB 3.8 client: no authentication, 32 bpp true colour, Raw encoding,
one non-incremental FramebufferUpdateRequest of the whole screen.  It connects
fresh for every shot, so the ServerInit size is the scanout's current mode.

    vncshot.py SOCK OUT.ppm        writes a binary PPM, prints "WxH"
"""
import socket
import struct
import sys


def _recv(s, n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError("vnc: connection closed")
        b += c
    return bytes(b)


def grab(sock_path, timeout=20.0):
    """Return (width, height, rgb bytes) of the current display."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    try:
        ver = _recv(s, 12)
        if not ver.startswith(b"RFB "):
            raise ValueError(f"vnc: not RFB: {ver!r}")
        s.sendall(b"RFB 003.008\n")
        n = _recv(s, 1)[0]
        if n == 0:
            ln = struct.unpack(">I", _recv(s, 4))[0]
            raise ValueError("vnc: refused: " + _recv(s, ln).decode(errors="replace"))
        types = _recv(s, n)
        if 1 not in types:
            raise ValueError(f"vnc: no 'None' security type offered: {list(types)}")
        s.sendall(b"\x01")
        if struct.unpack(">I", _recv(s, 4))[0] != 0:
            raise ValueError("vnc: security handshake failed")
        s.sendall(b"\x01")  # ClientInit: shared
        w, h = struct.unpack(">HH", _recv(s, 4))
        _recv(s, 16)  # server pixel format
        nl = struct.unpack(">I", _recv(s, 4))[0]
        _recv(s, nl)
        # SetPixelFormat: 32 bpp, depth 24, little endian, true colour, 8:8:8 at 16/8/0
        pf = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        s.sendall(struct.pack(">B3x", 0) + pf)
        s.sendall(struct.pack(">BxHi", 2, 1, 0))  # SetEncodings: Raw
        s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
        fb = bytearray(w * h * 4)
        while True:
            t = _recv(s, 1)[0]
            if t == 0:  # FramebufferUpdate
                _recv(s, 1)
                nr = struct.unpack(">H", _recv(s, 2))[0]
                for _ in range(nr):
                    x, y, rw, rh, enc = struct.unpack(">HHHHi", _recv(s, 12))
                    if enc != 0:
                        raise ValueError(f"vnc: unexpected encoding {enc}")
                    data = _recv(s, rw * rh * 4)
                    for r in range(rh):
                        o = ((y + r) * w + x) * 4
                        fb[o:o + rw * 4] = data[r * rw * 4:(r + 1) * rw * 4]
                break
            elif t == 1:  # SetColourMapEntries
                _, _, nc = struct.unpack(">xHH", _recv(s, 5))
                _recv(s, nc * 6)
            elif t == 2:  # Bell
                pass
            elif t == 3:  # ServerCutText
                _recv(s, 3)
                _recv(s, struct.unpack(">I", _recv(s, 4))[0])
            else:
                raise ValueError(f"vnc: unknown server message {t}")
    finally:
        s.close()
    rgb = bytearray(w * h * 3)
    rgb[0::3] = fb[2::4]
    rgb[1::3] = fb[1::4]
    rgb[2::3] = fb[0::4]
    return w, h, bytes(rgb)


def shot(sock_path, out_ppm):
    w, h, rgb = grab(sock_path)
    with open(out_ppm, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h) + rgb)
    return w, h


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    w, h = shot(sys.argv[1], sys.argv[2])
    print(f"{w}x{h}")
