#!/usr/bin/env python3
"""tools/cp/bench/present-split.py -- present's share of the frame, from the guest's counters.

Answers, for one run: how much of a frame is submit, how much is present (the
scanout flip), how much is idle, and whether present sits *inside* the frame
interval or *overlaps* the next submit.  Every number comes from the guest's
own clock -- Venus' WSI (`[HBP]` per 2 s window, or `[HBPP]` one row per
present with `HB_PRESENT_EACH=1`, tools/mesa/venus/hb_present_stats.h).  No
host clock, and no number here is host-native: a run is a resolution + a
renderer, and this tool only reports what that run's guest logged.

Two inputs, because the committed runs have both shapes:

  --hbpp LOG        per-present rows ([HBPP] t=... sub=... wait=... flip=...
                    dt=... idx=... path=...), the shape HB_PRESENT_EACH=1 writes
  --window-log LOG  per-window rows ([HBP] ... sub=a/mx wait=a/mx flip=a/mx),
                    optionally a whole harness serial log; `--only 'substr'`
                    keeps just the rows whose line contains it (a phase filter)

  present-split.py --hbpp hbpp.log --label 1280x720 --budget 33.33
  present-split.py --window-log ../../docs/cp-p7-evidence/game-venus2-menu.txt \\
                   --label 1920x1080 --only '[HBP]' --out-csv rows.csv

Definitions (all guest-clock):
  sub      vkQueuePresentKHR minus its fence waits: blit + semaphore submit +
           the hand-off to the WSI thread
  wait     the software WSI's fence wait inside that call (the GPU finishing
           the frame) -- the one stage of present that is *not* present
  flip     the WSI thread's present: the scanout ioctl (SET_SCANOUT_BLOB +
           RESOURCE_FLUSH) or xcb_put_image of the whole image
  present  flip (the part that is "scanout has it"); sub+flip is the present
           *path's* occupancy, which only costs the frame when it is not
           overlapped -- that is what the overlap column measures
  dt       [HBPP] only: present-call entry -> flip return
  frame    the app's present-to-present interval (from the [HBPP] entry times,
           or the window's own n/fps)
  idle     frame - (sub + wait + flip)

Exit 0 always unless an input is unreadable or empty.  Repository: HobbyOS;
this card is t_0552cbd5, report docs/nv-n9v-present-split.md.
"""
import argparse
import csv
import os
import re
import sys

BUDGET_MS = 1000.0 / 30.0            # N9v's gate: 720p >= 30 fps *including present*

HBPP = re.compile(
    r'\[HBPP\]\s+t=(\d+)\s+seq=(\d+)\s+idx=(\d+)\s+path=(\w+)\s+sub=(\d+)\s+'
    r'wait=(\d+)\s+flip=(\d+)\s+dt=(\d+)\s+nosub=(\d+)(?:\s+ppn=(\d+)\s+ppd=(\d+))?')
NVP = re.compile(r'(\w+)=(\d+)/(\d+)')
KV = re.compile(r'(\w+)=([-\w.]+)')


