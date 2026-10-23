# GB-079-V — verification evidence (DEEP Verifier, 2026-10-23)

Subject: GB-079 (#123) "t4_device.sh --check is mode-independent" — verdict **RED** (clause 3 fails in the
mode the gate picks by default).
Revision: **`82ecc29`** on `ticket/079-t4-check` (PR #77 — the fix is not on `origin/main`; this verdict
covers the branch head). Worktree fresh; tree clean before and after (`git status --short` → empty).

| # | file | what it is |
|---|---|---|
| 01 | `01-source-mode.log` | `--check` with no `obj/t4_legA.out` → PASS, 39 rows, `mode=source` |
| 02 | `02-absent-lega.log` | `T4_LEGA_OUT=/nonexistent/x.out --check` → PASS, `mode=source` |
| 03 | `03-legA-build.log` | `make ARCH=intel obj/intel/t4_legA.out` → exit 0, 39 rows |
| 04 | `04-emitted-mode.log` | same tree, leg A copied to the default `obj/t4_legA.out` → PASS, 39 rows, `mode=emitted` |
| 05 | `05-injected.log` | injected unconditional `prow("bogus_unconditional", …)` → **PASS** in emitted mode (the defect) |
| 06 | `06-injected-source-mode.log` | same injection, source mode → **FAIL**, 40 rows, names the row |
| 07/08 | `07-restored-emitted.log`, `08-restored-source.log` | restored probe → PASS in both modes |
| 09 | `09-selftest.log` | `--self-test` → `fail_count=0` |
| — | `t4_probe.c.orig` | the exact probe source the pass runs used |

## Clause by clause

**1. Same verdict, same row count with and without leg A — MET.**
```
no leg A      : device probe rows: 39 (mode=source)    PASS      exit 0
leg A present : device probe rows: 39 (mode=emitted)   PASS      exit 0
```
**2. Mode printed in the verdict line — MET** (`PASS (mode=source)` / `PASS (mode=emitted)`).

**3. The check can provably fail on an injected row — MET ONLY IN SOURCE MODE.**
```
$ python3 … inject prow("bogus_unconditional", 7, 0, NULL, 0) into src/user/t4_probe.c
$ bash tools/gate/t4_device.sh --check                      # leg A present → emitted mode
  device probe rows: 39 (mode=emitted)
  t4_device --check: PASS (mode=emitted)                    exit 0    <-- injection invisible
$ T4_LEGA_OUT=/nonexistent/x.out bash tools/gate/t4_device.sh --check
  device probe rows: 40 (mode=source)
  FAIL: device probe emits extra row 'bogus_unconditional' not in golden
  t4_device --check: FAIL (mode=source)                     exit 1    <-- caught
```
So the injected row is caught when the gate reads the **source** and ignored when it reads the saved
**artefact**. `mode=emitted` is selected whenever `obj/t4_legA.out` exists — which is the normal state
after any leg-A build, including the one this very gate documents in its own step 2.

## Why this is RED rather than a note

The card exists because the check changed verdict with the presence of a build artefact (GB-079 title).
On `t4_device.sh` at `82ecc29`, the *verdict* on an unchanged tree is now artefact-independent, but the
*failure detection* still is: a stale `obj/t4_legA.out` makes `--check` blind to a probe change, and
that stale file is the artefact the gate itself produces. DoD clause 3 says the injected row must make
`--check` FAIL, unqualified; in the default emitted mode it does not. A check that cannot fail on the
tree it is watching is the "silent pass" this program keeps paying for.

## One-line fix that would make it GREEN

Either (a) run **both** modes and require both to PASS (source catches probe edits, emitted catches
guest-vs-golden drift), or (b) treat leg A as stale — refuse emitted mode, or re-emit it — when
`obj/t4_legA.out` is older than `src/user/t4_probe.c`. (a) is one loop and needs no timestamps.
