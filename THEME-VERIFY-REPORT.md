# THEME-VERIFY-REPORT (Lane V)

Verification lane for the HobbyOS **"Chrome light"** re-theme.
Branch: `theme/verify-tools` (based on `main` @ 722e3b4 — this report's
first checkpoint covers the **dark-baseline** phase; the light receipt
follows after merging `main`).

## 1. Deliverables

| # | Deliverable | Status |
|---|-------------|--------|
| 1 | `tools/theme_check.py` — PPM palette checker (dark/light) | done |
| 2 | Dark-baseline capture + both-mode outputs | done |
| 3 | Repo-wide color-literal audit table | done |
| 4 | `tools/theme_acceptance.md` — re-run recipe | done |
| 5 | `THEME-VERIFY-REPORT.md` (this file) | done |

Also created: `tools/theme_shot.py` (QMP runner, PID-scoped kill),
`tools/theme_audit.py` (audit collector helper).

## 2. Dark-baseline receipt (pre-change)

- Screenshot: `/home/sarah/hobbyos-theme/evidence/baseline_dark.ppm` (1920x1080, P6).
- Runner log: `/home/sarah/hobbyos-theme/evidence/theme_shot_baseline.log`;
  serial: `/home/sarah/hobbyos-theme/evidence/theme_shot_serial.log`.
- Marker used: `SCREENSHOT_READY` (MODE=desktop_test boots **EDITOR_T.BIN**,
  which auto-launches NETTEST.BIN → EDITOR.BIN → opens the File dialog and
  prints `SCREENSHOT_READY`; the screen shows the tiled editor window with
  the file dialog open).
- Build: `make MODE=desktop_test hobbyos.elf disk.img` (exit 0; log at
  `/home/sarah/hobbyos-theme/evidence/build_desktop_test.log`).
- QEMU hygiene: pre-existing qemu processes checked (none); the runner
  records its own QEMU PID (`pgrep` + `/proc/<pid>/cwd` == worktree) and
  kills only that PID. No broad `pkill -f qemu-system*` was ever issued.

### Discrimination proof (saved verbatim)

`/home/sarah/hobbyos-theme/evidence/theme_check_baseline.txt`:

- `theme_check.py baseline_dark.ppm --mode dark` → **VERDICT: ALL CHECKS
  PASS** (exit 0). All 5 sample points, all 8 presence checks, all 3
  absence checks pass.
- `theme_check.py baseline_dark.ppm --mode light` → **VERDICT: CHECK
  FAILURES** (exit 1). Sample points are the old dark colors, white/blue
  presence thresholds fail, and 28 of the old-distinctive absence colors
  are found on screen.

### Calibration notes vs. the brief

- The brief's five sample coordinates were retained — they land on the
  described surfaces (taskbar hairline (960,1054), taskbar field (960,1064),
  title face (1500,8), fold (1500,17), content (960,200)).
- (960,1064) sits on a **taskbar gradient blend row**: dark value is
  (41,46,63) (the gradient interpolates (48,54,74)→(30,33,46) per row; the
  captured endpoint row is (48,54,73), off-by-one from the source literal —
  documented, not a defect).
- The dark palette list is **empirical**: exact-color histogram of the
  baseline, keeping every color with real count ≥ 100 that belongs to the
  old palette (39 colors: content bg, hairline/accent, frame outline
  (57,99,153), menu bar (206,208,214), fold, buttons, shadows, text, apps
  button, clock, close, plus 23 taskbar gradient rows). White (255,255,255)
  is excluded (exists in both themes). Source-literal colors with zero
  pixels in this capture state ((16,20,38) wallpaper, inactive-window
  surfaces, cursor colors) are retained only for the light-absence check.
