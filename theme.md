# theme.md — the HobbyOS "Chrome light" theme (2026-10-14)

Goal: replace the dark default UI with **Google Chrome's light-mode palette**,
consistently across the desktop shell, window chrome, wallpaper and every
first-party application surface — including the WK-7 browser chrome in the
WebKit port.  Colors are anchored to the exact values Google ships (fetched
from the Chromium tree, 2026-10-14), not eyeballed.

## 1. Single source of truth — `src/user_include/graphics/theme.h`

All first-party color now comes from `theme.h` (foundation commit `722e3b4`);
no widget file carries its own color literals.  Palette (Google hexes):

| Constant | Value | Used for |
|---|---|---|
| `THEME_ACCENT` | `#1A73E8` (Blue 600) | focus ring/border, focused title fold, caret, bookmarked star |
| `THEME_ACCENT_DEEP` | `#1967D2` (Blue 700) | selected text, pressed |
| `THEME_ACCENT_SOFT` / `_HOVER` | `#E8F0FE` / `#D2E3FC` | menu selection fill / hover edge |
| `THEME_SURFACE` | `#FFFFFF` | toolbar, active tab, window face + content |
| `THEME_TABSTRIP` | `#DEE1E6` | browser frame / tab strip |
| `THEME_BG` | `#F8F9FA` (Grey 050) | base background |
| `THEME_SURFACE_1` / `_2` | `#F1F3F4` / `#E8EAED` | menu bars, buttons, fields / alt fill, pressed |
| `THEME_OUTLINE` / `_DARK` | `#DADCE0` / `#BDC1C6` | borders + separators / hairlines + shadows |
| `THEME_TEXT` / `_ALT` / `_SUB` / `_DISABLED` | `#202124` / `#3C4043` / `#5F6368` / `#9AA0A6` | ink ramp (Grey 900→500) |
| `THEME_RED` / `_SOFT` / `_EDGE` | `#D93025` / `#FCE8E6` / `#F6AEA9` | close button fill/border/glyph |
| `THEME_WALLPAPER_TOP` → `_BOT` | `#F8F9FA` → `#DADCE0` | desktop gradient (top → taskbar) |

