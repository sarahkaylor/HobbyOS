# T2 Checklist — frozen rows, evidence map, gaps (FS-lane V2)

Lane: `browser/fs-v2` (OS worktree `/home/sarah/hobbyos-lanes/fs-v2` @ 8a5558f;
fork read-only `/home/sarah/webkit-lanes/fs-v2` @ 4536f622cc).
Freeze date: 2026-10-06.  Baseline docs: `final-stretch.md` §1 (T2 bar) + §3
G1..G12 + `browser.md` §9 R13 (clock-mitigations) + §11.

## Purpose

This is the fixed T2 acceptance table.  Every row below is a named check with
a stated evidence requirement and a pass criterion.  Section 4 is the **V2
gate deliverable**: *T2 table complete + dry run recorded* — every row is
either backed by committed evidence (path) or explicitly gated with a named
owner + pending item.  No row is left unmapped.

Rows T2-01..T2-12 are the T2 "capability set" (final-stretch §1 + G-inventory);
rows T2-13..T2-18 are the acceptance-run rows driven by
`tools/run_browser_accept.sh` (V1) modes; T2-19 is the new guest-clock row
(G5b); T2-20..T2-25 are the extended/x64 rows (mapped, out-of-scope for the
ARM T2 dry).  G1..G12 from §3 are cross-referenced per row.

## 1. T2 capability rows (what must work)

| Row | What's checked | Required evidence | Pass criterion | Owner / status |
|---|---|---|---|---|
| **T2-01** TLS 1.2+ ECDHE+AES-GCM (Wikimedia) | cipher suite negotiated on a real https fetch | serial `[WIN] net fetch … status=200` + netlog JSONL record with TLS/issuer fields | status=200 over https with ECDHE record | R1 — **GATED**: direct-https bytes GREEN on-device (`fs-r1 g0-direct-fixed`, 120003 B sha bb765a43…9e511) but TLS<->edge interop detail not yet re-verified post-fix; OQ-1 fix (R1 `054b307db1`) pending merge |
| **T2-02** ECDSA certificate verification (P-256/P-384, SHA-256/384, ISRG X1/X2) + SNI | real wiki cert chain validated, wrong-CA rejected | on-device CA-negative receipt (`curl … bogus CA → rc=77`) + positive fetch | negative rc=77 AND positive https 200 | N1/WK-4c — evidence on-device: WK-4c GATE GREEN (`browser.md` §11 2026-10-05: bogus CA → rc=77 ENFORCED); re-run on merged tree owed (see T2-17) |
| **T2-03** HTTP/1.1 + 301/302 incl. protocol-relative `Location` | redirect chain followed | netlog `upstream_status`/`status` + effective URL echo | 3xx → final content; effective-url evidence | N2 — **GATED**: WK-4c proved 302→follow on-device; protocol-relative row + merged-tree re-run = N2 matrix row pending |
| **T2-04** gzip decode (Wikimedia gzip-only) | decoded bytes == identity bytes | N1 receipts: `PAGE-NET.HTM` sha == fixture on gzip-vs-identity legs | decode byte-exact | **N1 GREEN** — on-device evidence `webkit-lanes/wbn/HobbyOS/continuation/fs-n1/evidence/RECEIPTS.md` (article 472784 B, big-body 1256646 B, identity controls byte-identical) |
| **T2-05** Large-body (≥1 MB decoded) transfer | 1.2 MB article fetches cleanly | N1 big-body leg receipts (wire 181437 B gzip → 1256646 B decoded, sha match) | fetch+persist 200, byte-exact | **N1 GREEN** (post-`477c5c4`); precise load-ok/ms owed post-R1-merge |
| **T2-06** Non-empty descriptive User-Agent | guest requests carry a real UA (empty UA → 403) | netlog `guest UA` + N1 wire logs (`Mozilla/5.0 (HobbyOS-WK6/1.0)`) | UA present on the wire | **N1 GREEN** — `Accept-Encoding: gzip, deflate` + UA in `fs-n1` wire logs |
| **T2-07** HTML5 parse (UTF-8) | real article HTML (multibyte) parses | on-device `[WIN] load-ok` + frame on reader/full row + screenshot | load-ok + non-blank frame | R2/R4 — reader row **PASSED** (T2-13); full-skin parse GATED (OQ-1) |
| **T2-08** CSS custom properties + flexbox + media queries + calc/min/max | CSS feature rows on real page | serial + screenshot; CSS feature confirmation on full-skin leg | features observed in render | R1-g1 — **GATED**: `fs-r1 g1-relay-full` root-caused (subresource wedge), fix pending merge (`054b307db1`) |
| **T2-09** JPEG + PNG decoders | ≥2 images decode and paint | R3 image census markers + screenshot + netlog image requests | ≥2 images fetched+decoded+painted | **WRAPPED into G3 / R3** — **GATED**: R3 lane (fetch+decode chain) not yet landed; owner R3 |
| **T2-10** Broad UTF-8 font fallback | glyphs render (not boxes) | glyph census (dark-pixel count on content text) + screenshot | legible glyphs at pixel level | R2 — **GATED**: font backend opt-in (`/USE-FONT`); R2-g glyph evidence pending |
| **T2-11** SVG render (SHOULD; site logo) / CSS grid (SHOULD) | logo + grid if present | screenshot + serial | renders without error | stretch — **GATED**: owner R2/R3, post-T2-09/10 |
| **T2-12** Fidelity ladder L1 (window-native render, legible text on reader page) | `view` == window content size; glyph census | serial viewport/paint lines + glyph census + screenshot (§6.3 v1.1) | native res, legible text | **GATED**: R5 (window-native viewport) + R2 (fonts) not merged |

