#!/usr/bin/env python3
"""
theme_check.py - Lane V HobbyOS theme verification checker.

Analyzes a QEMU PPM (P6, 1920x1080) framebuffer dump and asserts the
HobbyOS palette in one of two modes:

  python3 tools/theme_check.py <file.ppm> --mode dark    # OLD dark theme
  python3 tools/theme_check.py <file.ppm> --mode light   # NEW Chrome-light

Prints one line per check (PASS/FAIL + details), then a verdict line.
Exit 0 iff ALL checks pass, 1 otherwise.

Check families
  a. Sample points -- exact pixel colors at fixed coordinates (calibrated
     against real captures; see theme_verify report / theme_acceptance.md).
  b. Whole-image presence/absence -- robust to coordinates: exact-color
     pixel counts for the distinctive palette entries.

--dump-samples prints the exact RGB at every sample point (for eyeballing).
"""
import sys
import os
import argparse

SCREEN_W = 1920
SCREEN_H = 1080

# ---------------------------------------------------------------------------
# Palette lists.  The DARK list was derived empirically from the captured
# baseline (evidence/baseline_dark.ppm, exact-color histogram) -- only colors
# with real counts that belong to the old dark palette are kept.  The LIGHT
# list is the committed Chrome-light palette (theme.h).
# ---------------------------------------------------------------------------

# Old dark-theme distinctive colors.  Derived EMPIRICALLY from the captured
# baseline (evidence/baseline_dark.ppm, exact-color histogram, count >= 100).
# The gradient intermediate rows (33,36,50)..(48,54,73) are taskbar-gradient
# blends; the trailing entries ((12,16,34) etc.) come from window.c/desktop.c
# source but had ZERO pixels in this capture state (inactive surfaces) --
# kept for the light-mode absence check, not for dark presence.
DARK_OLD = [
    (16, 18, 30),    # window content background
    (96, 166, 255),  # taskbar/title hairline, focus accent
    (57, 99, 153),   # window frame outline (empirical)
    (206, 208, 214), # focused window menu bar
    (58, 108, 188),  # title-bar fold (focused)
    (52, 58, 78),    # window bar button fill
    (8, 10, 16),     # window shadow / cursor shadow
    (30, 33, 46),    # taskbar gradient bottom endpoint
    (48, 54, 73),    # taskbar gradient top endpoint (interpolated)
    (64, 74, 100),   # Apps button (closed)
    (20, 20, 24),    # menu / menubar text
    (140, 150, 180), # Apps button text
    (178, 54, 54),   # close button fill
    (220, 226, 240), # clock text
    (226, 230, 240), # window bar button text
    (235, 238, 245), # title text
    # taskbar gradient intermediate rows (all present in the capture):
    (47, 53, 72), (46, 52, 71), (46, 51, 70), (45, 50, 69), (44, 49, 68),
    (43, 49, 67), (43, 48, 66), (42, 47, 64), (41, 46, 63), (41, 45, 62),
    (40, 44, 61), (39, 44, 60), (38, 43, 59), (38, 42, 58), (37, 41, 57),
    (36, 40, 55), (36, 39, 54), (35, 39, 53), (34, 38, 52), (33, 37, 51),
    (33, 36, 50), (32, 35, 49), (31, 34, 48),
    # code-derived dark colors with zero pixels in this capture (kept for
    # light-absence robustness only):
    (12, 16, 34), (34, 44, 72), (16, 20, 38), (44, 56, 96), (64, 68, 82),
    (44, 54, 80), (72, 86, 120), (90, 96, 116), (48, 54, 74), (168, 170, 176),
    (230, 120, 120), (255, 240, 240), (250, 250, 252), (10, 10, 12),
    (232, 234, 240),
]

# New Chrome-light distinctive colors (theme.h / Chrome palette).
LIGHT_NEW = [
    (26, 115, 232),  # THEME_ACCENT  #1A73E8
    (25, 103, 210),  # THEME_ACCENT_DEEP  #1967D2
    (232, 240, 254), # THEME_ACCENT_SOFT #E8F0FE
    (210, 227, 252), # THEME_ACCENT_HOVER #D2E3FC
    (222, 225, 230), # THEME_TABSTRIP  #DEE1E6
    (248, 249, 250), # THEME_BG / wallpaper top  #F8F9FA
    (241, 243, 244), # THEME_SURFACE_1 #F1F3F4
    (232, 234, 237), # THEME_SURFACE_2 #E8EAED
    (189, 193, 198), # THEME_OUTLINE_DARK #BDC1C6
    (217, 48, 37),   # THEME_RED #D93025
    (252, 232, 230), # THEME_RED_SOFT #FCE8E6
    (246, 174, 169), # THEME_RED_EDGE #F6AEA9
    (249, 171, 0),   # THEME_GOLD #F9AB00
]

