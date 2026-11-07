#!/usr/bin/env python3
"""Host (T1) self-test for perf_game_cycle core logic: freeze-watchdog
detection and the RSS "no unbounded growth" assertion (GB-046), plus the
GB-046-V-RED hardening (playfield-crop hashing, whole-run floor->peak RSS,
and per-visit launch verification). No QEMU."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perf_game_cycle import (FreezeWatchdog, rss_trend_growth, flag_freeze_free,
                             rss_floor_peak_growth, parse_ppm_header,
                             crop_sha256, launch_seen, launch_retry_needed,
                             launched_name)

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

print("FreezeWatchdog")
# a static scene (same digest repeatedly) must freeze after `window` samples
w = FreezeWatchdog(4)
obs = [w.observe("A") for _ in range(6)]
check(obs[0] is False and obs[1] is False and obs[2] is False,
      "does not flag freeze before `window` identical samples")
check(w.frozen is True and obs[-1] is True,
      "flags freeze once 4 consecutive identical frames are seen")
check(w.runs_seen == 6, "counts observations")

# a changing scene (animated game) must never freeze
w2 = FreezeWatchdog(4)
obs2 = [w2.observe(f"h{i}") for i in range(10)]
check(True not in obs2 and w2.frozen is False,
      "an animated game (changing frames) is never flagged frozen")

# None samples (missing screendump) count toward the window
w3 = FreezeWatchdog(3)
w3.observe("x"); r = w3.observe(None)
check(r is False, "sparse None observed, not yet frozen")

print("rss_trend_growth (\u201cno unbounded growth\u201d)")
g, ok = rss_trend_growth([1000, 1001, 1002], 64)
check(ok is True and round(g, 2) == 0.0, "small RSS growth (2 KB) ok under a 64 MB bound")
g2, ok2 = rss_trend_growth([1000, 1000, 5000], 1)
check(ok2 is False and round(g2, 2) == 3.91, "4000 KB (3.91 MB) growth fails a 1 MB bound")
g3, ok3 = rss_trend_growth([1000, 1000, 5000], 4)
check(ok3 is True and round(g3, 2) == 3.91, "4000 KB (3.91 MB) growth is fine under a 4 MB bound")
g4, ok4 = rss_trend_growth([], 64)
check(ok4 is True and g4 == 0.0, "no samples => pass (nothing measured)")

print("flag_freeze_free")
check(flag_freeze_free(["a", "a", "a", "a"], 3)[-1] is True,
      "flags the static tail frozen")
check(flag_freeze_free(["a", "b", "c", "d"], 3)[-1] is False,
      "does not flag a varying tail")

print("parse_ppm_header / crop_sha256 (GB-046-V: clock excluded from freeze)")
def make_ppm(w, h, px):   # px: list of (x,y) or None-ish -> simple fn
    # raster = a background value with a marker pixel set as given
    bg = b"\x20\x30\x40"
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            c = list(bg)
            if (x, y) in px:
                c = [0xFF, 0x00, 0x00]
            row += bytes(c)
        rows.append(bytes(row))
    header = b"P6\n%d %d\n255\n" % (w, h)
    return header + b"".join(rows)

ppm_a = make_ppm(8, 8, {(2, 2)})                 # marker inside playfield
ppm_b = make_ppm(8, 8, {(7, 7)})                 # marker OUTSIDE playfield (top-right-ish)
ppm_c = make_ppm(8, 8, {(2, 2)})                 # identical to ppm_a
w_po, h_po, off_po = parse_ppm_header(ppm_a)
check((w_po, h_po) == (8, 8) and off_po is not None and off_po > 0,
      "PPM header parse: 8x8, raster offset > 0")
ck = (1, 1, 4, 4)                                 # playfield crop
check(crop_sha256(ppm_a, 8, 8, off_po, *ck) == crop_sha256(ppm_c, 8, 8, off_po, *ck),
      "identical playfields -> identical crop hash")
check(crop_sha256(ppm_a, 8, 8, off_po, *ck) != crop_sha256(ppm_b, 8, 8, off_po, *ck),
      "a change INSIDE the playfield changes the crop hash")
# marker (7,7) is outside the crop (1,1)-(4,4); the crop hash must NOT see it
check(crop_sha256(make_ppm(8, 8, {(7, 7)}), 8, 8, off_po, *ck) ==
      crop_sha256(make_ppm(8, 8, set()), 8, 8, off_po, *ck),
      "a change OUTSIDE the playfield (the desktop clock) does NOT change the "
      "crop hash — a frozen game + ticking clock reads frozen")
check(crop_sha256(ppm_a, 8, 8, off_po, 0, 0, 0, 0) ==
      crop_sha256(ppm_a, 8, 8, off_po, 5, 5, 4, 4),
      "empty/out-of-range crop -> deterministic digest")
wo, ho, oo = parse_ppm_header(b"not a ppm")
check(wo is None, "non-PPM returns no header dims")

print("rss_floor_peak_growth (GB-046-V: WHOLE-RUN floor->peak)")
fp, pk, gr, okt = rss_floor_peak_growth([1000, 1100, 1200, 1150], 64)
check(okt is True and fp == 0.98 and pk == 1.17 and round(gr, 2) == 0.20,
      "whole-run floor 0.98MB peak 1.17MB growth 0.20MB ok under 64MB")
fp2, pk2, gr2, ok2 = rss_floor_peak_growth([1000, 3000, 6000], 1)
check(ok2 is False and gr2 == 4.88,
      "floor->peak 5000KB (4.88MB) growth fails a 1MB bound")
fp3, pk3, gr3, ok3 = rss_floor_peak_growth([], 64)
check(ok3 is True and gr3 == 0.0, "no samples -> pass (nothing measured)")

print("launch_seen (GB-046-V: a no-op launch is a FAIL, not a pass)")
check(launch_seen("boot...\n[LAUNCH] XNIBBLER.BIN\nmore", "XNIBBLER.BIN"),
      "sees a real [LAUNCH] <name> in the new serial bytes")
check(launch_seen("boot...\n[LAUNCH] /GAMES/NES.BIN\n", "NES.BIN"),
      "sees a /GAMES-path launch in the new serial bytes")
check(launch_seen("boot...nothing happened...", "XNIBBLER.BIN") is False,
      "missing launch -> False (no-op visit must FAIL)")

print("launched_name (GB-046: a wrong-title launch is not a no-op)")
check(launched_name("a\n[LAUNCH] XNIBBLER.BIN\n") == "XNIBBLER.BIN",
      "reports the last launched name")
check(launched_name("a\n[LAUNCH] XNIBBLER.BIN\n[LAUNCH] MILLIPED.BIN\n") == "MILLIPED.BIN",
      "reports the LATEST launch when several appear")
check(launched_name("a\n[LAUNCH] /GAMES/NES.BIN\n") == "NES.BIN",
      "strips the /GAMES/ prefix")
check(launched_name("nothing happened") is None,
      "no launch -> None")

print("launch_retry_needed (bounded retry; persistent no-op still FAILs)")
check(launch_retry_needed(False, 0, 3) is True,
      "first attempt missed -> keep retrying (0 of 3 used)")
check(launch_retry_needed(False, 1, 3) is True,
      "second attempt missed -> keep retrying (1 of 3 used)")
check(launch_retry_needed(False, 2, 3) is False,
      "last attempt (2 of 3) exhausted -> stop, must FAIL")
check(launch_retry_needed(True, 0, 3) is False,
      "launch seen -> stop, no retry")
check(launch_retry_needed(False, 0, 1) is False,
      "single-attempt config -> no retry after a miss")

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
