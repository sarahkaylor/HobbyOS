# GB-136-V3 — RED at a1f4fd4 (verifier run, 2026-10-30)

Graded head **`a1f4fd4`** (PR #263, `fix/gb136-supersession-live-path`), 1 file, +96/−36.
Before-tree: **`origin/main` = `380dce8`** (a separate worktree; every count below names its tree).

Clauses 1–3's mechanical targets pass. The **accounting** clause fails the card: the tool still
manufactures REDs out of prose, and on this board that includes turning a card's genuine newest
GREEN into a RED.

## Clause 1 — newest-by-time governs ✓ (5/5)

`governing([(ordinal, gid, verdict, ts), …])` on the graded tree:

```
  [OK] RED re-posted after a GREEN leaves the card RED          -> GB-900-V RED
  [OK] GREEN newest supersedes an older RED                     -> GB-901-V2 GREEN
  [OK] later RED does NOT supersede an earlier RED              -> GB-902-V2 RED
  [OK] two cards share one id (#300 GREEN 16:10 vs #301 RED 16:15) -> GB-903-V RED
  [OK] tie on time -> ordinal breaks it (V2 GREEN beats V RED)  -> GB-904-V2 GREEN
failures: 0
```

The shared-id case is the one that matters here: #300 and #301 are both `GB-136-V`, and the
16:15Z RED on #301 correctly governs over the 16:10Z GREEN.

## Clause 2 — the four named prose cases ✓

```
  [OK] 'verdicts checked: 66, mismatched parents: 2, superseded REDs: 60'   -> None
  [OK] 'a GREEN-gated merge helper that refuses'                            -> None
  [OK] '**VERDICT: RED** — clause 1 fails'                                  -> RED
  [OK] '## VERDICT: **GREEN** — all clauses'                                -> GREEN
```

## Clause 3 — live counts, both trees

```
BEFORE  org/main 380dce8 : verdicts checked: 66, mismatched parents:  2, superseded REDs: 62
AFTER   head    a1f4fd4 : verdicts checked: 68, mismatched parents: 10, superseded REDs: 24
```

**The deliberate break moves the live count** — restoring the loose path (`verdict_line_of`
falling back to any GREEN/RED mention):

```
LOOSE   head    a1f4fd4 : verdicts checked: 72, mismatched parents: 12, superseded REDs: 38
```

12/38 loose → 10/24 strict, on the live board, from the same tree. Restored from the committed
file (`git status --short tools/board/` clean), clean run back to 10/24.

## Clause 3 — the account: the card expected 9, the board has 10, and three of them are fiction

The extra one is **GB-136 itself**: a RED verdict at 16:15Z (`c18416`, `**VERDICT: RED at
`721650e` …**`) now governs over my 16:10Z GREEN — correct newest-by-time behaviour, and the
card it points at (`#292`) is in `In Review`. So the tenth is genuine and the card's "9" was
taken before that RED landed.

The nine, each resolved against *every* verdict-shaped comment on its card:

| parent | reported | newest verdict-shaped comment | genuine? |
|---|---|---|---|
| GB-060 (#62) | RED | `VERDICT: **RED** — gate run by a farm Verifier … @ 9db1388` | genuine |
| GB-066-V2 (#74) | RED | `## VERDICT: RED — GB-066-V2 re-gate 2 …` | genuine |
| GB-085 (#136) | RED | `**VERDICT: RED** (from GB-085-V / #162) …` (06:38) | genuine |
| GB-099-V (#161) | RED | `**VERDICT: RED** (wverify-deep …)` | genuine |
| GB-106-V (#200) | RED | `## VERDICT: RED — GB-106-V (#202) …` | genuine |
| GB-101-V6 | GREEN | `**GREEN** for GB-123 items 3-4 …` | genuine lane error (Backlog) |
| **GB-044 (#44)** | RED | `red or pending run is not a defect and a green run is not correctness; the **local §6 gate** …` | **fiction** — CI-policy prose. The card's only real verdict is `GREEN → this ticket to Merge Gate` (c2336, 10-20) |
| **GB-098 (#159)** | RED | `RED fixed (F1 + F2) — pushed to ticket/GB-098-stale / PR #157` | **fiction** — a status sentence. No verdict-shaped verdict exists on this card at all |
| **GB-105 (#197)** | RED | `red log:   T3_intel: FAIL — 1 FAIL line(s); …` | **fiction, and it inverts a GREEN** — `**VERDICT: GREEN** (wverify-deep …, 19:01Z)` is the newest real verdict; a lowercase `red log:` line at 19:39Z overrides it |

So the answer to the card's explicit question — *"if any of the nine has a genuine newer GREEN,
say which"* — is **GB-105 (GREEN at 19:01Z, c10345) and GB-044 (GREEN, c2336)**. The tool is
wrong in the other direction, exactly as the card anticipated: `^\s*\**\s*(GREEN|RED)(?![\w-])`
matches a line-initial English sentence. `(?![\w-])` stops `REDs` and `GREEN-gated`; it does not
stop `red or pending …`, `RED fixed …`, or `red log:`.

Seven of the ten are real. Three are the parser reading prose — and one of those three (GB-105)
would, through GB-005's shared parser, refuse a merge for a card whose newest verdict is GREEN.

## The four suites

```
groom_test.py            EXIT=0
check_stale_test.py      EXIT=0
drift_check_test.py      EXIT=0
check_verdicts --self-test  EXIT=0   self-test result: fails=0
```

## Verdict line prints text, not an issue number ✓

```
  MISMATCH GB-060 = RED but GB-060#62 is in 'Done' (want 'In Progress')
           verdict line: VERDICT: **RED** — gate run by a farm Verifier (DEEP, swarm-farm-2) on main @ 9db1388. …
```

## Verdict

**RED.** Clauses 1, 2 (the four named strings) and 3's falsifier all pass, and the fix is a real
improvement — 62 superseded → 24, and the loose path demonstrably changes the live count. But
the clause the card exists for is the account, and the account says three of the ten reported
violations are prose, one of them inverting a GREEN. A checker that turns "red log: T3_intel:
FAIL …" into a verdict is the failure mode #301 named, narrowed rather than closed.

Minimal discriminator to close it, for whoever picks this up: require a verdict to be line-initial
**and** either carry a `VERDICT` marker or be a bold/heading token (`**GREEN — …**`, `## VERDICT:
GREEN`), and reject a bare lowercase `red`/`green` followed by a lowercase word (`red or`, `red
log:`, `RED fixed`). All five of my own verdicts and the four named cases still parse under that
rule; the three fictions do not.