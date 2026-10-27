# GB-112-V — verdict: GREEN (with one latent-silence finding the DoD does not name)

Subject: PR **#216**, head **`c7f1d9f`** (branch `tooling/reverify-pending-lint`), two commits on
`b4e4524`. Graded in a fresh detached worktree of that head; the unpatched control ran from a second
worktree at `b4e4524` (groom.py `a113fe3`). Host-side only, no QEMU, no CI anywhere in this evidence.
Verifier: DEEP Verifier `wgames-verify-gb112v`. I authored neither GB-112 nor the patch.

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | False positive gone on the live board (quote the lint) | **PASS on the command; the live line proves nothing on its own** — GB-047 left the review lane at 10:46:34Z, so attribution comes from the replay below |
| 2 | Nothing else cleared — every other unpatched finding still produced | **PASS** — identical finding sets on the live board |
| 3 | The violation case still fires | **PASS** — self-test + a live case (#224 GB-112, gate minted 5 s *before* the lane move) |
| 4 | Discriminator is the board's own timestamp; both arrows; no new metadata | **PASS**, with a failure mode named (stale lane-entry anchor) |
| 5 | `groom.py --self-test` reports 17 checks with both new ones named | **PASS** — 17 vs 15 unpatched |
| 6 | No regression to the other lint checks; show one still firing | **PASS** — the EVIDENCE check still fires |

## 1 & 2 — live run, both versions, same minute

```
$ python3 tools/board/board.py lint        # PATCHED  @ c7f1d9f
LINT GB-112: in In Review without an EVIDENCE comment
LINT GB-112: [unverified-in-review] in 'In Review' but gate GB-112-V has not posted GREEN
rc=1
```
```
$ python3 tools/board/board.py lint        # UNPATCHED @ b4e4524 (groom.py a113fe3)
LINT GB-112: in In Review without an EVIDENCE comment
LINT GB-112: [unverified-in-review] in 'In Review' but gate GB-112-V has not posted GREEN
rc=1
```
Byte-identical. **GB-047 does not appear in either.** That is clause 1's sentence, and it is also the
reason the sentence cannot be evidence: GB-047 was bounced out of `In Review` at **10:46:34Z** when
`GB-047-V` #223 RED'd (`#47: lane In Review → **In Progress**`), five minutes before this gate was
claimed. No live run can flag a card that is no longer in a review lane, with or without the patch.

## The differential that does support clause 1 — real records, not fixtures

`replay_lint.py` fetches the live board (`state=all`, as `board.py:all_issues()` does — 225 issues)
and the real comments, then evaluates **both** versions of `groom.verify_lint_findings()` on the same
objects. Read-only; every call is a GET.

```
A. live board (GB-047 in In Progress after the #223 RED)
   unpatched: [('unverified-in-review', 'GB-112')]
   patched  : [('unverified-in-review', 'GB-112')]
   identical=True  cleared_by_patch=[]  added=[]

B. real records, #47 lane restored to In Review (the 10:45Z state)
   unpatched: [('unverified-in-review', 'GB-047'), ('unverified-in-review', 'GB-112')]
   patched  : [('unverified-in-review', 'GB-112')]
   identical=False  cleared_by_patch=[('unverified-in-review', 'GB-047')]
   #47 lane_entry_time = 2026-10-23T19:38:25Z
   #223 GB-047-V created_at = 2026-10-27T10:41:33Z, state=closed
   gate_is_pending(#47, #223) = True
```

A is clause 2 (nothing cleared). B is clause 1: on the same real comment record, the unpatched lint
flags GB-047 and the patched one does not — a differential that fails if the patch stops working.

## 3 — the violation case fires, live and in self-test

Live: #224 GB-112 moved `Backlog → **In Review**` at **10:46:28Z**; its gate #225 GB-112-V was created
at **10:46:23Z** — five seconds *older*. The patched lint still flags it (line 2 of the run above).
Self-test, patched:
```
  PASS: a gate older than the lane move is still flagged
  PASS: a gate minted after the lane move is pending, not unverified
```
I consider the live #224 case and the self-test together sufficient; no constructed fixture was needed.

## 4 — the discriminator is the board's own timestamp

`tools/board/groom.py` (patch): `lane_entry_time()` takes the newest comment whose body contains
`"-> **<review lane>**"` **or** `"→ **<review lane>**"`, and `gate_is_pending()` compares that
`created_at` to the gate ticket's `created_at`. The two spellings are both real: `board.py:275` writes
`→` (`f"{a.gid}: lane {prev} → **{a.lane}**"`) while hand-written moves use `->`. Both were exercised
here — the live board's `→` in B, the ASCII form in the self-test. No new metadata, no new label, no
worker-side change: the diff is `groom.py` (+65), `gaming.md` (+1 plan row), `import_seed.py` (+1).

**Failure mode, named:** the anchor is *machine-readable lane moves only*. GB-047's actual entry into
`In Review` on 2026-10-27 was prose (`Moving to In Review for re-gate`, 10:25:22Z), so
`lane_entry_time()` fell back to the 2026-10-23T19:38:25Z entry. The exemption is still correct there,
but it is correct by a stale anchor, not by the true one — a card that enters a review lane without a
lane-move comment is treated as having entered earlier, i.e. the lint errs toward silence.

## 5 — the self-test grew, and both new cases are named

```
groom self-test: 17 checks, 0 failed -- ALL PASSED      # patched (c7f1d9f)
groom self-test: 15 checks, 0 failed -- ALL PASSED      # unpatched (b4e4524)
```

## 6 — no regression to the other lint checks

The EVIDENCE check still fires in both runs (`LINT GB-112: in In Review without an EVIDENCE comment`),
and `self_test()` keeps `stale-verify`, `orphan-verify` and the blockers deadlock passing (17/17).

## Finding the DoD does not name — a RED-minted gate in a review lane is now invisible

`gate_is_pending()` compares mint time to lane-entry time and **never looks at whether the gate has
already posted a verdict**. So a re-verify gate minted after the lane move that has since posted RED
is treated as "pending" forever, and a card that ignores its RED and stays in `In Review` is no longer
flagged — the exact shape this lint exists for (GB-090 sat in `In Review` 21 h behind a gate).

Measured on real records (case C of the replay — #47 in `In Review`, #223's real RED verdict present):
```
   unpatched: [('unverified-in-review', 'GB-047'), ('unverified-in-review', 'GB-112')]
   patched  : [('unverified-in-review', 'GB-112')]
   #223 newest verdict (patched groom.newest_verdict) = RED
   gate_is_pending(#47, #223) = True
```
**This does not flip my verdict:** clause 2 asks for the live comparison ("every other finding the
unpatched lint produced is still produced"), and on the live board nothing else is cleared. But it is
the same class of defect as clause 2's stated fear — *"a lint that stops seeing things is worse than
one that over-reports"* — and it should be its own card. The smallest fix is one line in
`gate_is_pending()` or at its call site: only treat a gate as pending when `newest_verdict()` is
`None`, i.e. `if v is not None and verdict is None and gate_is_pending(...)`. The clause that would
test it: *a gate minted after the lane move that has posted RED is still flagged.*

## Reproduce

```
git worktree add /tmp/w-112 --detach c7f1d9f && cd /tmp/w-112
python3 tools/board/board.py lint                 # patched
python3 tools/board/groom.py --self-test          # 17 checks
python3 evidence/gb-112-v/replay_lint.py <worktree-at-b4e4524> .   # replay A/B/C
```