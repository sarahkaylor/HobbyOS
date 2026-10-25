# GB-049-V — gate evidence (VERDICT: GREEN)

Ticket: GB-049-V (#165) — Verify Backlog grooming bot (close/await/block moves; blockers lint)
Verifies: GB-049 — PR #159, branch `hermes/hermes-331e2844` @ `85bddba`
  "GB-049: backlog grooming bot (verify-lint, deadlock, quant-DoD gate)"
  (`tools/board/groom.py` + `tools/board/groom_test.py` + `tools/board/board.py` lint wiring)
Verifier: farm Verifier session, DEEP, host `swarm-farm-2` (headless, own worktree)
Date: 2026-10-25 (UTC)

`85bddba` is not on `main` (main tip at gate time: `b874d54`), so the ship was checked
out into the verifier worktree for the run and reverted afterwards; only this evidence
directory is committed on branch `hermes/hermes-c1cf443d`.

## Gate results (ticket §"Gate (run these, exactly)")

| # | command | result |
|---|---|---|
| 1 | `python3 tools/board/groom.py --self-test` | PASS — `15 checks, 0 failed` → `01_groom_selftest.log` |
| 2a | `python3 tools/board/groom_test.py` | PASS — `GROOM_TEST: PASS — 15 checks` → `02_groom_test.log` |
| 2b | `python3 tools/t5/run_t5_tests.py` | PASS — `22/22 passed`, includes `tools/board/groom_test.py` → `03_run_t5_tests.log` |
| 3a | `python3 tools/board/board.py lint` | runs; names 6 `unverified-in-review` cards, exit 1 (pre-existing debt, see below) → `04_lint_and_groom.log`, `06_lint_baseline_main.log` |
| 3b | `python3 tools/board/groom.py` | report-only, names the same 6 cards, `groom: report only -- this tool never closes or re-lanes a card.` → `04_lint_and_groom.log` |

## Can-fail proof (honesty law §4.2 — no GREEN on a gate that cannot fail)

Two negative controls against a copy of the shipped `groom.py` with the
`unverified-in-review` finding append replaced by `pass`:

- `05_negative_control_selftest.log` — `FAIL: a review-lane card whose gate has no
  GREEN is flagged unverified-in-review` → `15 checks, 1 failed -- FAILED`, exit 1.
  The self-test detects the removal of the check it claims to test.
- `05_negative_control_mutant.log` — the mutant run against the **live** board prints
  `groom: clean -- no unverified, stale, orphan, deadlocked, or un-evidenced-threshold
  cards`, exit 0, versus 6 named findings from the shipped code. The 6 live findings are
  produced by this code path, not by static text; "clean" is reachable, so the check
  is not a hair-trigger either.

Report-only is code-true: `groom.py` contains no `PATCH`/`POST`/lane-move call —
`api()` is used only for `GET /issues?...` (line 262) and comment reads.

## Scoped-out, stated honestly

- `board.py lint` exits 1 today for **pre-existing** reasons: `06_lint_baseline_main.log`
  shows `main`'s own `board.py lint` already exits 1 on `LINT GB-099: in In Review
  without an EVIDENCE comment`. The change adds 6 truthful `[unverified-in-review]`
  findings (GB-099/098/061/049/039/031) on top of that same non-zero exit. The ticket
  says the board need not be globally GREEN for the tool to be proven; the tool proves
  detection, not board health.
- Gate 3 expects "the 8 unverified-in-review cards incl GB-061/GB-039 with no -V".
  The board moved since the ticket was minted: it is now 6 cards, and GB-061/GB-039
  have `-V` tickets (PLAN-006 minted them) and are reported as
  `gate GB-061-V has not posted GREEN` rather than "no gate ticket". Detection is
  working; the count difference is board evolution, not a regression.
- `make host_tests` is **not** part of this gate (the ticket's tier is the python T5
  suite). It was still attempted for context and stops on a pre-existing, unrelated
  break: `src/host/rtti_test.cpp:29: fatal error: 'typeinfo' file not found`
  (`07_host_tests_context.log`). Three Python-only files under `tools/board/` cannot
  cause that; it belongs to the libc++/host-header lane, not GB-049.
- No QEMU/guest tier (T2/T3) applies: `groom.py`, `groom_test.py` and the `board.py`
  lint wiring never enter the kernel image, so intel/arm parity is not exercised by
  this change and there is nothing for it to regress.
