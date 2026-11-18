#!/usr/bin/env python3
"""tools/cp/hunt/shotcheck.py -- visual-glitch screen of a game boot's
screendumps (lane H1-hunt, docs/cp-hunt.md).  The shots are game content: they
stay on strix; this prints numbers only.

    shotcheck.py RUN_DIR [RUN_DIR ...]          per-shot flags + a per-run summary
    shotcheck.py --compare DIR_A DIR_B          menu shots of two runs (Venus vs lavapipe)

Per shot (RUN_DIR/shots/sNNN-tNNNNNN*.png, in order):
  black      share of pixels with max(R,G,B) < 10            -> "black" >= 0.97
             (and in the top-left 1280x720 of a 1920x1080 shot: the game's
             borderless window on the P8 desktop)       -> "window-black"
  part-black a band of >= 12 % of the rows (top, bottom) or columns (left,
             right) that is all black while the rest is not  -> "partial-black"
  tear       the largest mean |row[y+1]-row[y]| over the median one, where that
             row's jump spans >= 80 % of the width (a full-width seam), on a
             row that is not a seam in half the run's shots (static edges)
                                                               -> "tear?" >= 12
  noise      mean |Laplacian| (high-frequency energy) vs the run's median
                                                               -> "garbage?" >= 4x
  stuck      identical to the previous shot (sha1 of the pixels) during
             play phases (TAG names from the play scripts) -> "same-as-prev"
  colour     mean R, G, B (wrong-colour check: compare runs, --compare)
Flags are candidates for a look, not verdicts.  Pure PIL + numpy.
"""
import hashlib
import json
import os
import re
import sys

import numpy as np
from PIL import Image

SHOT_RX = re.compile(r"^s(\d{3})-t(\d{6})(?:-(.*))?\.png$")


def load(p):
    im = Image.open(p).convert("RGB")
    return np.asarray(im, dtype=np.int16)


def metrics(a):
    h, w, _ = a.shape
    mx = a.max(axis=2)
    dark = mx < 10
    black = float(dark.mean())
    rows_black = dark.mean(axis=1) > 0.995
    cols_black = dark.mean(axis=0) > 0.995

    def band(v):
        n = len(v)
        top = int(np.argmin(v)) if not v.all() else n
        bot = int(np.argmin(v[::-1])) if not v.all() else n
        return top / n, bot / n
    rt, rb = band(rows_black)
    cl, cr = band(cols_black)
    g = a.mean(axis=2)
    rd = np.abs(np.diff(g, axis=0))                     # (h-1, w)
    rmean = rd.mean(axis=1)
    med = float(np.median(rmean)) + 0.5
    y = int(np.argmax(rmean))
    span = float((rd[y] > 24).mean())
    tear = float(rmean[y] / med) if span >= 0.8 else 0.0
    top3 = [int(v) for v in np.argsort(rmean)[-3:]]
    win_black = float(dark[:720, :1280].mean()) if (w, h) == (1920, 1080) else black
    lap = np.abs(4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:])
    return {"w": w, "h": h, "black": round(black, 3), "band_top": round(rt, 3), "band_bottom": round(rb, 3),
            "band_left": round(cl, 3), "band_right": round(cr, 3), "tear": round(tear, 1), "tear_row": y, "seams": top3,
            "win_black": round(win_black, 3),
            "noise": round(float(lap.mean()), 2),
            "rgb": [round(float(a[..., c].mean()), 1) for c in range(3)],
            "sha": hashlib.sha1(a.tobytes()).hexdigest()[:12]}


def shots(run):
    d = os.path.join(run, "shots")
    out = []
    for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        m = SHOT_RX.match(f)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), m.group(3) or "", os.path.join(d, f)))
    return out


def check_run(run):
    rows = []
    for n, t, tag, p in shots(run):
        try:
            m = metrics(load(p))
        except Exception as e:  # noqa: BLE001
            rows.append({"n": n, "t": t, "tag": tag, "err": str(e)})
            continue
        m.update({"n": n, "t": t, "tag": tag})
        rows.append(m)
    good = [r for r in rows if "err" not in r]
    if not good:
        return {"run": run, "shots": 0}
    nmed = float(np.median([r["noise"] for r in good])) or 1.0
    # rows that are a seam in at least half the shots are static edges (the
    # IceWM taskbar at y=1053, a 1280x720 window's bottom edge), not tears
    cnt = {}
    for r in good:
        for y in set(r["seams"]):
            cnt[y] = cnt.get(y, 0) + 1
    static = {y for y, c in cnt.items() if c >= max(2, len(good) // 2)}
    static |= {y + d for y in list(static) for d in (-1, 1)}
    prev = None
    flags = []
    for r in good:
        f = []
        if r["black"] >= 0.97:
            f.append("black")
        elif r["win_black"] >= 0.97 and r["w"] == 1920:
            f.append("window-black")
        elif max(r["band_top"], r["band_bottom"], r["band_left"], r["band_right"]) >= 0.12 and r["black"] < 0.9:
            f.append("partial-black")
        if r["tear"] >= 12 and r["tear_row"] not in static:
            f.append(f"tear?@{r['tear_row']}")
        if r["noise"] >= 4 * nmed:
            f.append("garbage?")
        if prev and prev["sha"] == r["sha"] and re.search(r"loop|soak|world|w\d|map|fp|rl", r["tag"] or "play"):
            f.append("same-as-prev")
        r["flags"] = f
        if f:
            flags.append(r)
        prev = r
    sizes = sorted({(r["w"], r["h"]) for r in good})
    return {"run": run, "shots": len(rows), "errors": len(rows) - len(good), "sizes": sizes,
            "noise_median": round(nmed, 2), "flagged": flags,
            "black_shots": sum(1 for r in good if "black" in r["flags"]),
            "menu_rgb": next((r["rgb"] for r in reversed(good) if "menu" in r["tag"] or r is good[-1]), None)}


def summary(res):
    print(f"== shotcheck {res['run']}: {res['shots']} shots, sizes {res.get('sizes')}, "
          f"noise median {res.get('noise_median')}, read errors {res.get('errors', 0)}")
    for r in res.get("flagged", []):
        print(f"   s{r['n']:03d} t{r['t']:06d} {r['tag']:<24} {' '.join(r['flags']):<28} black={r['black']} win={r['win_black']} "
              f"bands(t,b,l,r)=({r['band_top']},{r['band_bottom']},{r['band_left']},{r['band_right']}) "
              f"tear={r['tear']} noise={r['noise']} rgb={r['rgb']}")
    print(f"   flagged {len(res.get('flagged', []))} of {res['shots']}")


def compare(a, b):
    def menu(run):
        s = [x for x in shots(run) if not x[2]]          # the unnamed P8 watch shots
        return [(x, metrics(load(x[3]))) for x in s[-3:]]
    for run in (a, b):
        print(f"== {run}: last 3 watch shots (menu)")
        for (n, t, _, _), m in menu(run):
            print(f"   s{n:03d} t{t:06d} {m['w']}x{m['h']} rgb={m['rgb']} black={m['black']} noise={m['noise']}")


def main():
    a = sys.argv[1:]
    if a[:1] == ["--compare"]:
        compare(a[1], a[2])
        return 0
    js = "--json" in a
    a = [x for x in a if x != "--json"]
    for run in a:
        res = check_run(run)
        if js:
            print(json.dumps(res))
        else:
            summary(res)
    return 0


if __name__ == "__main__":
    sys.exit(main())
