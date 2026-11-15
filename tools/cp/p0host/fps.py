#!/usr/bin/env python3
"""tools/cp/p0host/fps.py <csv> [--skip S] — frame-time statistics.

Accepts a MangoHud per-frame log (header row starting "fps,frametime") or the game's own
benchmark frames.csv ("Frame index, Frame time (ms), ...").  Prints one line:
  frames, seconds, mean fps (frames/seconds), median fps (1000/p50 frame time),
  p90 fps (1000/p90 frame time: 10% of frames are slower), 1%-low fps (1000/p99 frame
  time), 0.1%-low (1000/p99.9), max frame time.
--skip drops the first S seconds (shader/streaming warm-up) — default 0.
"""
import sys, csv, statistics

def pct(v, p):
    v = sorted(v); k = (len(v) - 1) * p / 100.0
    f = int(k); c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)

def load(path):
    rows = list(csv.reader(open(path, newline="")))
    for i, r in enumerate(rows):
        h = [x.strip().lower() for x in r]
        if h[:2] == ["fps", "frametime"]:
            return [float(x[1]) for x in rows[i + 1:] if len(x) > 1 and x[1]]
        if len(h) > 1 and h[1].startswith("frame time"):
            return [float(x[1]) for x in rows[i + 1:] if len(x) > 1 and x[1].strip()]
    sys.exit("unknown csv format: " + path)

def main():
    path = sys.argv[1]
    skip = float(sys.argv[sys.argv.index("--skip") + 1]) if "--skip" in sys.argv else 0.0
    ft = load(path)
    if skip:
        acc, i = 0.0, 0
        while i < len(ft) and acc < skip * 1000: acc += ft[i]; i += 1
        ft = ft[i:]
    secs = sum(ft) / 1000.0
    print(f"frames={len(ft)} secs={secs:.1f} mean_fps={len(ft)/secs:.1f} "
          f"median_fps={1000/statistics.median(ft):.1f} p90_fps={1000/pct(ft,90):.1f} "
          f"low1_fps={1000/pct(ft,99):.1f} low01_fps={1000/pct(ft,99.9):.1f} max_ft_ms={max(ft):.1f}")

if __name__ == "__main__":
    main()
