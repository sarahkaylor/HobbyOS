#!/usr/bin/env python3
"""l88-stages.py -- the L88 stage table: where a RimWorld frame goes.

Reads a run's serial log (the harness writes obj/rw-desktop/serial-*.log) and
joins the three instruments that time the same frame:

  [XSWB]  the compat libX11's per-32-present batch: frame period (`dt_avg`),
          `self_avg` (XcPresentMark: sampled checksum + shadow blit + flush
          message), `put_avg` (XPutImage's copy into the shadow), `flush_avg`
          (shadow -> /dev/fb0 + the ESC ] F message), and the fb0 blit's
          coverage (`blit_px`, `blit=WxH+X+Y`).
  [VGLB]  the virgl winsys' per-32-present batch: `pres_avg` (the whole
          flush_frontbuffer), `thumb_avg` (the 64x64 damage probe -- it is
          ordered after Unity's frame on the same context, so it contains the
          host's render time), `copy_avg` (readback -> the sw display target),
          `disp_avg` (the frontend's XPutImage), `rd_b_avg` (bytes read back).
  [HBTS]  HBHOST's 2 s window: `put_avg_us` (its own XPutImage into XHOBBY),
          `copy_us` (a reference memcpy of the same rect in the same process),
          `busy_permille` (share of the window inside XPutImage), `bps`.

Usage:
  python3 tools/rw/l88-stages.py SERIAL.log [--from S] [--to S] [--json]

`--from/--to` are guest ms (the `t=` field); the default window is every row
after the last `[HBT] probe`-free stretch, i.e. the last 25 % of the log.  Pass
a window explicitly for a run whose phases matter.
"""
import argparse
import json
import re
import sys


def rows(text, tag):
    out = []
    for m in re.finditer(r"\[%s\]([^\n]*)" % tag, text):
        line = m.group(1)
        d = {}
        for k, v in re.findall(r"(\w+)=(-?\d+)", line):
            d[k] = int(v)
        m2 = re.search(r"\bbox=(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", line)
        if m2:
            d["box_w"], d["box_h"] = int(m2.group(1)), int(m2.group(2))
            d["box_x"], d["box_y"] = int(m2.group(3)), int(m2.group(4))
        m3 = re.search(r"\bblit=(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", line)
        if m3:
            d["blit_w"], d["blit_h"] = int(m3.group(1)), int(m3.group(2))
        out.append(d)
    return out


def avg(rs, key):
    vs = [r[key] for r in rs if key in r]
    return (sum(vs) / len(vs)) if vs else None


def pick(rs, t0, t1):
    return [r for r in rs if "t" in r and t0 <= r["t"] <= t1]


