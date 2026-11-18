#!/usr/bin/env python3
"""tools/cp/hunt/wavgaps.py -- dropouts, gaps and timing drift in the WAV that
QEMU's `wav` audiodev wrote for a game boot (lane H1-hunt, docs/cp-hunt.md).
tools/cp/audio/wavcheck.py grades level and tone; this looks for glitches.

    wavgaps.py [--log RUNLOG] [--json] OUT.wav

* dropout: a run of exact digital zero (both channels) of 5..500 ms with loud
  audio on both sides (>= -40 dBFS in the 50 ms before and after).  Game music
  and effects fade; an underrun (the guest did not feed a period in time and
  the device played silence) cuts to zero and back.
* gap: any zero / near-silent (< -70 dBFS) run of >= 500 ms after the first
  active window (scene changes have these too: listed, not graded).
* click: a sample-to-sample jump of more than 0.6 full scale on either
  channel next to a zero run (the edge of a dropout).
* drift: WAV length vs the boot's wall time between "game launched" and
  "QEMU stopped" in RUNLOG (run-p8 / run-p9 output).  QEMU writes samples only
  while the guest's stream runs, so WAV < wall means stream start latency plus
  any time the device was starved; the ratio is reported, not graded.

Exit 0 always (a measurement); --json for one JSON line.
"""
import argparse
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "audio"))
import numpy as np  # noqa: E402
from wavcheck import read_wav  # noqa: E402


def runs(mask):
    """(start, end) index pairs of True runs in a bool array."""
    if not len(mask):
        return []
    d = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    return list(zip(np.flatnonzero(d == 1), np.flatnonzero(d == -1)))


def dbfs(x):
    r = math.sqrt(float(np.mean(x * x))) if len(x) else 0.0
    return 20 * math.log10(r) if r > 0 else -200.0


def wall_window(log):
    if not log or not os.path.exists(log):
        return None
    t = open(log, errors="replace").read()

    def at(rx):
        m = re.search(rx, t)
        return float(m.group(1)) if m else None
    a = at(r"\+\s+([0-9.]+) s\s+\S+\s+game launched")
    b = at(r"\+\s+([0-9.]+) s\s+\S+\s+verdict: ")
    if a is None or b is None:
        return None
    return {"launch_s": a, "stop_s": b, "wall_s": round(b - a, 1)}


def analyse(path, log=None):
    rate, ch, x = read_wav(path)
    n = len(x)
    res = {"file": os.path.basename(path), "rate": rate, "channels": ch, "duration_s": round(n / rate, 2)}
    zero = np.all(x == 0, axis=1)
    mono = x.mean(axis=1)
    w = rate // 100                                       # 10 ms windows
    nw = n // w
    lv = np.array([dbfs(mono[i * w:(i + 1) * w]) for i in range(nw)])
    act = np.flatnonzero(lv > -50)
    res["active_s"] = round(len(act) * 0.01, 1)
    if not len(act):
        res["verdict"] = "no active audio"
        return res
    first, last = act[0] * w, (act[-1] + 1) * w
    res["first_active_s"] = round(first / rate, 2)
    res["last_active_s"] = round(last / rate, 2)
    ctx = rate // 20                                      # 50 ms
    drops, gaps, clicks = [], [], 0
    for a, b in runs(zero[first:last]):
        a += first
        b += first
        ms = (b - a) * 1000.0 / rate
        if ms >= 500:
            gaps.append((round(float(a) / rate, 2), int(round(ms))))
            continue
        if ms < 5:
            continue
        pre, post = mono[max(0, a - ctx):a], mono[b:b + ctx]
        if dbfs(pre) >= -40 and dbfs(post) >= -40:
            drops.append((round(float(a) / rate, 2), round(float(ms), 1), round(dbfs(pre), 1), round(dbfs(post), 1)))
            for e in (a, b):
                if 0 < e < n and np.max(np.abs(x[e] - x[e - 1])) > 0.6:
                    clicks += 1
    # near-silent (not zero) long runs after the first active window
    quiet = lv[act[0]:act[-1] + 1] < -70
    for a, b in runs(quiet):
        if (b - a) >= 50:
            s = (act[0] + a) * 0.01
            if not any(abs(s - g[0]) < 0.5 for g in gaps):
                gaps.append((round(float(s), 2), int((b - a) * 10)))
    gaps.sort()
    res["dropouts"] = len(drops)
    res["dropout_ms_total"] = round(sum(d[1] for d in drops), 1)
    res["dropouts_first"] = drops[:12]
    res["edge_clicks"] = clicks
    res["gaps_ge_500ms"] = len(gaps)
    res["gaps_first"] = gaps[:12]
    ww = wall_window(log)
    if ww:
        res.update(ww)
        res["wav_over_wall"] = round(res["duration_s"] / ww["wall_s"], 4) if ww["wall_s"] > 0 else None
        # QEMU keeps writing (silence included) until it stops, so the WAV's
        # t=0 is (wall - duration) s after the launch: dropouts in game seconds
        off = ww["wall_s"] - res["duration_s"]
        res["wav_t0_game_s"] = round(off, 1)
        res["dropouts_game_s"] = [round(d[0] + off, 1) for d in drops]
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--log")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    try:
        r = analyse(a.wav, a.log)
    except (OSError, ValueError) as e:
        print(f"wavgaps: {a.wav}: {e}")
        return 0
    if a.json:
        print(json.dumps(r))
    else:
        for k, v in r.items():
            print(f"wavgaps: {k}={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
