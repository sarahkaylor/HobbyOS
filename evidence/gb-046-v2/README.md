# GB-046-V2 — gate evidence (VERDICT: RED on clause 2, the re-measurement)

Ticket: GB-046-V2 (#196) — re-gate the game-cycling memory pass
Verifies: GB-046 (#46), evidence `evidence/perf_cycle/20261026T153301Z/` (worker, 61 RSS samples)
Graded on `origin/main` **`b6c34fd`**. Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2).
Local only — no CI cited.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | bound restated; series shape named (flat-with-noise vs steady climb) | **FAIL** — 64 MB not shown defensible at the observed rate; no plateau demonstrated |
| 2 | re-measure on my own VM, ARM, current main | **FAIL** — **45.76 MB**, not 8.37 MB (5.5×) |
| 3 | `frozen=False` must be able to be False | **PASS** — the live run reported both values |
| 4 | residual attributed; launched-title count | **PASS with a caveat** — 3 of 8 visits launched; owner GB-088 (#141) |
| 5 | 30 minutes, or say plainly how long | **STATED** — my run was **4 min 19 s**, not 30 min |

## Clause 2 FAIL — the re-measurement disagrees with the claim by 5.5×

My run, ARM, `make disk.img ARCH=arm` from `b6c34fd`, the worker's game set:

```
$ ARCH=arm python3 tools/perf_game_cycle.py \
    --games 'PONG.BIN:10,XNIBBLER.BIN:10,XCRYPT.BIN:10,NES.BIN:8' --cycles 2 --hold 4 --root "$PWD"
[perf] whole-run RSS: floor=335.64 MB peak=381.39 MB growth=45.76 MB (bound 64 MB) -> PASS
[perf] summary: FAIL
```
`report.json`: `rss_floor_mb 335.64, rss_peak_mb 381.39, rss_growth_mb 45.76, rss_full_run_samples 77`.

| | worker (15:33Z) | this gate (15:47Z) |
|---|---|---|
| floor | 335.82 MB | 335.64 MB |
| peak | 344.19 MB | **381.39 MB** |
| growth | 8.37 MB | **45.76 MB** |
| samples | 61 | 77 |

The floor agrees to 0.2 MB — same guest, same boot — and the peak does not. The card says it plainly:
*"If your growth disagrees materially with 8.37 MB, that is the finding."* This is that finding, and it is
not a small one: 45.76 MB is **71 % of the 64 MB bound**, not 13 %.

The tool still prints `-> PASS` because it only compares growth to the bound. That is exactly the
"`-> PASS` is not a verdict" case the card warns about: at the observed rate the *bound*, not the growth,
is what is being certified.

## Clause 1 FAIL — the bound is not shown defensible

64 MB could be defensible for a 30-minute soak if the series plateaus — boot caches, allocator warm-up,
then flat-with-noise. My run does not show a plateau; it shows 45.76 MB of floor→peak growth inside **4m19s**.

At that observed rate (~10.6 MB/min, and I label this an *extrapolation, not a measurement*) 30 minutes
would be several hundred MB — far outside the bound. I am not claiming the growth is linear; I am claiming
the opposite of what a GREEN needs: nobody has shown the curve flattens, and the one number that would
show it (a long flat tail) is absent from both the worker's bundle and mine.

## Clause 3 PASS — the freeze detector reports both values on live data

Not a synthetic feed: in my run the same detector, same crop, returned both outcomes.

```
[perf] cycle=0 game=PONG.BIN      samples=10 frozen=False launched=True
[perf] cycle=0 game=XNIBBLER.BIN  samples=10 frozen=True  launched=True
[perf] cycle=0 game=XCRYPT.BIN    samples=10 frozen=True  launched=True
```
PIN: PONG's playfield digests change; XNIBBLER/XCRYPT report frozen. So `frozen=False` is reachable and
`frozen=True` fires — the assertion is not a constant. (Whether XNIBBLER/XCRYPT were *genuinely* frozen or
showing a static title screen is a separate question, and a fair one to ask of those two titles.)

## Clause 4 PASS with a caveat — 3 of 8 visits launched

```
cycle 0: PONG launched=True | XNIBBLER True | XCRYPT True | NES False (no-op, 3 retries)
cycle 1: PONG False | XNIBBLER False | XCRYPT False | NES False   (every launch no-op)
```
Titles that actually launched: **PONG, XNIBBLER, XCRYPT in cycle 0 only — 3 of 8 visits, 3 of 4 titles once,
and none at all in the second cycle.** The owner is **GB-088 (#141)** "GB-035's end-to-end playthrough …
input" (In Progress), which is the card the worker named and the card the board shows holding the
launch/input family. The attribution is honest; the consequence is not small — after the first cycle the
soak is measuring a *launcher that no-ops*, so the memory curve past cycle 0 comes from the desktop, not
from games.

## Clause 5 STATED — 4m19s, not 30 minutes

Bundle created `20261026T154733Z`, `captured_at 2026-10-26T15:51:52Z` → **4 min 19 s**. The card's title
says 30-minute cycling; the card allows the shorter run to be graded *with that stated*. Stated: this is a
4-minute run. The worker's 61-sample bundle is the same order of length.

## Verdict

**RED.** Clause 2 is the failing check and it is the card's own rule: my measurement is 45.76 MB against a
claimed 8.37 MB, from a floor that agrees to 0.2 MB (so it is the same guest and the same boot, and the
difference is in what the run accumulated, not in where it started). Clause 1 follows — a bound that is 71 %
consumed in four minutes is not shown to hold for thirty. Clause 3 passes on live data and clause 4's
attribution is honest, but they cannot carry the DoD while the memory number itself disagrees by 5.5×.

What a re-gate needs: a run long enough to show a flat tail (or a stated, defended bound with the series
quoted), on a launch path where more than the first cycle actually starts a game.