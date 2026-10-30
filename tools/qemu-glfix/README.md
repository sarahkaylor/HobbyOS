# Patched QEMU for the x64 GL display path

`make run ARCH=intel` (desktop mode, with a display) uses
`-display gtk,gl=on` + `virtio-gpu-gl-pci` only when this patched QEMU is
installed. Without it, `QEMU_GPU=auto` stays on the soft path (plain `gtk`).

## Why stock QEMU 10.2.1 does not work

Both bugs are still present on upstream QEMU master (checked 2026-10-30).

1. **The GL context is stolen by a vCPU thread.** OVMF's VirtioGpuDxe resets
   the GPU at ExitBootServices, right after the bootloader line and before
   HobbyOS runs. `virtio_gpu_gl_reset()` runs on the vCPU thread that wrote the
   status register, and calls `virtio_gpu_virgl_reset_scanout()` directly. That
   makes GTK's EGL context current on the vCPU thread, so every later
   main-thread `eglMakeCurrent` fails with `EGL_BAD_ACCESS`. On Wayland you see
   `Gdk-WARNING: eglMakeCurrent failed` about 5 times a second. On X11 the
   result is racy and sometimes aborts in libepoxy.
   **Fix (0001):** run the scanout reset on the main loop. It uses the same
   bottom-half + `reset_cond` handshake that `virtio_gpu_reset()` already uses
   for the 2D reset.
2. **A resolution change blanks the window.** `virgl_cmd_set_scanout` calls
   `qemu_console_resize()` before `dpy_gl_scanout_texture()`. The resize queues
   a full-surface 2D update (`glupdates++`). On the next refresh tick, the GTK
   GL backends see that update, drop back out of scanout mode, and show the
   blank 2D surface until the next mode set. With bug 1 present this is
   partly masked, because GL calls land in whatever context happens to be
   current.
   **Fix (0002):** when a console enters scanout mode, discard pending 2D
   updates, because the new scanout supersedes them.

HobbyOS itself needs no change. The guest only negotiates
`VIRTIO_F_VERSION_1`, without virgl, and the trigger is in the firmware.

## Build

```bash
sudo apt install libglib2.0-dev libpixman-1-dev libepoxy-dev libvirglrenderer-dev libgbm-dev libgtk-3-dev libslirp-dev zlib1g-dev
tools/qemu-glfix/build.sh
```

- **Installs to:** `~/.local/share/hobbyos-tools/qemu-glfix/prefix`. Override
  with `PREFIX=`. The Makefile looks in `QEMU_GLFIX_DIR`, which defaults to
  `<that prefix>/bin`.
- **Leaves alone:** the system QEMU.
- **Build time:** about 5 minutes on 32 cores.
- **Integrity:** the tarball is checked against a pinned SHA-256.

## Overrides

- `QEMU_GPU=soft`: plain `gtk`, even when the patched QEMU is installed.
- `QEMU_GPU=gl`: force GL. If the patched QEMU is missing, this falls back to
  the stock `qemu-system-x86_64` and reproduces both bugs above.
- `make gpu-check`: shows which QEMU and mode `auto` picks.
