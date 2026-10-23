# GB-007-V (re-gate) — verdict: GREEN

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-23.
Revision: `origin/main` @ **`5f6e3c3`**, clean worktree (`git status` empty before and after; the
criterion-4 mutation was reverted with `git checkout --`).

**Read this first:** the card's criteria 3 and 4 name artifacts that do not exist on this tree — there is
no `--self-test` in `src/host/t4_parity.sh` (`grep -c 'self-test' → 0`) and there is no `ho_getrandom`
(the probe makes **no** `ho_*` calls, so a `compat.c` flip cannot move any row). I graded the substance
those criteria exist for, and state the substitution below rather than pass silently.

## 1. Healthy — exit 0, and the number on the summary line explained

```
$ MUSL_GCC=~/toolchains/musl/musl-gcc make t4_compat_parity
t4 host differential: A=0 B=0 cases=3
t4_parity: PASS (host differential locked; device leg gated)
EXIT=0
```

How I established which number is which, since the card warns this is the trap:

* `A`/`B` are the **legs' exit statuses** (`A=$(cat "$TMP/runs/legA_musl.rc")`, `t4_parity.sh:116`) — not
  row counts. Both legs exited 0.
* `cases=3` is `N`, incremented **per harness step** (`N=$((N+1))` at lines 80 and 95): capture leg A,
  capture leg B, compare. It is not the row count either.
* The real comparison, measured by building and running the two legs by hand:

```
legA (musl -static): rc=0 rows=79
legB (clang -DHOST_TEST + compat.c): rc=0 rows=79
legA and legB byte-identical
```

So the healthy path did compare 79 rows; `cases=3` counts harness steps and says nothing about coverage.

## 2. Leg absent must FAIL — both forms, with leg B isolated

**True skip path** (a PATH farm with every tool but `musl-gcc`, so leg B still builds):

```
$ env -u MUSL_GCC PATH=/tmp/t4-pathbin bash src/host/t4_parity.sh
SKIP leg A: musl-gcc not found (install musl musl-tools)
t4 host differential: SKIPPED (a required leg did not run: A=skip B=/…/t4_probe.host) — FAILING the tier per GB-066 (no false green)
t4_parity: FAIL
EXIT=1
```

**Build-failure path**:

```
$ MUSL_GCC=/nonexistent/musl-gcc bash src/host/t4_parity.sh
FAIL: leg A build (static musl)
t4 host differential: SKIPPED (a required leg did not run: A=buildfail B=/…/t4_probe.host) — FAILING the tier per GB-066 (no false green)
t4_parity: FAIL
EXIT=1
```

Note on the isolation: a naive `PATH=/usr/bin:/bin` is **not** an absence test on this box — `musl-tools`
is installed, so `/usr/bin/musl-gcc` exists and the run passes. The farm above is the real test, and my
first attempt broke leg B's link step (`clang: posix_spawn failed`) until `ld` was added to the farm —
i.e. exactly the "unrelated PATH reason" the card warns about. Both final runs have `B=<a built path>`.

## 3. Self-test — **does not exist** (card criterion unimplementable as written)

```
$ grep -c "self-test\|selftest" src/host/t4_parity.sh
0
```

Inside `make host_tests` the only thing that runs is `T4_COMPAT_PARITY` — the healthy path (`Makefile:3474`,
`T4_COMPAT_PARITY` in the prerequisite list). Nothing plants a divergence. The *intent* of criterion 3 is
exercised by criterion 4 below; the harness itself still has no self-test. Recommend a follow-up mint
(`t4_parity.sh --self-test`) so this proof lives in the suite instead of in a verifier's shell history.

## 4. Deliberately-wrong leg-B value — FAILS, then PASSes on restore

`ho_getrandom` does not exist and the probe calls no `ho_*` function, so the card's prescribed edit is a
no-op by construction. I planted the same divergence where leg B can see it — inside the probe, compiled
only into leg B (`-DHOST_TEST`), flipping `getrandom_16` from 16 to 15:

```
$ make t4_compat_parity          # leg-B-only wrong value
FAIL: stdout differs (legA_musl vs legB_host)
  A: …|getrandom_16 rc=16|…
  B: …|getrandom_16 rc=15|…
t4_parity: FAIL
make: *** [Makefile:3389: t4_compat_parity] Error 1
EXIT=2

$ git checkout -- src/host/t4_compat_probe.c
$ make t4_compat_parity          # restored
t4_parity: PASS (host differential locked; device leg gated)
EXIT=0
```

## Pass criteria

| # | criterion | result |
|---|---|---|
| 1 | healthy exit 0, non-zero case count quoted, A/B explained | **PASS** (`cases=3` = harness steps; 79 rows compared, byte-identical) |
| 2 | leg absent → non-zero with the GB-066 line | **PASS** (skip and build-fail forms, leg B isolated) |
| 3 | harness self-test plants divergences | **NOT RUNNABLE — no self-test exists**; intent covered by 4 |
| 4 | deliberately-wrong leg-B value FAILS, restore PASSes | **PASS** (substituted probe-level injection; `compat.c` route is a no-op) |

## Scope

This says the **host differential harness is not lying** — leg A (static-musl Linux) vs leg B (host compat
model). It says nothing about a HobbyOS guest; that distinction is GB-066's and its device leg is not
covered here. GB-073/GB-076 are untouched.