## 2. Acceptance-run rows (driven by `run_browser_accept.sh`, V1)

| Row | Mode / URL | Required evidence | Pass criterion | Status (dry run 2026-10-06) |
|---|---|---|---|---|
| **T2-13** reader `<Topic>` leg | `--mode reader` (proxy REST extract) | report.json v2 row + screenshot + netlog | `[WIN] load-ok` + frame | **PASSED** (V1 `demo-reader` + this dry, see §4) |
| **T2-14** fixture FIX01-05 + FIX02D + HOME/TALL | `--mode fixture` | per-row report.json (load_ok, checksum=FPC, net, persist) + netlog | FIX01/02/03/04 + HOME + TALL load-ok with stable FPC | FIX01-04 + HOME + TALL **PASS**; FIX02D + FIX05 **GATED** (paint but load-timeout → F6/F7, owner WN2) |
| **T2-15** full-skin article (L2) | `--mode full` / raw relay | frame checksum + screenshot + CSS feature rows | full article renders, ≥2 images | **GATED**: OQ-1 (R1 `054b307db1` + rebuild); do not chase pre-merge |
| **T2-16** direct https leg | `--mode direct --url https://…` | per-request TLS/netlog + serial `[WIN] net fetch … status=200` | direct 200 + render | **GATED**: OQ-1 — fetch bytes GREEN on-device (R1 g0), windowed render pending merge |
| **T2-17** guest-shell HTTP GET + DNS | runner `--shell-get` + `dnstst` | `ROB.WC` sha + `SOCK2TST …: PASS` + DHCP DNS serial | guest GET 200 prefix + DNS resolve | **GATED/partial**: WK-4c receipts exist; merged-tree shell-GET leg pending V4/soak |
| **T2-18** 5-min/5-page soak + memory/time numbers | V4 runner | soak `ok=True`, per-page verdicts, memory high-water | clean soak + numbers recorded | **GATED**: V4 lane (needs R1-R4 complete) |

## 3. NEW row — guest clock (G5b, from final-stretch §3 G5b + browser.md R9 R13)

| Row | What's checked | Required evidence | Pass criterion | Status |
|---|---|---|---|---|
| **T2-19** guest wall clock ≈ host (cert validity window depends on it) | on-device RTC/date vs host `date -u`; wrong-CA still rejected when clock sane | guest `clock`/`sysinfo(6)` output (serial) + host wall clock side-by-side in the same receipt; CA-negative already covered in T2-02 | `|guest_epoch − host_epoch| ≤ 5 min` | **GATED (dry):** no on-device guest-epoch receipt committed yet and none of the runner's current modes (fixture/reader/direct/full) drives a wall-clock print — the receipt needs a small clock-probe step in a runner mode (console `sysinfo(6)`/kernel `time_test` `[rtc] epoch=` line — the machinery exists: PL031 RTC at 0x09010000 `src/kernel/arch/arm/rtc.c`, `libc/src/time.c`, kernel `time_test_suite` prints `[rtc] epoch=… -> YYYY-MM-DD HH:MM:SS`).  Owner: V2 (probe step, next wave) with N-lane as fix-owner only if the receipt fails (±5 min).  Related browser.md R9 R13 mitigations folded in: `clock_gettime`/`gettimeofday` calendar parity (F2.3, merged `1fce4f9`, host 314 checks), CA bundle pinned `/CERTS/CA.PEM`, wrong-CA negative (T2-02). |

