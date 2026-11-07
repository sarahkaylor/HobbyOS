#!/usr/bin/env python3
"""tools/rw/l80-budget.py -- the present path's frame budget, per 32-present batch.

Reads a serial log from a run with /XPROF + /GLPROF staged (L80) and joins the
three rows the three layers write:

  [XSWB]  compat libX11: the frame period (dt), the time inside XcPresentMark
          (self: the surface checksum, the fb0 blit, the flush message) and
          the XPutImage copy (put).  One row per 32 presents.
  [XSWL]  libGL's XMesaSwapBuffers: st_context_flush (flush -- the frame's own
          submit), fence_finish (fence), xmesa_swap_st_framebuffer (swap --
          the frontend's present, which ends in the winsys), invalidate.
  [VGLB]  the virgl winsys' flush_frontbuffer: the GPU present itself (pres)
          plus submits, readbacks, uploads and how often damage was idle.

The budget the table checks is

    dt  =  [XSWL] total  +  [XSWB] self  +  gap (the engine between swaps)
    [XSWL] swap  >=  [VGLB] pres  +  [XSWB] put

Usage:
  l80-budget.py <serial.log> [--window 120] [--all]
"""
import argparse
import re
import sys

NUMS = r"([\d]+)"


def parse(path):
    xswb, xswl, vglb = [], [], []
    rx = re.compile(r"\[XSWB\] n=(\d+) t=(\d+) frames=(\d+) dt_avg=(\d+) "
                    r"dt_max=(\d+) self_avg=(\d+) self_max=(\d+) put_avg=(\d+) "
                    r"flush_avg=(\d+) row_us=(\d+) w=(-?\d+) h=(-?\d+)")
    rl = re.compile(r"\[XSWL\] frames=(\d+) flush_avg=(\d+) fence_avg=(\d+) "
                    r"swap_avg=(\d+) inval_avg=(\d+) total_max=(\d+)")
    rv = re.compile(r"\[VGLB\] n=(\d+) t=(\d+) frames=(\d+) pres_avg=(\d+) "
                    r"pres_max=(\d+) sub_avg=(\d+) sub_b_avg=(\d+) rd_avg=(\d+) "
                    r"rd_b_avg=(\d+) up_avg=(\d+) up_b_avg=(\d+) idle=(\d+) "
                    r"thumb_avg=(\d+) copy_avg=(\d+) disp_avg=(\d+) boxes=(\d+)")
    with open(path, "rb") as f:
        for raw in f:
            s = raw.decode("latin1")
            m = rx.search(s)
            if m:
                g = [int(x) for x in m.groups()]
                xswb.append(dict(n=g[0], t=g[1], frames=g[2], dt=g[3], dt_max=g[4],
                                 self_us=g[5], self_max=g[6], put=g[7],
                                 flush=g[8], row_us=g[9], w=g[10], h=g[11]))
                continue
            m = rl.search(s)
            if m:
                g = [int(x) for x in m.groups()]
                xswl.append(dict(frames=g[0], flush=g[1], fence=g[2], swap=g[3],
                                 inval=g[4], total_max=g[5]))
                continue
            m = rv.search(s)
            if m:
                g = [int(x) for x in m.groups()]
                vglb.append(dict(n=g[0], t=g[1], frames=g[2], pres=g[3],
                                 pres_max=g[4], sub=g[5], sub_b=g[6], rd=g[7],
                                 rd_b=g[8], up=g[9], up_b=g[10], idle=g[11],
                                 thumb=g[12], copy=g[13], disp=g[14],
                                 boxes=g[15]))
    return xswb, xswl, vglb


