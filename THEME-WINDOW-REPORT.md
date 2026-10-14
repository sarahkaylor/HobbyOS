# THEME-WINDOW-REPORT — window.c → Chrome-light palette

Lane W (window chrome). Branch `theme/ui-window`, base `main` @ `722e3b4`
(shared palette `src/user_include/graphics/theme.h`).

## Conversion (before → after)

File: `src/user/graphics/window.c` (added `#include "theme.h"`).

| Where | Old | New |
|---|---|---|
| `wm_create_window` inactive border default (~L76) | `COLOR(64, 68, 82)` | `THEME_OUTLINE` |
| frame border focused (`wm_draw_windows` L289) | `COLOR(96, 166, 255)` | `THEME_ACCENT` (inactive keeps `win->border_color`) |
| drop shadow bands (L308) | `COLOR(8, 10, 16)` | `THEME_OUTLINE_DARK` |
| title bar face focused / inactive (L316) | `COLOR(96, 166, 255)` / `COLOR(72, 86, 120)` | `THEME_SURFACE` / `THEME_SURFACE_1` |
| title bar fold focused / inactive (L317) | `COLOR(58, 108, 188)` / `COLOR(44, 54, 80)` | `THEME_ACCENT` / `THEME_OUTLINE` |
| title text (L325, uses `focused` flag) | `COLOR(235, 238, 245)` | focused `THEME_TEXT`, inactive `THEME_TEXT_SUB` |
| close button fill (L330) | `COLOR(178, 54, 54)` | `THEME_RED_SOFT` |
| close button outline (L331) | `COLOR(230, 120, 120)` | `THEME_RED_EDGE` |
| close glyph `'x'` (L332) | `COLOR(255, 240, 240)` | `THEME_RED` |
| menu bar focused / inactive (L335) | `COLOR(206, 208, 214)` / `COLOR(168, 170, 176)` | `THEME_SURFACE` / `THEME_SURFACE_1` |
| menu item text (L339) | `COLOR(20, 20, 24)` | `THEME_TEXT` |
| content ink row repair `window_paint_rows` (L222) | `COLOR(255, 255, 255)` | `THEME_TEXT` |
| content ink full paint (L375) | `COLOR(255, 255, 255)` | `THEME_TEXT` |
| terminal-mode ink (L742) | `COLOR(255, 255, 255)` | `THEME_TEXT` |
| terminal reverse-video field (L745) | `COLOR(235, 238, 245)` | `THEME_TEXT` (dark field; reverse ink stays `win->bg_color` — swap mechanism untouched) |
| terminal caret bar (L764) | `COLOR(96, 166, 255)` | `THEME_ACCENT` |
| cursor outline 8 stamps (L815) | `COLOR(10, 10, 12)` | `THEME_SURFACE` |
| cursor fill (L818) | `COLOR(250, 250, 252)` | `THEME_TEXT` |

Invariants preserved: damage bookkeeping (`rendered_text/rendered_rows/
rendered_skip`, `row_damage`, row-granularity repair, base-clip usage),
geometry/line heights, terminal grid semantics, all signatures and struct
fields. Only colors (and the two state-dependent text colors) changed.
Comments describing the old colors were updated to stay truthful.

`grep -n 'COLOR(' src/user/graphics/window.c` → no matches.

## Gates

### 1. cstyle
```
$ python3 tools/cstyle.py format src/user/graphics/window.c && python3 tools/cstyle.py check src/user/graphics/window.c
cstyle: 0 file(s) updated, 1 checked
cstyle: all 1 file(s) are properly formatted
```
(exit 0; formatting was a no-op — file already conformant)

### 2. Both-arch compile
```
$ make obj/arm/user_window.o
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -c src/user/graphics/window.c -o obj/arm/user_window.o   # exit 0, no warnings

$ make ARCH=intel obj/intel/user_window.o
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -nostdinc -isystem /usr/lib/llvm-21/lib/clang/21/include -mcmodel=large -c src/user/graphics/window.c -o obj/intel/user_window.o   # exit 0, no warnings
```

### 3. `make host_tests` — full gate (~70 binaries)

