#!/usr/bin/env python3
"""acceptance_gate_test.py — GB-075 self-test: the acceptance rig must NOT
fabricate PASS.

Proves, on the host (no QEMU), the DoD behaviours by INJECTION — the injection
IS the test:

  1. whole-frame liveness: content in the BOTTOM rows (the old ppm_live only
     sampled the top ~104/1080 rows) is now detected live.
  2. a captured desktop-only (menu) frame FAILS the rig (identity is not
     observed — the title never loaded).
  3. a game that NEVER LOADS FAILS even when the on-screen frame is live (the
     XCRYPT/NES fabricated-PASS mechanism).
  4. games that really run MUST differ over the whole frame by a stated margin:
     two near-identical 'different' captures FAIL the margin (the
     0.15-0.97/255 fabrication), while a genuinely distinct rendered title
     PASSES.

Self-registering: run_t5_tests.py (GB-064) auto-discovers tools/t5/*_test.py,
so this needs no edit to the shared Makefile host_tests line.
"""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from acceptance_run import (ppm_live, ppm_whole_mad, title_loaded,
                            gate_game, GAME_DIFF_MARGIN)

W, H = 1920, 1080
tmp = tempfile.mkdtemp(prefix="gb075-")
fails = []
_serial_no = 0


def write_ppm(name, w, h, white_rows=(), raw=None):
    """Write a P6 PPM.  Either `white_rows` (fully-white rows, rest black) or
    an explicit raw RGB byte string is used."""
    global _serial_no
    _serial_no += 1
    path = os.path.join(tmp, f"{name}-{_serial_no}.ppm")
    data = raw if raw is not None else bytearray(w * h * 3)
    if raw is None:
        for row in white_rows:
            base = row * w * 3
            data[base:base + w * 3] = b"\xff" * (w * 3)
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(data))
    return path


def patched(w, h, px):
    """raw byte string: base all-black, then (x, y, (r,g,b)) writes."""
    raw = bytearray(w * h * 3)
    for (x, y, c) in px:
        b = (y * w + x) * 3
        raw[b:b + 3] = bytes(c)
    return bytes(raw)


def check(name, cond):
    if cond:
        print(f"  PASS: {name}")
    else:
        print(f"  FAIL: {name}")
        fails.append(name)


# ---- 1. whole-frame liveness (bottom-row content) ------------------------
p_bottom = write_ppm("bottom", W, H, white_rows=[1000])
p_blank = write_ppm("blank", W, H)
check("bottom-row content (row 1000) detected live",
      ppm_live(p_bottom) is True)
check("blank 1080-row frame is not live", ppm_live(p_blank) is False)

# ---- 2. desktop-only frame FAILS (identity not observed) -----------------
serial_menu = "boot ok\n[MENU] dump ... STARDUST.BIN listed ...\n"  # menu only
ok, reasons = gate_game(serial_menu, "STARDUST.BIN", p_bottom)
check("desktop/menu-only frame FAILS the rig (title never loaded)", ok is False)
check("and the reason names 'never loaded'",
      any("never loaded" in r for r in reasons))

# ---- 3. a game that NEVER LOADS FAILS ------------------------------------
serial_no_load = "boot ok\n[MENU] dump: PONG MILLIPED XNIBBLER\n"
ok2, reasons2 = gate_game(serial_no_load, "XCRYPT.BIN", p_bottom)
check("never-loaded title FAILS even with a live frame", ok2 is False)
serial_loaded = ("... Loading program for scheduler (v2 AS): STARDUST.BIN ...\n"
                 "v2 loader: image mapped ...\n")
check("title_loaded True when serial shows the load line",
      title_loaded(serial_loaded, "STARDUST.BIN") is True)

# ---- 4. distinct rendered titles must differ over the whole frame --------
LOADED = "... Loading program for scheduler (v2 AS): GAME.BIN ...\n"
# a: one white pixel at top-left (live), b: near-identical a (one channel +1),
# c: clearly different (a solid band) -> live and far from a.
a = write_ppm("a", 64, 64, raw=patched(64, 64, [(0, 0, (255, 255, 255))]))
b = write_ppm("b", 64, 64, raw=patched(64, 64, [(0, 0, (255, 255, 254))]))
c = write_ppm("c", 64, 64, raw=patched(64, 64,
                                       [(x, 32, (0, 0, 255)) for x in range(64)]))

check("frame a is live (a distinct white pixel)", ppm_live(a) is True)
check("frame c is live (a blue band)", ppm_live(c) is True)
check("a vs c differ above the stated margin",
      ppm_whole_mad(a, c) >= GAME_DIFF_MARGIN)

okA, _ = gate_game(LOADED, "GAME.BIN", a, None)
check("first real, loaded, live title PASSES", okA is True)
okB, rB = gate_game(LOADED, "GAME.BIN", b, a)      # b ~ a (near-identical)
check("near-identical 'different' title FAILS the whole-frame margin",
      okB is False and any("changed only" in r or "distinct" in r for r in rB))
okC, rC = gate_game(LOADED, "GAME.BIN", c, a)      # c clearly distinct
check("genuinely distinct second title PASSES", okC is True)

print()
if fails:
    print(f"ACCEPTANCE_GATE_TEST: FAIL — {len(fails)} failed")
    sys.exit(1)
print("ACCEPTANCE_GATE_TEST: PASS — the acceptance rig no longer fabricates PASS")
sys.exit(0)
