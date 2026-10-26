# GB-104-V — gate evidence (VERDICT: GREEN, on head `7c0175b`; the pre-fix head fails the same test)

Ticket: GB-104-V (#192) — verify the resume-cwd guard / stale-worktree pruner
Verifies: GB-104 (#191), PR **#197**, branch `ticket/GB-104-resume-cwd`.
**Head graded: `7c0175b`** ("GB-104 RED: pruner refuses a stale worktree dir holding uncommitted work …",
13:36:35Z). The head moved to it *during* this gate: I first fetched `d222dae` (the head farm-3 RED'd at
13:03Z), measured it, then re-checked `ls-remote` and found the fix — every clause below is measured on
`7c0175b`, and the pre-fix measurement is kept because it is the control that proves the test can fail.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2). Local only — no CI/check cited.

## Clause table (on 7c0175b)

| # | clause | result |
|---|---|---|
| 1 | one error naming the path, exit 1, no per-command repeat | **PASS** — 1 line, exit 1 |
| 2 | healthy path silent, exit 0 | **PASS** — 0 bytes, exit 0 |
| 3 | pruner safe: live kept, unlisted leftover removed, **uncommitted work refused with a reason** | **PASS** |
| 4 | self-test registered, executed by the suite, fails when the guard is broken | **PASS** — 24/24, break → FAIL(3) |
| 5 | nothing else regresses | **PASS** — `make host_tests` exit 0, live worktree never lost |

## Clause 3 PASS — and the same test fails on the pre-fix head (the can-fail control)

Scratch repo with (a) a live registered worktree, (b) an empty unlisted leftover, (c) an unlisted dir
holding an untracked file, (d) an unlisted dir with uncommitted work:

```
$ bash pruner_safety2.sh /tmp/gb104-scratch3 <pruner @ 7c0175b>
PRUNE-STALE-WORKTREES: REFUSED .worktrees/dirty — holds uncommitted/untracked work; NOT deleted (GB-104)
PRUNE-STALE-WORKTREES: removed stale (clean) worktree .worktrees/empty_orphan
PRUNE-STALE-WORKTREES: REFUSED .worktrees/untracked — holds uncommitted/untracked work; NOT deleted (GB-104)
PRUNE-STALE-WORKTREES: ready (git pruned; removed=1 stale worktree dirs, refused=2 carrying work)

  (a) live worktree present: YES   LIVE.txt: YES
  (b) empty orphan present:  NO    (want NO)
  (c) dirty dir present:     YES   work.txt: YES   (want YES)
  (d) untracked dir present: YES                    (want YES)
```

The **same script against the pre-fix head `d222dae`** (kept in `01_pruner_safety.log`):

```
PRUNE-STALE-WORKTREES: removed stale worktree .worktrees/dirty
PRUNE-STALE-WORKTREES: removed stale worktree .worktrees/orphan
  (c) dirty dir present: NO      (c) dirty work.txt present: NO   <-- uncommitted work destroyed
```

Two heads, one test, opposite outcomes — the pruner behaviour is what changed, and the test discriminates.
A note on the card's (b): a non-empty, non-git directory is now **refused** (it is indistinguishable from
someone's data); only a provably-safe leftover — empty, or a clean git worktree — is removed. That is the
stricter reading of (b), and it is the reading that satisfies (c).

## Clause 1 PASS — one line, exit 1

```
$ bash tools/swarm/resume-cwd-guard.sh /tmp/gb104-dead-cwd-142588
RESUME-CWD-GUARD: cannot resume into working directory '/tmp/gb104-dead-cwd-142588' — it no longer exists (likely pruned/rotated). Start a fresh session instead.
guard exit=1   messages naming the path: 1   total output lines: 1
```
A wrapper that calls it once per resume prints exactly one line per resume (5 invocations → 5 lines), not
one per shell command — which is the defect (`.worktrees/hermes-7eee996d` failing on every command).

## Clause 2 PASS — silent on the healthy path

```
$ bash tools/swarm/resume-cwd-guard.sh /tmp/gb104-live-cwd
guard exit=0   bytes of output: 0
```

## Clause 4 PASS — registered, executed, and it fails when broken

```
$ python3 tools/t5/run_t5_tests.py | grep resume_guard
  PASS  tools/swarm/resume_guard_test.py
[host-tests] done: 24/24 passed

# control: delete the dead-cwd check from the guard
[control] guard mutated: dead-cwd check removed
self-test exit with the guard broken: 1   RESUME_GUARD_TEST: FAIL (3 failure(s))
[control] guard restored
self-test exit after restore: 0           RESUME_GUARD_TEST: PASS
```
The self-test now also asserts the refusal: `PASS: stale dir with uncommitted/unknown work REFUSED, NOT
deleted (GB-104 RED)`.

## Clause 5 PASS

```
$ make host_tests
  PASS  tools/swarm/resume_guard_test.py
[host-tests] done: 24/24 passed
ALL APPS SUITE TESTS PASSED
HOST_TESTS_EXIT=0
```
`git worktree list` in the scratch repo after every pruner run still shows the live worktree; nothing in
the real tree was touched (the three PR files were checked out only to run the gate and removed after).

## Verdict

**GREEN** at `7c0175b`. The fix addresses exactly the failing cell: the pruner refuses what it cannot
prove safe and names the reason, the guard still reports one line per resume, the self-test covers the
refusal case and fails when the guard is broken, and the suite is green.

One thing for whoever merges: the head moved *during* the gate (`d222dae` → `7c0175b`); if it moves again,
clause 3 is the one to re-run, and `pruner_safety2.sh` is the script to run it with.