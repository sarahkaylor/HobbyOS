---
name: hobbyos-graphics-acceleration
description: "Use when making HobbyOS graphics fast (driver + QEMU GL)."
version: 1.0.0
author: Hermes Agent
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, graphics, gpu, virtio-gpu, qemu, gl, performance]
    related_skills: [hobbyos-build-and-run, hobbyos-desktop-compositor, hobbyos-kernel-constraints, hobbyos-run-tests]
---

# HobbyOS: Graphics acceleration (display + driver + QEMU GL)

## Overview

"High-performance accelerated graphics" on HobbyOS is two halves that must both
be wired (covers the virtio-gpu driver, damage-rect flush plumbing, and QEMU GL
launchers on both arches):

1. **QEMU side** — offer GL: a `virtio-gpu-gl-*` device paired with a GL display
   backend. With no GPU render node the host GL is llvmpipe (software), which is
   the accepted "emulated acceleration" path.
2. **Guest side** — stop paying full-frame costs: the compositor's damage rects
   flow to a rect-based flush, and the kernel driver does partial
   TRANSFER_TO_HOST_2D / RESOURCE_FLUSH instead of whole-frame work per frame.

Guest-side 3D (virgl contexts) would need a DRM + gallium port and is out of
scope for now; the gl-capable devices are chosen so it can light up later.

## When to Use

- Making the desktop, browser, or X apps display faster; touching
  `src/kernel/virtio_gpu.c`, the flush path (`SYS_FLUSH_FB` / `flush_fb`), or
  display-related QEMU device/display flags.
- Adding or updating any QEMU launcher when the graphics device or display
  config changes.
- Not for: compositor damage-model edits (→ [[hobbyos-desktop-compositor]]),
  generic build/boot targets (→ [[hobbyos-build-and-run]]), kernel C rules
  (→ [[hobbyos-kernel-constraints]]).

## QEMU GL matrix (empirically verified — don't re-run the failures)

| combo | result |
|---|---|
| `gtk,gl=on` + virtio-gpu-gl-* | works under X11 (Mesa falls back to llvmpipe; the DRI3 warning is not an error) |
| `sdl,gl=on` + virtio-gpu-gl-* | works |
| `egl-headless,gl=on` | fails on hosts exposing only `card0`: "no drm render node available" (needs `/dev/dri/renderD*`) — don't chase it |
| any virtio-gpu-gl-* + `-display none` | fails at launch: "display backend does not have OpenGL support enabled" |
| plain 2D device (`virtio-gpu-device` / x64 default VGA) + any display incl. none | works — the headless picture |

- Device names per bus: `virtio-gpu-gl-device` (ARM virtio-mmio),
  `virtio-gpu-gl-pci` / `virtio-vga-gl` (x64 PCI); they ship in
  `qemu-system-modules-opengl`. Probe with
  `qemu-system-<arch> -device help | grep virtio-gpu`.
- **Headless tiers keep the plain 2D device** (`MODE` ≠ desktop, CI waves): the
  guest-side command protocol is identical, so GL and non-GL runs do not drift.
- **Every launcher moves together** — Makefile `QEMU_CMD` (both arches),
  `tools/run_waves.sh`, the `run_*.py` harnesses (flags ride in `QEMU_ARGS`),
  `continuation/*/run_*.sh`, and the fork-side
  `~/webkit-hobbyos/HobbyOS/continuation/{smoke,wk3,wk4,wk5}/run_*.sh`. The VM CI
  tier is make-driven and inherits Makefile edits — re-verify one tier after
  touching it. (Runner `~/bin/hobbyos-ci.sh` + `~/ci` were ABSENT at last
  check; recreate against hobytest if the clean-machine battery is needed.)

## The guest pipe and the named wins

