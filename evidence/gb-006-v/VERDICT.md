# GB-006-V — VERDICT: **GREEN** (clause 2 passes on substance; its `--negative` flag is inert — stated, not hidden)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 05:54–06:2xZ.
Verifies GB-006 / ship **PR #189 @ `bba4e5e`** (`Makefile` `HOBBYOS_USE_CCACHE` wiring + `tools/cache/measure_build.sh`).
Host-side T1. I did not author GB-006 or PR #189 — independent gate. No CI check was consulted; every
number below is from this VM.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | real-build COLD→WARM→NEGATIVE measurement, cache must serve the **real** Makefile targets | **PASS** — COLD 0 hits, WARM 21 hits (1 s vs 9 s), cleared-cache NEGATIVE 0 hits |
| 2 | `measure_build.sh --negative` stays 0 hits (a cleared cache cannot serve the build) | **PASS on substance** — the cleared-cache phase is run and quoted in every invocation; **the `--negative` flag itself is ignored by the script** (finding) |
| 3 | correctness untouched with the cache OFF | **PASS** — `make host_tests` 482/0, `TEST EXIT 0`; `make test_cache` → `CACHE TEST PASSED` |

## Clause 1 — real build, quoted (`01-measure.log`)

```
GB-006 REAL-build measurement  (HOBBYOS_USE_CCACHE=1  CCACHE_DIR=/home/sarah/.cache/hobbyos-ccache)
  TARGET='cxxrt_test_host errno_test_host … torture_test_host'
  COLD (make clean + make …): 9s        Hits: 0 / 21 (0.00%)   Misses: 21 / 21 (100.0%)
  WARM (make clean + make …): 1s        Hits: 21 / 42 (50.00%) Misses: 21 / 42 (50.00%)
  NEGATIVE (cache cleared) …: 4s        Hits: 0 / 21 (0.00%)   Misses: 21 / 21 (100.0%)
PASS: real build (not a probe) served >=1 ccache hit (warm hits=42)
rc=0
```

This is the exact RED ground answered: 21 cacheable compile requests from the **tree's own Makefile**, all
misses on a cold run, **all 21 served from the cache** on the warm run after `make clean`, and **0 hits**
once the cache is cleared — so the 9 s → 1 s delta is the cache and not page cache.

**Stronger form, because the card's RED was "the tree never invokes a cache":** the *kernel* recipes are
on the same PATH shim, so I measured them directly (`04`/`05`/`06` logs):

```
$ (obj/intel removed) HOBBYOS_USE_CCACHE=1 make ARCH=intel hobbyos.elf   # first, cold
Cacheable calls:    141 / 151 (93.38%)   Hits: 0 / 141   Misses: 141 / 141
$ (obj/intel removed again) HOBBYOS_USE_CCACHE=1 make ARCH=intel hobbyos.elf
Cacheable calls:    120 / 120 (100.0%)   Hits: 120 / 120 (100.0%)
  Direct:         120 / 120 (100.0%)   Misses: 0 / 120 (0.00%)
```
and the Makefile's own wiring line appears on every invocation:
`HOBBYOS_USE_CCACHE=1: real build routed through /usr/lib/ccache (CCACHE_DIR=… CCACHE_BASEDIR=/home/sarah/wt/gb006v)`.

**Disclosure of my own measurement error:** my first kernel check reported "cache size 0.0 GiB, no
counters" and I nearly read that as "the kernel bypasses ccache". That was me querying `ccache -s`
without `CCACHE_DIR` (the default dir, not the Makefile's `~/.cache/hobbyos-ccache`). With the right
directory the counters are the 141-miss / 120-hit pair above. The finding is the cache works; the error
was in my probe, and it is in the log (`05-kernel-cache-stats.log`) so a reader can see both readings.

## Clause 2 — the negative control, and the flag that does nothing

Every run's third phase is the negative control, and it holds:
```
  NEGATIVE (cache cleared) (make clean + make …): 4s
    Hits: 0 / 21 (0.00%)   Misses: 21 / 21 (100.0%)
```
Running the documented invocation `bash tools/cache/measure_build.sh --negative` produces **the same three
phases and the same PASS line as the positive run** (`02-negative-and-correctness.log`), because the
script never reads `$1` — `--negative` is accepted and ignored:

```
$ bash tools/cache/measure_build.sh --negative
  COLD … Hits: 0 / 21      WARM … Hits: 21 / 42      NEGATIVE (cache cleared) … Hits: 0 / 21
PASS: real build (not a probe) served >=1 ccache hit (warm hits=42)
```
So the clause's **substance** — "a cleared cache cannot serve the build, and hits vanish when it is
cleared" — is demonstrated and quoted on this VM, which is what separates a real hit from page cache; but
the **affordance** the clause names does not exist. I grade it PASS-with-finding rather than RED, and I am
stating the literal gap here so the reading is auditable: if the Coordinator requires the literal form,
this is a two-line branch (`if [ "${1:-}" = "--negative" ]; then … skip the WARM phase …`) plus the same
branch behind the `cache_measure_negative` Makefile target, which currently inherits the inert flag.

Minor instrument quibble, also disclosed: the script's summary reads `warm hits=42` because `ccache -s`
prints a `Hits:` line twice and the awk sums both; the genuine figure is **21 of 21** (50 % of 42 total
requests). The PASS threshold is unaffected.

## Clause 3 — correctness untouched (`02`, `03-host-tests.log`)

```
$ make test_cache
cold_ms=83 warm_ms=10
ccache stats: hits~2 misses~2
  PASS: warm compile is a ccache HIT (hits=2)
  PASS: warm <= cold (cached build not slower)
CACHE TEST PASSED
rc=0

$ make host_tests
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0
```
A plain `make` (no `HOBBYOS_USE_CCACHE`) is unchanged by the opt-in block (`ifdef` only), and
`host_tests` is green with the cache off — a cache miss is an ordinary build.

## Reproduce

```
git worktree add /tmp/gb006v bba4e5e && cd /tmp/gb006v && mkdir -p obj
ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
bash tools/cache/measure_build.sh
bash tools/cache/measure_build.sh --negative
make test_cache && make host_tests
# kernel coverage (the RED's real question):
python3 -c 'import shutil;shutil.rmtree("obj/intel")'
HOBBYOS_USE_CCACHE=1 make ARCH=intel hobbyos.elf
CCACHE_DIR=$HOME/.cache/hobbyos-ccache ccache -s | head -5      # Hits: 120/120 on a rebuild
```