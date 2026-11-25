#!/usr/bin/env python3
"""tools/cp/bench/test_present_split.py -- checks for present-split.py on synthetic data.

The two real inputs the tool must read are committed (a harness serial log with
`[X] ... [HBP]` rows and the derived CSVs), but their numbers are the measurement,
not the test: these cases pin the parser and the arithmetic on inputs whose
answers are known.

  python3 tools/cp/bench/test_present_split.py [-v]
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'present-split.py')

# A raw harness log: the [HBP] row wrapped in the serial capture, CRLF line
# ends, set=/flush= present.  sub 6000/28800 us, wait 4/60, flip 9000/15000.
RAW_LOG = (
    '--- serial\r\n'
    '[X] [HBP] pid=94 n=71 fps=17.7 path=scanout acq=1/6 sub=10361/28731 wait=15/110 '
    'flip=9151/15082 geom=0/0 set=225 flush=8841 us scanout=71 putimage=0 refused=0 '
    'total=4707\r\n'
    '[X] [HBP] pid=94 n=88 fps=43.9 path=scanout acq=1/2 sub=11171/186710 wait=12/61 '
    'flip=10273/16892 geom=0/0 set=249 flush=9913 us scanout=88 putimage=0 refused=0 '
    'total=4795\r\n'
    '[X] 87.379 trace:fps:win32u_vkQueuePresentKHR @ approx 48.57fps\r\n'
)

WINDOW_CSV = (
    'host_s,n,fps,period_ms,sub_ms,wait_ms,flip_ms,present_ms,present_pct,rest_ms\r\n'
    '224.3,93,46.4,21.55,4.03,0.004,2.83,6.86,31.8,14.69\r\n'
    '226.3,68,33.6,29.76,5.78,0.004,2.82,8.6,28.9,21.17\r\n'
)


def hbpp_line(t_ns, seq, idx, sub_ns, wait_ns, flip_ns, dt_ns, nosub=0):
    return ('[HBPP] t=%d seq=%d idx=%d path=scanout sub=%d wait=%d flip=%d dt=%d '
            'nosub=%d ppn=%d ppd=0' % (t_ns, seq, idx, sub_ns, wait_ns, flip_ns, dt_ns,
                                       nosub, nosub))


def run(args):
    r = subprocess.run([sys.executable, TOOL] + args, capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError('tool failed (%d): %s%s' % (r.returncode, r.stdout, r.stderr))
    return r.stdout


def num(out, label, col=0):
    """The first number of the data row whose label matches (the table's row 2)."""
    for line in out.splitlines():
        if line.startswith(label):
            return [float(x.strip('%')) for x in line.split()[2:]]
    raise AssertionError('no row %r in:\n%s' % (label, out))


def close(a, b, tol=0.02):
    if abs(a - b) > tol:
        raise AssertionError('%.4f != %.4f' % (a, b))


def t_raw_log(tmp):
    p = os.path.join(tmp, 'serial.txt')
    open(p, 'w').write(RAW_LOG)
    out = run(['--window-log', p, '--label', 'raw'])
    row = num(out, 'raw')
    # n, submit, %bud, flip, %bud, present, %bud, wait, %bud, idle, %bud
    check = {'n': 2, 'sub': 10.766, 'flip': 9.712, 'present': 20.478,
             'wait': 0.0135, 'idle': 19.147}
    close(row[0], check['n'], 0)
    close(row[1], check['sub'], 0.01)
    close(row[3], check['flip'], 0.01)
    close(row[5], check['present'], 0.02)
    close(row[7], check['wait'], 0.01)
    close(row[9], check['idle'], 0.02)
    # the shares are of the default 33.333 ms budget
    close(row[6], 100.0 * check['present'] / (1000.0 / 30.0), 0.2)
    assert 'flush = 94' in out or 'flush = 9' in out, 'set/flush must be reported:\n' + out


def t_window_csv(tmp):
    p = os.path.join(tmp, 'windows.csv')
    open(p, 'w').write(WINDOW_CSV)
    out = run(['--window-log', p, '--label', 'csv'])
    row = num(out, 'csv')
    close(row[1], 4.905, 0.01)     # p50 of 4.03, 5.78
    close(row[3], 2.825, 0.01)     # p50 of 2.83, 2.82
    close(row[9], 17.921, 0.01)    # p50 idle: 14.686, 21.156
    assert 'set' not in out.splitlines()[-6] or 'flush' not in out, 'no set/flush in this CSV'


def t_overlap(tmp):
    # 30 fps frames (33.333 ms apart).  Case A: the flip finishes 16 ms after
    # entry, well inside the interval -> no overlap.  Case B: it finishes 40 ms
    # after entry, i.e. 6.67 ms into the next present.
    ms = 1000000
    a = [hbpp_line(0, 1, 0, 5 * ms, 100000, 10 * ms, 16 * ms),
         hbpp_line(33 * ms + 333333, 2, 1, 5 * ms, 100000, 10 * ms, 16 * ms),
         hbpp_line(66 * ms + 666666, 3, 2, 5 * ms, 100000, 10 * ms, 16 * ms)]
    p = os.path.join(tmp, 'a.log')
    open(p, 'w').write('\n'.join(a) + '\n')
    out = run(['--hbpp', p, '--label', 'nofly'])
    assert 'overlaps the next present in 0.0 %' in out, out
    row = num(out, 'nofly')
    close(row[1], 5.0, 0.01)       # sub p50
    close(row[5], 15.0, 0.01)      # present = sub + flip
    close(row[9], 18.2, 0.05)      # idle = 33.33 - 15 - 0.1

    # Case B: every flip outlives the frame interval.
    b = [hbpp_line(i * (33 * ms + 333333), i + 1, i % 3, 5 * ms, 100000, 10 * ms, 40 * ms)
         for i in range(4)]
    p = os.path.join(tmp, 'b.log')
    open(p, 'w').write('\n'.join(b) + '\n')
    out = run(['--hbpp', p, '--label', 'fly'])
    # the last present has no successor, so the denominator is the 3 that do
    assert 'overlaps the next present in 100.0 %' in out, out
    assert 'p50 overlap 6.67 ms' in out, out


def t_nosub_and_csv_out(tmp):
    ms = 1000000
    rows = [hbpp_line(0, 1, 0, 5 * ms, 100000, 10 * ms, 16 * ms, nosub=1),
            hbpp_line(33 * ms + 333333, 2, 1, 5 * ms, 100000, 10 * ms, 16 * ms, nosub=1)]
    p = os.path.join(tmp, 'n.log')
    open(p, 'w').write('\n'.join(rows) + '\n')
    out_csv = os.path.join(tmp, 'dump.csv')
    out = run(['--hbpp', p, '--label', 'nosub', '--out-csv', out_csv])
    assert 'samples without a hand-off: 2' in out, out
    assert 'dump: %s (2 rows)' % out_csv in out, out
    head = open(out_csv).read().splitlines()
    assert head[0].startswith('seq,t_ms,idx,path,sub_ms'), head[0]
    assert len(head) == 3, head


def t_phase_filter(tmp):
    p = os.path.join(tmp, 'serial.txt')
    open(p, 'w').write(RAW_LOG)
    out = run(['--window-log', p, '--label', 'one', '--only', 'n=88'])
    row = num(out, 'one')
    close(row[0], 1, 0)


def main():
    errors = []

    def one(name, fn):
        try:
            fn()
            print('ok   %s' % name)
        except AssertionError as ex:
            errors.append(name)
            print('FAIL %s: %s' % (name, ex))

    with tempfile.TemporaryDirectory() as tmp:
        for name, fn in (('parses a raw [HBP] serial row (CRLF, set/flush)', t_raw_log),
                         ('reads the derived window CSV', t_window_csv),
                         ('overlap: inside the interval vs across the next present', t_overlap),
                         ('nosub lines + per-sample CSV dump', t_nosub_and_csv_out),
                         ('--only filters to one phase window', t_phase_filter)):
            one(name, lambda f=fn: f(tmp))

    if errors:
        print('%d check(s) failed' % len(errors))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