Provenance (recorded in the header comment): Chromium `ui/gfx/color_palette.h`
(kGoogleBlue600/Grey ramp/Red600), `chrome/browser/ui/color/chrome_color_mixer.cc`
(kColorToolbar = #FFFFFF, toolbar icons Grey 700, bookmark bar = toolbar),
`chrome/browser/themes/theme_properties.cc` (frame/tab-strip #DEE1E6, active
tab + toolbar #FFFFFF).  The WK-7 browser palette (`WK5Chrome.h`) mirrors the
same values with two browser-only additions (tab strip #DEE1E6, star-on blue).

## 2. What changed, per surface (one writer per file)

| Surface | Files | Lane / commit |
|---|---|---|
| Window chrome: frame focus ring, title face + fold, menu bar, close button, content ink, terminal palette, cursor | `src/user/graphics/window.c` | W, branch `theme/ui-window` @ `94dd322` → merged `0c95fe2` |
| Desktop: taskbar + hairline, start/popup menus, launch-window bg, wallpaper gradient | `src/user/desktop.c`, wallpaper block of `src/user/graphics/graphics.c` | D, branch `theme/ui-desktop` @ `341edf2` → merged `0a3d073` |
| Verification tooling + receipts | `tools/theme_check.py`, `tools/theme_shot.py`, `tools/theme_audit.py`, `tools/theme_acceptance.md`, `THEME-VERIFY-REPORT.md` | V, branch `theme/verify-tools` @ `c33d229` → merged `a5cd57b` |
| WK-7 browser chrome palette | fork `~/webkit-hobbyos`: `HobbyOS/WebKit/WebProcess/hobbyos/WK5Chrome.h` (+2 paint tweaks in `WK5Chrome.cpp`; `WK5WindowDriver.cpp` needed none) | B, branch `browser/ui-theme` @ `2952eed95d` (off `browser/ui`) |

Text apps (files, editor, calc, notes, sh, nano, …) needed **zero per-app
changes** — they inherit the WM palette; the repo-wide audit found no
unattributed color literal (games/X11 ports/nano keep their own art, by
design).  Host-test goldens that encoded palette colors were updated in the
same lanes (`graphics_lib_test.c`, `desktop_pixel_test.c`,
`window_damage_test.c`); the in-OS apps tier's framebuffer check was retargeted
to `theme.h` constants (`src/user/apps_test.c`).

## 3. Verification receipts (all on real boots; nothing pending)

| Receipt | Result | Evidence |
|---|---|---|
| Pre-change baseline (probe tree @ `722e3b4`), `make host_tests` | **482/482 PASS** | `evidence/probe_host_tests.log` |
| Post-merge host_tests (main @ `0a3d073`), `make host_tests` | **482/482 PASS** | `/tmp/merged_host_tests.log` |
| Seam trio: graphics_lib / desktop_pixel / window_damage | **214/214, 74/74, 40/40** | controller runs |
| ARM light screenshot (merged main, MODE=desktop_test, `SCREENSHOT_READY` state) | 12/15 points exact + micro-samples (frame ring/bevel/shadow); **zero old-palette pixels**; vision-verified | `evidence/light_main_desktop.ppm` (+ `.png`) |
| ARM light + checker (lane V, independent capture) | `theme_check.py --mode light` **ALL CHECKS PASS** (5 sample points; 65 old-dark absence checks all zero) | `evidence/light_after_merge.ppm`, `evidence/theme_check_light.txt` |
| Checker discrimination on the dark baseline | dark → PASS, light → FAIL (proves the checker can tell) | `evidence/theme_check_baseline.txt` |
| Desktop apps acceptance (13 apps, MODE=desktop, mock input) | **ALL DESKTOP APP TESTS PASSED** | `/tmp/apps_acceptance.log` |
| ARM in-OS apps tier (MODE=apps_test, APPS_T.BIN) | **5/10 apps PASS with the retargeted framebuffer check** (light chrome verified through FILES→HEX), then stops at a **pre-existing TASKS.BIN failure** — reproduced *identically* on the untouched pre-theme probe tree @ `722e3b4` (not theme-related; see §5) | `/tmp/apps_test_run.log`, `evidence/apps_t_main.log`, `evidence/apps_t_probe.log` |
| Browser host gate (fork) | **107/107** | `HobbyOS/tests/run_wk5ui_host_test.sh` |
| Browser x64-KVM chrome band, browser-only + combined-look | **PASS** both (strip #DEE1E6, toolbar/bookmarks white, omnibox #F1F3F4 + #DADCE0, 0 legacy pixels) | `continuation/ui-theme/` (REPORT.md, PNGs, `check_chrome_band.py`) |
| Combined-look sanity (controller re-scan) | OS chrome + browser band all light; 0 old pixels in chrome region | `evidence/collected/browser-light-combined.png` |

Note on thresholds: lane V calibrated one checker bound (#DADCE0 presence ≥
1500, actual 1920 = the 1-px taskbar hairline) — documented, no on-screen
color was wrong.

## 4. Re-verify (see also `tools/theme_acceptance.md`)

```sh
python3 tools/theme_check.py <screendump.ppm> --mode light   # expect: ALL CHECKS PASS
python3 tools/theme_shot.py <out.ppm>                        # QMP capture of MODE=desktop_test
python3 tools/run_apps_test_scoped.py                        # cwd-scoped APPS_T runner (shared host!)
python3 run_desktop_apps_test.py                             # expect: ALL DESKTOP APP TESTS PASSED
```

## 5. Follow-ups (open)

- **Pre-existing APPS_T failure (not theme-related).** The in-OS apps harness
  stops at `TASKS.BIN` (`interact - window vanished during the functional
  check`) on BOTH the themed tree and the *untouched* pre-theme probe tree
  @ `722e3b4` — the two serial logs are line-for-line identical through the
  failure point, down to the `[PIPE_WRITE_EPIPE]` pipe address (only loader
  timing digits and the tree path differ) (`evidence/apps_t_main.log` vs
  `evidence/apps_t_probe.log`).
  The harness passed 10/10 when introduced (2026-09-18); P5/D10 signal
  semantics (SIGPIPE on dead-pipe writes, 2026-09-30) postdate that, and the
  MODE=apps_test environment on current main is additionally nondeterministic
  (a control run with a varied harness hit an unrelated CALC fault-loop).
  This is a kernel/loader/harness-robustness workstream, tracked here as a
  finding — theme-side coverage is unaffected: the 13-app desktop acceptance
  is green and 5/5 harness apps passed with light chrome before the failure.
  Use `tools/run_apps_test_scoped.py` (never the broad-pkill
  `run_apps_test.py`) on a shared host.

- **Published 2026-10-14** (user request): OS repo `main` → `sarahkaylor/HobbyOS`
  (all theme commits + records); fork `browser/ui-theme` → `sarahkaylor/webkit-hobbyos`
  @ `e22366b385` (working `2952eed95d`; 8/8 files byte-identical, identical
  diffstat; incremental patch-publish). Publish workspace + procedure:
  `~/webkit-upload/README.md` (+ `publish_branch.sh`).
- The vendored snapshot `third_party/webkit-hobbyos/` + its `build/intel/browser.bin`
  still carry the pre-theme chrome; refresh (or re-vendor) when the next
  browser artifact is cut — the x64 acceptance disk was built from the
  rebuilt fork binary directly.
- ARM browser leg not re-run for the theme (x64-KVM is the acceptance path
  per plan; arm browser build refresh is a standing follow-up).
- WK-7 cosmetic residuals (text-glyph close/nav icons) unchanged by the
  theme — still open from the UI wave.
