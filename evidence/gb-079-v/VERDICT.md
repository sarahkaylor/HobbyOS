# GB-079-V (re-open) — verdict: GREEN

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-23.
Revision graded: **`9e107c3`** (PR #79 head, branch `ticket/079-t4-both-modes`) — *not* `origin/main`,
which is what this card specifies. Worktree: fresh `git worktree add --detach /tmp/gb079 9e107c3`,
clean before and after (`git status --porcelain` empty at case 6).

The discriminator this card exists for — clause 3, an injected row in the **default** mode — is
non-zero. Every other case lands where the card says it must.

## Matrix (each case from a clean tree, in order)

| case | what | want | got |
|---|---|---|---|
| 0 | `--self-test` | 0, `fail_count=0` | **rc=0**, `fail_count=0` |
| 1 | healthy / leg A absent | 0 | **rc=0**, `PASS (mode=source)` |
| 2 | healthy / leg A present | 0 | **rc=0**, `PASS (mode=source+emitted)` |
| 3 | **injected unconditional row, leg A present** | **non-zero** | **rc=1** |
| 4 | same injection, leg A absent | non-zero | **rc=1** |
| 5 | stale capture, one row dropped | non-zero | **rc=1** |
| 6 | restore, `--self-test` | 0, `fail_count=0` | **rc=0**, `fail_count=0` |

## Case 3 — the gate, quoted in full

```
### 3. INJECT an unconditional row, leg A present (THE GATE)
injected at line 85
85:  prow("bogus_unconditional", 7, 0, NULL, 0);
=== t4_device --check: row-name set parity (GB-073, GB-079) ===
  golden rows: 39
  --- mode=source: device probe rows: 40
  … 39 PASS rows …
  FAIL: device probe emits extra row 'bogus_unconditional' not in golden [mode=source]
  --- mode=emitted: device probe rows: 39
  … 39 PASS rows …
  mode=emitted: PASS
  FAIL: recorded leg A is STALE — it disagrees with the device probe's source:
        @@ -1,4 +1,3 @@
        -bogus_unconditional
         dent_created
        …
t4_device --check: FAIL (mode=source+emitted)
case3 rc=1
```

Two independent detections of the same edit in the default mode: the row-set comparison
(`extra row … not in golden`) **and** the staleness check against the recorded capture. That is the
behaviour GB-079 was re-opened for, and it is the behaviour the previous closed card never exercised.

## The other cases, quoted at their verdict lines

```
case1: t4_device --check: PASS (mode=source) — … leg A ('/tmp/gb079/obj/t4_legA.out') absent — emitted mode not graded
case2: t4_device --check: PASS (mode=source+emitted) — both extractions carry exactly the golden row-name set and agree
case4: t4_device --check: FAIL (mode=source)          (injected row, no leg A)
case5: FAIL row: dent_have_charlie (device=0 host=1) [mode=emitted] — a row the M1 -V DoD names is missing
       FAIL: recorded leg A is STALE — it disagrees with the device probe's source
case6: self-test result: fail_count=0
```

The verdict line names the mode(s) graded in each case (`mode=source` / `mode=source+emitted`), as the
card requires, so no reader has to infer which extraction answered. The self-test now carries GB-079's
own clauses explicitly:

```
[self-test] GB-079 (a probe edit must FAIL in both --check modes):
  OK — healthy probe PASSES (leg A absent)
  OK — injected probe row FAILS (leg A absent)
  OK — healthy probe PASSES (leg A present)
  OK — injected probe row FAILS (leg A present)
self-test result: fail_count=0
```

## Scope statements

* This verdict is about **probe/golden/host-model agreement**, per the card. It says nothing about guest
  syscall behaviour, and it does not re-open GB-073 or GB-076 (both stay Done).
* The fix is **on the PR head only**. `origin/main` still has the old `--check`; the Merge Gate verdict
  must be taken on the merged tree, and a fresh `GB-079-V` should grade `main` after the merge.
* `make obj/t4_probe.elf` (used by sibling `-V` cards) is still not a target — the working form is
  `make ARCH=intel obj/intel/t4_probe.elf`. Not this card's business, but it is the same class of
  dead-path gate text.
