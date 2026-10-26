# GB-106-V — gate evidence (VERDICT: GREEN)

Ticket: GB-106-V (#201) — verify `tools/board/merge_pr.py` fail-closed merge guard
Verifies: GB-106 (#200), ship = PR **#204** head `68982dc` (`tools/board/merge_pr.py` 156 lines +
`tools/board/merge_pr_test.py`). Runs host-side, T1 only — no QEMU, no CI.

Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2).

## Clause table

| # | clause | result |
|---|---|---|
| 1 | `--self-test`: fail-closed rule, override, **#194 goes red** | **PASS** |
| 2 | live guard refuses a no-sha merge and a mismatched merge, named reason, exit 1 | **PASS** (both) |
| 3 | override writes a durable record and is the only way through | **PASS** |
| — | can-fail control: the self-test fails when the guard fails open | **PASS** — 2 mutations, both caught |

## Clause 1 PASS — the self-test

```
$ python3 tools/board/merge_pr.py --self-test
  PASS: graded==head -> allow (ok: graded sha aaaaa)
  PASS: graded!=head -> refuse (REFUSE: graded sha b)
  PASS: no graded sha -> FAILS CLOSED (REFUSE: GREEN names )
  PASS: no head -> refuse (REFUSE: no current h)
  PASS: parses a 40-hex sha from a VERDICT/graded line
  PASS: no sha in VERDICT -> empty (fails closed upstream)
  PASS: today's PR #194 has no recorded graded sha -> RED/fail-closed (REFUSE: GREEN names no graded )
MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red
exit 0
```

## Clause 2 PASS — the live guard, on a real PR

```
$ python3 tools/board/merge_pr.py --pr 202
[merge_pr] PR #202: REFUSE: GREEN names no graded sha — cannot confirm the graded tree is the current head d706c8cd0714
[merge_pr] NOT MERGED (fail closed)                                    [exit 1]

$ python3 tools/board/merge_pr.py --pr 202 --head d706c8c… --graded bbbb…
[merge_pr] PR #202: REFUSE: graded sha bbbbbbbbbbbb != current head d706c8cd0714 — refusing to merge a tree that was not graded
[merge_pr] NOT MERGED (fail closed)                                    [exit 1]
```
Both refusals name the reason and exit 1. `--pr 202` also fetched the real head from the API (`d706c8c`),
so the live path is exercised against GitHub, not a fixture.

## Clause 3 PASS — the override is a written record

```
$ python3 tools/board/merge_pr.py --pr 202 --head d706c8c… --override "gate check: manual re-run confirms head d706c8c (GB-106-V)"
[merge_pr] OVERRIDE recorded (…/obj/gate/merge_pr_override/pr-202.reason.json); merge allowed   [exit 0]

$ cat obj/gate/merge_pr_override/pr-202.reason.json
{ "pr": 202, "reason": "gate check: manual re-run confirms head d706c8c (GB-106-V)",
  "by": "sarah", "at": "2026-10-26T19:01:16.037998+00:00", "head": "81a2eaee…" }
```
`--override` is the only branch that reaches `MERGE ALLOWED` with a failing rule, and it writes before it
allows. The script performs no merge itself (it decides and exits 0/1), so nothing was merged by this gate.

## Can-fail control — the self-test is not a rubber stamp

Two mutations of `decide()`, each restored afterwards (`evidence/gb-106-v/canfail.sh`):

```
=== mutation 1: a GREEN with NO graded sha is allowed (the fail-open bug)
  mutant 'no_sha_allowed': self-test exit=1
  FAIL: no graded sha -> FAILS CLOSED (ok: MUTANT allows a )
  FAIL: today's PR #194 has no recorded graded sha -> RED/fail-closed
  MERGE_PR_SELFTEST: FAIL (2)

=== mutation 2: a mismatched graded sha is allowed
  mutant 'mismatch_allowed': self-test exit=1
  FAIL: graded!=head -> refuse (ok: MUTANT allows a )
  MERGE_PR_SELFTEST: FAIL (1)

=== restored
  self-test exit after restore=0
  MERGE_PR_SELFTEST: PASS - fail-closed rule + override + #194 goes red
```
Each mutation is caught by the specific assertion that owns it, and the #194 case trips on mutation 1 —
the historical fail-open is genuinely replayed, not just printed.

## Two limitations, stated rather than hidden (neither is a clause failure)

1. **The graded sha comes from a local file, not the board.** `graded_sha_of()` reads
   `obj/gate/graded-<pr>.sha`; it does not read the Forgejo verdict comments. A `graded_shas_in()` parser
   exists for verdict text but is not wired to the board. Consequence: a GREEN that names the correct sha
   on the ticket is still refused unless the file was written. That is the *safe* direction (refuse, never
   allow), so it does not weaken the guard — but "extracts the sha a GREEN says it graded" is satisfied by
   a file convention rather than by the verdict itself. Worth a follow-on ticket.
2. **The override record does not bind to the PR head.** `record_override()` stores the *repo* HEAD
   (`81a2eae`) and the reason, not the PR head sha it overrode. A reader of the record cannot tell which
   tree was blessed. The written reason exists, which is what the DoD asks; binding it to the head would
   make the record auditable on its own.

## Verdict

**GREEN.** All three clauses have real output, the guard refuses on both live failure shapes with the
reason named, the override is the only path through and it writes first, and the rule can provably fail —
two independent mutations of `decide()` make `--self-test` go red, and restoring it makes it green again.