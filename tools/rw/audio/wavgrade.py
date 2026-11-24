#!/usr/bin/env python3
"""Grade a QEMU `wav` audiodev file without numpy.

    wavgrade.py OUT.wav [--freq HZ] [--min-tone-s S] [--min-rms-dbfs D]

Prints rate/channels/frames/duration, peak and RMS in dBFS, active time
(100 ms windows above -50 dBFS), and the tone frequency from upward zero
crossings (a clean sine gives one up-crossing per period).  Exit 0 when the
requested thresholds hold, 1 when they do not, 2 on a bad file.

This is the no-numpy companion to tools/cp/audio/wavcheck.py, for hosts where
the only Python has no numpy.
"""
import argparse
import math
import struct
import sys


def read_wav(path):
    data = open(path, "rb").read()
    if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", data[body:body + 16])
        elif cid == b"data":
            end = len(data) if size == 0 or body + size > len(data) else body + size
            pcm = data[body:end]
            break
        pos = body + size + (size & 1)
    if fmt is None or pcm is None:
        raise ValueError("no fmt/data chunk")
    tag, ch, rate, _, _, bits = fmt
    if tag != 1 or bits != 16:
        raise ValueError(f"unsupported format tag={tag} bits={bits}")
    n = len(pcm) // (2 * ch)
    s = struct.unpack("<%dh" % (n * ch), pcm[:n * ch * 2])
    mono = [sum(s[i * ch:(i + 1) * ch]) / ch for i in range(n)]
    return rate, ch, bits, mono


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--freq", type=float, default=0)
    ap.add_argument("--tol", type=float, default=2.0)
    ap.add_argument("--min-tone-s", type=float, default=0)
    ap.add_argument("--min-rms-dbfs", type=float, default=-50)
    a = ap.parse_args()
    try:
        rate, ch, bits, mono = read_wav(a.wav)
    except (OSError, ValueError) as e:
        print(f"wavgrade: {a.wav}: {e}")
        return 2

    n = len(mono)
    dur = n / rate
    peak = max(abs(x) for x in mono) if mono else 0
    rms = math.sqrt(sum(x * x for x in mono) / n) if n else 0
    db = lambda v: 20 * math.log10(v / 32768) if v > 0 else -float("inf")
    print(f"file={a.wav} rate={rate} ch={ch} bits={bits} frames={n} duration={dur:.3f}s")
    print(f"peak={peak:.0f}/32767 ({db(peak):.1f} dBFS) rms={rms:.0f} ({db(rms):.1f} dBFS)")

    win = rate // 10
    active = sum(
        1 for i in range(n // win)
        if math.sqrt(sum(x * x for x in mono[i * win:(i + 1) * win]) / win) > 32768 * 10 ** (-50 / 20)
    )
    print(f"active(> -50 dBFS, 100 ms windows)={active * 0.1:.1f}s of {dur:.1f}s")

    up = sum(1 for i in range(1, n) if mono[i - 1] <= 0 < mono[i])
    freq = up / dur if dur else 0
    print(f"zero-crossings up={up} -> tone={freq:.1f} Hz")

    ok = True
    if a.min_rms_dbfs > -999 and db(rms) < a.min_rms_dbfs:
        print(f"wavgrade: FAIL rms {db(rms):.1f} dBFS < {a.min_rms_dbfs} dBFS")
        ok = False
    if a.freq > 0:
        lo, hi = a.freq * (1 - a.tol / 100), a.freq * (1 + a.tol / 100)
        if not (lo <= freq <= hi):
            print(f"wavgrade: FAIL tone {freq:.1f} Hz outside {lo:.1f}..{hi:.1f} Hz")
            ok = False
        if a.min_tone_s > 0:
            # active seconds are a lower bound on tone seconds for a pure tone
            if active * 0.1 + 1e-9 < a.min_tone_s:
                print(f"wavgrade: FAIL active {active * 0.1:.1f}s < {a.min_tone_s}s")
                ok = False
    print("wavgrade: PASS" if ok else "wavgrade: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