Result: **GREEN** (`HOST_TESTS_RC=0`).

Failures found and fixed during the gate (all host-test palette goldens that
hard-coded a color my conversion changed):

1. `desktop_pixel_test_host` — 2/74 failed (`pointer sprite re-stamped over
   the content`, `the pointer still re-stamps over the content`): asserted
   the OLD cursor fill `C(250, 250, 252)`. Fixed (commit `988e2a0`).
2. `graphics_lib_test_host` — 34 window-chrome goldens failed (title bar
   face/fold, frame border, menu bar, shadow edges, close button, tiled
   pair). Fixed (commit `3906b2a`).
3. `window_damage_test_host` — 3/11 failed (`changed row shows the new text
   glyphs`, `third line was drawn`, `remaining lines still drawn`): counted
   pixels of the OLD content ink `C(255, 255, 255)`. Fixed (commit
   `8e12206`).

Final full run: no failures, apps suite reports `Checks run: 482, Checks
failed: 0`, `ALL APPS SUITE TESTS PASSED`, `TEST EXIT: 0`.

### 4. Direct runs
```
$ ./window_damage_test_host
=== Results: 11 run, 0 failed ===
=== Checks: 40 individual pixel/logic checks ===
ALL WINDOW DAMAGE TESTS PASSED        (run-rc 0)

$ ./window_text_test_host
=== Results: 5 run, 0 failed ===
=== Checks: 19 individual capture checks ===
ALL WINDOW TEXT TESTS PASSED          (run-rc 0)
```

## Bytes: every `COLOR(` removed, per the brief's map

`grep -n 'COLOR(' src/user/graphics/window.c` → no matches (verified before
and after all gates; `grep` exit 1).

## src/host files touched (list per controller rule)

1. `src/host/desktop_pixel_test.c` — 2 cursor-fill goldens
   (`== C(250, 250, 252)` → `== (uint32_t)THEME_TEXT`) + theme.h include.
   <-- NOTE for the controller: this file was flagged as lane D's; the two
   edited checks are CURSOR (window.c) goldens that hard-coded MY old cursor
   fill color, not wallpaper, and they fail on my change without this edit
   (I verified: they are `wm_draw_cursor` re-stamp pixels at lines 404/642,
   far from the wallpaper-carve tests). No wallpaper/carve region touched.
   If the controller prefers, these two lines can be re-merged from my
   commit `988e2a0`.
2. `src/host/graphics_lib_test.c` — window-chrome goldens only (functions
   `test_window_tiling_and_titlebar` .. `test_window_tiled_pair`, file lines
   ~456-580) + theme.h include + `TP()` cast helper. Wallpaper golden blocks
   (lines ~300-390, `C(12,16,34)`/`C(34,44,72)`/`C(14,18,37)` …) are
   UNTOUCHED (that is lane D's region). CHECK description strings updated to
   the new colours.
3. `src/host/window_damage_test.c` — content-ink golden macro
   `TEXT_WHITE C(255,255,255)` → `TEXT_INK ((uint32_t)THEME_TEXT)` (3 uses)
   + theme.h include.

No other src/host file needed a change.

## Surprising / notes

- `graphics_lib_test.c` line ~505 (`frame inner bevel sits next to the
  shadow`) checks a DERIVED shade, not a palette literal: the 1px inner
  bevel of `graphics_draw_rect_outline` is `shade_color(border, 3, 5)`. With
  the focused border now `THEME_ACCENT` (26,115,232) the bevel is
  `C(15, 69, 139)` (was `C(57,99,153)` from the old blue). Computed, not
  guessed; comment added next to the check.
- Any host test that draws with its own colour argument (the icon-in-window-
  text test's `wm_draw_text(..., C(255,255,255))`, the X11 apps' white)
  was left alone — those are caller-supplied colours, not window.c palette.
- `theme.h` includes `graphics.h`; include order
  (`window.h, graphics.h, libc.h, theme.h, font.h`) is clean on both arches.
- Both arches compile with zero warnings from window.c; the one remaining
  host-test warning (`unused variable 'cw'` in desktop_pixel_test.c) is
  pre-existing and unrelated.
