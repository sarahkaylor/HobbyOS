# GB-099-V — VERDICT: **RED**

Verifier: `wverify-deep` on swarm-farm-3, DEEP tier, 2026-10-25.
Verifies GB-099 (#161) / ship PR #155, head `d7c3c5e` ("GB-099: define resume_mark for ARM").
I did not implement GB-099 and had no hand in PR #155 — independent gate.

## One-line verdict

The `resume_mark` fix itself is correct and its own gate passes at the PR head, but the card's
primary DoD clause — "`make ARCH=arm hobbyos.elf` links on `main`" — is **NOT met on today's
`main`**: a *second* x64-only symbol (`cpu_locals`, from `9ebfbbd` / PR #158) now breaks the ARM
build before the linker is even reached. Merging PR #155 does not make the ARM gates runnable,
which is the card's whole purpose. RED, with the exact failing check below.

## Clause table

| # | Clause (verbatim from GB-099-V #164) | Result |
|---|---|---|
| 1 | `make ARCH=arm hobbyos.elf` links on the fix, and `make ARCH=intel hobbyos.elf` still links | **PARTIAL — RED on main.** At PR head `d7c3c5e`: ARM rc=0, intel rc=0. Merged with current `main` (`b874d54`): ARM FAILS (compile error, below). |
| 2 | ARM implementation emits the same record, matching the x64 field set; `nm obj/arm/trap.o \| grep -w resume_mark` shows `T` | **PASS** |
| 3 | `make ARCH=arm unit_tests_arm` -> UNIT TESTS PASSED (0 failed); `make host_tests` green | **PASS** at `d7c3c5e` |
| — | Self-test: pre-fix tip fails with `undefined symbol: resume_mark` | **PASS (control fires)** |

## The failing check (RED cell)

Current `main` merged with PR #155 (`9c632ea` = merge of `b874d54` x `d7c3c5e`, clean `ort` merge,
no conflicts), `make ARCH=arm hobbyos.elf`:

```
clang -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf ... -c src/kernel/process.c -o obj/arm/process.o
src/kernel/process.c:1827:27: error: use of undeclared identifier 'cpu_locals'
 1827 |           int live_pid = (cpu_locals[cl].current_proc)
      |                           ^~~~~~~~~~
src/kernel/process.c:1828:32: error: use of undeclared identifier 'cpu_locals'
 1828 |                              ? cpu_locals[cl].current_proc->pid
      |                                ^~~~~~~~~~
2 warnings and 2 errors generated.
make: *** [Makefile:744: obj/arm/process.o] Error 1
rc=2
```
(`evidence/gb-099-v/07-merged-main-arm-link.log`)

Same error, same lines, on **plain `origin/main` `b874d54` without PR #155** — so this is not
merge-induced and not caused by the fix:
```
$ make ARCH=arm hobbyos.elf          # rev b874d54
src/kernel/process.c:1827:27: error: use of undeclared identifier 'cpu_locals'
src/kernel/process.c:1828:32: error: use of undeclared identifier 'cpu_locals'
rc=2
```
(`evidence/gb-099-v/08-plain-main-arm-build.log`)

Cause, in the source: `cpu_locals` is declared **inside `#ifdef __x86_64__`**
(`src/kernel/process.c:355-358`), and the `[ALIVE-SET]` block added by `9ebfbbd` (PR #158, GB-057,
landed 06:05Z — *after* GB-099 was measured and PR #155 opened) reads it unconditionally at
`process.c:1827-1828`. It is the **same defect class as GB-099 itself**: an x64-only symbol used
from shared, both-arch code. PR #155 fixes the `resume_mark` instance; the `cpu_locals` instance
keeps every ARM gate dark.

Intel is unaffected, which is what hides it — on the merged tree `make ARCH=intel hobbyos.elf`
is rc=0, 0 errors (`evidence/gb-099-v/09-merged-main-intel-link.log`).

## What does pass (quoted)

At the PR head `d7c3c5e`:

```
$ make ARCH=arm hobbyos.elf
ld.lld -T linker.ld -o hobbyos.elf obj/arm/... obj/arm/trap.o obj/arm/uart.o
rc=0
```
(`evidence/gb-099-v/01-arm-link-fix.log`)

```
$ nm obj/arm/trap.o | grep -w resume_mark
0000000000000000 T resume_mark
$ nm obj/arm/process.o | grep -w resume_mark
                 U resume_mark
$ nm hobbyos.elf | grep -w resume_mark
00000000400bba30 T resume_mark
```
(`evidence/gb-099-v/02-nm-arm.log`) — defined, referenced by the shared scheduler path, and present
in the linked ARM image.

`make ARCH=intel hobbyos.elf` at the PR head: rc=0, 0 `error:` lines
(`evidence/gb-099-v/03-intel-link.log`).

```
$ make ARCH=arm unit_tests_arm
Tests run:    369
Tests failed: 0
UNIT TESTS PASSED
rc=0
```
(`evidence/gb-099-v/04-arm-unit-tests.log`)

```
$ make host_tests
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0
```
(`evidence/gb-099-v/05-host-tests.log`)

## Control — the gate can fail, and did

Pre-fix tip (PR #155's parent `d189a9b`), `make ARCH=arm hobbyos.elf`:
```
ld.lld: error: undefined symbol: resume_mark
>>> referenced by process.c:1687 (src/kernel/process.c:1687)
>>>               obj/arm/process.o:(schedule)
>>> referenced by process.c:3331 (src/kernel/process.c:3331)
>>>               obj/arm/process.o:(start_scheduler)
rc=2
```
(`evidence/gb-099-v/06-prefix-arm-link-fails.log`) — the card's RED reproduces exactly, so the
link leg is a real gate and not a tripwire that never fires.

## Non-blocking findings (not counted in the RED)

1. **The ARM record is written but never read.** The ARM copy of `resume_mark` is byte-identical to
   the x64 one (same `volatile int64_t resume_mark_pid[MAX_CPUS]` / `volatile uint64_t
   resume_mark_sp[MAX_CPUS]`, same `get_cpuid()` store, `src/kernel/arch/arm/trap.c` after the
   patch vs `src/kernel/arch/x64/trap.c:2271-2280`), so DoD 2's "matching the x64 field set" is met
   literally. But the only consumer is the **x64** watchdog stall dump
   (`src/kernel/arch/x64/trap.c:2365-2369`); nothing in `arch/arm/` prints
   `resume_mark_pid`/`resume_mark_sp`. The record therefore exists in ARM memory and is never
   surfaced — "GB-057's ARM resume diagnosis is not blinded" is true only in the weak sense
   (the data is stored). A one-line ARM dump alongside the existing ARM watchdog output would make
   the claim true in the strong sense. Not a DoD clause; noting it because the clause invokes
   GB-057's ARM diagnosis as its rationale.
2. **GB-057's PR #158/#160 were merged without an ARM build.** PR #158 (+18 in `process.c`,
   `9ebfbbd`) and PR #160 (+16, `b874d54`, same file) both landed at 06:05Z with a green x64 gate
   only. That is precisely the failure mode DoD 3 of GB-099 asks to close ("the merge gate builds
   **both** arches before it runs either") — the gate improvement does not appear to be in force
   yet for these two merges.

## Suggested fix for the RED (one change, either form)

- Move `struct cpu_local` / `extern struct cpu_local cpu_locals[]` out of `#ifdef __x86_64__`, or
- Provide an arch-neutral accessor next to `current_pid_of_cpu()` (e.g.
  `int current_pid_on_cpu(uint32_t)` implemented per arch, `-1` on ARM) and have the `[ALIVE-SET]`
  block call it, so the shared dump cannot name an arch-private symbol again.

Either way the gate that must go green is `make ARCH=arm hobbyos.elf` **on a tree merged with
`main`**, not on a PR branch 2 commits behind it.

## Reproduce

```
git worktree add /tmp/gb099v d7c3c5e
cd /tmp/gb099v && mkdir -p obj && ln -sfn <primary>/third_party/llvm-21.1.8/dist-staging \
    third_party/llvm-21.1.8/dist-staging
export PATH=/usr/sbin:$PATH
make ARCH=arm hobbyos.elf            # rc=0  (fix revision)
git merge origin/main                # clean, then:
make ARCH=arm hobbyos.elf            # rc=2  <-- cpu_locals, the RED
```
