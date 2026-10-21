# GB-006-V — verification evidence (DEEP Verifier, 2026-10-21)

Subject: GB-006 (#11) "Shared build cache: sccache/ccache + prebuilt clang 21.1.8" — verdict **RED**.
Revision: **`badc665`** as pinned by PLAN-008, fresh worktree on each host. `origin/main` has since
moved to `03058e6`, so this verdict covers `badc665` only.

Hosts: **swarm-farm-1** (192.168.0.230, the host this session runs on) and **swarm-farm-2** (192.168.0.231),
both at `badc665`, both with the gitignored vendored trees symlinked from the main checkout (the GB-006
worktree fix), both cold (cache zeroed, `obj/` absent).

| # | file | what it is |
|---|---|---|
| 01 | `01-farm1-cold.txt` | farm-1 cold: rc, seconds, ccache and sccache counters after the build |
| 02 | `02-farm2-timings.txt` | farm-2 cold + warm1 + warm2 + negative control, with counters |
| 03 | `03-farm1-cold-build.log` | farm-1 cold build log (full) |
| 04 | `04-farm1-warm-and-stats.txt` | farm-1 warm1/warm2 and counters |
| 05 | `05-farm1-negative-and-wired-control.txt` | negative control (cache cleared) and the ccache-wired control |

## Measurements

| host | cold | warm 1 | warm 2 | negative control (cache cleared) | ccache hits (default PATH) | sccache |
|---|---|---|---|---|---|---|
| swarm-farm-1 | **57 s** | 48 s | 47 s | **47 s** | **0 / 0.0 GiB** | 0 requests |
| swarm-farm-2 | **56 s** | 46 s | 47 s | **46 s** | **0 / 0.0 GiB** | 0 requests |

Both builds: `make host_tests` in a fresh worktree, `make clean` between every run so the same 136+
compilations happen each time.

## Why this is RED, not a pass with a small delta

1. **The cache is not in the build path.** `ccache -s` / `sccache --show-stats` are zero after the cold
   build *and* after two warm builds on both hosts (`Cache size (GiB): 0.0 / 5.0`, `Cache hits 0`,
   `Compile requests 0`). The Makefile contains no `sccache`/`ccache` reference, `clang` resolves to
   `/usr/bin/clang -> clang-21` (a real compiler, not a wrapper), and `/usr/lib/ccache` is **not** on
   `PATH`. A cache that records nothing did not serve a single compile.
2. **The 9–10 s "warm" delta is not the cache.** Clearing the cache (`ccache -C`) and rebuilding gives
   **47 s on farm-1** and **46 s on farm-2** — the same as warm. The delta survives erasing the cache,
   so it is OS page cache / run-to-run variance, exactly the case the ticket's clause 4 negative control
   exists to catch.
3. **It is not shared, because it is not used.** No remote storage is configured (nothing in the repo
   sets `CCACHE_REMOTE_STORAGE`/`SCCACHE_*`), and the configured store is the local default
   `/home/sarah/.cache/ccache` per host. GB-006's premise is a *shared* cache; what exists is two idle
   local caches.
4. **It works when wired — so the gap is wiring, not capability.** With `PATH=/usr/lib/ccache:$PATH` on
   farm-1: build 1 → 134 misses / 2 hits, build 2 → **126 hits of 272 compiles (46.32 %)** and 40 s vs
   47 s. That is the measurement GB-006 asked for, and it only appears when the shim dir is prepended —
   which nothing in the tree does. (This probe is what filled farm-1's cache to 11 MB; the zero-hit rows
   above are from before it.)

## Context that is not the verdict

`make host_tests` exits 2 at `badc665` on both hosts: 3 host tests are red — `tools/board/claim_race_test.py`,
`tools/t5/acceptance_gate_test.py`, `tools/t5/acceptance_test.py`. They are the regression guards for
GB-074 / GB-075, which are not merged at this revision. Compilation itself completes; the exit status
comes from the test runner, so the timings above are usable but the "green suite" precondition is not met
at this sha.