> R13-device clock tests folded in (from browser.md R9 row + F2.3 + WK-4c):
> (a) guest epoch vs host epoch receipt — **T2-19**; (b) wrong-CA negative
> (bogus CA → rc=77) — **T2-02**; (c) CA bundle pinned `/CERTS/CA.PEM` —
> artifact-verified in N1/WK-4c build receipts; (d) calendar-math parity
> (F2.3 `1fce4f9`) — host-side unit tier (host 482/0) + `clock_test_host`.
> All four are folded into the T2 table; only (a) lacks an on-device receipt
> and is the V2 addition.

## 4. Evidence map (every row → path or gated-with-owner) — THE V2 GATE

| Row | Evidence path (committed) | Or gated-with-owner |
|---|---|---|
| T2-01 | `webkit-lanes/ib/…/fs-r1/evidence/g0-direct-fixed*/` (direct https fetch GREEN, serial `[WIN] net fetch … status=200 bytes=120003`) | GATED: R1 `054b307db1` + rebuild (OQ-1) |
| T2-02 | WK-4c evidence (browser.md §11 2026-10-05; WN2 gate: HTTP/HTTPS/redirect/cookie + CA-negative rc=77 ENFORCED) | merged-tree re-run owed (N2/V4) |
| T2-03 | WK-4c 302→follow on-device | protocol-relative matrix row: N2 (pending single fetch path) |
| T2-04 | `webkit-lanes/wbn/…/fs-n1/evidence/RECEIPTS.md` + `evidence/gzip*/`, `identity*` legs | — (N1 GREEN) |
| T2-05 | `fs-n1/evidence/bigbody*/` (1256646 B byte-exact, wire 181437 B) | — (N1 GREEN) |
| T2-06 | `fs-n1` wire logs (`wire-guest-*.txt` UA + Accept-Encoding) | — (N1 GREEN) |
| T2-07 | reader row PASSED (T2-13 evidence) | full-skin gated: R1 (OQ-1) |
| T2-08 | — | GATED: R1-g1 (`fs-r1 g1-relay-full` RCA; fix `054b307db1` pending merge) |
| T2-09 | — | GATED: R3 lane |
| T2-10 | — | GATED: R2 lane (glyph census pending) |
| T2-11 | — | GATED: R2/R3 stretch |
| T2-12 | — | GATED: R5 (+R2) |
| T2-13 | `fs-v1/evidence/fs-v1/demo-reader/report.json` (verdict pass, load-ok 8863 ms, FPC 0xdabdfbc5) **+ this dry run** | — (PASSED) |
| T2-14 | `fs-v1/evidence/fs-v1/demo-fixture/report.json` (6 pass/2 gated/0 fail) + V2 re-run | FIX02D/FIX05 gated-with-owner WN2 (F6/F7 class) |
| T2-15 | — | GATED: OQ-1 (R1 + rebuild); reassess at R1-g1 post-merge |
| T2-16 | R1 `g0-direct-fixed` fetch receipts | GATED: OQ-1 render side |
| T2-17 | WK-4c + N1 DNS/dnstst receipts | merged-tree shell-GET/dns leg: V4/soak owner |
| T2-18 | — | GATED: V4 (post R1-R4) |
| T2-19 | code path verified (PL031 `src/kernel/arch/arm/rtc.c`, `time.c`, kernel `time_test_suite`) but **no on-device receipt in this dry** — see §3/§4a | GATED: V2 (needs a clock-probe runner step); N-lane = fix owner only if receipt fails |

### 4a. V2 dry-run receipts (this session, instance v2)

| Run | Mode | Instance | Port | Evdir | Result |
|---|---|---|---|---|---|
| DRY-1 | fixture | v2 | 8855 | `continuation/fs-v2/evidence/dry-fixture` | **6 pass / 2 gated / 0 fail** (report.json v2, schema 2) |
| DRY-2 | reader | v2 | 8855 | `continuation/fs-v2/evidence/dry-reader` | **1 pass / 0 gated / 0 fail — verdict pass** (report.json v2) |

