# GB-010-V — verification evidence (DEEP Verifier, 2026-10-22)

Subject: GB-010 (#14) — docs/linux-abi.md + the drift gate. Verdict **GREEN**.
Revision: `9f2c3ea` on `hermes/hermes-65d64814` (the branch the ticket names), fresh worktree at that commit,
clean tree before and after the run (`git status --short` → 0 lines).

| # | file | what it is |
|---|---|---|
| 01 | `01-pass.log` | `make abi_doc_check` on the shipped doc → exit 0, `docs/linux-abi.md matches a fresh ABI probe` |
| 02 | `02-mutation.log` | the ticket's own mutation command as written — **a no-op**, see finding 1 |
| 02b | `02-mutation-real.log` | a real mutation (`read 0` → `read 9` in the PROBE block) → exit 1, `docs/linux-abi.md is STALE` |
| 03 | `03-restored.log` | `git checkout -- docs/linux-abi.md`, re-run → exit 0 |
| 04 | `04-merge-gate.log` | `GATE_T1=0 bash tools/gate/merge_gate.sh` → `DOCS_abi PASS`, `GATE: PASS`, rc 0 |
| — | `linux-abi.md.orig` | the exact doc bytes the pass run used |

## Results

```
$ make abi_doc_check            -> exit 0    docs/linux-abi.md matches a fresh ABI probe
$ sed -i 's/^  read                   0$/  read                   9/' docs/linux-abi.md
$ make abi_doc_check            -> exit 1    docs/linux-abi.md is STALE — re-run tools/linux-abi/gen_abi_doc.py
                                    make: *** [Makefile:3350: abi_doc_check] Error 1
$ git checkout -- docs/linux-abi.md
$ make abi_doc_check            -> exit 0    docs/linux-abi.md matches a fresh ABI probe
$ GATE_T1=0 bash tools/gate/merge_gate.sh
  [GATE] DOCS_abi: python3 tools/linux-abi/gen_abi_doc.py --check
  [GATE] DOCS_abi: PASS
    DOCS_abi         PASS
  GATE: PASS (no required tier failed; skipped tiers are reported above)     rc=0
```

Wiring (DoD 3), read at the same sha:
- `ABI_DOC_CHECK` is in the `host_tests` prerequisite list (`Makefile:3420`), with its own rule at 3349
  depending on the doc, the generator and `probe_abi.c` — so a stale doc fails `host_tests`, not just CI.
- `tools/gate/merge_gate.sh:79` — `if [ "${GATE_DOCS:-1}" = "1" ]` — the docs tier is **on by default**
  with `GATE_DOCS=0` as the escape hatch, exactly as the ticket describes.

## Finding 1 — the ticket's mutation command is a no-op (record it, do not follow it blindly)

Step 1 of the gate says:

```
grep -q 'read' docs/linux-abi.md && sed -i 's/#define __NR_read 0/#define __NR_read 9/' docs/linux-abi.md
```

`docs/linux-abi.md` contains **no** `#define __NR_read` line — that is probe/header syntax, not doc syntax.
The `grep -q 'read'` succeeds (the word appears everywhere), the `sed` matches nothing, and the check then
passes. `02-mutation.log` is that run: `MUTATION_RESULT=ZERO`, i.e. the "mutation" mutated nothing.
Anyone taking that output as "the gate cannot fail" would be wrong — the gate can fail, as `02-mutation-real.log`
shows. The mutation has to be applied to the generated table row (`  read                   0`), which is what
`gen_abi_doc.py --check` actually compares.

## What this GREEN does and does not say

It says the doc on `9f2c3ea` is not stale against a fresh probe, that the checker fails on a real ABI change,
and that the check is wired into both `host_tests` and the merge gate by default. It does not say anything about
whether the documented contract is *complete* or correct for musl — that is the doc's content review, not this gate.
