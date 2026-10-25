# GB-085-V (round 2) — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-085-V (#172) · verifies GB-085 (#136) · ship = PR #156 head `735cbb5`
Gate tree `67d8913` = `origin/main` `8f95204` × `735cbb5` (`ort` merge, no conflicts)
Run window 2026-10-25T07:55Z–08:04Z · worktree `hermes-8e3572cf` (swarm-farm-1)

Tier demanded: **T1 host only** (no guest). T3/T4/T5 not named by GB-085, not run.
Independence: I wrote none of this fix; the fix is the worker's at `735cbb5`.

## VERDICT: GREEN

| # | Gate item | Result |
|---|---|---|
| 1 | `grep -rn '"/tmp/[A-Z]' src/host/*_test.c` → no matches | **PASS** — rc=1, 0 matches |
| 2 | `make host_concurrency_gate` → A rc=0, B rc=0 | **PASS** — `A rc=0, B rc=0`, gate `PASS` |
| 3 | `make host_concurrency_gate_negative` → the pair-run FAILs | **PASS** — `A rc=2 ok=False ; B rc=2 ok=False` |
| 4 | `tools/t5/run_t5_tests.py` → replay_test PASS, no shared `_bad_tmp` | **PASS** — `PASS tools/t5/replay_test.py`, 21/21; `grep _bad_tmp` → rc=1 |

### G1 / G4 — no fixed shared paths, replay fixture per-process

    ### G1: grep -rn '"/tmp/[A-Z]' src/host/*_test.c
    grep-rc=1

    ### G4a: grep -n "_bad_tmp" tools/t5/replay_test.py
    grep-rc=1

    ### G4: python3 tools/t5/run_t5_tests.py
      PASS  tools/t5/replay_test.py
    [host-tests] done: 21/21 passed
    rc=0

The RED cause from #162 (`replay_test.py` writing a shared `_bad_tmp.jsonl` into the repo tree) is gone:
the fix routes it through a `mkdtemp()` fixture, and after every run in this session the worktree carries
no new tracked/untracked fixture files (`git status` clean apart from one build binary).

### G2 — two simultaneous `make host_tests`, both green

    [gb085] building host_tests once (serial)...
    [gb085] serial build green.
    [gb085] starting two make host_tests simultaneously...
    PASS: two concurrent make host_tests runs both green (A rc=0, B rc=0)
    CONCURRENCY_GATE: PASS
    rc=0

### G3 — the negative control fires (this is what makes the GREEN citable)

    [gb085] NEGATIVE mode: HO_TEST_SHARED_DIR=/home/sarah/.hermes/cache/scratch/gb085_gate_mmbvoun7/SHARED
    [gb085] starting two make host_tests simultaneously...
    PASS (negative): shared path made the pair-run fail (A rc=2 ok=False ; B rc=2 ok=False)
    CONCURRENCY_GATE: PASS (negative control)
    rc=0

Both shapes the card predicts, exactly: `A rc=2, B rc=2` with a forced shared dir, and `A rc=0, B rc=0`
without it. A gate that could not tell those apart would be worthless; this one distinguishes them on the
same tree, minutes apart.

### G5 — stronger audit than the card's grep (informational, not a verdict cell)

11 `/tmp` mentions remain in `src/host/*_test.c`, all comments or `chdir("/tmp")` in `tasks_test.c`; 7 of
the host tests route paths through `ho_scratch`. The operative proof is G2/G3: two concurrent suites pass
without the override and fail with it, so no remaining shared path is load-bearing for the suite.

## Two notes for the Coordinator (neither is a verdict cell)

1. **PR #156 is unmerged and its branch also carries `gaming.md` edits** (`git diff origin/main HEAD`:
   12 files, incl. `tools/t5/selfreg_test.py`, `tools/t5/replay_test.py`, `src/host/*`). The merge is
   clean, but the delta is wider than the card's one-line description.
2. **A board-listing defect worth naming:** the ready-lane read I used in earlier rounds fetched a single
   page of `/issues`, and this repo's API caps that at 50 — so `GB-085-V` (#172) was invisible to a
   one-page scan and only appeared once the read was paginated (72 open issues over 2 pages). Any agent
   claiming from a one-shot `limit=200` read may be reading half a board. That is how this card nearly
   went another hour unclaimed.