- Apps draw **directly into the kernel framebuffer** (`map_fb()`).
- **LANDED (GX program 2026-10; main `9fce7ec`):** damage rects flow to
  `SYS_FLUSH_FB_RECTS` (89) as per-rect partial transfers. ARM `virtio_gpu.c`
  does rect TRANSFER_TO_HOST_2D + IRQ-acknowledged completion — park the CPU
  only after the IRQ/timer are armed (an early-boot park has no wake source).
  x64 `virtio_gpu_x64.c` uses the **modern virtio 1.0 PCI transport** (legacy
  I/O-port is impossible for virtio-gpu on QEMU 10.2.1 — modern-only
  `1af4:1050`, `disable-modern=on` is a no-op; 64-bit BAR remapped via
  `cpu_pd7` like NVMe; completion = bounded used-ring poll) with a BGA
  row-wise rect fallback. Launcher: `QEMU_GPU=auto|gl|soft` in the Makefile;
  the x64 GL path adds `-device virtio-gpu-gl-pci -vga none`
  (`GX_X64_GPU_DEV`, empty on headless tiers so their qemu lines are
  unchanged); ARM GL = `virtio-gpu-gl-device` + `gtk,gl=on`.
- Contract: `SYS_FLUSH_FB_RECTS(const struct fb_rect*, int count)` → 0 |
  -EINVAL | -EFAULT; count 0..32; covered pixels byte-identical to a full
  flush; overlaps/order meaningless (driver may expand/merge).
- Driver internals, syscall rows, x64 transport notes, and the boot +
  screendump recipes: `references/graphics-pipe-map.md`. Plan + evidence log:
  `docs/graphics-accel.md` (in-repo).

## Verification & evidence (standing rules)

- No "fixed" without executed, observed evidence; a rendering claim needs
  **pixels** (QMP screendump / framebuffer checksums), never just "it booted".
- Rect-flush work must keep pixel outputs **bit-identical** to the full-frame
  path — the existing E2E checksums are the regression net, and legacy
  `flush_fb()` (full-frame) semantics must keep working for old callers.
- De-risk a GL launcher change by booting the desktop against the gl device and
  screendumping before wiring scripts (recipe in the reference).
- Launch long QEMU runs with the terminal tool's `background=true`/`notify=true`
  (shell wrappers like `nohup`/`setsid` are rejected by the harness guard).

## Pitfalls

1. **gl device + non-GL display is a launch error, `-display none` included.**
   Headless launchers must keep the plain 2D device or they die before boot.
2. **`egl-headless,gl=on` is not a workaround for headless GL** on a
   card0-only host (no render node); it fails even with
   `LIBGL_ALWAYS_SOFTWARE=1`.
3. **x64 virtio-gpu is MODERN-only** (QEMU 10.2.1 `virtio-gpu-pci` is
   `1af4:1050` modern; there is no legacy I/O-port transport to copy from the
   net driver). Shipped: PCI-capability walk + split queue in
   `virtio_gpu_x64.c`, with BGA as the probed fallback. `virtio-blk-pci`
   remains broken (NVMe is used instead).
4. **Don't add new full-frame round trips**: long IRQ-off poll loops in the
   guest starve the QEMU host threads that do the actual rendering — shrink the
   transferred area first, then move completion to IRQs.
5. **Syscall rows:** `SYS_FLUSH_FB_RECTS` is row 89 (`SYS_MAX` = 89); new
   rows go above, and both arch dispatch paths + libc wrappers + host-test
   compat stubs must land in the same change (see the GX freeze commit
   `9a3b87e` for the pattern).

6. **QMP screendump captures console 0.** With BOTH a std-VGA and a virtio
   GPU present, screendump shows the VGA console (blank) — x64 GL boots pass
   `-vga none` so the virtio scanout is console 0 (the launcher does this).
7. **x64 program dies at startup with `memory fault ... in=HOLE`?** That is
   the large-model flat-image truncation class (`.lbss` orphaned past
   `.tls_meta` in the user linker script) — see [[hobbyos-build-and-run]]
   before re-diagnosing driver code.

Reference: `references/graphics-pipe-map.md` — guest driver map, syscall
numbering notes, x64 transport realities, and the verified boot/screendump
recipe.
