# S0/S1 Final Acceptance Run (GB-053)
- arch: intel, pinned games: 5, at 2026-11-04T09:05:45.176216+00:00
- verdict from OBSERVED evidence (GB-075, no fabricated PASS): each game
  must LOAD (serial load line / live process), render a live whole-frame,
  and (after the first title) differ from the previous one over the whole
  frame by at least the stated margin.

| game | slot | loaded | live frame | result |
|---|---|---|---|---|
| PONG.BIN | 16 | yes | True | PASS |
| MILLIPED.BIN | 17 | yes | True | PASS |
| XNIBBLER.BIN | 20 | yes | True | PASS |
| XCRYPT.BIN | 21 | yes | True | PASS |
| NES.BIN | 22 | yes | True | PASS |

Summary: **PASS**

- arch: intel; 'S0+S1 signed off' is the milestone judgment, this run is its evidence base.
