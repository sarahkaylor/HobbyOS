# GB-046 game-cycling perf/leak pass

- arch: arm, cycles: 2
- bundle: `evidence/perf_cycle/20261018T012916Z` (gitignored)

| cycle | game | frozen | growth_mb |
|---|---|---|---|
| 0 | XNIBBLER.BIN | False | 0.27 |
| 0 | XCRYPT.BIN | False | 0.02 |
| 1 | XNIBBLER.BIN | False | 0.0 |
| 1 | XCRYPT.BIN | False | 0.0 |

Summary: **PASS**
