# GB-104-V — VERDICT: **RED** (clauses 1, 2, 4, 5 pass; clause 3 fails — the pruner deletes uncommitted work)

Verifier: `wverify-deep`, DEEP tier, swarm-farm-3, 2026-10-26 12:5x–13:1xZ.
Verifies GB-104 (#191) / ship **PR #197 @ `d222dae`** (3 files, +142/−0). Host-side gate, local only; no
CI consulted. I did not author GB-104 or PR #197.

## One line

The resume-cwd guard does exactly what the card asks — **one** named error, silent on the happy path, and
a self-test that fails when the guard is broken — but the **pruner deletes a directory that contains
uncommitted work** without refusing and without naming a reason. The card says that is "worse than the
bug", so this is a RED on clause 3 alone.

## Clause table

| # | Clause | Result |
|---|---|---|
| 1 | one error, not N: exactly one message naming the dead path, exit 1, no repeat per command | **PASS** — count = 1 across a 5-command session |
| 2 | healthy path silent, exit 0 | **PASS** — 0 bytes of output, exit 0 |
| 3 | pruner safe: (a) live worktree untouched, (b) unlisted dir removed, (c) uncommitted work **refused with a named reason** | **FAIL** — (a) and (b) hold, (c) is **deleted** |
| 4 | self-test registered and executed by `host_tests`, and fails when the guard is broken | **PASS** — discovered by `run_t5_tests.py`, break ⇒ 3 failures |
| 5 | nothing else regresses | **PASS** — `make host_tests` 482/0, `git worktree list` 15 → 15 |

## Clause 3 — the failing cell, measured (`03-pruner-safety.log`)

Scratch repo with (a) a registered live worktree, (b) an unregistered directory with no work, (c) an
unregistered directory containing an uncommitted file:

```
=== before ===
worktree /tmp/gb104-scratch
worktree /tmp/gb104-scratch/.worktrees/live
.worktrees/dirty/   .worktrees/live/   .worktrees/orphan/
dirty file present: YES

$ bash tools/swarm/prune-stale-worktrees.sh /tmp/gb104-scratch
PRUNE-STALE-WORKTREES: removed stale worktree .worktrees/dirty
PRUNE-STALE-WORKTREES: removed stale worktree .worktrees/orphan
PRUNE-STALE-WORKTREES: ready (git pruned; removed=2 stale worktree dirs)
pruner rc=0

=== after ===
worktree /tmp/gb104-scratch
worktree /tmp/gb104-scratch/.worktrees/live
.worktrees/live/
a) live worktree dir present:      YES
a) live worktree LIVE.txt present: YES
b) orphan dir present:             NO
c) dirty dir present:              NO
c) dirty work.txt present:         NO      <-- uncommitted work destroyed
```

**Cause:** the script's only test is `git worktree list --porcelain | grep "\.worktrees/$name$"`. A
directory git does not list is treated as stale and `rm -rf`'d, so "(b) orphaned checkout" and "(c)
someone's uncommitted work in an unregistered directory" are indistinguishable to it — the case the card
singles out ("a pruner that deletes uncommitted work is worse than the bug") is the case it gets wrong.
Its self-test does not cover it either: the 9 checks include "stale (orphaned) worktree dir removed" and
"live/git-listed worktree kept", no uncommitted case, which is why clause 4 passes while the behaviour
fails.

**Fix direction (not applied):** before removing, require positive evidence the directory *is* a hermes
worktree (its `.git` file's `gitdir:` pointer resolves inside this repo), and refuse anything with a
non-empty `git status --porcelain` / untracked files, printing a named reason
(`PRUNE-STALE-WORKTREES: REFUSED .worktrees/<name> — uncommitted work (N paths); remove by hand`).

## Clause 1 — one error, not N (`01-02-guard.log`)

```
$ tools/swarm/resume-cwd-guard.sh /tmp/gb104-dead-cwd-170305
RESUME-CWD-GUARD: cannot resume into working directory '/tmp/gb104-dead-cwd-170305' — it no longer exists (likely pruned/rotated). Start a fresh session instead.
guard exit=1
cmd1 ok … cmd5 ok
--- count of RESUME-CWD-GUARD lines across the whole session ---
1
```
One line, naming the path, exit 1, and five further commands in the simulated session produce no repeat.

## Clause 2 — happy path silent (`01-02-guard.log`)

```
$ tools/swarm/resume-cwd-guard.sh /home/sarah/wt/gb104v
guard exit=0
output bytes=0
```

## Clause 4 — registered, executed, and can fail (`04b`, `04c`)

```
$ python3 tools/t5/run_t5_tests.py | grep -iE "resume_guard|host-tests"
  PASS  tools/swarm/resume_guard_test.py
[host-tests] done: 24/24 passed
```
`make host_tests` invokes `python3 tools/t5/run_t5_tests.py` (Makefile:3647), which globs
`tools/*/*_test.py`, so the new test is executed by the suite rather than merely added (the GB-064/GB-080
trap).

Deliberate break — the dead-cwd check removed so the guard always exits 0:
```
  PASS: existing cwd -> guard exit 0 (normal resume succeeds)
  FAIL: deleted cwd -> guard exits non-zero
  FAIL: deleted cwd -> EXACTLY ONE startup error (got 0)
  FAIL: the single error NAMES the path (not generic)
RESUME_GUARD_TEST: FAIL (3 failure(s))     self-test rc=1
```
restored:
```
RESUME_GUARD_TEST: PASS - one startup error for a dead cwd, normal resume succeeds, stale worktrees pruned
self-test rc=0
```

## Clause 5 — nothing else regresses (`05b-host-tests-summary.log`)

```
$ make host_tests
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0
$ git worktree list | wc -l    15 (before) -> 15 (after)
```

**Disclosure of my own environment error:** my first `make host_tests` failed with
`src/host/rtti_test.cpp:29:10: fatal error: 'typeinfo' file not found` (rc=2) — because I had not created
the `third_party/libcxx-21.1.8/src` symlink in this fresh worktree. That was my setup, not the PR; after
adding the symlink the suite is 482/0. Both logs are committed (`05-…` and `05b-…`) so the false start is
visible.

## Reproduce

```
git worktree add /tmp/gb104v d222dae && cd /tmp/gb104v
ln -sfn <primary>/third_party/libcxx-21.1.8/src third_party/libcxx-21.1.8/src
tools/swarm/resume-cwd-guard.sh /nonexistent-dir ; echo rc=$?         # one line, rc=1
tools/swarm/resume-cwd-guard.sh "$PWD" ; echo rc=$?                   # silent, rc=0
# clause 3: scratch repo with a live worktree + an orphan dir + a dir holding uncommitted work
python3 tools/swarm/resume_guard_test.py && make host_tests
```