#!/usr/bin/env python3
"""tools/cp/bench/frames-dist.py -- what the Cyberpunk 720p frame-time distribution is made of.

Reads the game's own per-frame log (frames.csv, written inside the guest by the game,
so the clock is the guest's) and the [HBP] present-counter rows of the same run
(hbp.csv), and answers four questions with numbers:

  1. where the time goes per frame (present sub/flip from [HBP] vs the rest),
  2. whether the slow frames are periodic, input-correlated or warm-up,
  3. what the 10/1/0.1 %-low percentiles require of the rest of the distribution,
  4. the per-segment level (the benchmark's camera path changes it by ~1.9x).

Usage:
  frames-dist.py [RUN_EVIDENCE_DIR] [--annotate OUT.csv]

RUN_EVIDENCE_DIR holds frames.csv + hbp.csv (default: the committed
docs/cp-bench-evidence/hobbyos-venus-720p copy of the 2026-11-24 HobbyOS/venus run).
Nothing here measures anything: it is arithmetic on counters the benchmark and
Venus' present path already wrote.  Repository: HobbyOS.
"""
import csv, os, statistics, sys

DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       '..', '..', '..', 'docs', 'cp-bench-evidence', 'hobbyos-venus-720p')
GATE_MS = 1000.0 / 30.0            # N9v: 30 fps including present
SLOW_MS = 45.0                     # a "slow" frame for the burst/gap counts


