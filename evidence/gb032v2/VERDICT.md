VERDICT: GREEN sha=7667e5d3

**GREEN — GB-032-V2 @ `7667e5d3` (PR #351, one commit on `4f0b4a97`).** One intel T3 boot with `SDL2TEST.BIN` as the only wave member, plus the host suite and both kernel links, all run from this worktree.

**Clause 1 — the compat device path is real, not the fallback.** From the pass serial (`evidence/gb032v2/serial_pass.log`):

```
[WAVE-TRY] SDL2TEST.BIN
[ARRIVED] SDL2TEST.BIN rc>0
sdl test app: fb /dev/fb0 fd=3 mode=1920x1080 bpp=32
sdl test app: drew 64x48 window, blitted 8x8 tile @(10,10)
[EXITPC] pid=2 last_user_pc=0x1000005E8E code=0x0
System halt.
```

**Clause 2 — the echo is a real key.** `sdl test app echo: KEY 1e=1` — the QMP key `a`, `KEY_A` down, read back from `/dev/input/event0`; not the literal fallback.

**Clause 3 — the scanout actually changed.** The runner's PPM count from the same run:

```
--- screendump: blue_window_px=3008 magenta_tile_px=32 ---
run_sdl_test_app_t3: PASS
```

The 64×48 blue window (3008 px after the 64-px tile) and the 32 magenta tile pixels are present; the PPM is kept at `evidence/gb032v2/draw_pass.ppm`.

**Clause 4 — host suite and links.** `sdl_test_app_test_host: 20 checks, 0 failed`; `Checks run: 482`, `Checks failed: 0`, `TEST EXIT: 0`; `make ARCH=intel hobbyos.elf` rc=0; `make ARCH=arm hobbyos.elf` rc=0.

**Clause 5 — falsifier, reproduced here (not quoted).** Reverting only the three pre-fix kernel files to `4f0b4a97` while keeping the PR's app and runner makes the same T3 run fail:

```
sdl test app: fb unavailable (errno=25), using the local buffer
sdl test app: drew 64x48 window, blitted 8x8 tile @(10,10)
[EXITPC] pid=2 last_user_pc=0x1000005E8E code=0x0
System halt from CPU 1.
--- screendump: blue_window_px=0 magenta_tile_px=0 ---
run_sdl_test_app_t3: FAIL
  fb_line=False drew=True echoed=True exit0=True halted=True blue=0 magenta=0
```

rc=1. A black scanout with the fb line absent is exactly the failure the pass leg must not produce.

**Non-blocking note — the runner's `--no-gpu` leg is not a negative control on this kernel.** Run twice, `--no-gpu` PASSES with the same `blue_window_px=3008 magenta_tile_px=32`; the guest still opens `/dev/fb0` mode=1920x1080 bpp=32 and the draw lands on the scanout through the default VGA/GOP framebuffer. The runner's help text says it should be black, but it is not. Clause 5's second option (pre-fix `errno=25`) is the falsifier used, and it is a real one; the `--no-gpu` expectation is worth correcting in the runner separately.

Evidence: durable commit `8f59543d` on `origin/verify-gb032`, path `evidence/gb032v2/` — both serials, all four runner logs, `host_tests.log`, both link logs, and the pass PPM. Graded sha `7667e5d3`.

GREEN → GB-032 to Merge Gate; GB-032-V2 to Done.
