---
name: hobbyos-desktop-compositor
description: "Use when touching HobbyOS's desktop/WM rendering (desktop.c, window.c, graphics.c): the damage-driven repaint model, the app-output drain, the flush contract, and how to measure a frame."
version: 1.0.0
author: Hermes Agent
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, desktop, compositor, window-manager, damage, rendering, performance]
    related_skills: [hobbyos-gui-test, hobbyos-build-and-run, hobbyos-kernel-constraints, hobbyos-run-tests, hobbyos-add-userland-program]
---

# HobbyOS: Desktop Compositor & Damage Model

## Overview

The desktop (`src/user/desktop.c`) is a **damage-driven compositor**: each
frame repaints only what changed since the previous frame, instead of
repainting the whole screen. Windows (`src/user/graphics/window.c`) keep
bookkeeping of what their framebuffer region shows and repair themselves at
**line granularity**; the graphics library (`src/user/graphics/graphics.c`)
confines every draw call to a *base clip* (the region being repainted).
`flush_fb()` (kernel, `SYS_FLUSH_FB`) still pushes the whole framebuffer to
the device - once per frame.

This model is what makes a full-screen `\f + print()` app (FILES, CALC, ...)
cheap to drive: the app keeps its "rewrite the whole screen" convention, and
the WM turns it into a couple of repainted text rows.

## When to Use

- Changing how the desktop, window chrome, taskbar, menus or the cursor is
  painted; anything that decides *when* the screen updates.
- A GUI app feels slow, flickers, or leaves stale pixels behind.
- Measuring keyboard-to-screen latency on the desktop.

Don't use for: driving/testing the GUI (→ [[hobbyos-gui-test]]), app code
itself (→ [[hobbyos-add-userland-program]]), kernel rules (→
[[hobbyos-kernel-constraints]]).

## The Three Layers

| Layers | File(s) | Job |
|---|---|---|
| Compositor | `desktop.c` | chrome diff → damage rects; window-text damage pass; frame loop |
| Window painter | `graphics/window.c` | full paint (`wm_draw_windows`) + row repair (`wm_draw_window_rows`) |
| Primitives | `graphics/graphics.c` | base clip, current clip, paint counters (`-DPAINT_STATS`) |

## The Rules (keep these invariants when editing)

1. **Windows are repaired at line granularity before rect passes.**
   `wm_draw_window_rows()` compares the window's live text against
   `rendered_text/rendered_rows/rendered_skip` and repaints only the rows
   that differ. `wm_draw_windows()` paints a window whole **and updates that
   bookkeeping** - so it must run only when the window's pixels really get
   painted (a full frame or a rect pass whose rect covers it), or the
   bookkeeping lies. The frame loop therefore always runs the row pass
   *before* the rectangle passes.
2. **The base clip confines every draw.** `graphics_set_base_clip(rect)`
   makes `set_clip` intersect with it and `reset_clip` restore *it* (not the
   full screen). A damage-rect pass = set base clip, call `paint_scene()`,
   repeat. Never leave a base clip set across frames.
3. **`desktop_damage()` is the only place that decides rects.** It diffs two
   `struct desktop_chrome` snapshots (cursor, focus, menus, clock,
   per-window `chrome_dirty`). Returns `-1` (or ≥ DMG_MAX rects) → full
   scene repaint. Window count changes are always full repaints (the tiling
   moved everything). Keep the rect helpers in sync with the drawing code.
4. **`chrome_dirty` marks title/menu changes.** `wm_set_window_title()` and
   the `ESC ] M` handler set it; the frame loop consumes and clears it.
5. **Every frame must still call `graphics_flush()`.** The APPS_T in-OS
   harness (and `editor_test_host`) hang off `flush_fb` as the frame
   boundary; skipping the flush when damage is empty breaks them. Empty
   damage = no repaint, but still a flush.
6. **Drain each window's output per frame.** The frame loop reads a window's
   pipe until it goes quiet (bounded: 4096 B, then `yield()` up to 6 rounds
   for a writer between two writes). This is what turns one app update into
   one frame. Do not go back to a small per-iteration read cap.

## The Numbers (regression baseline)

Measured by the APPS_T harness line `[APPS_T] FILES down-arrow: ...` (one
Down arrow in FILES, real QEMU/ARM):

| | before | after |
|---|---|---|
| desktop frames carrying a visible change | 25 | 1 |
| framebuffer pixels painted for the keypress | ~69,600,000 | ~21,000 |
| ms from key to settled screen | ~62 | ~7 |

The pixel count includes only *changed* work; a full desktop frame is
~2.3M px (wallpaper 760K + window chrome/background ~1.5M + taskbar), so
the old path repainted the whole screen tens of times per keypress. Any
change that pushes `painting frames` above ~2 or the pixel count above
~100K has re-broken the model.

## Measuring

- **APPS_T responsiveness line** (above) via `python3 run_apps_test.py`.
- **Host regression tests**: `window_damage_test_host` (row repair + base
  clip, pixel-level: markers must survive in untouched rows, and the repair
  must equal a full repaint pixel-for-pixel) and `desktop_damage_test_host`
  (the chrome diff: rect lists per change). Both run in `make host_tests`.
- **Real-screen evidence**: QMP screendump diffs (`run_console_test.py`
  pattern) plus the FILES nav check - the only way to see artifacts that
  text-level host tests cannot.

## Common Pitfalls

1. **A small per-iteration read cap** (`read(fd, buf, 63)`) turned one
   app update into ~25 frames - each with a full-screen repaint. Drain
   instead.
2. **A lone `\f` frame.** `gui_clear()` is its own `print()` and can arrive
   a scheduler-quantum before the screen text. Without the drain's settle
   rounds the compositor paints the cleared screen first (all rows blank,
   ~224K px) and then the real screen. If you see ~2x the expected pixels
   or a blank flash, the settle is the knob.
3. **Window geometry change without a full repaint.** `update_layout()` only
   runs on window count changes, which are full repaints; do not move or
   resize a window from anywhere else without `wm_window_invalidate()` or a
   full frame.
4. **Games draw the framebuffer directly.** They are unaffected by the
   damage model (their window text never changes); fewer desktop repaints
   actually keeps their pixels on screen longer. Game exit = window removal
   = full repaint.
5. **Cursor re-stamping.** Any pass that repaints rows a pointer sits on
   must re-stamp the cursor (`wm_draw_cursor`) or the sprite gets erased.
6. **Host pixel tests must reset the base clip** (`graphics_reset_base_clip`)
   between cases or everything draws into the previous test's clip.

## Verification Checklist

- [ ] `make host_tests` (window_damage + desktop_damage + apps_suite + ...) green.
- [ ] `python3 run_apps_test.py`: APPS TEST PASSED and the FILES down-arrow
      line shows ~1 painting frame / tens of thousands of pixels.
- [ ] `python3 run_console_test.py` and `run_desktop_apps_test.py` still pass
      (framebuffer diffs and 10-app acceptance).
- [ ] Screenshots before/after a keypress: only the expected rows changed,
      no stale pixels and no blank flash.
- [ ] If a user binary grew: `llvm-size obj/arm/*.elf` stays under the
      loader cap (see [[hobbyos-kernel-constraints]]).