def pct(v, p):
    """The interpolation tools/cp/p0host/fps.py uses, so the percentiles match."""
    v = sorted(v)
    k = (len(v) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


def load_frames(d):
    ft = []
    with open(os.path.join(d, 'frames.csv'), newline='', encoding='utf-8-sig') as f:
        for x in csv.DictReader(f):
            x = {k.strip(): v for k, v in x.items()}
            ft.append(float(x['Frame time (ms)']))
    return ft


def load_hbp(d):
    rows = []
    with open(os.path.join(d, 'hbp.csv'), newline='', encoding='utf-8-sig') as f:
        for x in csv.DictReader(f):
            x = {k.strip(): v for k, v in x.items()}
            rows.append(x)
    return rows


def local_median(ft, w=31):
    n = len(ft)
    return [statistics.median(ft[max(0, i - w // 2):min(n, i + w // 2 + 1)]) for i in range(n)]


def runs_of(idx):
    out = []
    if not idx:
        return out
    cur = [idx[0]]
    for a, b in zip(idx, idx[1:]):
        if b - a == 1:
            cur.append(b)
        else:
            out.append(cur)
            cur = [b]
    out.append(cur)
    return out


def main():
    d = DEFAULT
    annotate = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == '--annotate':
            annotate = args[i + 1]
            i += 2
        else:
            d = args[i]
            i += 1
    ft = load_frames(d)
    hbp = load_hbp(d)
    n = len(ft)
    secs = sum(ft) / 1000.0

    print("# frame distribution, %s" % d)
    print("# frames %d, %.2f s, mean %.2f fps" % (n, secs, n / secs))
    print("# p50 %.2f ms (%.2f fps)  p90 %.2f (%.2f)  p99 %.2f (%.2f)  p99.9 %.2f (%.2f)  max %.2f"
          % (pct(ft, 50), 1000 / pct(ft, 50), pct(ft, 90), 1000 / pct(ft, 90),
             pct(ft, 99), 1000 / pct(ft, 99), pct(ft, 99.9), 1000 / pct(ft, 99.9), max(ft)))
    print("# frames over the %.2f ms gate line: %d (%.1f %%)"
          % (GATE_MS, sum(1 for t in ft if t > GATE_MS), 100 * sum(1 for t in ft if t > GATE_MS) / n))
    print("# frames over 100 ms: %d" % sum(1 for t in ft if t > 100))

    # ---- 1. present-path term, per [HBP] window (the window's own fps, so no alignment needed)
    print("\n## 1. present path, world-rate [HBP] windows (fps 20..50)")
    print("window_s  n    fps   period_ms  sub_ms flip_ms  present_%  rest_ms")
    world = [r for r in hbp if r['phase'] == 'bench' and 20.0 <= float(r['fps']) < 50.0]
    shares = []
    for r in world:
        per = 1000.0 / float(r['fps'])
        sub, fl = float(r['sub_us']) / 1000.0, float(r['flip_us']) / 1000.0
        shares.append(100.0 * (sub + fl) / per)
        print("%8s %4s %6.1f %9.2f %7.2f %7.2f %8.1f %8.2f"
              % (r['host_s'], r['n'], float(r['fps']), per, sub, fl, 100 * (sub + fl) / per,
                 per - sub - fl))
    print("windows %d, presents %d" % (len(world), sum(int(r['n']) for r in world)))
    print("present share of the frame: min %.1f %% mean %.1f %% max %.1f %%"
          % (min(shares), statistics.mean(shares), max(shares)))
    mx = max(float(r['sub_us']) / 1000 + float(r['flip_us']) / 1000 for r in world)
    print("worst-window present cost %.2f ms = %.1f %% of the %.2f ms p99 frame; "
          "p99 minus present = %.2f ms (%.1f fps)"
          % (mx, 100 * mx / pct(ft, 99), pct(ft, 99), pct(ft, 99) - mx, 1000 / (pct(ft, 99) - mx)))

    # ---- 2. classification: periodic? input? warm-up?
    print("\n## 2. the slow frames: periodic, input-correlated or warm-up?")
    slow = [i for i, t in enumerate(ft) if t > SLOW_MS]
    print("slow (> %.0f ms): %d frames (%.2f %%)" % (SLOW_MS, len(slow), 100 * len(slow) / n))
    gaps = {}
    for a, b in zip(slow, slow[1:]):
        gaps[b - a] = gaps.get(b - a, 0) + 1
    top = sorted(gaps.items(), key=lambda kv: -kv[1])[:6]
    print("gaps between consecutive slow frames (gap: count): %s" % ", ".join(
        "%d:%d" % kv for kv in top))
    print("runs of *consecutive* slow frames: %d runs (of >=2), longest %d frames"
          % (len([r for r in runs_of(slow) if len(r) >= 2]),
             max((len(r) for r in runs_of(slow)), default=0)))
    loc = local_median(ft)
    res = [ft[i] - loc[i] for i in range(n)]
    print("residual over the 31-frame local median: mean %+.2f ms, sd %.2f ms, max %+.2f ms"
          % (statistics.mean(res), statistics.stdev(res), max(res)))
    for t in (4, 8, 12, 15, 18):
        print("  residual >= +%2d ms: %3d frames (%.2f %%)"
              % (t, sum(1 for r in res if r >= t), 100 * sum(1 for r in res if r >= t) / n))
    print("residual histogram 0..22 ms, 1 ms bins:")
    for b in range(22):
        c = sum(1 for r in res if b <= r < b + 1)
        print("  %2d %4d %s" % (b, c, '#' * int(c / 4)))
    # a periodic stall of period N would put one index residue above the rest.
    # Compare the real series' best residue against the same statistic on shuffled
    # copies (same marginal distribution, no order): if they agree, there is no period.
    import random, math
    sd = statistics.stdev(res)
    def best_fold(series):
        b = (0.0, 0, 0)
        for N in range(2, 161):
            means = [statistics.mean(series[r::N]) for r in range(N) if len(series[r::N]) >= 3]
            m = max(means)
            if m > b[0]:
                b = (m, N, means.index(m))
        return b
    real_best = best_fold(res)
    rnd = random.Random(20261125)
    null = []
    for _ in range(120):
        sh = res[:]
        rnd.shuffle(sh)
        null.append(best_fold(sh)[0])
    null.sort()
    p95 = null[int(0.95 * (len(null) - 1))]
    print("fold test on the residual, N=2..160: best residue %.2f ms = %.2f sd at N=%d"
          % (real_best[0], real_best[0] / sd, real_best[1]))
    print("  same statistic on %d shuffles: median %.2f sd, p95 %.2f sd, max %.2f sd -> %s"
          % (len(null), null[len(null) // 2] / sd, p95 / sd, null[-1] / sd,
             "NO periodic component" if real_best[0] <= p95 else "a periodic component to chase"))
    # spectrum: one dominant peak would mean a periodic source; broadband 1/f is jitter
    N = 1024
    seg = res[:N]
    spec = []
    for k in range(N // 64, N // 2):
        re = sum(seg[i] * math.cos(2 * math.pi * k * i / N) for i in range(N))
        im = sum(seg[i] * math.sin(2 * math.pi * k * i / N) for i in range(N))
        spec.append((re * re + im * im, N / k))
    tot = sum(p for p, _ in spec)
    print("residual spectrum, periods 2..64 frames: top 3 peaks by power share "
          "(uniform = %.1f %%)" % (100.0 / len(spec)))
    for p, per in sorted(spec, reverse=True)[:3]:
        print("  period %6.1f frames: %.2f %% of the power" % (per, 100 * p / tot))
    # warm-up: are the slow frames a front-loaded population?
    third = n // 3
    for k in range(3):
        a, b = k * third, (k + 1) * third if k < 2 else n
        print("  residual >= +15 ms in frames %4d-%4d: %d frames" % (a, b, sum(1 for i in range(a, b) if res[i] >= 15)))
    # level, not spikes: how much of the flight is in a *slow state* (9-frame smoothed median)
    sm = [statistics.median(ft[max(0, i - 4):min(n, i + 5)]) for i in range(n)]
    over = [i for i in range(n) if sm[i] > GATE_MS]
    ovr = runs_of(over)
    print("9-frame smoothed median over the %.2f ms gate: %d frames (%.1f %%) in %d runs, longest %d"
          % (GATE_MS, len(over), 100 * len(over) / n, len(ovr), max((len(r) for r in ovr), default=0)))

    # ---- 3. what the percentiles require
    print("\n## 3. what the 1 %-low requires of the rest of the distribution")
    p50, p99 = pct(ft, 50), pct(ft, 99)
    print("p50 %.2f ms = %.2f fps; p99 %.2f ms = %.2f fps; gate %.2f ms = 30.00 fps"
          % (p50, 1000 / p50, p99, 1000 / p99, GATE_MS))
    print("p99/p50 shape = %.2f; at that shape a 30 fps 1 %%-low needs a p50 of %.1f fps"
          % (p99 / p50, 30 * p99 / p50))
    clamp = [min(t, p50) for t in ft]
    print("if every frame above the median were clamped to it: mean %.2f fps, 1 %%-low %.2f fps "
          "(tail-only fixes cannot pass)" % (len(clamp) / (sum(clamp) / 1000), 1000 / pct(clamp, 99)))
    mx_ms = mx
    print("if the present path cost nothing at all: p99 %.2f ms = %.1f fps"
          % (p99 - mx_ms, 1000 / (p99 - mx_ms)))

    # ---- 5. what the measured overheads can and cannot buy at the p99
    # The guest-kernel share is a share of the game threads' wall time, measured on the
    # same stack and box in the in-world scene ([THRW], docs/cp-present.md §5.1, integ10:
    # 11-14 %).  It is a share, so it is applied to this run's p99 as a scale, and it is
    # labelled as coming from another scene; it is the only kernel number the evidence set
    # has.  Both bound rows below assume the overhead is *serialized* with the frame and
    # can be removed for free -- the most optimistic reading possible.
    print("\n## 5. what the measured overheads can and cannot buy at the p99")
    kb = (0.11, 0.14)
    print("p99 frame %.2f ms; gate %.2f ms; present (worst window) %.2f ms; "
          "guest kernel %.2f-%.2f ms (11-14 %% of wall)"
          % (p99, GATE_MS, mx_ms, kb[0] * p99, kb[1] * p99))
    for name, saved in (("present only", mx_ms),
                        ("kernel only (14 %)", kb[1] * p99),
                        ("kernel only (11 %)", kb[0] * p99),
                        ("present + kernel (14 %)", mx_ms + kb[1] * p99),
                        ("present + kernel (11 %)", mx_ms + kb[0] * p99)):
        left = p99 - saved
        print("  remove %-24s -> p99 %6.2f ms = %5.1f fps %s"
              % (name, left, 1000 / left, "PASS" if left <= GATE_MS else "still short"))
    print("to pass, the p99 frame must lose %.2f ms (%.0f %% of itself); the two measured "
          "overheads are %.2f ms together at most"
          % (p99 - GATE_MS, 100 * (p99 - GATE_MS) / p99, mx_ms + kb[1] * p99))

    # ---- 4. level by segment (split where the log has its two longest quiet stretches)
    print("\n## 4. level, not spikes: the benchmark's camera path moves the median by ~1.9x")
    gaps = sorted(((b - a, a, b) for a, b in zip(slow, slow[1:])), reverse=True)
    print("longest stretches with no frame > %.0f ms (frames, seconds):" % SLOW_MS)
    for g, a, b in gaps[:3]:
        print("  %4d..%4d len %3d  t %5.1f..%5.1f s"
              % (a + 1, b - 1, g, sum(ft[:a + 1]) / 1000, sum(ft[:b]) / 1000))
    cuts = sorted(a + 1 for g, a, b in gaps[:2])
    print("segments split there: frames / time / n / mean / p50 / p90 / p99 / max / fps")
    edges = [0] + cuts + [n]
    for k in range(len(edges) - 1):
        a, b = edges[k], edges[k + 1]
        s = ft[a:b]
        t0, t1 = sum(ft[:a]) / 1000, sum(ft[:b]) / 1000
        print("  seg%d %4d-%4d %5.1f-%5.1f s %4d %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f"
              % (k + 1, a, b, t0, t1, len(s), statistics.mean(s), pct(s, 50), pct(s, 90),
                 pct(s, 99), max(s), len(s) / (t1 - t0)))
    blk = 50
    print("50-frame blocks: start_frame  median_ms  p90_ms  max_ms  t_s")
    for b in range(0, n, blk):
        s = sorted(ft[b:b + blk])
        print("%11d %10.2f %7.2f %7.2f %6.1f"
              % (b, s[len(s) // 2], s[int(0.9 * (len(s) - 1))], max(s), sum(ft[:b]) / 1000))

    if annotate:
        # per-frame distribution dump.  The [HBP] present rows are per 2 s window and the
        # window timestamps in this run were read from one serial burst after ~243 s, so
        # the present term is NOT merged into per-frame rows here: it stays in the window
        # table above, where each row is internally consistent (fps, sub, flip of one
        # window).  Mixing them would need a per-frame counter, which this run did not log.
        with open(annotate, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['frame', 't_ms', 'frame_ms', 'fps_inst', 'local_median_ms', 'residual_ms'])
            t = 0.0
            for idx, v in enumerate(ft):
                t += v
                w.writerow([idx, round(t, 2), v, round(1000 / v, 2), round(loc[idx], 2),
                            round(res[idx], 2)])
        print("\nwrote %s (%d frames)" % (annotate, n))
        # the present-path windows, as their own file
        out = os.path.join(os.path.dirname(annotate), 'present-windows.csv')
        with open(out, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['host_s', 'n', 'fps', 'period_ms', 'sub_ms', 'wait_ms', 'flip_ms',
                        'present_ms', 'present_pct', 'rest_ms'])
            for r in world:
                per = 1000.0 / float(r['fps'])
                sub, wt, fl = (float(r['sub_us']) / 1000, float(r['wait_us']) / 1000,
                               float(r['flip_us']) / 1000)
                w.writerow([r['host_s'], r['n'], r['fps'], round(per, 2), round(sub, 2),
                            round(wt, 3), round(fl, 2), round(sub + fl, 2),
                            round(100 * (sub + fl) / per, 1), round(per - sub - fl, 2)])
        print("wrote %s (%d windows)" % (out, len(world)))


if __name__ == '__main__':
    main()