def med(v):
    if not v:
        return 0
    v = sorted(v)
    return v[len(v) // 2]


def f(v):
    return v / 1000.0


def table(label, xs, xl, vg):
    if not xs:
        print(f"  {label}: no [XSWB] rows")
        return
    dt = [r["dt"] for r in xs]
    swapped = [xl[i]["flush"] + xl[i]["fence"] + xl[i]["swap"] + xl[i]["inval"]
               for i in range(min(len(xs), len(xl)))]
    print(f"  {label}: batches={len(xs)}  frames={sum(r['frames'] for r in xs)}")
    print(f"    dt      median {f(med(dt)):8.2f} ms   (max {f(max(r['dt_max'] for r in xs)):8.2f})")
    if xl:
        n = min(len(xl), len(xs))
        print(f"    swap    median {f(med(swapped)):8.2f} ms   "
              f"flush {f(med([xl[i]['flush'] for i in range(n)])):6.2f}  "
              f"fence {f(med([xl[i]['fence'] for i in range(n)])):6.2f}  "
              f"front {f(med([xl[i]['swap'] for i in range(n)])):6.2f}  "
              f"inval {f(med([xl[i]['inval'] for i in range(n)])):5.2f}")
    print(f"    mark    median {f(med([r['self_us'] for r in xs])):8.2f} ms   "
          f"(the shim: checksum + shadow blit + flush msg)")
    print(f"    put     median {f(med([r['put'] for r in xs])):8.2f} ms   "
          f"flush   {f(med([r['flush'] for r in xs])):8.2f} ms")
    if vg:
        m = min(len(vg), len(xs))
        print(f"    present median {f(med([r['pres'] for r in vg])):8.2f} ms   "
              f"(winsys flush_frontbuffer)")
        print(f"    submits/frame {med([r['sub'] for r in vg]):.1f} "
              f"({med([r['sub_b'] for r in vg])/1024:.0f} KiB)   "
              f"readbacks/frame {med([r['rd'] for r in vg]):.1f} "
              f"({med([r['rd_b'] for r in vg])/1024:.0f} KiB)   "
              f"uploads {med([r['up'] for r in vg]):.1f} "
              f"({med([r['up_b'] for r in vg])/1024:.0f} KiB)   "
              f"idle {100.0*sum(r['idle'] for r in vg)/max(1,sum(r['frames'] for r in vg)):.0f}%")
        print(f"    thumb   median {f(med([r['thumb'] for r in vg])):8.2f} ms   "
              f"copy {f(med([r['copy'] for r in vg])):6.2f}  "
              f"disp {f(med([r['disp'] for r in vg])):6.2f}  "
              f"boxes {med([r['boxes'] for r in vg]):.0f}")
        del m
    if xl:
        n = min(len(xs), len(xl))
        gap = [xs[i]["dt"] - swapped[i] - xs[i]["self_us"] for i in range(n)]
        print(f"    gap     median {f(med(gap)):8.2f} ms   "
              f"(dt - swap - mark: the engine between swaps)")
    print(f"    probe   row_us {f(med([r['row_us'] for r in xs])):.2f} ms "
          f"(its own I/O, once per {med([r['frames'] for r in xs]):.0f} presents)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--window", type=float, default=0,
                    help="only the last N seconds of the run")
    ap.add_argument("--all", action="store_true", help="print every batch")
    a = ap.parse_args()

    xs, xl, vg = parse(a.log)
    if not xs:
        print(f"{a.log}: no [XSWB] rows -- was /XPROF staged?")
        return 1
    print(f"{a.log}\n  [XSWB] {len(xs)}  [XSWL] {len(xl)}  [VGLB] {len(vg)} rows")
    if a.window and xs:
        t_end = xs[-1]["t"]
        cut = t_end - int(a.window * 1000)
        ncut = next((r["n"] for r in xs if r["t"] >= cut), xs[0]["n"])
        xs = [r for r in xs if r["n"] >= ncut]
        vg = [r for r in vg if r["n"] >= ncut]
        # XSWL has no n; keep the same number of trailing rows
        xl = xl[-len(xs):] if len(xl) >= len(xs) else xl
        print(f"  window: last {a.window:.0f} s (from present {ncut})")
    print()
    if a.all:
        print("    n      t_s    frames    dt    swap    mark   present    gap")
        for i, r in enumerate(xs):
            tot = 0
            if i < len(xl):
                tot = (xl[i]["flush"] + xl[i]["fence"] + xl[i]["swap"] +
                       xl[i]["inval"])
            pr = vg[i]["pres"] if i < len(vg) else 0
            gap = r["dt"] - tot - r["self_us"]
            print("%6d %7.1f %6d %6.1f %6.1f %6.1f %8.1f %6.1f" % (
                r["n"], r["t"] / 1000.0, r["frames"], f(r["dt"]), f(tot),
                f(r["self_us"]), f(pr), f(gap)))
        print()
    table("whole run", xs, xl, vg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
