---
name: hobbyos-desktop-app
description: "Use when implementing a HobbyOS desktop app + host tests."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, gui-app, host-tests, compat-mocks, freestanding]
    related_skills: [hobbyos-add-userland-program, hobbyos-run-tests, hobbyos-build-and-run]
---

# HobbyOS: Desktop GUI App from Stub + Host Tests

## Overview

A HobbyOS windowed app lives in `src/user/<app>.c` (stub) with tests in `src/host/<app>_test.c`. The Makefile already compiles `<app>.bin` onto the FAT disk and links `<app>_test_host`; you only replace the two files. Multiple agents often share the tree: never run `make` or `git`; compile single files by hand into `/tmp/agentobj/`.

## Exact commands (run from the repo root)

ARM compile check (must be ZERO warnings):
```
clang -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -c src/user/<app>.c -o /tmp/agentobj/<app>.o
```
Host test (prebuilt mock objects: compat.o gui.o dialog.o filedialog.o in /tmp/agentobj):
```
clang -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -DHOST_TEST -c src/host/<app>_test.c -o /tmp/agentobj/<app>_test.o
clang -o /tmp/agentobj/<app>_test /tmp/agentobj/<app>_test.o /tmp/agentobj/compat.o /tmp/agentobj/gui.o /tmp/agentobj/dialog.o /tmp/agentobj/filedialog.o
timeout 20 /tmp/agentobj/<app>_test
```

## Non-negotiables

- Keep the stub entry pattern exactly: `#ifdef HOST_TEST / int main(void) { / #else / __attribute__((section(".text._start"))) / void _start(void) { / #endif`.
- Startup marker via `print_console("[APP] NAME started\n")` — the acceptance test greps for it.
- Test file: `#define main <app>_app_main` then `#include "../user/<app>.c"` (rename before include). Never redefine symbols compat.c provides (link error).
- Freestanding aarch64: no string.h/stdio/math, no floats, only libc.h/gui.h/malloc.h (+ optional dialog.h/filedialog.h).
- Window text buffer is 2048 bytes: keep one full screen < ~1800 chars, no line > ~110 chars (≈70 cols x 24 rows; 8px/col, 10px/row). Redraw = `gui_clear()` then print the whole screen.

## Testability pattern that works

- App state in one static struct + `handle_event(st, ev) -> needs_redraw` + `render_into(buf, cap, st)` + thin main loop. Tests call those directly, never the blocking loop.
- Render into a caller buffer (`app_render`) instead of only print(): lets tests check exact lines/rows/length and determinism (render twice, memcmp).
- Build rows with fixed-width fields (e.g. 66-char rows) so tests can compare whole lines; check the screen budget (<1800) and max line length in a test.
- Check framework: counter + PASS/FAIL per check (no early return), final "N checks run, M failed" + `ALL TESTS PASSED`, exit non-zero on failure.

## compat.c mock facts (host)

- `read_dir`: fixed flat 7-file list (EDITOR.BIN DESKTOP.BIN SH.BIN LS.BIN CAT.BIN TEST.TXT NOTES.TXT), `path` ignored, `attr == 0` (so no directories!) — recursion/descent cannot be tested on the host; factor the walk with a depth param and test matching/paths/counters directly, and document the in-OS verification path in the app header.
- `chdir`/`getcwd`: in-memory string (default "/home"); `open/read/write` hit the REAL host FS — chdir to /tmp or avoid file I/O.
- `sysinfo` canned (cmd6 = 2026-09-17 12:00:00 Thursday, cmd1 = 12345 ms); `mkdir/unlink/rename` inert with `mock_*_result` globals for failure paths.

## Pitfalls

1. `gui_ci_find(hay, "")` returns 0 (empty needle matches at index 0) — guard an empty query yourself if the spec wants "no results"; document the decision.
2. `gui_list_move` with `count == 0`: selected = delta then clamped via `count-1` (= -1) back to 0 — safe, but set `list.count = nresults` after the walk or the list looks empty.
3. `gui_list_click_row(list, y, first_row)` needs the first drawn row index and `visible`; pair it with a fixed row layout so mouse lines up with rendering.
4. A "press c to clear" shortcut collides with typing 'c' in a search/query box — bind lowercase only, let uppercase 'C' type, and document it (users can still search C-names).
5. Off-by-one in fixed-width fields (right-aligned size in 10 cols: "0B" = 8 spaces + 2 chars) — prefer building expected strings with the app's own formatter instead of hand-counting spaces in tests.
6. Static render buffers, not big stack arrays, and avoid >120-char path buffers (kernel paths stay short; document truncation).
7. `fat16_read_dir` filters LFN/volume-label but NOT "."/".." — skip dot names in a recursive walk or it never terminates.

## Quick visual check (scratch, /tmp only)

A tiny /tmp/<app>_peek.c with absolute-path includes (`#include "/…/src/user/<app>.c"`) can call the app's render function and `cat -A` the screen to verify layout without QEMU. Compile it against the same compat/gui/dialog objects.
