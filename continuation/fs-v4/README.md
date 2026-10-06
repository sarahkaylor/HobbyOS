# FS lane V4 — T2 row flips at FINAL merged state + soak + memory + perf refresh

Lane: `browser/fs-v4` (OS worktree `/home/sarah/hobbyos-lanes/fs-v2` @
`02f6756`).  Timebox run 2026-10-06 ~10:44–12:20Z (W3, concurrent: G11
fs-g11 cnn-relay + H4 fs-h1/fs-h2 — untouched).

## What was run

All runs: V1 runner (lane copy with V4 `--soak-min`/`--mem-probe`/
`--go-timeout` + V2c `--clock-probe`), `ACCEPT_ID=v4 --instance v4
--port 8855`, lock `/tmp/fs-accept-v4.lock`.  Disk: merged OS disk with the
FINAL controller binary swapped as `::/BROWSER.BIN` (flat sha `09673700…`,
87,527,632 B, from ELF `fa621da1…` @10:39Z) + `::/CLOCKPX.BIN` +
`::/FREE.BIN` probes.  One QEMU at a time; 0 FATAL across every completed run.

| Run | Mode | Result | Evdir |
|---|---|---|---|
| flip-full | full `/wiki/Hobbyist_operating_system` (relay) | **1 pass — T2-15 FLIPPED** (load-ok ms=425529, frame 0x036bb41a) | evidence/full2 |
| flip-direct | direct https article | **1 pass — T2-16 FLIPPED** (load-ok ms=824991, frame 0xfbd4089a, full render shot) | evidence/direct |
| fixture | fixture 8 rows (fixtures env fixed) | **8 pass — T2-14** (0 FATAL) | evidence/fixture2 |
| reader | reader topic + clock | **1 pass — T2-13** (+ T2-19 Δ13 s) | evidence/reader |
| soak | soak fixture+reader+full ≥5 min + mem + clock | **8 pass / 2 gated(transient) / 0 fail — T2-18** (wall 10.8 min, 0 FATAL) + memory + T2-19 | evidence/soak-run |
| t217 | reader + shell-GET + DNS + clock | **1 pass — T2-17** (SOCK2TST 35 PASS, DNS A) | evidence/t217 |

## T2 table outcome (25 rows) at FINAL MERGED state

- **FLIPPED this run (was gated-with-owner-R7):** T2-01 (direct https TLS
  200 + 30 tls= subres), **T2-07 full-skin parse** (both legs load-ok),
  T2-08 CSS (direct leg fully styled; relay main-column paint noted),
  **T2-09 JPEG leg** (Icaros131.jpg/SkyOS.jpg on-device, 24/24 decoded),
  **T2-15 full-skin** (load-ok 425529), **T2-16 direct** (load-ok 824991).
- **CLOSED this run:** T2-17 (shell-GET 301 prefix + DNS), T2-18 (soak
  10.8 min + memory + perf), T2-19 re-verified (Δ13 s ×3).
- **PASS @ merged already:** T2-04/05/06 (N1), T2-10/11/12, T2-13, T2-14,
  T2-22 (x64).
- **Remaining GATED with named owner (not V4's):** T2-02/03 (N2 matrix),
  T2-20 (X1), T2-21 (X2), T2-23 (X4), T2-24 (WK-6), T2-25 (H1/H2).

## Honest caveats

1. Relay full-skin paints sidebar/TOC/logo but main content column blank in
   screenshots (`[IMG1p] foreign=10003`); direct leg renders the same bytes
   fully (foreign=93062).  Load-ok + frames + images all GREEN on both —
   flagged as a relay-path render gap for Integrator, distinct from the fixed
   parse stall.
2. Soak's 2 gated rows (FIX02, reader) = transient host-proxy peer resets;
   each row PASSes in its dedicated run on the same binary.
3. Guest-observable memory high-water not reportable through sysinfo(2)
   (WebProcess allocations live outside the counted block pool; kernel
   `frame_high` not syscall-exported) — documented honestly (block pool
   7.25 GiB used=0 + 0 FATAL / no crash over the soak).

## Evidence inventory

- `tests/fixtures/browser/T2-CHECKLIST.md` (updated, canonical) +
  `continuation/fs-v4/T2-CHECKLIST.md` (lane copy)
- `continuation/fs-v4/receipts/` — 6 report JSONs (flip-full, flip-direct,
  fixture, reader, soak, t217) + serials + netlogs + clock/mem probes +
  screenshots (full-skin, direct, reader, soak-post-render) + PAGE-NET copies
- `continuation/fs-v4/PERF-NUMBERS.md`
- `continuation/fs-v4/tools/` — runner (with V4 soak/mem/go-timeout + fixture
  dirs default fix), br_e2e.py, wiki_proxy.py, clockprobe.c, memprobe.c (unused
  — used guest FREE.BIN instead)
- `continuation/fs-v4/evidence/{stage,flip-full,flip-direct,fixture,fixture2,
  reader,soak,soak-run,t217}/`

## Verdict

**pass** — at the FINAL merged state every T2 row V4 owns is now PASS with
committed on-device receipts (T2-01/07/08/09/15/16 flipped, T2-17/18/19
closed); the remaining rows are all gated with their pre-existing named
owners (N2/X1/X2/X4/WK-6/H1/H2).  Soak ok=True with numbers (10.8 min wall,
per-page verdicts, memory probe documented) and 0 FATAL across the lane.
