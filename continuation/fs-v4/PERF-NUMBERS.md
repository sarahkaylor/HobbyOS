# PERF-NUMBERS — browser acceptance perf (FS lane V4, FINAL merged state, flip + soak runs)

Binary: FINAL merged-verification WebProcess (ELF sha `fa621da1…`, disk flat
sha `09673700…`, 87,527,632 B; built 10:39Z).  OS FINAL merged tip `02f6756`;
fork `1af2b6d3fc` (R7+R4+X6).  Runner: V1 lane copy (+ V4 `--soak-min`,
`--mem-probe`, `--go-timeout`) `--instance v4 --port 8855`, lock
`/tmp/fs-accept-v4.lock`, QEMU virt AArch64 `-smp 8 -m 16384M -accel
tcg,thread=multi` (pure TCG).  All timings are **slow-but-working TCG numbers**
— baselines for the same runner/config, not hardware claims.

## Startup / first frame

Window native render 1916×982 (window content 1916×1018, chrome 36) at
scale 100%.  1st-page HOME load-ok ~2.4–2.9 s (browser monotonic).

## Load ms per page (browser `[WIN] load-ok url=… ms=` marker)

Fixture run (T2-14), 8 pages, one session, 0 FATAL:

| Row | URL | load-ok ms | delta | FPC |
|---|---|---|---|---|
| FIX01 | /fixture/FIX01 (267 B) | 10140 | 10140 | 0xb6ef95b9 |
| FIX02 | /fixture/FIX02 | 39382 | 29242 | 0xfefa62d3 |
| FIX02D | /fixture/FIX02D | 48265 | 8883 | 0xbd7d6a35 |
| FIX03 | /fixture/FIX03 | 57725 | 9460 | 0xe0736d7f |
| FIX04 | /fixture/FIX04 (642 B) | 68153 | 10428 | 0x6a3ac9a2 |
| FIX05 | /fixture/FIX05 (440 B) | 76820 | 8667 | 0xfa23e7c3 |
| HOME | /fixture/HOME | 85849 | 9029 | 0x6dfa1363 |
| TALL | /fixture/TALL | 95436 | 9587 | 0xa3e34d32 |

Reader (T2-13): reader/Hobbyist_operating_system load-ok `ms=19708`, FPC
`0x78168b3c`, page 1448 B (PAGE-NET sha `eaa65cad…` — byte-identical to V2c).

**Full-skin (T2-15, flipped):** relay `/wiki/Hobbyist_operating_system`
(120003 B wire) net fetch status=200, **load-ok `ms=425529`** (≈7.1 min TCG),
styled native frame `0x036bb41a`, 26 subresources (2 CSS load.php, 2 JPEGs,
wiki logo SVG, footer SVGs), 24/24 images decoded, 0 FATAL.  Soak repeat:
load-ok `ms=653995`, frame `0x1140aa94`.

**Direct https (T2-16, flipped):** `https://en.wikipedia.org/wiki/Hobbyist_operating_system`
status=200 (120003 B), **load-ok `ms=824991`** (≈13.7 min TCG — includes
33 subresource fetches, 30 over TLS, 24/24 images), styled full-skin frame
`0xfbd4089a` @ 1916×982, full article render vision-verified.

Per-page deltas are honest "one page fetch+parse+paint under this stack"
figures; the full-skin and direct legs are dominated by the real Wikipedia
page's many subresources parsed under TCG.

## Frame-times (R5 `[WIN] frame-times render=Xms blit=Yms total=Zms`)

| Run | N | render ms (mean / min / max) | blit ms (mean / min / max) | total (mean / min / max) |
|---|---|---|---|---|
| fixture (T2-14) | 18 | 410 / 81 / 1612 | 36 / 25 / 199 | 475 / 125 / 1670 |
| reader (T2-13) | 6 | 943 / 185 / 3266 | 45 / 18 / 162 | 1025 / 227 / 3309 |
| full-skin flip (T2-15) | 3 | 9219 / 254 / 26327 | 78 / 31 / 173 | 9355 / 312 / 26379 |
| direct flip (T2-16) | 3 | 12070 / 192 / 35111 | 77 / 25 / 181 | 12205 / 237 / 35158 |
| soak (T2-18, 10 rows) | 22 | 2993 / 84 / 33086 | 31 / 9 / 149 | 3045 / 138 / 33133 |

Interpretation: same as V2c — render dominates under TCG; the 1612–35111 ms
spikes are first-paint frames of a new page (parse burst).  Steady-state
frames 81–330 ms render.  Reblit-cached frames keep the settle cheap
(stable-repaint 3–10/pages).  0 FATAL across every run.

## Soak numbers (T2-18)

Wall: **10.8 min** (11:50:32 → 12:01:23), one full cycle = 10 rows
(8 fixture + reader + full-skin), per-row verdicts 8 pass / 2 gated / 0 fail,
0 FATAL.  The 2 gated rows (soak1-FIX02, soak1-reader) are transient
`Failure when receiving data from the peer` host-proxy drops — each row PASSES
in its dedicated T2-13/T2-14 run on the same binary.  Full row in-soak
load-ok `ms=653995` (frame `0x1140aa94`).

## Memory high-water (honest)

Guest `free` (sysinfo(2)) copy-back `mem-probe-soak.txt` / `MEMFREE.TXT`:
`total 7,784,628,224 B free 7,784,628,224 B used 0`.  The guest user-region
block pool counter reads used=0 because the WebProcess working set is
allocated through the frame allocator + lazy AS_V2 loader, which the
sysinfo(2) block-pool counter does not include (kernel `frame_high` exists but
is not syscall-exported).  → Guest-observable memory high-water is **not
reportable** through this kernel's sysinfo(2); V4 documents the block-pool
number + 0 FATAL + no crash across 10 pages as the honest metric.  (A small
kernel change exporting `frame_high` via sysinfo would close this; owner:
next lane.)

## Netlog census (per run)

- flip-full: 10 lines (relay 9, fixture 1 — relay routes; browser-side
  subresource fetches counted in serial `[WIN-sr]` = 26).
- flip-direct: 2 lines (self-checks only — the direct leg goes straight to
  en.wikipedia.org over TLS, bypasses the proxy).
- soak: relay+fixture routes as driven.

## Honesty notes

- Pure TCG: every wall number carries a large emulation tax; use as
  regression baselines for the same runner/config.
- The full-skin relay leg paints sidebar/TOC/logo but its main content column
  renders blank in screenshots (`[IMG1p] foreign=10003`), while the direct leg
  renders the same bytes fully (`foreign=93062`) — the relay-path paint gap is
  real and separate from the (fixed) parse stall; flagged to Integrator.
- 2 soak rows gated by transient host-proxy peer resets; standalone PASS
  receipts exist for both.
