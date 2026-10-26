# GB-012-V2 — gate evidence (VERDICT: GREEN, with one card-text defect named)

Ticket: GB-012-V2 (#186) — re-gate the re-scoped GB-012 (selfhost kernel core merged behind arch guards)
Graded tree: `origin/main` @ `20084d8` (the re-scope commit) — worktree `3314507` = `20084d8` + this
evidence; `git diff --stat 9ed2059 HEAD -- src/kernel/{process,vm,fs,process_test,vm_test}.c` is empty,
i.e. no source difference between the graded tree and the coordinator's reference point.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2). I authored neither GB-012 nor its
code. Local gate only — no GitHub check/status result is cited, per the 2026-10-26 CI policy.

Re-scoped DoD being graded: *the selfhost core is merged behind arch guards, and **T2 is green on both
arches**; T3 parity is tracked by GB-058/081/093/095/102, not here.*

## Clause table

| # | clause | result |
|---|---|---|
| 1 | presence by name: futex, memfd/MAP_SHARED, ARCH_SET_FS, sigreturn | **PASS** (4/4) |
| 2 | T2 both arches, counts quoted | **PASS** — intel 374/374, ARM 370/370 |
| 3 | `vm_test`, `process_test`, `memfd_test`, `thrd_test` observed running | **met in substance at the tiers where each lives**; the literal "in (2)" is unsatisfiable — see below |
| 4 | carry-over claim (`git diff --stat 9ed2059 origin/main` on the five files empty) | **PASS** — reproduced |

## Clause 1 PASS — presence by name

```
$ grep -c futex src/kernel/process.c                      -> 20
$ grep -rn "memfd_create\|MAP_SHARED" src/kernel/vm.c     -> 185,186,686,1344,1348 (VM_MAP_SHARED handling, memfd-backed object)
$ grep -n "ARCH_SET_FS" src/kernel/arch/x64/trap.c        -> 2008: tf->regs[0] = (code == 0x1002ULL) /* ARCH_SET_FS */
$ grep -rn "sigreturn" src/kernel/arch/x64/trap.c src/kernel/process.c
   src/kernel/arch/x64/trap.c:2187:    process_sigreturn(tf);
   src/kernel/process.c:2782:int process_sigreturn(struct trap_frame *tf) {
```

## Clause 2 PASS — T2 both arches, built here

```
$ ./run_unit_tests_intel.sh
Tests run:    374
Tests failed: 0
UNIT TESTS PASSED
INTEL_EXIT=0

$ ./run_unit_tests.sh
Tests run:    370
Tests failed: 0
UNIT TESTS PASSED
ARM_EXIT=0
```
Both match the coordinator's figures (intel 374/374, arm 370/370).

## Clause 3 — the suites do run, but two of the four are not T2 suites

What actually executes in T2 (from the intel console, 176 `Running test_` lines):

```
process_test_suite:   8 cases   (22 `futex` references in src/kernel/process_test.c)
vm_test_suite:       12+ cases  (vm_as_lifecycle, vm_region_list, vm_map_grow_commit,
                                 vm_windows_distinct, vm_hole_find, vm_demand_fault,
                                 vm_walk_dead_as_safe, vm_mmap_family, vm_shared_objects, …)
```
where `vm_shared_objects` is the memfd/MAP_SHARED materialisation case.

The other two the card names do not exist at T2, by file location:
```
$ git ls-files | grep -i memfd   -> src/host/memfd_test.c        (T1 host test, not a kernel suite)
$ git ls-files | grep -i thrd    -> src/user/thrd_test.c         (T3 userland binary THRD_T.BIN)
$ grep -an "thrd_test" evidence/gb-012-v2/03_unit_intel.log
  88:mcopy -i disk.img obj/intel/thrd_test.bin ::/THRD_T.BIN
```
At their own tiers, measured here:
```
$ ./memfd_test_host
memfd_test_host: 6 checks, 0 failed            (incl. "child MAP_SHARED mapping sees the parent's write")
$ make host_tests   -> ALL APPS SUITE TESTS PASSED, TEST EXIT: 0, 17 THRD_T rows
```
So clause 3's *purpose* — "not a tier that passed while these never ran" — is met: every one of the four is
covered, at the tier where it exists. The *literal* wording ("as run tests in (2)") cannot be satisfied by
any tree, because two of the four named suites are not T2 suites. That is a card-text defect, not a
GB-012 defect, and I am not failing the card for it; the next re-plan should correct the clause to name
each suite with its tier.

## Clause 4 PASS — the carry-over claim holds on current main

```
$ git diff --stat 9ed2059 origin/main -- src/kernel/process.c src/kernel/vm.c src/kernel/fs.c \
                                          src/kernel/process_test.c src/kernel/vm_test.c
(empty)          EXIT=0
```
and the same against this gate tree — also empty.

## Negative controls (the gate can fail)

* the carry-over check is not vacuous: against an older sha (`5b3159d`) the same command reports
  `process.c | 207 +`, `process_test.c | 67 +` — 274 insertions;
* the presence grep discriminates: `grep -c epoll_ctl src/kernel/process.c` → 0, `futex` → 20;
* the T2 harness exits non-zero on `UNIT TESTS FAILED` / timeout (`run_unit_tests_intel.sh` lines 26, 35,
  59-63, 72), so "PASSED" is a verdict, not a print.

## Verdict

**GREEN.** All four presence checks positive, both T2 tiers green with counts quoted on a tree I built
myself, the four named subsystems' coverage observed, and the carry-over claim reproduced rather than
assumed. The one defect is in the card's clause-3 wording (two of its four suites are T1/T3, not T2) —
reported here so the Coordinator can correct the text instead of a future verifier rediscovering it.

Lane moves: **GB-012 → Merge Gate**, **GB-012-V2 → Done**.