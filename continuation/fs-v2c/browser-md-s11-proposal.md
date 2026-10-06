# browser.md §11 proposal — FS V2c (merged-state T2 dry + guest clock + perf)

For Integrator I to apply (kept separate from OS commits, per V2c brief).

## §11 entry text (merge-train record)

- **FS-V2c (T2 dry at merged state + T2-19 guest-clock receipt + perf numbers):**
  fixture 8/8 PASS (FIX02D/FIX05 previously-gated rows now pass on the merged
  binary), reader 1/1 PASS with native 1916×982 legible styled frame (0x91d24588;
  glyph census 18055 dark px; vision-verified), viewport render=1916×982
  (content 1916×1018, chrome 36, scale 100%) on every run, 0 FATAL / 0
  IDLESTUCK.  Full-skin (415,161 B fetched+persisted, relay) and direct-https
  render rows remain **gated-with-owner R7** (parser-completion,
  g0-direct-fixed9).  **T2-19 GUEST CLOCK PASS:** runner clock-probe receipts —
  guest epoch 1791278242 (2026-10-06 09:17:22Z) vs host 1791278229/1791278262
  → Δ13s ≤ ±5 min (PL031 RTC starts from host time; document-only, no fix).
  Perf: PERF-NUMBERS.md/JSON (pure-TCG slow-but-working; fixture per-page
  load-ok deltas 7.2-10.0 s, frame-times render mean 209 ms fixture / 368 ms
  reader, blit mean ~28 ms; loaded on `browser/fs-v2c`).
- **Tools (V2c, additive):** runner `--clock-probe` step (lane copy,
  `continuation/fs-v2c/tools/run_browser_accept.sh`) + guest probe
  `CLOCKPX.BIN` (`clockprobe.c`, standalone build, sha b34db413) staged on
  acceptance disks during the dry.  Proposal only — V1 runner untouched.

## Evidence mapping (canonical)

- T2-CHECKLIST.md (25 rows, evidence-mapped) → `tests/fixtures/browser/T2-CHECKLIST.md`
- Runner receipts → `continuation/fs-v2c/evidence/{dry-fixture,dry-reader,dry-clock,dry-full}/`
- Perf → `continuation/fs-v2c/PERF-NUMBERS.md` + `PERF-NUMBERS.json`
- Browser binary under test: ELF `649713f0…`, flat `f3761afd…` (87,560,416 B)

## Notes for Integrator

- T2-15/T2-16/T2-08 full-skin + direct remain OQ-gated on R7 — do NOT mark
  them green in §6 boxes until R7's parser-completion lands and the full-skin
  leg passes.
- The `--clock-probe` runner extension and `CLOCKPX.BIN` staging recipe are
  ready to fold into V1's runner + V4 soak (small, backward-compatible).
