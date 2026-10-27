# S0/S1 Final Acceptance Run (GB-053)
- arch: arm, pinned games: 5, at 2026-10-27T10:59:29.683534+00:00
- verdict from OBSERVED evidence (GB-075, no fabricated PASS): each game
  must LOAD (serial load line / live process), render a live whole-frame,
  and (after the first title) differ from the previous one over the whole
  frame by at least the stated margin.

| game | slot | loaded | live frame | result |
|---|---|---|---|---|
| PONG.BIN | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | yes | True | FAIL |
  - reasons: never loaded (no serial load line / process)
| XNIBBLER.BIN | 20 | no | True | FAIL |
  - reasons: never loaded (no serial load line / process)
| XCRYPT.BIN | 21 | no | True | FAIL |
  - reasons: never loaded (no serial load line / process)
| NES.BIN | 22 | no | True | FAIL |
  - reasons: never loaded (no serial load line / process)

Summary: **FAIL**

- intel leg gated on GB-057 (intel spawn deadlock); this run is ARM.
- 'S0+S1 signed off' is the milestone judgment; this run is its evidence base.
