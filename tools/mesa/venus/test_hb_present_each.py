#!/usr/bin/env python3
"""tools/mesa/venus/test_hb_present_each.py -- build and check the [HBPP] counter.

The instrument (hb_present_stats.h) is a header copied into Mesa's WSI, so the
full build needs the Mesa tree (tools/mesa/venus/build.sh, on the build host).
What can be checked anywhere is its own logic: the env mode, the app->WSI
hand-off ring, the pairing of sub/flip, the guest-clock fields and the drop
counters.  This test compiles test_hb_present_each.c against two stub headers
(util/os_time.h, hb_env_defaults.h) and asserts on the lines it writes.

  python3 tools/mesa/venus/test_hb_present_each.py [-v]

Exit 0: every check passed.  Nothing here needs a GPU, a guest or Mesa.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
CC = os.environ.get('CC', 'cc')

STUB_OS_TIME = """
/* test stub for Mesa's util/os_time.h: the guest's CLOCK_MONOTONIC, ns. */
#ifndef HB_TEST_OS_TIME_H
#define HB_TEST_OS_TIME_H
#include <stdint.h>
#include <time.h>
static inline int64_t os_time_get_nano(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}
#endif
"""

STUB_ENV = """
/* test stub for the header build.sh generates: no baked environment. */
#define HB_ENV_DEFAULTS ""
"""

LINE = re.compile(r'^\[HBPP\] t=(\d+) seq=(\d+) idx=(\d+) path=(\w+) sub=(\d+) '
                  r'wait=(\d+) flip=(\d+) dt=(\d+) nosub=(\d+) ppn=(\d+) ppd=(\d+)$')


def build(tmp, src='test_hb_present_each.c', out='test_hb_present_each'):
    for rel, body in (('stubs/util/os_time.h', STUB_OS_TIME),
                      ('stubs/hb_env_defaults.h', STUB_ENV)):
        p = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w') as f:
            f.write(body)
    exe = os.path.join(tmp, out)
    cmd = [CC, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
           '-I', os.path.join(tmp, 'stubs'), '-I', HERE,
           os.path.join(HERE, src), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('cc failed:\n' + r.stdout + r.stderr)
    return exe


def run(exe, mode, n=None, env=None):
    e = dict(os.environ)
    e.update(env or {})
    cmd = [exe, mode] + ([str(n)] if n is not None else [])
    r = subprocess.run(cmd, capture_output=True, text=True, env=e)
    if r.returncode != 0:
        raise SystemExit('run %s failed: %s' % (mode, r.stderr))
    rows = []
    for line in r.stderr.splitlines():
        m = LINE.match(line)
        if m:
            rows.append(dict(zip(('t', 'seq', 'idx', 'path', 'sub', 'wait', 'flip',
                                  'dt', 'nosub', 'ppn', 'ppd'),
                                 [int(m.group(1)), int(m.group(2)), int(m.group(3)),
                                  m.group(4)] +
                                 [int(m.group(k)) for k in range(5, 12)])))
    return rows, r.stderr


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def main():
    verbose = '-v' in sys.argv
    errors = []

    def one(name, fn):
        try:
            fn()
            print('ok   %s' % name)
        except AssertionError as ex:
            errors.append('%s: %s' % (name, ex))
            print('FAIL %s: %s' % (name, ex))

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)

        def t_clean():
            rows, err = run(exe, 'clean', 8, {'HB_PRESENT_EACH': '1'})
            check(len(rows) == 8, 'expected 8 [HBPP] lines, got %d\n%s' % (len(rows), err))
            check(all(r['path'] == 'scanout' for r in rows), 'path must be scanout')
            check([r['seq'] for r in rows] == list(range(1, 9)), 'seq must be 1..8')
            check([r['idx'] for r in rows] == [0, 1, 2, 0, 1, 2, 0, 1],
                  'idx must cycle the 3-deep chain: %s' % [r['idx'] for r in rows])
            check(all(r['nosub'] == 0 and r['ppn'] == 0 and r['ppd'] == 0 for r in rows),
                  'clean run must have nosub=ppn=ppd=0')
            ts = [r['t'] for r in rows]
            check(all(b > a for a, b in zip(ts, ts[1:])), 't must increase')
            # dt is entry -> flip return, so it must cover the three stages.
            for r in rows:
                check(r['dt'] >= r['sub'] + r['wait'] + r['flip'],
                      'dt %d < sub+wait+flip %d' % (r['dt'], r['sub'] + r['wait'] + r['flip']))
            # One calibration for every line: the implied cycles/ns of each
            # stage is the same within 1 % (100000 + 1000*i cycles etc.).
            rates = [r['sub'] / (100000.0 + 1000.0 * i) for i, r in enumerate(rows)]
            check(max(rates) / min(rates) < 1.01, 'sub: inconsistent cycles/ns: %r' % rates)
            rates = [r['flip'] / (2500000.0 + 50000.0 * i) for i, r in enumerate(rows)]
            check(max(rates) / min(rates) < 1.01, 'flip: inconsistent cycles/ns: %r' % rates)
            # The stage shares must survive the conversion (sub:flip ~ 1:25).
            for r in rows:
                check(20.0 < r['flip'] / max(r['sub'], 1) < 30.0,
                      'flip/sub out of range: %d/%d' % (r['flip'], r['sub']))

        def t_nosub():
            rows, err = run(exe, 'nosub', env={'HB_PRESENT_EACH': '1'})
            check(len(rows) == 1, 'expected 1 line, got %d' % len(rows))
            r = rows[0]
            check(r['nosub'] == 1 and r['ppn'] == 1, 'a present with no hand-off: nosub=1 ppn=1')
            check(r['sub'] == 0 and r['wait'] == 0 and r['dt'] == 0,
                  'no hand-off: sub/wait/dt are 0')
            check(r['flip'] > 0, 'the flip is still measured without a hand-off')

        def t_drop():
            rows, err = run(exe, 'drop', env={'HB_PRESENT_EACH': '1'})
            check(len(rows) == 1, 'expected 1 line, got %d' % len(rows))
            r = rows[0]
            check(r['ppd'] == 36, 'ring of 64 + 100 hand-offs: ppd=36, got %d' % r['ppd'])
            check(r['nosub'] == 0, 'the ring was not empty: nosub=0')

        def t_off():
            rows, err = run(exe, 'clean', 4, {'HB_PRESENT_EACH': '0'})
            check(not rows, 'HB_PRESENT_EACH=0 must print no [HBPP] lines')
            check('[HBP]' not in err and '[HBV]' not in err,
                  'HB_PRESENT_EACH=0 alone must print no window rows either')

        def t_windows_default_off():
            # EACH=1 with no HB_PRESENT_STATS_MS: 8 presents in a few ms, so
            # there would be no window row in either case; what matters is
            # that no [HBP] row appeared at the first present.
            rows, err = run(exe, 'clean', 8, {'HB_PRESENT_EACH': '1'})
            check('[HBP]' not in err,
                  'HB_PRESENT_EACH=1 alone must not print window rows (window_ms=0)')

        def t_both():
            rows, err = run(exe, 'both', env={'HB_PRESENT_EACH': '1',
                                              'HB_PRESENT_STATS_MS': '100'})
            check(len(rows) == 2, 'expected 2 [HBPP] lines, got %d' % len(rows))
            hb = [l for l in err.splitlines() if l.startswith('[HBP]')]
            check(len(hb) == 1, 'expected 1 [HBP] window row, got %d' % len(hb))
            check('n=2' in hb[0], 'the window must count both presents: %s' % hb[0])

        def t_call_sites():
            # The two sites tools/mesa/venus/build.sh injects C into, compiled
            # against the real header: a signature change must break here, not
            # at Mesa build time on strix.
            b = build(tmp, 'test_hb_present_call_sites.c', 'call_sites')
            r = subprocess.run([b], capture_output=True, text=True,
                               env=dict(os.environ, HB_PRESENT_EACH='1'))
            check(r.returncode == 0, 'call_sites exited %d: %s' % (r.returncode, r.stderr))
            rows = [LINE.match(l) for l in r.stderr.splitlines()]
            rows = [m for m in rows if m]
            check(len(rows) == 4, 'expected 4 [HBPP] lines from 4 presents, got %d' % len(rows))
            check(all(m.group(4) == 'scanout' for m in rows), 'stub grants the scanout path')
            check([int(m.group(3)) for m in rows] == [0, 1, 2, 3], 'idx must be the image index')
            check(all(int(m.group(9)) == 0 for m in rows), 'every present had its hand-off')
            check(all(int(m.group(5)) > 0 for m in rows), 'sub must be measured')

        for name, fn in (('clean run pairs sub/flip per present', t_clean),
                         ('present with no hand-off -> nosub=1', t_nosub),
                         ('full ring drops hand-offs and counts them', t_drop),
                         ('HB_PRESENT_EACH=0 is silent', t_off),
                         ('per-present mode alone prints no window rows', t_windows_default_off),
                         ('explicit HB_PRESENT_STATS_MS keeps window rows', t_both),
                         ('build.sh call sites compile against the header', t_call_sites)):
            one(name, fn)

    if verbose:
        print()
    if errors:
        print('%d check(s) failed' % len(errors))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
