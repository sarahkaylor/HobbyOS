# GB-074-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-21.
Tree: `origin/main` @ `6636741`, clean worktree; `tools/board/board.py` at `31d15c4` (the GB-074 fix).

The lock works. The **refusal message names the wrong winner** once a ticket has any earlier claim — which
is the normal state of this board, because cards are routinely moved back to `Backlog`/`Ready` and
re-claimed.

## What passes, measured

**Criterion 2 — exactly one claim comment, one `Ready -> In Progress`, per race.** Four races, four
winners, never two claims (three trials on scratch #113, one on clean scratch #114):

```
########## TRIAL 1 ##########
alpha rc=1  beta rc=0
--- alpha --- GB-990 #113 already claimed by `agent-beta` (lane already In Progress) — not re-claiming (GB-074); run no gate
--- beta ---  GB-990 #113 claimed by agent-beta (was Ready)
claims=1 ready->inprogress comments=1

########## TRIAL 2 ##########
alpha rc=0  beta rc=1
--- beta ---  GB-990 #113 already claimed by `agent-beta` (…)     <-- wrong name: alpha won this trial
```

**Criterion 3 — the negative control reproduces the double claim on the pre-fix script.**

```
########## B. PRE-FIX code (31d15c4^), CLEAN ticket GB-992 ##########
$ grep -c flock /tmp/oldboard/board.py
0
alpha rc=0  beta rc=0
--- alpha --- GB-992 #115 claimed by agent-alpha (was Ready)
--- beta ---  GB-992 #115 claimed by agent-beta (was Ready)
GB-992 pre-fix #115: 2 claim comment(s)      <-- both claimed: the race the ticket describes
```

So the test is a real test: the same harness that double-claims on `31d15c4^` claims once on `31d15c4`.

**Criterion 4 — `board.py move` prints the transition it acted on**, observed on every move this
session, e.g. `GB-032-V #108: In Progress -> Done`, `GB-070 #79: In Review -> In Progress`.

## The failing check — the loser names the wrong agent (criterion 1)

On a **clean** ticket the name is right:

```
########## A. current claim.sh, CLEAN ticket GB-991 ##########
alpha rc=1  beta rc=0
--- alpha --- GB-991 #114 already claimed by `agent-beta` (lane already In Progress) — not re-claiming (GB-074); run no gate
```

On a ticket that was claimed before — trials 2 and 3 of the three-repeat test — it is not:

```
########## TRIAL 3 ##########
alpha rc=0  beta rc=1
--- alpha --- GB-990 #113 claimed by agent-alpha (was Ready)          <-- the actual winner
--- beta ---  GB-990 #113 already claimed by `agent-beta` (…)         <-- the stale winner from trial 1
```

Cause (`tools/board/board.py:225-233`): `claimant_of()` returns the **first** `claimed by \`X\`` comment
in the thread, not the current one. A ticket re-claimed after being returned to `Backlog` keeps its old
claim comments, so the loser is told the name of whoever claimed it *last time*. The card's criterion 1
is "the other exits non-zero **naming the winner**" — non-zero holds 4/4, the name fails 2 of 3 on a
ticket with history, which is the common case here (I moved four cards back to `Backlog` this session
alone).

Impact is not a double gate — the loser still runs nothing — but a loser told the wrong agent can
"help" the wrong session, and the message is the only signal it has.

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | one exits 0, the other exits non-zero **naming the winner** | **FAIL** — refusal is correct in count but names a stale claimant when the ticket has claim history (2 of 3 trials) |
| 2 | loser posts no claim; exactly one `Ready -> In Progress` | PASS (4/4 races, 1 comment each) |
| 3 | negative control on the pre-fix script reproduces the double claim | PASS (`31d15c4^`: both rc=0, 2 claim comments) |
| 4 | `board.py move` prints `X -> Y` | PASS |

## Fix (small)

`claimant_of()` should read the **latest** `claimed by \`X\`` comment (iterate reversed), or better, take
the claimant from the claim that moved the lane to `In Progress` (the last `lane: … → In Progress`
comment), falling back to the assignee. Then re-mint this card — the rest of GB-074 is working and the
negative control is one of the better tests in this program.

## Scratch tickets

#113 (GB-990), #114 (GB-991), #115 (GB-992) were created for this test and are closed with this verdict.
They are not program work.
