# GB-006 — shared build cache (`tools/cache/`)

Speeds up the farm's repeated builds (and the GB-044 two-arch parity CI) by
wrapping the compiler in ccache.  The M0 infra goal: a **cold vs cached build
time measured** — the DoD — so parity and per-merge gates re-run cheaply.

## What's here
- `setup_ccache.sh` — probes ccache + the compiler, defaults `CCACHE_DIR` to
  `$HOME/.cache/hobbyos-ccache`, smoke-compiles, prints the env snippet:
  `eval "$(bash tools/cache/setup_ccache.sh)"`, then
  `make CC="ccache clang" <target>`.
- `cold_vs_cached.sh OUT.o SRC.c [compiler] [flags...]` — compiles SRC cold
  (cache cleared) then warm (cache hit), prints `cold_ms`, `warm_ms`,
  `speedup` plus ccache stats.  This is the DoD evidence producer.
- `cache_test.sh` — T1 self-test: real Miss→Hit transition + measured
  cold/vs/warm times; FAILS if ccache is missing or no hit is recorded.

## Seeding the shared cache across the farm
- Prebuilt cross tree: `third_party/llvm-21.1.8/dist-staging` already built on
  farm-1; rsync it to farms 2/3 + coordinator so every VM uses the same
  toolchain:
      rsync -a /path/to/dist-staging <farmN>:/path/to/HobbyOS/third_party/llvm-21.1.8/dist-staging
- Then point every VM at one shared `CCACHE_DIR` (NFS or a per-branch dir):
      export CCACHE_DIR=/var/cache/hobbyos-ccache      # shared NFS on the farm
  With a shared CCACHE_DIR a warm build of the whole tree is a series of
  cache hits — parity CI / merge gates go from minutes to seconds.

## Make
- `make cache_setup`      — print the env snippet from setup_ccache.sh.
- `make cold_vs_cached`   — demo the measurement on a tiny probe.
- `make test_cache`       — run the T1 self-test (wired into host_tests).

Alternative: `sccache` (parallel compile server) is equivalent for the same
CCACHE_DIR intent; set `CCACHE=sccache` in the scripts and call `sccache
--start-server` once.
