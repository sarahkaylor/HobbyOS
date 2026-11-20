#!/usr/bin/env python3
"""tools/cp/hunt/glasswatch.py -- frame gaps on the glass of a Venus game boot
(lane H9-hunt4, docs/cp-hunt.md round 4).

The fps rows (winevulkan, one per ~1.5 s) and the present-cost rows ([HBP],
one per 2 s) are window averages: a single 250-400 ms frame inside a window
is invisible in them.  This watcher sees every frame the guest puts on the
glass, from the host, without touching the guest, Wine or Mesa:

  host-venus.sh's `-vnc unix:OUT/vnc-sock` (VENUS_VNC=1) is the console that
  egl-headless reads every flushed scanout back into.  QEMU's console refresh
  timer runs every 30 ms while egl-headless is attached (its listener has no
  interval of its own: GUI_REFRESH_INTERVAL_DEFAULT), and each tick the VNC
  server compares the surface with its own copy and sends the changed
  rectangles to a client that has an update request outstanding.  So with one
  incremental request always outstanding, an update arrives on the first
  30 ms tick after the picture changed, and the time between two updates is
  how long the glass showed no new pixels, to within one tick.

    glasswatch.py watch --sock OUT/vnc-sock --out OUT/glass.txt [--pid P]
    glasswatch.py report GLASS.txt [--states OUT/drive-states.txt] [--min-ms 250]

watch: a shared RFB 3.8 client (no auth, 32 bpp, Raw: QEMU's cheapest path, a
copy), the pixel data is read and dropped; it waits for the socket, and
reconnects after QEMU restarts the display, until --pid exits.  glass.txt rows:
    # header lines
    C t_epoch WxH                  a (re)connect
    G t_epoch gap_ms area_pct big_gap_ms     every gap >= 100 ms (and the big one)
    M t_epoch n big max_gap_ms p50 p99       one per 60 s: updates, big updates,
                                             the longest gap, gap percentiles
"big" = an update that changed >= 10 % of the screen; big_gap_ms = time since
the last big update (a frozen world under a ticking HUD clock shows up here).

report: gaps per drive state (drive.py writes OUT/drive-states.txt: "t_epoch
STATE" at every transition: world, vehicle, menu:map, load, ...; boot_p9's own
phases are not known here), counted >= --min-ms; a gap's state is the one in
force at its start.  The menus and loading screens are static by design, so
only world-like states (world*, walk*, vehicle*, drive*) count as hitches.
"""
import argparse
import os
import socket
import struct
import sys
import time

BIG = 0.10


def _recv_exact(s, n, buf=None):
    if buf is None:
        b = bytearray()
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise EOFError("vnc: closed")
            b += c
        return bytes(b)
    mv = memoryview(buf)
    while n > 0:
        k = s.recv_into(mv[:min(n, len(buf))])
        if not k:
            raise EOFError("vnc: closed")
        n -= k
    return None


def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(path)
    ver = _recv_exact(s, 12)
    if not ver.startswith(b"RFB "):
        raise ValueError(f"not RFB: {ver!r}")
    s.sendall(b"RFB 003.008\n")
    n = _recv_exact(s, 1)[0]
    if n == 0:
        raise ValueError("vnc refused")
    if 1 not in _recv_exact(s, n):
        raise ValueError("no None security")
    s.sendall(b"\x01")
    if struct.unpack(">I", _recv_exact(s, 4))[0] != 0:
        raise ValueError("security failed")
    s.sendall(b"\x01")                       # shared: boot_p9's vncshot connects beside us
    w, h = struct.unpack(">HH", _recv_exact(s, 4))
    _recv_exact(s, 16)
    _recv_exact(s, struct.unpack(">I", _recv_exact(s, 4))[0])
    pf = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
    s.sendall(struct.pack(">B3x", 0) + pf)
    # Raw + DesktopSize (-223): the game's resolution / window changes resize the console
    s.sendall(struct.pack(">BxH", 2, 2) + struct.pack(">ii", 0, -223))
    s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))          # one full update first
    s.settimeout(None)
    return s, w, h


class LazyOut:
    """OUT/ may not exist until the harness has made it: open on first write"""
    def __init__(self, path):
        self.path, self.f, self.pending = path, None, []

    def write(self, s):
        if self.f is None:
            if not os.path.isdir(os.path.dirname(os.path.abspath(self.path))):
                self.pending.append(s)
                return
            self.f = open(self.path, "a", buffering=1)
            for x in self.pending:
                self.f.write(x)
        self.f.write(s)


