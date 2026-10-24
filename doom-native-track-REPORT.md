# S0 native track — DOOM (doomgeneric) port: build evidence

Ticket: GB-034 (Forgejo sarah/gaming-program#9) — DOOM native port.
Branch: this worktree (hermes/hermes-33d61d61 → origin/main).

## Port summary

doomgeneric (GPLv2+ engine, vendored at third_party/doomgeneric) runs as a
user-space process on the HobbyOS ABI under QEMU/virtio-gpu, full-screen,
launcher slot 12 (after PONG=10, MILLIPED=11).

- Backend: src/user/doom/doomgeneric_hobbyos.c (DG_* callbacks — VirtIO GPU
  framebuffer blit, VirtIO input, monotonic timer, sleep).
- Entry: src/user/doom/doom.c.
- Graphics: new accessors graphics_fb()/graphics_width()/graphics_height()
  (src/user/graphics/graphics.c/h).
- Engine patches: i_video.c `fb_scaling=2` + `s_Fb` geometry init.
- Build: Makefile `DOOM.BIN` rules (arm + intel), disk.img mcopy + IWAD copy,
  launcher slot 12.
- T5 harness: run_doom_test.py (QMP input + screendump raycaster assertions,
  ≤30 s rule, auto-fail).
- License-clean IWAD: tools/fetch_doom_wad.sh (shareware DOOM / Freedoom).

## BUILD VERIFIED — both ABIs

$ make obj/arm/doom.bin ARCH=arm
  ... ~90 engine objects + libc.a ...
  ld.lld -T src/user/linker.ld -e _start -o obj/arm/doom.elf <engine+backend+libc.a>
  llvm-objcopy -O binary obj/arm/doom.elf obj/arm/doom.bin
  -> obj/arm/doom.bin   885760 bytes   (aarch64-none-elf)

$ make obj/intel/doom.bin ARCH=intel
  -> obj/intel/doom.bin 892040 bytes   (x86_64-none-elf)

Symbol sanity (llvm-nm obj/arm/doom.elf):
  0000001000000000 T _start
  0000001000000170 T main
  0000001000000190 T DG_Init
  0000001000000210 T DG_DrawFrame
  00000010000051c0 T doomgeneric_Tick
  0000001000005950 T D_DoomMain
  (image base = USER_IMG_BASE 0x1000000000, matches linker.ld ENTRY(_start))

Only warnings were unused-parameter/sign-compare in upstream engine code; no
errors; the single-Player binary links clean against the userland libc.a
(stdio/printf/malloc/math resolve, incl. w_file_stdc.c fopen path).

## RUNTIME QEMU — pending infra (not the port)

The full playthrough (run_doom_test.py) boots `make run` → disk.img.  A disk
build needs the `tools-staged`/`browser-staged` prerequisites; the selfhost
LLVM dist-staging is NOT present in this fresh worktree (cold path = hours,
per tools/build-llvm-dist-staging.sh).  So the end-to-end QEMU screendump run
is queued behind that cold build, not behind the port itself.  Steps to close:

  1. bash tools/fetch_doom_wad.sh        # get a license-clean IWAD
  2. make disk.img ARCH=arm              # cold tools staging (hours)
  3. ARCH=arm python3 run_doom_test.py   # T5 playthrough + evidence
  (repeat ARCH=intel)

Harness is written and parses; assertions target a real raycaster frame
(horizon signature + many colours + >200k nonzero px) and forwarded-key
view movement, with the ≤30 s rule and IWAD-not-found auto-fail.