DRY-1 per-row (report `continuation/fs-v2/evidence/dry-fixture/report.json`):
FIX01 **pass** (load_ms 7016, FPC **0x759431c5** ✓ anchor) · FIX02 pass (14474,
0x63309a45) · FIX02D gated (frames 1, FPC 0x5f62b9c5 — paints, no load-ok,
F6/F7→WN2) · FIX03 pass (332236, 0xb1e75dc5) · FIX04 pass (342890, 0xbc622575)
· FIX05 gated (FPC 0xa0bf6dc5 — paints, no load-ok, F6/F7→WN2) · HOME **pass**
(666703, FPC **0x0e8f25c5** ✓ anchor) · TALL pass (679711, 0xe0982225).  0
FATAL, 0 open-fail, netlog 17 lines (routes relay 2 / fixture 13), DNS serial
evidence true (DHCP 10.0.2.3).  PAGE-NET.HTM sha 47e0fe49… == TALL fixture.
DRY-2 per-row: reader-Hobbyist_operating_system **pass** (load_ms 10483, FPC
**0xdabdfbc5** == documented reader-mode paint milestone), netlog 4 lines
(relay 1 / fixture 1 / reader 1), 0 FATAL, PAGE-NET.HTM 983 B sha 7d2caa19….

Instance hygiene: own sockets `/tmp/br-wc-ctrl-v2.sock`(+qmp, serial),
own port-file `/tmp/wiki-proxy-port-v2`, port 8855 (default 8800 untouched);
teardown removed QEMU + proxy cleanly; stale v2 socket/port-file removed by
lane after the runs (H4).  Direct/full classified **gated — OQ-1 fix pending
merge (R1 `054b307db1` + rebuild)**, not run this dry (per brief; R1's
decisive re-test holds the ARM slot concurrently).  Concurrent with R1's
`g0-direct-fixed5` and X4's intel KVM run — all isolated instances; my two
runs were sequential (one QEMU at a time per lane).

## 5. Gap list (rows not yet demonstrable + why + owner)

1. **T2-15 full-skin, T2-08 CSS features, T2-16 direct render, T2-07 full-skin
   parse** — one root cause: windowed subresource loads schedule to a dead
   NetworkProcessConnection and never resolve (R1 RCA).  Direct-https *bytes*
   are GREEN; the *render* is what the `054b307db1` fix enables. Owner R1;
   reassess on merge.  Until then every full-skin result is **pending**, not
   failed.
2. **T2-09 images, T2-10 fonts, T2-12 L1 fidelity** — R3 (resource loader) and
   R2 (fonts) lanes plus R5 (window-native viewport) are unmerged; no OS-side
   action.
3. **T2-18 soak + memory numbers** — V4 lane, dependent on R1-R4; the runner
   and report schema are ready (V1).
4. **T2-19 guest-clock receipt** — the V2 dry run could NOT collect an
   on-device guest-epoch receipt: the runner's modes never drive a wall-clock
   print (no CLI `date`; CLOCK.BIN is GUI; `[rtc] epoch=` lives in the kernel
   unit suite).  The QEMU virt PL031 RTC starts from host time (code path
   verified), so this is a **receipt-probe gap, not a fix gap**: add a small
   console/probe step to a runner mode (or capture the unit-tier
   `time_test_suite` `[rtc] epoch=` line) — owner V2, next wave; N-lane is
   fix-owner only if the receipt then comes back > ±5 min.
5. **T2-02/03/17 merged-tree re-runs** — the definitive re-runs happen on the
   merged tree with the R1 fix present (so the direct leg, not just relay,
   is the contract); folded into V3/V4 batteries.  Do not re-run the full
   direct matrix against the pre-merge tree beyond the OQ-1 decision.

## 6. Proposed permanent home (diff to Integrator)

`T2-CHECKLIST.md` should live at **`tests/fixtures/browser/T2-CHECKLIST.md`**
(next to the fixture README it cross-references and owns the same refs) with
the runner row-definitions duplicated into `tools/` (V1).  It is invoked by:
- V-gates: `V2 = T2 table complete + dry run recorded` (this doc),
  `V4 = soak ok=True`.
- final-stretch §6.3 acceptance protocol (every T2 row records pixel readback
  + DOM/probe + per-request netlog).
- CI (V3) as the acceptance subset row-list.

Kept also under `continuation/fs-v2/` for this lane's evidence trail.

## 7. Proposed browser.md §11 entry (for Integrator I to apply)

See `continuation/fs-v2/browser-md-s11-proposal.md`.
