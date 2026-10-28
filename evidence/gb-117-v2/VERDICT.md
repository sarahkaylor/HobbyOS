# GB-117-V2 — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-117-V2 (#240) · verifies GB-117 (#238) · ship = PR #228 head **`3a55959`**
(`tools/board/claim_age.py` +307, `claim_age_test.py` +23)
Gate tree = `origin/main` × `3a55959` (`ort`, clean; HEAD is the ship itself)
Local only — no CI consulted.

## VERDICT: RED

The report is honest about `updated_at` and the authors-are-uniform point is settled. But the card's own
central promise — **two labelled clocks** — is not implemented (one clock is printed), the round-1 fixture
does not exist, the test is registered nowhere, and the kind-classifier contradicts its own header on the
board's most common comment.

| # | Clause | Result |
|---|---|---|
| 1 | authorship is uniform, and the code says so | **PASS** |
| 2 | two labelled clocks, both printed | **FAIL** — one clock |
| 3 | the DoD number restated as a pair | **PASS** — `split=46/14` |
| 4 | the round-1 fixture (fresh notch + 30 h substantive, same line) | **FAIL** — no such case |
| 5 | the sweep falsifier still fires | **PASS** |
| 6 | `make host_tests` PASS, test self-registered | **PARTIAL** — suite passes 482/0, the test is not registered |
| 7 | the classifier is falsifiable | **FAIL** — lane notches count as activity, contrary to the header |

### 1 — every comment posts under one login

    #240: 2 comments, logins=['sarah']
       2026-10-28T00:43:01Z  sarah     'GB-117-V2: lane - → **Ready**'
       2026-10-28T01:02:10Z  sarah     'Duplicate gate: `GB-117-V2` is on the board twice …'
    #238: 19 comments, logins=['sarah']
       2026-10-27T23:27:44Z  sarah     "**Coordinator kickoff — `Ready`…"

and the code records why that rules a login filter out:

    comments post under one bot account, so a login filter cannot separate
    coordinator from worker.                                    (claim_age.py:72)

### 2 — the failing cell: there is one clock

    $ python3 tools/board/claim_age.py
    claim-age report (clock = NEWEST activity comment; coordinator bookkeeping + lane notches do NOT re-arm it; updated_at NOT used): 60 open cards

Lines mentioning a second clock or `any-comment`: that header, and nothing else. `grep -n "any-comment"`
over the file: no match. The card asks for `any-comment` **and** the working clock, both printed, both
labelled; the docstring is explicit that only one exists ("The ONLY clock here is …").

### 4 — no round-1 fixture

The self-test's cases are b1 (sweep-proof stale), b2 (bucket map), b3 (decision/blocked separation):

      PASS: b1 sweep-proof stale  b2 bucket map  b3 decision/blocked sep -> split=4/1
    CLAIM_AGE_TEST: ALL PASSED

The snapshot's coordinator case is `{"body": "Coordinator status"}` on card #5 — a coordinator-body test,
which is the *round-1* shape. There is no card carrying a **fresh lane-move comment** over a **30 h-old
substantive comment**, and no line that reads it on two clocks, because there is no second clock.

### 5 — the sweep falsifier fires

Live:

    cards STALE despite fresh updated_at (<24h): 46 (a bulk sweep cannot re-arm them)

and b1 asserts it in the fixture (`STALE despite fresh updated_at (<24h): 2` expected). `updated_at` never
enters the age computation.

### 3 — the pair is printed

    STALE (comment age >= 24h, or no comments): 46   FRESH: 14   split=46/14

with the clock defined in the header line directly above it — this is the form the card asked for (the
docstring still quotes the old "42/28", which is stale prose, not a defect in the output).

### 6 — the suite passes, and does not run this test

    make host_tests →  Checks run: 482 / Checks failed: 0 / ALL APPS SUITE TESTS PASSED   rc=0
    grep -c claim_age Makefile                     → 0
    make -n host_tests | grep -c claim_age         → 0

`claim_age_test.py` exists and passes when run by hand, but nothing in the build or the suite invokes it —
so "self-registered" is not met and the 482 checks do not include the three.

### 7 — the classifier contradicts its own header

The header says "lane notches do NOT re-arm it". The predicate's whitelist is
`COORDINATOR / SWARM HEALTH / CARETAKER / VERIFICATION MINTED / RE-GATE MINTED / VERDICT-`; anything else
is activity. Measured on real comments:

    #240 ACTIVITY (re-arms) 'GB-117-V2: lane - → **Ready**'
    #238 ACTIVITY (re-arms) 'GB-117: lane Backlog → **Ready**'
    #238 ACTIVITY (re-arms) 'GB-117: lane Ready → **In Progress**'
    #238 ACTIVITY (re-arms) 'GB-117: lane In Progress → **In Review**'   (×2)

A card bounced Backlog→Ready→In Progress→In Review by the Coordinator reads *fresh* with no worker content
at all — which is exactly the re-arming the tool claims to exclude, reached by a different route than
round 1's body substring. **The comment a reasonable person would classify the other way** is
`GB-117: lane Ready → **In Progress**`: the tool treats it as activity; it is a lane notch.

Quantified so the severity is not overstated: re-running the whole board with lane transitions and claim
notices also excluded leaves **14 fresh either way — 0 cards flip** today. The number stands today; the
*stated property* does not, and the first card that is only lane-moved will read fresh forever.

## Fix-forward

1. Print both clocks in one line per card: `any-comment <age>` and `working <age>`, with the working
   definition in the output header.
2. Add the round-1 fixture: fresh lane-move comment + 30 h substantive comment → `any=fresh / working=stale`
   on one line, and assert it.
3. Register `claim_age_test.py` in `make host_tests` (the 482 checks should include its three).
4. Either add lane transitions and claim notices to the bookkeeping predicate, or correct the header to
   say they re-arm the clock. Right now code and prose disagree, and the prose is the part a reader trusts.

## Process note

Claiming by title resolved `GB-117-V2` to the **Done** duplicate #241 and moved it; I restored #241 to Done
and claimed #240 by number. The board still carries two `GB-117-V2` cards (#240 open, #241 Done) — worth
deleting one.