#!/usr/bin/env python3
"""tools/cp/audio/wavcheck.py -- grade the WAV QEMU's `wav` audiodev wrote
(lane C4-audio, docs/cp-audio.md).

    wavcheck.py OUT.wav [--freq HZ] [--tol PCT] [--min-tone-s S]
                        [--min-active-s S] [--json]

Reads a 16-bit PCM WAV (QEMU writes the sizes into the header only when it
exits cleanly; a header with zero sizes is read up to the end of the file).
Reports: duration, overall RMS (dBFS), "active" time (100 ms windows whose
RMS is above -50 dBFS), the dominant frequency of the active windows (FFT
peak, per window, then the median), and, with --freq, how many seconds of
active windows have their peak within --tol percent of HZ.

Exit 0 when every requested threshold holds, 1 otherwise, 2 on a bad file.
Pure stdlib + numpy (numpy is on strix; the script refuses without it).
"""
import argparse
import json
import math
import struct
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover
    print("wavcheck: needs numpy", file=sys.stderr)
    sys.exit(2)


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
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
    a = np.frombuffer(pcm[:n * 2 * ch], dtype="<i2").astype(np.float64).reshape(n, ch) / 32768.0
    return rate, ch, a


def dbfs(x):
    r = math.sqrt(float(np.mean(x * x))) if len(x) else 0.0
    return 20 * math.log10(r) if r > 0 else -float("inf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--freq", type=float, default=0)
    ap.add_argument("--tol", type=float, default=2.0)
    ap.add_argument("--min-tone-s", type=float, default=0)
    ap.add_argument("--min-active-s", type=float, default=0)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    try:
        rate, ch, x = read_wav(a.wav)
    except (OSError, ValueError) as e:
        print(f"wavcheck: {a.wav}: {e}")
        return 2
    mono = x.mean(axis=1) if ch > 1 else x[:, 0]
    win = rate // 10
    nwin = len(mono) // win
    active, peaks, tone_win = 0, [], 0
    first_active = None
    for i in range(nwin):
        seg = mono[i * win:(i + 1) * win]
        lvl = dbfs(seg)
        if lvl <= -50:
            continue
        active += 1
        if first_active is None:
            first_active = i * 0.1
        spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
        spec[0] = 0
        k = int(np.argmax(spec))
        f = k * rate / len(seg)
        peaks.append(f)
        if a.freq and abs(f - a.freq) <= a.freq * a.tol / 100.0:
            tone_win += 1
    res = {
        "file": a.wav,
        "rate": rate,
        "channels": ch,
        "duration_s": round(len(mono) / rate, 3),
        "rms_dbfs": round(dbfs(mono), 1),
        "active_rms_dbfs": None,
        "active_s": round(active * 0.1, 1),
        "first_active_s": first_active,
        "peak_hz_median": round(float(np.median(peaks)), 1) if peaks else None,
        "tone_hz": a.freq or None,
        "tone_s": round(tone_win * 0.1, 1) if a.freq else None,
    }
    if active:
        sel = np.concatenate([mono[i * win:(i + 1) * win] for i in range(nwin)
                              if dbfs(mono[i * win:(i + 1) * win]) > -50])
        res["active_rms_dbfs"] = round(dbfs(sel), 1)
        res["active_peak_dbfs"] = round(20 * math.log10(float(np.max(np.abs(sel))) + 1e-12), 1)
    ok = True
    if a.min_active_s and res["active_s"] < a.min_active_s:
        ok = False
    if a.freq and a.min_tone_s and res["tone_s"] < a.min_tone_s:
        ok = False
    res["verdict"] = "PASS" if ok else "FAIL"
    if a.json:
        print(json.dumps(res))
    else:
        print("wavcheck: " + " ".join(f"{k}={v}" for k, v in res.items()))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
