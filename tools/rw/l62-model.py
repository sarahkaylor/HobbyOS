#!/usr/bin/env python3
"""tools/rw/l62-model.py -- ms/frame vs pixels, and the 30 fps requirement.

Input: a CSV of measured points, one per line:

    label,width,height,fps

The model is the only one a single-threaded present path can have:

    ms_per_frame = a + b * Mpx        (a = per-frame fixed cost, b = per-Mpx)

so the fit's `b` is the pipeline's current fill cost and 1/b is the fill rate it
sustains (Mpx/s).  The playable target is 30 fps = 33.33 ms/frame, i.e.
30 * Mpx Mpx/s of fill plus whatever `a` costs; the program prints the fill rate
that requires at each measured size, the speedup over what is measured today,
and the largest surface the current cost model reaches at 30 fps.

Usage: l62-model.py points.csv
"""
import sys


def main():
    rows = []
    for line in open(sys.argv[1]):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("label"):
            continue
        p = [x.strip() for x in line.split(",")]
        if len(p) < 4:
            continue
        w, h, fps = int(p[1]), int(p[2]), float(p[3])
        if fps <= 0:
            continue
        rows.append((p[0], w, h, fps, w * h / 1e6, 1000.0 / fps))

    if not rows:
        print("l62-model: no usable points")
        return 1

    print(f"{'config':<14}{'surface':>12}{'Mpx':>7}{'fps':>7}{'ms/frame':>10}"
          f"{'Mpx/s':>8}")
    print("-" * 58)
    for lab, w, h, fps, mpx, ms in rows:
        print(f"{lab:<14}{f'{w}x{h}':>12}{mpx:>7.3f}{fps:>7.3f}{ms:>10.1f}"
              f"{mpx * fps:>8.3f}")

    # Least squares on (Mpx, ms).
    n = len(rows)
    sx = sum(r[4] for r in rows)
    sy = sum(r[5] for r in rows)
    sxx = sum(r[4] * r[4] for r in rows)
    sxy = sum(r[4] * r[5] for r in rows)
    den = n * sxx - sx * sx
    if n < 2 or abs(den) < 1e-9:
        print("\nl62-model: one point (or no spread) -- slope not identifiable")
        return 0
    b = (n * sxy - sx * sy) / den
    a = (sy - b * sx) / n
    mean = sy / n
    ss_tot = sum((r[5] - mean) ** 2 for r in rows)
    ss_res = sum((r[5] - (a + b * r[4])) ** 2 for r in rows)
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 1.0

    print(f"\nmodel: ms/frame = {a:.1f} + {b:.1f} * Mpx     (R^2={r2:.4f}, n={n})")
    print(f"       fixed cost {a:.1f} ms/frame, fill cost {b/1000.0*1e3:.1f} ms/Mpx"
          f" = {1e3/b:.3f} Mpx/s sustained")

    print(f"\n30 fps needs 33.33 ms/frame; the speedup each size needs over its"
          f" measured rate:")
    for lab, w, h, fps, mpx, ms in rows:
        need = 30.0 * mpx
        print(f"  {lab:<14}{f'{w}x{h}':>12}  {need:7.3f} Mpx/s of fill"
              f"  (30/{fps:.2f} = {30.0/fps:5.1f}x the measured frame rate)")
    if b > 0:
        budget = 33.333 - a
        if budget <= 0:
            print(f"\n  the fixed cost alone ({a:.1f} ms) exceeds the 33.3 ms budget")
        else:
            px = budget / b                     # MP
            side = int((px * 1e6) ** 0.5)
            print(f"\n  at today's cost the 30 fps budget buys {px:.3f} Mpx of surface"
                  f" (about {side}x{side});")
            print(f"  1920x1080 at 30 fps needs 62.2 Mpx/s, i.e. "
                  f"{62.208/(1e3/b):.1f}x the fill rate measured here.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
