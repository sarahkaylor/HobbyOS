# GB-084-V — verdict: GREEN

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-23.
Revision graded: **PR #95 head `83a379a`**, branch `ticket/084-open-mode` — the card's gate text does not
name a sha; the parent ticket's review comment (wrev, 20:22Z) does, and the work is **not** on
`origin/main` (`grep -rn mode_is_0600` on main → nothing). Graded in a clean detached worktree.

## 1. `make open_mode_scan` — 0 sites, exit 0

```
$ make open_mode_scan
open-mode scan: 0 two-argument O_CREAT open() calls in '/tmp/gb084/tools/gate/../../src'
case1 rc=0
```

## 2. Reverting one call site makes the scan FAIL, restoring passes

```
### before
2213:    if (r->mode == 0) fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
### revert (one site, src/user/sh.c)
reverted: open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644) -> open(fname, O_WRONLY | O_CREAT | O_TRUNC)
open-mode scan: 1 two-argument O_CREAT open() calls in '/tmp/gb084/tools/gate/../../src'
  /tmp/gb084/tools/gate/../../src/user/sh.c:59646
make: *** [Makefile:3470: open_mode_scan] Error 1
case2 rc=2

### restore
open-mode scan: 0 two-argument O_CREAT open() calls …
restored rc=0
```

The scan is not vacuous: one reverted site is caught and the make target fails.

Small defect to fix later: the reported position `sh.c:59646` is a **byte offset**, not a line number
(the file is ~2.2k lines). It names the file, so the failure is actionable, but the number will mislead.

## 3. `ho_open` honours a mode — `mode_is_0600 rc=1` on **both** legs

```
$ bash src/host/t4_parity.sh
t4 host differential: A=0 B=0 cases=3
t4_parity: PASS (host differential locked; device leg gated)
case3 rc=0

# the rows themselves, both legs built by hand (the harness prints A/B only on mismatch):
legA (musl -static):  mode_is_0600 rc=1
legB (clang -DHOST_TEST + compat.c): mode_is_0600 rc=1
```

Both legs agree on the row, and `rc=1` is the probe's "mode read back is 0600" answer, so a 2-arg call
(path that would have read garbage) is excluded on the model leg too.

## 4. Full `host_tests` green, with the scanner wired in (not skipped)

```
$ make host_tests
… open-mode scan: 0 two-argument O_CREAT open() calls …        (suite log line 770)
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
host_tests EXIT=0
```

`OPEN_MODE_SCAN` is a real prerequisite of `host_tests` (`Makefile:3555`), and its output appears in the
suite run, so the check is executed rather than built-and-forgotten.

## Pass criteria

| # | criterion | result |
|---|---|---|
| 1 | scan reports 0 two-arg `O_CREAT` sites, exit 0 | **PASS** |
| 2 | reverting one site FAILs the scan; restore passes | **PASS** (rc=2 on the revert, rc=0 restored) |
| 3 | `mode_is_0600 rc=1` on both differential legs | **PASS** (both legs, rows quoted) |
| 4 | full `host_tests` green, scanner wired in | **PASS** (482/0, scan line in the suite log) |

## Scope

The green covers `ho_open`'s mode handling and the 2-arg `O_CREAT` sweep on the PR head. It does **not**
touch a HobbyOS guest — this is the host differential and the host suite. `origin/main` still lacks the
change; Merge Gate must grade the merged tree, and a fresh `GB-084-V` should grade `main` afterwards.

Incidental observation: this branch's `host_tests` now carries `T4_COMPAT_SELFTEST` in its prerequisite
list — the self-test GB-007-V reported as missing has since landed, which closes that earlier gap.
