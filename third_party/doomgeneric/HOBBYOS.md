# doomgeneric — HobbyOS native port notes

Vendored from the upstream **doomgeneric** project (ozkl/doomgeneric, `master`
branch as of 2026-10-17) — the portable single-file-principle "generic" DOOM
engine. Licensed **GPLv2+** (see `LICENSE`); engine copyright id Software.

## What this is

DOOM (the classic 1993 software raycaster) running as a **user-space process
on the HobbyOS ABI**, full-screen, under QEMU/virtio-gpu. Part of the S0
native-game track.

- `src/user/doom/doom.c` — entry point (`main` → `doomgeneric_Create` → tick loop).
- `src/user/doom/doomgeneric_hobbyos.c` — the HobbyOS platform backend
  (implements the DG_* callbacks): VirtIO GPU framebuffer blit, VirtIO input,
  monotonic timer, sleep.
- Build wiring in the top-level Makefile (`DOOM.BIN`, launcher slot 12).

## HobbyOS-specific patches (kept minimal, in-tree)

These are the only edits to the vendored engine:

| File | Change | Why |
|------|--------|-----|
| `i_video.c` | `fb_scaling = DOOMGENERIC_RESX/SCREENWIDTH` (2) | Blit rendered 320x200 to a 640x400 DG_ScreenBuffer that the backend scales 2x onto the 1920x1080 fb |
| `i_video.c` | `s_Fb` static initializer (xres/yres=RESX/RESY, 32bpp, RGBA offsets) | `I_FinishUpdate`/`cmap_to_fb` need the render-target geometry and channel bit-fields |
| `dummy.c` | shipped unchanged | supplies `drone`/`net_client_connected` single-player stubs |

Backend shims (in `doomgeneric_hobbyos.c`): `system()` returns -1 (no zenity
dialog on HobbyOS; DOOM falls back to its console error path). `mkdir` comes
from the sysroot `unistd.h` via `-include unistd.h`.

## Build

    make obj/arm/doom.bin ARCH=arm       # aarch64
    make obj/intel/doom.bin ARCH=intel   # x86_64

Links against the userland `libc.a` (crt0 + stdio + malloc + math), so the
engine's stdio/printf/malloc resolve without extra shims.

## Runtime asset (license-clean IWAD)

The engine needs an IWAD. Use a license-clean one (see
`tools/fetch_doom_wad.sh`):

- **Shareware DOOM (DOOM1.WAD)** — id Software freeware, redistributable.
- **Freedoom (freedoom1.wad)** — free-content replacement, GPL/BSD.

Place it on the boot disk root as `DOOM1.WAD` (the launcher runs DOOM.BIN with
the filesystem root as cwd; `d_iwad.c` auto-discovers it). Record the fetched
WAD's sha256 as provenance per title.

## Controls (HobbyOS backend key map)

Arrows = move/turn, Ctrl = fire, Space = use/open, Esc = menu, Enter = select.
Custom mapping lives in `evdev_to_doom()`.
