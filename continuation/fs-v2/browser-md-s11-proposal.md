# Proposed browser.md §11 entry — FS-wave-2 T2 (V2) — for Integrator I

Suggested position: newest entry at the top of `## 11. Fix log` (append-only).
Integrator applies/edits; propose only.

---

- 2026-10-06 — **T2 protocol + receipts (FS-lane V2): frozen checklist, fixture
  reconciliation, dry run, clock row.**  V2 froze the T2 acceptance table
  (`continuation/fs-v2/T2-CHECKLIST.md`; permanent home proposed:
  `tests/fixtures/browser/T2-CHECKLIST.md`) mapping every T2 row from
  final-stretch §1 + §3 G1..G12 + the new **G5b guest-clock row (T2-19)**.
  browser.md R9/R13 clock mitigations folded in: on-device guest-epoch-vs-host
  receipt (new), wrong-CA negative (T2-02, WK-4c rc=77 ENFORCED), CA bundle
  pinned `/CERTS/CA.PEM`, calendar parity (F2.3 `1fce4f9`).  Fixture staging
  reconciled: fork `wk3/fixtures` (FIX01-05+FIX02D, IMG01, PROBE01, font) is
  authoritative for the T0 ladder; OS `tests/fixtures/browser/` (HOME/TALL)
  for the start/scroll pages — disjoint by name, one `/fixture` route serves
  both byte-identical; canonical map + sha/FPC anchors in the new
  `tests/fixtures/browser/README.md`.  Dry run on the current tip
  (OS 8a5558f / fork 4536f622cc) via the V1 acceptance runner
  (`tools/run_browser_accept.sh --mode fixture|reader --instance v2`, own
  port 8855, report.json v2): fixture leg re-ran with the documented pattern
  (FIX01-04 + HOME + TALL pass, stable FPC 0x759431c5/0x0e8f25c5 anchors held;
  FIX02D + FIX05 gated — F6/F7 resource-settle class, owner WN2); reader leg
  pass (load-ok at 0xdabdfbc5).  Direct/full modes classified **gated —
  OQ-1 fix pending merge (R1 `054b307db1` + rebuild)**, per the wave rule.
  **V2 gate: T2 table complete + dry run recorded.**