# ---------------------------------------------------------------------------
# Sample points per mode: (x, y, label, expected_rgb).
# Dark expectations are the surfaces that actually render at 722e3b4
# (bare desktop: wallpaper + taskbar; the auto-launch block is empty, so no
# window tiles -- coordinates land on described surfaces where present).
# ---------------------------------------------------------------------------
SAMPLE_POINTS = {
    "dark": [
        (960, 1054, "taskbar top hairline", (96, 166, 255)),
        (960, 1064, "taskbar field (gradient row)", (41, 46, 63)),
        (1500, 8,   "title bar face", (96, 166, 255)),
        (1500, 17,  "title bar fold", (58, 108, 188)),
        (960, 200,  "window content", (16, 18, 30)),
    ],
    "light": [
        (960, 1054, "taskbar top hairline", (218, 220, 224)),  # #DADCE0
        (960, 1064, "taskbar field", (255, 255, 255)),          # #FFFFFF
        (1500, 8,   "title bar face", (255, 255, 255)),
        (1500, 17,  "title bar fold", (26, 115, 232)),          # #1A73E8
        (960, 200,  "window content", (255, 255, 255)),
    ],
}

# Presence/absence thresholds per mode: (rgb, min_count).
PRESENCE = {
    "dark": [
        ((16, 18, 30), 500000),
        ((96, 166, 255), 10000),
        ((206, 208, 214), 5000),
        ((57, 99, 153), 1000),
        ((58, 108, 188), 500),
        ((52, 58, 78), 500),
        ((8, 10, 16), 500),
        ((30, 33, 46), 500),
    ],
    "light": [
        ((26, 115, 232), 300),     # #1A73E8 accent
        ((255, 255, 255), 200000), # #FFFFFF surface
        ((218, 220, 224), 2000),   # #DADCE0 hairline
    ],
}
ABSENCE = {
    "dark": [(26, 115, 232), (222, 225, 230), (218, 220, 224)],
    "light": DARK_OLD,   # zero pixels of any old dark distinctive color
}


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 PPM")
    # Parse header tokens (magic, w, h, maxval).
    pos = 0
    toks = []
    while len(toks) < 4:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos >= len(data):
            raise ValueError("truncated PPM header")
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] not in b"\r\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        toks.append(data[start:pos].decode())
    w, h, maxv = int(toks[1]), int(toks[2]), int(toks[3])
    if maxv != 255:
        raise ValueError(f"unsupported maxval {maxv} (want 255)")
    pixels = data[pos + 1:]
    needed = w * h * 3
    if len(pixels) < needed:
        raise ValueError(f"short pixel data: {len(pixels)} < {needed}")
    return w, h, pixels


def get_px(pixels, w, x, y):
    i = (y * w + x) * 3
    return (pixels[i], pixels[i + 1], pixels[i + 2])


def count_color(pixels, w, h, want):
    target = bytes(want)
    count = 0
    step = w * 3
    for y in range(h):
        row = pixels[y * step:(y + 1) * step]
        for x in range(w):
            if row[x * 3] == target[0] and row[x * 3 + 1] == target[1] \
               and row[x * 3 + 2] == target[2]:
                count += 1
    return count


def histogram(pixels, w, h):
    from collections import Counter
    c = Counter()
    step = w * 3
    for y in range(h):
        row = pixels[y * step:(y + 1) * step]
        for x in range(0, len(row), 3):
            c[(row[x], row[x + 1], row[x + 2])] += 1
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ppm")
    ap.add_argument("--mode", required=True, choices=["dark", "light"])
    ap.add_argument("--dump-samples", action="store_true")
    ap.add_argument("--hist-top", type=int, default=0,
                    help="print top-N colors of the exact histogram")
    args = ap.parse_args()

    w, h, pixels = read_ppm(args.ppm)
    print(f"image: {w}x{h} mode={args.mode} file={args.ppm}")
    if w != SCREEN_W or h != SCREEN_H:
        print(f"WARN: expected {SCREEN_W}x{SCREEN_H}, got {w}x{h}")

    if args.hist_top:
        for col, n in histogram(pixels, w, h).most_common(args.hist_top):
            print(f"  {col} x{n}")

    results = []  # (ok, line)

    if args.dump_samples:
        print("-- sample dump --")
        for (x, y, label, _) in SAMPLE_POINTS[args.mode]:
            if 0 <= x < w and 0 <= y < h:
                print(f"  ({x},{y}) {label}: {get_px(pixels, w, x, y)}")

    # (a) sample points
    for (x, y, label, want) in SAMPLE_POINTS[args.mode]:
        if x >= w or y >= h:
            results.append((False, f"FAIL sample {label} ({x},{y}): off-image"))
            continue
        got = get_px(pixels, w, x, y)
        ok = got == want
        results.append((ok,
                        f"{'PASS' if ok else 'FAIL'} sample {label} at ({x},{y}): "
                        f"got {got} want {want}"))

    # (b) presence
    for (rgb, minn) in PRESENCE[args.mode]:
        n = count_color(pixels, w, h, rgb)
        ok = n >= minn
        results.append((ok,
                        f"{'PASS' if ok else 'FAIL'} presence {rgb}: "
                        f"count={n} min={minn}"))

    # (c) absence
    for rgb in ABSENCE[args.mode]:
        n = count_color(pixels, w, h, rgb)
        ok = n == 0
        results.append((ok,
                        f"{'PASS' if ok else 'FAIL'} absence {rgb}: "
                        f"count={n} (want 0)"))

    all_ok = True
    for ok, line in results:
        print(line)
        all_ok = all_ok and ok
    print("VERDICT: " + ("ALL CHECKS PASS" if all_ok else "CHECK FAILURES"))
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
