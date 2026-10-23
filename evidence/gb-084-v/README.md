# GB-084-V — verification evidence (DEEP Verifier, 2026-10-23)

Subject: GB-084 (#135) — verdict **GREEN**. Revision: **`83a379a`** on `ticket/084-open-mode` (PR head).
Fresh worktree `/tmp/gb084`, tree clean (`git status --porcelain` → 0) at every measurement.

| # | file | what |
|---|---|---|
| 01 | `01-scan.log` | `make open_mode_scan` → 0 two-arg O_CREAT calls, exit 0 |
| 02 | `02-scan-reverted.log` | one call site reverted to 2-arg → 1 call reported, non-zero |
| 03 | `03-scan-restored.log` | restore → 0, exit 0 |
| 05 | `05-parity.log` | `bash src/host/t4_parity.sh` → PASS |
| 06 | `06-parity-ho-open-lie.log` | `ho_open` made to ignore the mode → leg B `mode_is_0600 rc=0`, parity **FAIL** |
| 07 | `07-host-tests-clean.log` | `make host_tests` → `ALL APPS SUITE TESTS PASSED`, exit 0 |
| — | `pa84.out`, `pb84.out` | the two legs' stdout, 82 rows each |

## 1. Scanner reports 0 and exits 0
```
$ make open_mode_scan
open-mode scan: 0 two-argument O_CREAT open() calls in '/tmp/gb084/tools/gate/../../src'
rc=0
```

## 2. It can provably fail
Reverted `src/user/sh.c:2213` to `open(fname, O_WRONLY|O_CREAT|O_TRUNC)`:
```
open-mode scan: 1 two-argument O_CREAT open() calls in '…/src'
  /tmp/gb084/tools/gate/../../src/user/sh.c:59646
make: *** [Makefile:3470: open_mode_scan] Error 1        make rc=2
```
Restored (`git checkout -- src/user/sh.c`, 0 changes) → `0 … calls`, rc=0.

Small defect worth a tidy-up mint: the reported location `sh.c:59646` is a **byte offset**, not a line number
(the call is at line 2213). The scanner names the file and proves the failure, but a reader cannot jump to it.

## 3. `ho_open` honours the supplied mode — and the row is causally about it
```
$ bash src/host/t4_parity.sh
t4 host differential: A=0 B=0 cases=3
t4_parity: PASS (host differential locked; device leg gated)          rc=0

leg A (musl -static) : mode_is_0600 rc=1
leg B (clang + compat.c, -DHOST_TEST) : mode_is_0600 rc=1
82 rows each, byte-identical
```
`A`/`B` are the legs' exit statuses; `cases=3` is two captures + one compare. The row is not decoration:
at this sha the probe includes `extern int ho_open(const char *, int, ...)` and `#define open ho_open`
(`t4_compat_probe.c:52,59`), so leg B really does route through the shim. To prove the row grades
**ho_open's** mode handling and not glibc's, I made `ho_open` ignore the va_arg mode:
```
FAIL: stdout differs (legA_musl vs legB_host)
  A: …|mode_is_0600 rc=1|…
  B: …|mode_is_0600 rc=0|…
t4_parity: FAIL                                                       rc=1
```
Restored → PASS. So the 0600 clause fails precisely when the supplied mode is dropped — the check has teeth.

## 4. Full host suite green, with the scanner and probe wired in
```
$ make host_tests
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0                                                          rc=0
```
Wiring: `host_tests: … $(OPEN_MODE_SCAN) $(T4_COMPAT_PARITY) …` (`Makefile:3555`) and
`merge_gate.sh:105` runs `make host_tests` as T1 — not a skipped or disabled check.

**One honesty note about a log in this directory:** `04-host-tests.log` records a **FAIL** (`rc=2`). That was
my own interference — I patched `compat.c` for the lie test while that background suite was still running, and
it hit `t4_compat_parity` mid-patch. It is not a property of the revision; `07` is the clean serial re-run of
the same command on a verified-clean tree.

## Verdict
GREEN. All four DoD clauses measured on `83a379a`: 0 sites, a fail-on-revert with the offending file named,
`mode_is_0600 rc=1` on both legs with a lie test proving the row depends on `ho_open`, and `host_tests` green
with both instruments in the dependency list.