def watch(a):
    out = LazyOut(a.out)
    out.write(f"# glasswatch sock={a.sock} pid={a.pid} start={time.strftime('%F %T')}\n")
    buf = bytearray(1 << 20)

    def alive():
        if not a.pid:
            return True
        try:
            os.kill(a.pid, 0)
            return True
        except OSError:
            return False

    while alive():
        if not os.path.exists(a.sock):
            time.sleep(1)
            continue
        try:
            s, w, h = connect(a.sock)
        except (OSError, ValueError, EOFError) as e:
            out.write(f"# connect {a.sock}: {e}\n")
            time.sleep(2)
            continue
        out.write(f"C {time.time():.3f} {w}x{h}\n")
        last = last_big = None
        mins = time.time() + 60
        gaps, nbig = [], 0
        s.settimeout(5)
        try:
            while True:
                try:
                    t = _recv_exact(s, 1)[0]
                except socket.timeout:
                    if not alive():
                        break
                    continue
                if t == 0:
                    _recv_exact(s, 1)
                    nr = struct.unpack(">H", _recv_exact(s, 2))[0]
                    area = 0
                    for _ in range(nr):
                        x, y, rw, rh, enc = struct.unpack(">HHHHi", _recv_exact(s, 12))
                        if enc == -223:            # DesktopSize: the console changed size
                            w, h = rw, rh
                            out.write(f"C {time.time():.3f} {w}x{h} resize\n")
                            continue
                        if enc != 0:
                            raise ValueError(f"encoding {enc}")
                        _recv_exact(s, rw * rh * 4, buf)
                        area += rw * rh
                    now = time.time()
                    # ask for the next one at once: one request always outstanding
                    s.sendall(struct.pack(">BBHHHH", 3, 1, 0, 0, w, h))
                    pct = 100.0 * area / max(1, w * h)
                    if last is not None:
                        g = (now - last) * 1000.0
                        gaps.append(g)
                        bg = (now - last_big) * 1000.0 if last_big else 0.0
                        if g >= 100 or (pct >= BIG * 100 and bg >= 250):
                            out.write(f"G {now:.3f} {g:.0f} {pct:.1f} {bg:.0f}\n")
                    last = now
                    if pct >= BIG * 100:
                        last_big = now
                        nbig += 1
                    if now >= mins:
                        gs = sorted(gaps) or [0]
                        out.write(f"M {now:.3f} {len(gaps)} {nbig} {gs[-1]:.0f} {gs[len(gs) // 2]:.0f} "
                                  f"{gs[min(len(gs) - 1, int(len(gs) * 0.99))]:.0f}\n")
                        gaps, nbig, mins = [], 0, now + 60
                elif t == 1:
                    _, _, nc = struct.unpack(">xHH", _recv_exact(s, 5))
                    _recv_exact(s, nc * 6)
                elif t == 2:
                    pass
                elif t == 3:
                    _recv_exact(s, 3)
                    _recv_exact(s, struct.unpack(">I", _recv_exact(s, 4))[0])
                else:
                    raise ValueError(f"server message {t}")
        except (OSError, ValueError, EOFError) as e:
            out.write(f"# disconnected {time.time():.3f}: {e}\n")
        finally:
            s.close()
        time.sleep(1)
    out.write(f"# end {time.strftime('%F %T')}\n")
    return 0


WORLDLIKE = ("world", "walk", "vehicle", "drive", "sprint", "play")


def load_states(path):
    st = []
    if path and os.path.exists(path):
        for ln in open(path, errors="replace"):
            p = ln.split(None, 1)
            if len(p) == 2:
                try:
                    st.append((float(p[0]), p[1].strip()))
                except ValueError:
                    pass
    return st


def state_at(st, t):
    cur = "?"
    for ts, name in st:
        if ts > t:
            break
        cur = name
    return cur


def report(a):
    st = load_states(a.states)
    rows, mins, conns = [], [], 0
    for ln in open(a.glass, errors="replace"):
        p = ln.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0] == "C":
            conns += 1
        elif p[0] == "G" and len(p) >= 5:
            t, g, pct, bg = float(p[1]), float(p[2]), float(p[3]), float(p[4])
            rows.append((t - g / 1000.0, g, pct, bg))
        elif p[0] == "M" and len(p) >= 7:
            mins.append([float(x) for x in p[1:7]])
    # a gap's state = the drive state in force when the glass froze; a gap that
    # spans a state change (into a menu, out of a load) is not the world's
    by = {}
    hitches = []
    for t0, g, pct, bg in rows:
        s0, s1 = state_at(st, t0), state_at(st, t0 + g / 1000.0)
        key = s0 if s0 == s1 else f"{s0}->{s1}"
        e = by.setdefault(key, [0, 0, 0.0])
        if g >= a.min_ms:
            e[0] += 1
            e[2] = max(e[2], g)
            if s0 == s1 and s0.startswith(WORLDLIKE):
                hitches.append((t0, g, s0))
        if g >= 1000:
            e[1] += 1
    n_up = sum(int(m[1]) for m in mins)
    print(f"== glass {a.glass}: {conns} connect(s), {len(mins)} minute rows, {n_up} updates"
          + (f", gap p50 median {sorted(m[4] for m in mins)[len(mins) // 2]:.0f} ms" if mins else ""))
    print(f"   gaps >= {a.min_ms:.0f} ms by drive state (count, >= 1 s, max ms):")
    for k, (n, n1, mx) in sorted(by.items(), key=lambda kv: -kv[1][0]):
        if n:
            print(f"   {k:<28} {n:5d} {n1:5d} {mx:8.0f}")
    print(f"HITCHES (world-like states, gap >= {a.min_ms:.0f} ms): {len(hitches)}"
          + (f"; max {max(h[1] for h in hitches):.0f} ms" if hitches else ""))
    for t0, g, s0 in hitches[:40]:
        print(f"   {time.strftime('%H:%M:%S', time.localtime(t0))}.{int((t0 % 1) * 1000):03d} {g:6.0f} ms  {s0}")
    if a.minutes and mins:
        print("   per minute: HH:MM updates big max_gap p50 p99")
        for m in mins:
            print(f"   {time.strftime('%H:%M', time.localtime(m[0]))} {int(m[1]):5d} {int(m[2]):5d} {m[3]:6.0f} {m[4]:4.0f} {m[5]:5.0f}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("watch")
    w.add_argument("--sock", required=True)
    w.add_argument("--out", required=True)
    w.add_argument("--pid", type=int, default=0)
    r = sub.add_parser("report")
    r.add_argument("glass")
    r.add_argument("--states")
    r.add_argument("--min-ms", type=float, default=250)
    r.add_argument("--minutes", action="store_true")
    a = ap.parse_args()
    return watch(a) if a.cmd == "watch" else report(a)


if __name__ == "__main__":
    sys.exit(main())
