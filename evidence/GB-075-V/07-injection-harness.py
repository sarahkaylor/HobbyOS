#!/usr/bin/env python3
"""Independent injection harness for GB-075-V (not the ticket's own self-test).

Feeds the rig's public functions deliberately-wrong evidence and records
whether they fail, and with what reason.  Run from the repo root at badc665.
"""
import os, sys, tempfile

sys.path.insert(0, os.path.join(os.getcwd(), "tools", "t5"))
from acceptance_run import (ppm_live, ppm_whole_mad, title_loaded, gate_game,
                            GAME_DIFF_MARGIN)

W, H = 1920, 1080
tmp = tempfile.mkdtemp(prefix="gb075v-ind-")


def ppm(name, white_blocks=(), white_rows=()):
    raw = bytearray(W * H * 3)
    for (x0, y0, size) in white_blocks:
        for y in range(y0, min(y0 + size, H)):
            for x in range(x0, min(x0 + size, W)):
                b = (y * W + x) * 3
                raw[b:b + 3] = b"\xff\xff\xff"
    for row in white_rows:
        base = row * W * 3
        raw[base:base + W * 3] = b"\x80\x80\x80" * W
    p = os.path.join(tmp, name + ".ppm")
    open(p, "wb").write(b"P6\n%d %d\n255\n" % (W, H) + bytes(raw))
    return p


results = []


def record(label, ok_expected, got, detail=""):
    verdict = "OK" if got == ok_expected else "WRONG"
    results.append((verdict, label, got, detail))
    print(f"  {verdict}: {label} -> {got}  {detail}")


print("== clause 3: whole-frame liveness (the GB-053-V sampling bug) ==")
for row in (5, 540, 1000):
    p = ppm(f"block50-row{row}", white_blocks=[(W // 2, row, 50)])
    live = ppm_live(p)
    record(f"50x50 block at row {row} reads live", True, live)

print("\n== clause 1: a desktop-only (live, non-uniform) frame must FAIL ==")
desk = ppm("desktop", white_rows=[10, 11, 500, 1000])   # live everywhere
serial_desktop = "Loading program for scheduler (v2 AS): DESKTOP.BIN\n[GPU] irq handled\n"
ok, reasons = gate_game(serial_desktop, "XNIBBLER.BIN", desk)
record("desktop-only frame for XNIBBLER fails", False, ok, f"reasons={reasons}")

print("\n== clause 2: staged + menu-listed but never loaded must FAIL ==")
serial_listed = ("mcopy -i disk.img obj/arm/xcrypt.bin ::/XCRYPT.BIN\n"
                 "[MENU] 20=XCRYPT.BIN\n[GPU] irq handled\n")
ok, reasons = gate_game(serial_listed, "XCRYPT.BIN", desk)
record("XCRYPT staged+listed, never loaded, fails", False, ok, f"reasons={reasons}")

print("\n== control: same frame, but the title really loaded -> PASS ==")
serial_loaded = "Loading program for scheduler (v2 AS): XCRYPT.BIN\n"
ok, reasons = gate_game(serial_loaded, "XCRYPT.BIN", desk)
record("loaded + live frame passes (gate is not stuck red)", True, ok, f"reasons={reasons}")

print("\n== margin: near-identical frames FAIL, distinct frames PASS ==")
a = ppm("a", white_blocks=[(W // 2, 540, 50)])
b = ppm("b", white_blocks=[(W // 2, 540, 50), (0, 0, 2)])   # 2x2 px differs
c = ppm("c", white_blocks=[(x, y, 120) for y in range(0, H, 120) for x in range(0, W, 240)])
serial_nes = "Loading program for scheduler (v2 AS): NES.BIN\n"
mad_ab = ppm_whole_mad(a, b)
mad_ac = ppm_whole_mad(a, c)
ok_ab, r_ab = gate_game(serial_nes, "NES.BIN", b, prev_ppm=a)
ok_ac, r_ac = gate_game(serial_nes, "NES.BIN", c, prev_ppm=a)
record(f"near-identical (mad={mad_ab:.5f}) fails margin {GAME_DIFF_MARGIN}", False, ok_ab, f"reasons={r_ab}")
record(f"distinct (mad={mad_ac:.5f}) passes margin", True, ok_ac, f"reasons={r_ac}")

bad = [r for r in results if r[0] != "OK"]
print(f"\nINJECTION RESULT: {len(results)-len(bad)}/{len(results)} behaved as required")
sys.exit(1 if bad else 0)
