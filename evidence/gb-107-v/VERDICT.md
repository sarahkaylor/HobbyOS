# GB-107-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-107-V (#205) · verifies GB-107 (#204) · ship = branch `games/gb107` head `509d9ce`
Gate tree = `origin/main` `7fd0910` × `509d9ce` (`ort` merge; the ship touches 3 files, +156)
Run window 2026-10-26T21:51Z–22:10Z · worktree `hermes-2192a712` (swarm-farm-1)
Local only — no CI consulted.

## VERDICT: GREEN

| # | Clause | Result |
|---|---|---|
| 1 | classifier self-test PASS, and a flipped branch is caught | **PASS** — 9/9; my own flip → `FAIL (1)` rc=1 |
| 2 | device self-test `fail_count=0`, and a monitored capture records its serial shape | **PASS** — real capture → `SERIAL-FULL`; same capture cut at the arrival line → `SERIAL-TRUNCATED` |

### 1 — classification, driven by me on my own synthetic logs

    full (end marker):
       SERIAL-FULL lines=41 bytes=322 alive-at-cutoff=0 end-marker=1 (expected 40) 41 lines, end marker seen: complete capture
    mid-cut (guest alive, no marker):
       SERIAL-TRUNCATED lines=18 bytes=134 alive-at-cutoff=1 end-marker=0 (expected 40) 18 lines (expected 40), guest ALIVE at cutoff, no end marker: capture cut mid-stream
    early death (guest not alive):
       SERIAL-GUEST_STOPPED lines=3 bytes=21 alive-at-cutoff=0 end-marker=0 (expected 40) 3 lines, guest not alive at cutoff, no end marker: guest stopped writing

    TRUNCATED != GUEST_STOPPED: TRUNCATED vs GUEST_STOPPED -> True

The record line carries exactly what the DoD asks: lines, bytes, alive-at-cutoff, end-marker.

**Can-fail, by my own edit** (not the test's built-in flip): the live-cut branch rewritten from
`return TRUNCATED, …` to `return GUEST_STOPPED, …`:

    FAIL: mid-cut log (guest alive, no end marker) -> TRUNCATED (got GUEST_STOPPED)
    SERIAL_CLASSIFIER_TEST: FAIL (1)                        rc=1

restored → 9/9 PASS, rc=0.

### 2 — the device leg records the shape of a real capture

    $ bash tools/gate/t4_device.sh --self-test
    [self-test] clause 2 (wrong shim must FAIL): OK
    [self-test] clause 3 (absent probe must FAIL, not skip): OK
    [self-test] GB-079 (probe edit must FAIL in both --check modes): 4 × OK
    self-test result: fail_count=0                              rc=0

A real monitored run (static-PIE `t4_probe` staged as `::T4PROBE.BIN` with a `::WAVE.CFG`, intel T3),
then `t4_device.sh` over that serial:

    t4_device: probe ARRIVED in guest (t4-probe-arrived present).
    t4_device: SERIAL-FULL lines-captured-end-marker (probe-exit seen)
    t4_device: row count A=41 B=17
    t4_device: FAIL — row count differs (A=41 B=17)

The same capture cut at the guest's arrival line (alive, no end marker):

    t4_device: probe ARRIVED in guest (t4-probe-arrived present).
    t4_device: SERIAL-TRUNCATED lines=145 bytes=8984 alive-at-cutoff=1 end-marker=0 (expected 43) 145 lines (expected 43), guest ALIVE at cutoff, no end marker: capture cut mid-stream

A capture with no arrival at all still exits on the existing clause-3 path ("the probe never arrived…"),
so a wrong classification cannot be produced by absence.

### Two things stated plainly

1. **The row-diff FAIL in those runs is not this card.** A=41 rows on Linux vs B=17 in-guest: the guest's
   `dent_*` rows never run because directory creation fails there — that is GB-020/GB-067's create-path
   defect, and the card itself says the classification is a recorded annotation while "the row-diff gate
   stays separate". I am not citing the diff as a GB-107 result in either direction.
2. **The live leg took a retry.** My first staged run gave `[ARRIVED] T4PROBE.BIN rc<0` with no rows — the
   probe image did not load on that attempt. Rebuilt in the same tree immediately before the wave, the
   second run arrived `rc>0` and emitted `t4-probe-arrived` + `probe-exit`. Both logs are in the evidence
   dir (`g5-device-annotation.log` failed first attempt, `g5b-live.log` the good one), so the false start
   is visible rather than hidden.

## Board note (not a verdict cell)

`GB-107-V` was minted twice, 39 seconds apart: #205 and #208 ("three-way classification on a real
monitored run"), both `Ready`. I gated #205; #208 is the same gate on the same head. Close one.