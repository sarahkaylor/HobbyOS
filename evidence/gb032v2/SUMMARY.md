# GB-032-V2 — independent gate, swarm-farm-1, 2026-11-04

Head graded: `7667e5d3` (PR #351, one commit on `4f0b4a97`). Worktree branch
`verify-gb032`; toolchain from `third_party/llvm-21.1.8/dist-staging`;
`PATH=/usr/sbin:/sbin:$PATH` for the image build.

## Commands and results

| Step | Command | Result |
|---|---|---|
| T1/T4 | `make host_tests` | rc=0; `sdl_test_app_test_host: 20 checks, 0 failed`; 482 / 0 / `TEST EXIT: 0` |
| image | `make MODE=test ARCH=intel disk.img` | rc=0 |
| T3 pass | `python3 tools/sdl/run_sdl_test_app_t3.py` | rc=0; fb line, draw line, `echo: KEY 1e=1`, `[EXITPC] code=0x0`, `System halt.`, `blue_window_px=3008 magenta_tile_px=32`, PASS |
| T3 no-gpu | same with `--no-gpu` (twice) | PASS both times, same 3008/32 — not a negative control on this kernel |
| T3 pre-fix | three kernel files reverted to `4f0b4a97`, app+runner kept | rc=1; `fb unavailable (errno=25)`, `blue_window_px=0 magenta_tile_px=0`, FAIL |
| links | `make ARCH=intel hobbyos.elf`, `make ARCH=arm hobbyos.elf` | rc=0 / rc=0 |

## Verdict

`VERDICT: GREEN sha=7667e5d3` — clauses 1–4 met from one T3 boot; clause 5 met by
the pre-fix `errno=25` leg, reproduced here. The `--no-gpu` flag is documented as
the black-scanout control but passes on this kernel; noted as non-blocking.

Raw artifacts: `evidence/gb032v2/` (serials, runner logs, host and link logs,
`draw_pass.ppm`), durable at commit `8f59543d` on `origin/verify-gb032`.
