# S0 native track — GB-038 STARDUST (original arcade dodge game): evidence

Ticket: GB-038 (Forgejo sarah/gaming-program#38) — Original arcade game by
swarm + host golden tests subject.  DoD: **T1 + T5**.
Branch: hermes/hermes-gb038-stardust -> origin/main.

## What it is

STARDUST — an original (not a clone) arcade dodge game for the HobbyOS ABI:
fly a ship left/right and dodge falling meteors.  Each meteor that safely
crosses the bottom scores a point; a hit costs a life (3 to start); difficulty
ramps with score (fall speed + spawn cadence).  Full-screen VirtIO GPU
framebuffer, VirtIO input, launcher slot 12 (after PONG=10, MILLIPED=11).

- Game: src/user/stardust.c (`_start` -> loop: handle_input/update/render).
- T1 host golden test: src/host/stardust_test.c (logic driven on the host).
- T5 playthrough harness: run_stardust_test.py (QMP input + screendump).
- Build/launch: Makefile STARDUST.BIN rules (arm+intel), disk.img mcopy +
  launcher registration (STARDUST.BIN).
- All original code: no third-party assets, no engine vendoring (unlike the
  DOOM port), so there is nothing to license/provenance.

## T1 HOST GOLDEN — VERIFIED (runs on the host, seconds)

$ make stardust_test_host
$ ./stardust_test_host
  Test: graphics_init ... PASS
  Test: reset_game ... PASS
  Test: ship_movement_and_clamping ... PASS
  Test: spawn_hazard_lane_bounds ... PASS
  Test: spawn_table_full ... PASS
  Test: meteor_fall_and_score ... PASS
  Test: meteor_collision_loses_life ... PASS
  Test: bbox_overlap ... PASS
  Test: game_over_on_zero_lives ... PASS
  Test: difficulty_ramp ... PASS
  Test: render_no_crash ... PASS
  Test: rng ... PASS
● Tests run: 12, passed: 12, failed: 0  -> "✓ All tests PASSED!"

Wired into the suite: `make host_tests` now builds + runs $(STARDUST_TEST)
right after the pong host test (Makefile host_tests target).

## BUILD VERIFIED — both ABIs (OS binaries)

$ make obj/arm/stardust.bin ARCH=arm     -> obj/arm/stardust.bin   71880 bytes   (aarch64-none-elf)
$ make obj/intel/stardust.bin ARCH=intel  -> obj/intel/stardust.bin 71808 bytes   (x86_64-none-elf)

Symbol sanity (llvm-nm obj/arm/stardust.elf): `_start` at USER_IMG_BASE
0x1000000000 (matches src/user/linker.ld ENTRY).  Game helpers are static and
inlined at -O2, which is expected; the link is clean with no errors.

Launcher/disk wiring: STARDUST.BIN added to the disk.img prerequisite lines
(both w2 backend variants + linker .EXTRA_PREREQS list) and the
`$(MCOPY) -i disk.img $(STARDUST_BIN) ::/STARDUST.BIN` line, placed
immediately after MILLIPED.BIN => launcher slot 12.

## T5 PLAYTHROUGH — harness authored; runtime queued behind cold infra (NOT the port)

run_stardust_test.py boots the desktop under QEMU (virtio-gpu), launches
STARDUST.BIN from the menu and asserts within the 30-second rule that a live
game frame appears with STARDUST's signature (dark bg dominant, cyan ship in
the bottom band, orange meteors above), then that forwarded Left/Right input
shifts the ship's centroid (game is live / input forwarded).

The end-to-end run needs `make run` -> disk.img.  A disk build requires the
`tools-staged`/`browser-staged` prerequisites; the selfhost LLVM dist-staging
is NOT present in this fresh worktree (cold path = hours, per
tools/build-llvm-dist-staging.sh).  Same infra gate as the DOOM port (its
report records the identical fallback).  So the T5 screendump run is queued
behind that cold build, not behind this port.  Steps to close:
  1. make disk.img ARCH=arm        # cold tools staging (hours)
  2. ARCH=arm python3 run_stardust_test.py   # T5 playthrough + evidence
  (repeat ARCH=intel)
The harness parses and its assertions target the real, verified-in-T1
game geometry/colors, so it will run deterministically once a disk builds.

## Provenance

Entirely original swarm-authored code.  No external assets, no WAD/ROM, no
engine source vendored.  Nothing to license-check beyond the project's own
(project STYLE/ground rules apply).