def pct(v, p):
    """The interpolation tools/cp/p0host/fps.py uses, so the numbers match."""
    v = sorted(v)
    if not v:
        return 0.0
    k = (len(v) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


def read_text(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8', 'replace')


def parse_hbpp(path, only=None):
    """Per-present rows.  `only` keeps lines containing that substring."""
    out = []
    for line in read_text(path).splitlines():
        if only and only not in line:
            continue
        m = HBPP.search(line)
        if not m:
            continue
        g = dict(t_ns=int(m.group(1)), seq=int(m.group(2)), idx=int(m.group(3)),
                 path=m.group(4), sub_ns=int(m.group(5)), wait_ns=int(m.group(6)),
                 flip_ns=int(m.group(7)), dt_ns=int(m.group(8)), nosub=int(m.group(9)))
        g['ppn'] = int(m.group(10)) if m.group(10) else 0
        g['ppd'] = int(m.group(11)) if m.group(11) else 0
        out.append(g)
    return out


def parse_windows(path, only=None):
    """[HBP] window rows: sub/wait/flip are avg/max pairs in us.

    Accepts the raw log rows and the CSVs derived from them (hbp.csv: n,fps,
    path,sub_us,wait_us,flip_us; present-windows.csv adds period_ms and *_ms).
    """
    text = read_text(path)
    lines = text.splitlines()
    if lines and ',' in lines[0] and ('fps' in lines[0]):
        out = []
        for x in csv.DictReader(lines):
            try:
                n = int(x['n'])
                fps = float(x['fps'])
            except (KeyError, TypeError, ValueError):
                continue
            if n <= 0 or fps <= 0:
                continue
            def us(k_ms, k_us):
                if k_ms in x and x[k_ms] not in (None, ''):
                    return float(x[k_ms]) * 1000.0
                if k_us in x and x[k_us] not in (None, ''):
                    return float(x[k_us])
                return None
            out.append(dict(
                n=n, fps=fps, path=x.get('path') or '?',
                sub_us=us('sub_ms', 'sub_us') or 0.0,
                wait_us=us('wait_ms', 'wait_us') or 0.0,
                flip_us=us('flip_ms', 'flip_us') or 0.0,
                set_us=None, flush_us=None,
                period_ms=(float(x['period_ms']) if x.get('period_ms') else 1000.0 / fps)))
        return out
    out = []
    for line in lines:
        if only and only not in line:
            continue
        i = line.find('[HBP]')
        if i < 0:
            continue
        body = line[i:]
        pairs = dict((k, (int(a), int(b))) for k, a, b in NVP.findall(body))
        kv = dict((k, v) for k, v in KV.findall(body) if not k in pairs)
        if 'n' not in kv or 'fps' not in kv or 'sub' not in pairs:
            continue
        n = int(kv['n'])
        fps = float(kv['fps'])
        if n <= 0 or fps <= 0:
            continue
        out.append(dict(
            n=n, fps=fps, path=kv.get('path', '?'),
            sub_us=pairs['sub'][0], wait_us=pairs.get('wait', (0, 0))[0],
            flip_us=pairs['flip'][0],
            set_us=int(kv['set']) if 'set' in kv else None,
            flush_us=int(kv['flush']) if 'flush' in kv else None,
            period_ms=1000.0 / fps))
    return out


def summarize_samples(rows, budget):
    """Per-present rows -> (per-present dicts in ms, summary dict)."""
    s = []
    for r in rows:
        ms = lambda x: x / 1e6
        s.append(dict(t_ms=ms(r['t_ns']), idx=r['idx'], path=r['path'],
                      sub_ms=ms(r['sub_ns']), wait_ms=ms(r['wait_ns']),
                      flip_ms=ms(r['flip_ns']), dt_ms=ms(r['dt_ns']),
                      present_ms=ms(r['flip_ns']),
                      path_ms=ms(r['sub_ns'] + r['flip_ns']),
                      nosub=r['nosub']))
    # frame interval and the overlap of one present's flip with the next present
    for k in range(len(s) - 1):
        frame = s[k + 1]['t_ms'] - s[k]['t_ms']
        s[k]['frame_ms'] = frame
        s[k]['idle_ms'] = frame - s[k]['path_ms'] - s[k]['wait_ms']
        # the flip runs on the WSI thread: [flip_start, flip_end]
        flip_start = s[k]['t_ms'] + s[k]['dt_ms'] - s[k]['flip_ms']
        flip_end = s[k]['t_ms'] + s[k]['dt_ms']
        nxt = s[k + 1]['t_ms']
        s[k]['overlap_ms'] = max(0.0, min(flip_end, nxt + s[k + 1]['dt_ms']) - max(flip_start, nxt))
    if s:
        s[-1]['frame_ms'] = None
        s[-1]['idle_ms'] = None
        s[-1]['overlap_ms'] = None
    frames = [x['frame_ms'] for x in s if x['frame_ms']]
    shares = [100.0 * x['path_ms'] / x['frame_ms'] for x in s if x['frame_ms']]
    summary = dict(
        mode='per-present', n=len(s),
        present_ms=pct([x['present_ms'] for x in s], 50),
        present_mean=sum(x['present_ms'] for x in s) / max(len(s), 1),
        sub_ms=pct([x['sub_ms'] for x in s], 50),
        wait_ms=pct([x['wait_ms'] for x in s], 50),
        flip_p95=pct([x['flip_ms'] for x in s], 95),
        flip_max=max((x['flip_ms'] for x in s), default=0.0),
        frame_ms=pct(frames, 50) if frames else 0.0,
        idle_ms=pct([x['idle_ms'] for x in s if x['idle_ms'] is not None], 50) if frames else 0.0,
        share_min=min(shares, default=0.0), share_mean=(sum(shares) / len(shares) if shares else 0.0),
        share_max=max(shares, default=0.0),
        overlap_frac=(sum(1 for x in s if x['overlap_ms'] and x['overlap_ms'] > 0) /
                      max(len([x for x in s if x['overlap_ms'] is not None]), 1)),
        overlap_ms=pct([x['overlap_ms'] for x in s if x['overlap_ms']], 50) if frames else 0.0,
        nosub=sum(1 for x in s if x['nosub']),
        ppn=(rows[-1]['ppn'] if rows else 0), ppd=(rows[-1]['ppd'] if rows else 0),
        budget_ms=budget)
    return s, summary


def summarize_windows(rows, budget):
    s = []
    for r in rows:
        path_ms = (r['sub_us'] + r['flip_us']) / 1000.0
        s.append(dict(n=r['n'], fps=r['fps'], path=r['path'],
                      period_ms=r['period_ms'],
                      sub_ms=r['sub_us'] / 1000.0, wait_ms=r['wait_us'] / 1000.0,
                      flip_ms=r['flip_us'] / 1000.0, present_ms=r['flip_us'] / 1000.0,
                      path_ms=path_ms,
                      set_ms=None if r['set_us'] is None else r['set_us'] / 1000.0,
                      flush_ms=None if r['flush_us'] is None else r['flush_us'] / 1000.0,
                      idle_ms=r['period_ms'] - path_ms - r['wait_us'] / 1000.0,
                      overlap_ms=None))
    shares = [100.0 * x['path_ms'] / x['period_ms'] for x in s]
    summary = dict(
        mode='per-window', n=len(s),
        present_ms=pct([x['present_ms'] for x in s], 50),
        present_mean=sum(x['present_ms'] for x in s) / max(len(s), 1),
        sub_ms=pct([x['sub_ms'] for x in s], 50),
        wait_ms=pct([x['wait_ms'] for x in s], 50),
        flip_p95=pct([x['flip_ms'] for x in s], 95),
        flip_max=max((x['flip_ms'] for x in s), default=0.0),
        frame_ms=pct([x['period_ms'] for x in s], 50) if s else 0.0,
        idle_ms=pct([x['idle_ms'] for x in s], 50) if s else 0.0,
        share_min=min(shares, default=0.0), share_mean=(sum(shares) / len(shares) if shares else 0.0),
        share_max=max(shares, default=0.0),
        overlap_frac=None, overlap_ms=None, nosub=0, ppn=0, ppd=0,
        set_ms=(pct([x['set_ms'] for x in s if x['set_ms'] is not None], 50)
                if any(x['set_ms'] is not None for x in s) else None),
        flush_ms=(pct([x['flush_ms'] for x in s if x['flush_ms'] is not None], 50)
                  if any(x['flush_ms'] is not None for x in s) else None),
        set_frac=((sum(x['set_ms'] for x in s if x['set_ms'] is not None) /
                   max(sum(x['flip_ms'] for x in s if x['set_ms'] is not None), 1e-9))
                  if s and s[0]['set_ms'] is not None else None),
        budget_ms=budget)
    return s, summary


def table(label, sm, budget):
    """The card's table row: resolution x (submit / present-flip / wait / idle)
    in ms and as a share of the 33.3 ms budget, plus the present *path*
    (submit+flip), which is the figure the parent card quotes."""
    def sh(ms):
        return 100.0 * ms / budget
    path_ms = sm['sub_ms'] + sm['present_ms']
    lines = []
    lines.append('%-22s %-11s %5s  %7s %6s  %7s %6s  %7s %6s  %7s %6s  %7s %6s' %
                 ('label', 'input', 'n', 'submit', '%bud', 'flip', '%bud',
                  'present', '%bud', 'wait', '%bud', 'idle', '%bud'))
    lines.append('%-22s %-11s %5d  %7.2f %5.1f%%  %7.2f %5.1f%%  %7.2f %5.1f%%  '
                 '%7.2f %5.1f%%  %7.2f %5.1f%%' %
                 (label, sm['mode'], sm['n'], sm['sub_ms'], sh(sm['sub_ms']),
                  sm['present_ms'], sh(sm['present_ms']), path_ms, sh(path_ms),
                  sm['wait_ms'], sh(sm['wait_ms']), sm['idle_ms'], sh(sm['idle_ms'])))
    extra = ['  (present = sub + flip = the present path; flip alone is the scanout term)',
             '  frame (p50)   %.2f ms   = %.1f fps' % (sm['frame_ms'], 1000.0 / sm['frame_ms']
                                                       if sm['frame_ms'] else 0.0),
             '  flip share of the frame (p50)      %.1f%%'
             % (100.0 * sm['present_ms'] / sm['frame_ms'] if sm['frame_ms'] else 0.0),
             '  present path share of the frame    %.1f%%   (sub+flip; upper bound on '
             'the serial cost, the two halves are on different threads)'
             % (100.0 * path_ms / sm['frame_ms'] if sm['frame_ms'] else 0.0),
             '  present path share, all samples    %.1f / %.1f / %.1f %%  (min/mean/max of %d)'
             % (sm['share_min'], sm['share_mean'], sm['share_max'], sm['n'])]
    if sm.get('set_ms') is not None:
        extra.append('  flip = set %.2f ms + flush %.2f ms   (flush = %.1f %% of flip)'
                     % (sm['set_ms'], sm['flush_ms'], 100.0 * sm['flush_ms'] / sm['present_ms']))
    if sm.get('overlap_frac') is not None:
        extra.append('  flip overlaps the next present in %.1f %% of frames, p50 overlap %.2f ms'
                     % (100.0 * sm['overlap_frac'], sm['overlap_ms']))
    else:
        extra.append('  overlap: not derivable from window rows (they aggregate; '
                     'HB_PRESENT_EACH=1 is what measures it per frame)')
    extra.append('  present cost: p95 %.2f ms, max %.2f ms; present-call wait p50 %.2f ms'
                 % (sm['flip_p95'], sm['flip_max'], sm['wait_ms']))
    if sm['mode'] == 'per-present':
        extra.append('  samples without a hand-off: %d (ppn=%d ppd=%d)'
                     % (sm['nosub'], sm['ppn'], sm['ppd']))
    return '\n'.join(lines + extra)


def write_csv(path, s, mode):
    keys = (['seq', 't_ms', 'idx', 'path', 'sub_ms', 'wait_ms', 'flip_ms', 'present_ms',
             'dt_ms', 'frame_ms', 'idle_ms', 'overlap_ms', 'nosub'] if mode == 'per-present'
            else ['n', 'fps', 'path', 'period_ms', 'sub_ms', 'wait_ms', 'flip_ms',
                  'present_ms', 'path_ms', 'set_ms', 'flush_ms', 'idle_ms'])
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction='ignore')
        w.writeheader()
        for i, r in enumerate(s):
            r = dict(r)
            if mode == 'per-present':
                r['seq'] = i + 1
            w.writerow(r)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--hbpp', help='log with [HBPP] per-present rows')
    ap.add_argument('--window-log', help='log with [HBP] per-window rows')
    ap.add_argument('--label', default='', help='e.g. 1280x720 or 1920x1080')
    ap.add_argument('--only', default=None, help='keep only lines containing this')
    ap.add_argument('--min-fps', type=float, default=None,
                    help='drop windows below this fps (loading/title windows)')
    ap.add_argument('--max-fps', type=float, default=None,
                    help='drop windows above this fps (the menu at 190 fps is a '
                         'different regime from the flight)')
    ap.add_argument('--budget', type=float, default=BUDGET_MS,
                    help='frame budget in ms (default 33.33: N9v gate)')
    ap.add_argument('--out-csv', help='write the per-sample / per-window dump here')
    a = ap.parse_args(argv)
    if not a.hbpp and not a.window_log:
        ap.error('give --hbpp or --window-log')

    if a.hbpp:
        raw = parse_hbpp(a.hbpp, a.only)
        if not raw:
            print('present-split: no [HBPP] rows in %s' % a.hbpp, file=sys.stderr)
            return 2
        s, sm = summarize_samples(raw, a.budget)
    else:
        raw = parse_windows(a.window_log, a.only)
        if a.min_fps is not None:
            raw = [r for r in raw if r['fps'] >= a.min_fps]
        if a.max_fps is not None:
            raw = [r for r in raw if r['fps'] <= a.max_fps]
        if not raw:
            print('present-split: no [HBP] rows in %s (after the fps filter)'
                  % a.window_log, file=sys.stderr)
            return 2
        s, sm = summarize_windows(raw, a.budget)

    print(table(a.label or os.path.basename(a.hbpp or a.window_log), sm, a.budget))
    if a.out_csv:
        write_csv(a.out_csv, s, sm['mode'])
        print('  dump: %s (%d rows)' % (a.out_csv, len(s)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