def fmt(v, div=1.0, nd=2, suffix=""):
    if v is None:
        return "-"
    return ("%." + str(nd) + "f%s") % (v / div, suffix)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="t0", type=int, default=None)
    ap.add_argument("--to", dest="t1", type=int, default=None)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    text = open(a.log, errors="replace").read()
    # The Player.log section is a copy of the same rows; keep only the serial
    # part (the harness appends it after this marker).
    cut = text.find("--- Player.log (")
    if cut > 0:
        text = text[:cut]

    xsw = rows(text, "XSW")
    xswb = rows(text, "XSWB")
    vglb = rows(text, "VGLB")
    hbts = rows(text, "HBTS")
    hbt = rows(text, "HBT")

    if not xsw:
        print("no [XSW] rows in %s" % a.log, file=sys.stderr)
        return 2
    t0 = a.t0 if a.t0 is not None else xsw[0].get("t", 0)
    t1 = a.t1 if a.t1 is not None else xsw[-1].get("t", 0)

    X = pick(xsw, t0, t1)
    XB = pick(xswb, t0, t1)
    VB = pick(vglb, t0, t1)
    HB = pick(hbts, t0, t1)
    HT = pick(hbt, t0, t1)

    changed = sum(1 for r in X if r.get("changed") == 1)
    same = sum(1 for r in X if r.get("changed") == 0)
    dn = (X[-1]["n"] - X[0]["n"]) if len(X) > 1 else 0
    dt = ((X[-1].get("t", 0) - X[0].get("t", 0)) / 1000.0) if len(X) > 1 else 0
    fps = dn / dt if dt > 0 else 0

    dt_avg = avg(XB, "dt_avg")          # us per frame, app side
    pres = avg(VB, "pres_avg")          # us, whole winsys present
    thumb = avg(VB, "thumb_avg")        # us, damage probe (host render wait)
    copy = avg(VB, "copy_avg")          # us, readback -> dt image
    disp = avg(VB, "disp_avg")          # us, frontend XPutImage
    rd_b = avg(VB, "rd_b_avg")          # bytes read back per present
    self_avg = avg(XB, "self_avg")
    put_avg = avg(XB, "put_avg")
    flush_avg = avg(XB, "flush_avg")
    blit_px = avg(XB, "blit_px")
    row_us = avg(XB, "row_us")
    host_put = avg(HB, "put_avg_us")
    host_copy = avg(HB, "copy_us")
    host_busy = avg(HB, "busy_permille")
    host_bps = avg(HB, "bps")
    host_mpx = avg(HB, "Mpx_s")
    host_blit_px = avg(HT, "px") if HT else None

    frame_us = dt_avg or (1e6 / fps if fps else None)
    res = {
        "log": a.log, "window": [t0, t1], "fps": round(fps, 2),
        "frames_xsw": dn, "changed": changed, "same": same,
        "frame_us": frame_us,
        "app": {"dt_avg": dt_avg, "self_avg": self_avg, "put_avg": put_avg,
                "flush_avg": flush_avg, "blit_px": blit_px, "row_us": row_us,
                "pres_avg": pres, "thumb_avg": thumb, "copy_avg": copy,
                "disp_avg": disp, "rd_b_avg": rd_b},
        "host": {"put_avg_us": host_put, "copy_us": host_copy,
                 "busy_permille": host_busy, "bps": host_bps,
                 "Mpx_s": host_mpx, "blit_px": host_blit_px},
        "rows": {"XSW": len(X), "XSWB": len(XB), "VGLB": len(VB),
                 "HBTS": len(HB), "HBT": len(HT)},
    }
    if a.json:
        print(json.dumps(res, indent=1))
        return 0

    print("window t=%d..%d ms  frames(XSW n)=%d  changed=%d  same=%d"
          % (t0, t1, dn, changed, same))
    print("fps=%.2f  frame=%.1f ms  (%d [XSWB], %d [VGLB], %d [HBTS] rows)"
          % (fps, (frame_us or 0) / 1000.0, len(XB), len(VB), len(HB)))
    if changed == 0 and dn > 0:
        print("WARNING: every sampled frame reports changed=0 -- this window "
              "is a still image, not a frame rate.")
    print()
    hdr = "%-42s %10s %7s" % ("stage", "ms/frame", "% frame")
    print(hdr)
    print("-" * len(hdr))

    def line(name, us):
        if us is None or not frame_us:
            print("%-42s %10s %7s" % (name, "-", "-"))
            return
        print("%-42s %10.2f %6.1f%%" % (name, us / 1000.0,
                                        100.0 * us / frame_us))

    # The app side, in the order the frame runs.
    line("winsys: damage probe (thumb+host render)", thumb)
    rb = None
    if pres is not None:
        parts = [p for p in (thumb, copy, disp) if p is not None]
        rb = max(0.0, pres - sum(parts))
    line("winsys: boxed readback (host -> guest)", rb)
    line("winsys: readback -> XImage copy", copy)
    line("winsys: XPutImage -> shim shadow", disp)
    line("shim: XcPresentMark self (put+flush+crc)", self_avg)
    line("shim:   of which XPutImage copy", put_avg)
    line("shim:   of which shadow -> fb0 blit", flush_avg)
    line("shim:   of which sampled checksum", row_us)
    print()
    if host_put is not None:
        line("HBHOST: XPutImage -> XHOBBY (wire+server)", host_put)
        line("HBHOST: reference memcpy of same rect", host_copy)
        if host_put is not None and host_copy is not None:
            line("HBHOST: wire+server minus the copy", max(0, host_put - host_copy))
        print()
        print("HBHOST busy: %.1f%% of the window inside XPutImage "
              "(%.0f blits/s, %.1f Mpx/s, %.0f px/blit)"
              % ((host_busy or 0) / 10.0, host_bps or 0, host_mpx or 0,
                 host_blit_px or 0))
    if blit_px:
        print("app blit coverage: %.0f px/frame (%.2f Mpx)" %
              (blit_px, blit_px / 1e6))
    if rd_b:
        print("readback: %.2f MiB/frame" % (rd_b / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