- Wallpaper: with the full-screen editor window + file dialog at
  SCREENSHOT_READY, wallpaper colors are **not visible** (zero pixels) —
  the wallpaper presence check is therefore not exercised by the dark
  receipt (its conversion is lane D's; verified at source + desktop_pixel_test).

## 3. Repo-wide color audit (no conversions made)

Scope: `src/user/*.c`, `src/user/graphics/*`, `src/user/x11/**`,
`src/user/nano/**`, `src/user/pong.c`, `src/user/millipede.c`,
`src/host/*_test.c`. Raw collector output:
`/home/sarah/hobbyos-theme/evidence/audit_raw.txt` (305 colored-line hits,
of which the non-color hex-constant noise is classified below).

| file:line(s) | color(s) | classification |
|---|---|---|
| src/user/graphics/window.c:76–816 (18 sites) | (64,68,82) border, (96,166,255) focus, (8,10,16) shadow, (72,86,120)/(58,108,188)/(44,54,80) title face/fold, (235,238,245)/(255,240,240) glyphs, (178,54,54)/(230,120,120) close, (206,208,214)/(168,170,176) menubar, (20,20,24)/(255,255,255) ink, (250,250,252)/(10,10,12) cursor | **converted by lane W** (window.c) |
| src/user/desktop.c:956–1175 (23 sites) | (16,18,30) content, (96,166,255) hairline/selected, (48,54,74)/(30,33,46) taskbar grad, (52,58,78)/(90,96,116)/(226,230,240) window buttons, (220,226,240) clock, (64,74,100)/(140,150,180)/(240,242,248) Apps btn, (200,200,200)/menu, (16,20,38)/(44,56,96) wallpaper | **converted by lane D** (desktop.c) |
| src/user/pong.c:28–237 | black/white paddles+ball, (80,80,80) dash, (0,255,0)/(255,50,50) winner | **intentional art (game) — keep** |
| src/user/millipede.c:42–606 | green/amber/orange snake, mushroom browns, black bg | **intentional art (game) — keep** |
| src/user/x11/xlib_display.c:40–73, xlib_draw.c:221 | default FG white 0x00FFFFFF, bg 0x000000 | **intentional art (X11 port) — keep** |
| src/user/x11/apps/antfarm/main.c:102–105 | 0x00F2EFE9 air, 0x009A7B55 dirt, 0x00E8C87A sand, 0x00B01E1E ants | **intentional art (X11 port) — keep** |
| src/user/x11/apps/xeyes/main.c:82–83 | 0x00F2EFE9 paper, 0x00262B33 ink | **intentional art (X11 port) — keep** |
| src/user/nano/color.c | curses COLOR_WHITE/BLACK/RED/YELLOW + init_pair | **intentional art (nano own terminal palette) — keep** |
| src/user/apps_test.c:504–514 | (96,166,255), (206,208,214), (255,255,255) pixel asserts | **TEST-LOCAL (keep) — expects OLD palette; must be updated when chrome converts** |
| src/user/graphics_test.c:35–102 | arbitrary test RGBs (GRAPHICS.BIN) | **TEST-LOCAL (keep)** |
| src/user/pong_test.c:159–238 | game test pixel asserts | **TEST-LOCAL (keep)** |
| src/host/desktop_pixel_test.c:318,358 | wallpaper grad (16,20,38)/(44,56,96) + taskbar asserts | **TEST-LOCAL (keep) — palette assertion; will need updating for light wallpaper (finding)** |
| src/host/x11_lib_test.c, xcalc_test.c | 0xFF00FF, 0x112233 … X11 test colors | **TEST-LOCAL (keep)** |
| all other first-party apps: files, calc, hex, clock, editor, notes, tasks, unit, sysmon, monitor, console, sh, gui, dialog, filedialog, find, diff, grep, less, … | (none) — zero COLOR( / hex literals (grep-verified) | **WM-inherited (no action)** |
| cksum_gnu.c, md5sum_gnu.c | CRC/MD5 constant tables | **not colors (audit noise) — no action** |
| nc.c, ping.c, net_test.c, net_timeout_test.c, netfix_test.c | IP masks / addresses | **not colors (audit noise) — no action** |
| mem_test.c (0x40000000), notes.c, sqlite_os.c, nano/winio.c (0x110000), host libc/hex/diff/nfs tests | addresses / offsets / unicode bounds / hash vectors | **not colors (audit noise) — no action** |

**Audit verdict:** zero | **NEEDS-THEME** entries — every first-party color
literal is either in lane W's window.c or lane D's desktop.c, intentional
game/X11/nano art, or a test-local assertion. The claim "text apps inherit
the WM palette and carry no colors" is confirmed exhaustively.

## 4. Gates

- [x] `tools/theme_check.py` exists, runs, and discriminates on the baseline (dark PASS / light FAIL, outputs saved).
- [x] Baseline PPM captured + both mode outputs in `evidence/`.
- [x] Audit table complete (no unattributed color literal).
- [x] `tools/` + this report committed on `theme/verify-tools`; evidence stays outside git.
- [x] `cstyle.py` not applied (.py/.md — per brief).
- [x] Never pushed, never merged; deletions none.

## 5. Residual / next

- Light receipt: after `git merge main` (lanes W+D merged @ 0a3d073) →
  rebuild desktop_test → capture light PPM → `--mode light` must PASS.
  Expected finding if a check fails: distinguish coordinate drift (calibrate)
  from genuinely wrong on-screen color (report exact pixel values).
- Post-merge fallout to watch (test-local, kept intentionally):
  `src/user/apps_test.c` and `src/host/desktop_pixel_test.c` assert old
  palette colors and will need their expectations updated to Chrome-light.
