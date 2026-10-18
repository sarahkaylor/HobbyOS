# GB-045 performance baseline (WGAMES, ARM)
- captured: 2026-10-18T01:36:28Z

## Boot to desktop+menu (QEMU arm, wall-clock)
- samples: [13.03, 12.53, 12.53]
- mean: **12.70s**, min 12.53s, max 13.03s

## Game-cycling (GB-046): freeze + host-RSS growth
| game | cycle | frozen | growth_mb |
|---|---|---|---|
| XNIBBLER.BIN | 0 | False | 0.19 |
| XCRYPT.BIN | 0 | False | 0.0 |
| XNIBBLER.BIN | 1 | False | 0.0 |
| XCRYPT.BIN | 1 | False | 0.0 |
| XNIBBLER.BIN | 0 | False | 0.27 |
| XCRYPT.BIN | 0 | False | 0.02 |
| XNIBBLER.BIN | 1 | False | 0.0 |
| XCRYPT.BIN | 1 | False | 0.0 |

- freezes: False; max game-cycle RSS growth: **0.27 MB** (bound 64 MB) -> PASS

## Notes
- intel leg gated on GB-057 (intel spawn deadlock); full gfxbench baseline-vs-post is the milestone-acceptance leg.
