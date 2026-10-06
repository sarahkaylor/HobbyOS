# T2 Checklist — frozen rows, evidence map, gaps (FS-lane V2c — merged-state dry)

Lane: `browser/fs-v2c` (OS worktree `/home/sarah/hobbyos-lanes/fs-v2` @ `734b619`
= OS merged tip; fork `browser/l8-wk5` @ `16b88d4fb8` read-only).  Browser binary
under test: controller's merged-verification build
`/home/sarah/webkit-lanes/ib/WebKitBuild/HobbyOS-arm/bin/WebProcess`, ELF sha
`649713f0d8d36df6febfec641ce81c7e706c4757d175596a2af2836d0fbb409d`,
flat (disk) sha `f3761afd75a2d15441e67f8b770da525a4b0342dab8e3a920abf84fc1f3b178a`
(87,560,416 B).  Dry date: 2026-10-06 (W3).  Baseline docs: `final-stretch.md`
§1 (T2 bar) + §3 G1..G12 + `browser.md` §9 R13.

## Purpose

Fixed T2 acceptance table.  Section 4 is the **V2c gate deliverable**: every
row either backed by committed evidence (path) or explicitly gated with a named
owner + pending item, at the MERGED state (R1/R2/R3/R5/R1c/N1/X1-X5/H1/H2 in).
Rows T2-01..T2-12 are the capability set; T2-13..T2-18 the acceptance-run rows
(runner `tools/run_browser_accept.sh` V1 modes); T2-19 the guest-clock row
(G5b); T2-20..T2-25 extended/x64 (mapped, out of ARM T2-dry scope).

Runner config this dry: V1 runner (+ V2c clock-probe extension, lane copy
`continuation/fs-v2c/tools/run_browser_accept.sh`), `ACCEPT_ID=v2c
--instance v2c --port 8854`, lock `/tmp/fs-accept-v2c.lock`, disk staged from
the merged OS build (fs-v1 @ 734b619) + ib flat binary swap + `::/CLOCKPX.BIN`
probe.  All runs sequential (one QEMU per lane), 0 FATAL, 0 IDLESTUCK.

## 1. T2 capability rows (what must work) — status at MERGED state

| Row | What's checked | Required evidence | Pass criterion | Owner / status @ merged |
|---|---|---|---|---|
| **T2-01** TLS 1.2+ ECDHE+AES-GCM (Wikimedia) | cipher suite negotiated on a real https fetch | serial `[WIN] net fetch … status=200` + netlog TLS/issuer | status=200 over https | **GATED with owner R7** — direct-leg bytes GREEN (R1 `g0-direct-fixed` 120003 B sha `bb765a43…9e511`) but direct **render** needs the parser-completion fix R7 is landing (`bw-fs-r7`, g0-direct-fixed9).  OQ-1 wording retired at W2 merge. |
| **T2-02** ECDSA certificate verification (P-256/384, ISRG X1/X2) + SNI | real wiki cert chain valid, wrong-CA rejected | on-device CA-negative (bogus CA → rc=77) + positive fetch | neg rc=77 AND positive 200 | **GATED with owner R7/N2** — WK-4c on-device rc=77 ENFORCED pre-merge; merged-tree CA-negative re-run folded into direct-leg re-run (T2-16) once R7 lands. |
| **T2-03** HTTP/1.1 + 301/302 incl. protocol-relative `Location` | redirect chain followed | netlog upstream/status + effective URL echo | 3xx → final content | **GATED with owner N2/R7** — WK-4c 302→follow proven on-device pre-merge; protocol-relative matrix row + merged-tree re-run pending N2. |
| **T2-04** gzip decode (Wikimedia gzip-only) | decoded bytes == identity bytes | N1 receipts (`PAGE-NET.HTM` sha == fixture on gzip-vs-identity) | decode byte-exact | **N1 GREEN** (unchanged at merge) — `webkit-lanes/wbn/…/fs-n1/evidence/RECEIPTS.md` (472784 B, 1256646 B, identity controls byte-identical). |
| **T2-05** Large-body (≥1 MB decoded) transfer | 1.2 MB article fetches cleanly | N1 big-body receipts (181437 B wire → 1256646 B decoded) | 200, byte-exact | **N1 GREEN** — post-`477c5c4` receipts on-device. |
| **T2-06** Non-empty descriptive User-Agent | guest UA on the wire (empty UA → 403) | N1 wire logs | UA present | **N1 GREEN** — `Mozilla/5.0 (HobbyOS-WK6/1.0)` + `Accept-Encoding: gzip, deflate` in fs-n1 wire logs. |
| **T2-07** HTML5 parse (UTF-8) | real article HTML parses | `[WIN] load-ok` + frame + screenshot | load-ok + non-blank frame | Reader row **PASSED** (T2-13, dry-reader).  Full-skin parse: fetched+persisted 415161 B, **no load-ok** → **GATED owner R7** (parser-completion). |
| **T2-08** CSS custom properties + flexbox + media queries + calc/min/max | CSS feature rows on real page | serial + screenshot on full-skin | features observed | **GATED owner R7** — full-skin render pending parser-completion; reader-mode styled CSS (h1/paragraph/link) renders on the reader leg. |
| **T2-09** JPEG + PNG decoders | ≥2 images decode and paint | R3 census + screenshot + netlog img requests | ≥2 fetched+decoded+painted | **PASS @ merged (PNG leg)** — fixture FIX02 has 2 PNGs (IMG01.PNG + inline data: PNG): `[IMG1i] src=…IMG01.PNG complete=1 w=64 h=64 renderer=1` ×3 + `[IMG1d] imgs=1,1 rt=1` ×6, `[IMG1p] view=1916x982 … foreign=47824` (dry-fixture serial).  JPEG decoder merged (R3) but no JPEG exercised on-device this dry → JPEG sub-row noted gated-owner R7 full-skin. |
| **T2-10** Broad UTF-8 font fallback | glyphs render (not boxes) at pixel level | glyph census (dark-pixel count) + screenshot | legible glyphs | **PASS @ merged** — R2 font backend default-on (merged `48ce362` + `de579d93e6`), DejaVu staged `::/DEJAVU.TTF`; reader post-render screenshot glyph census dark_px=18055 / pure_black=13260 / nonwhite=103817 @ 1916×982 and VISION-READABLE text ("Hobbyist operating system" headline + lead paragraph) — dry-reader `e2e-post-render.png`. |
| **T2-11** SVG render (SHOULD) / CSS grid (SHOULD) | logo + grid if present | screenshot + serial | renders w/o error | **PASS @ merged (no-error)** — FIX05 SVG mix loaded load-ok at native res, 0 errors/FATAL; full-skin logo gated w/ R7. |
| **T2-12** Fidelity ladder L1 (window-native render, legible text on reader page) | `view` == window content; glyph census | serial `viewport`/`frame` lines + glyph census + screenshot | native res + legible | **PASS @ merged** — `[WIN] viewport render=1916x982 (window content 1916x1018, chrome 36, scale 100%)`; `[WIN] frame view=1916x982`; glyph census 18055 dark px; R5 native render + R2 fonts both merged. |

## 2a. Extended / x64 rows (G7..G12 — mapped, out-of-scope for the ARM T2 dry)

| Row | What's checked | Evidence / owner | Status |
|---|---|---|---|
| **T2-20** x64 PS/2 button delivery (G7) | click-launch + click-nav on intel | X1 — `fs-x1` `4b611c1` (E0 parity) + `efbc25e` rel-pointer drivers | **GATED: X1** (wheel stretch remains) |
| **T2-21** x64 LOSTWAKE/TSC (G8) | `-smp 8` stability + TSC ~18× fix | X2 — `fs-x2` `2f48639` PIT-cal, `d65e75a` RCA (docs RESOLVED) | **GATED: X2** (unit-x64 hard gate) |
| **T2-22** x64 white-paint (G9) | frame parity post-WA-fix | X3 — CURED run 134; WA fix `49f28a3bb5` merged | **PASSED (x64 leg)** |
| **T2-23** intel runtime net leg (G10) | intel https fetch + load ms | X4 — `fs-x4` (WE2): x64 https 200/28,275 B sha `48b98d80` == windowed == ARM | **GATED: X4** (windowed re-run in flight) |
| **T2-24** Google Search + CNN (G11) | WK-6 §8.3 rows re-run with render fix | WN3 `wk6-live` receipts; re-run post-merge | **GATED: WK-6 close** |
| **T2-25** license package, docs refresh, WK3 unification (G12) | license relink + docs | H1 `wk7-license/` + H2 docs wave | **GATED: H1/H2** (both merged) |

## 2b. Acceptance-run rows (run_browser_accept.sh, V1) — status @ merged

| Row | Mode / URL | Required evidence | Pass criterion | Status (V2c dry 2026-10-06) |
|---|---|---|---|---|
| **T2-13** reader `<Topic>` leg | `--mode reader` (proxy REST extract) | report.json v2 + screenshot + netlog | `[WIN] load-ok` + frame + legible | **PASS** — dry-reader: load-ok `ms=11418` (post-shot run), FPC `0x91d24588` (styled article frame), netlog 4 lines, 0 FATAL; post-render shot vision-verified legible text. |
| **T2-14** fixture FIX01-05 + FIX02D + HOME/TALL | `--mode fixture` | per-row report.json + netlog | all load-ok with stable FPC | **PASS — 8/8** (all rows passed) — dry-fixture: FIX01..FIX05 + FIX02D + HOME + TALL all `load-ok` (FIX02D and FIX05 — previously gated F6/F7 — now PASS on the merged binary), checksums FIX01/2 `0xab92ba47`, FIX02D/3 `0x1739d8d3`, FIX04 `0x8f7f0f87`, FIX05 `0xe8700da6`, HOME/TALL `0x44135d8c`; 0 FATAL/0 open-fail. |
| **T2-15** full-skin article (L2) | `--mode full` — `http://…/wiki/Web_browser` (proxy relay) | frame checksum + screenshot + CSS rows | article renders, ≥2 images | **GATED owner R7** — dry-full: net fetch 200 `bytes=415161`, persisted to PAGE-NET; **no load-ok** in 300 s window (parser-completion stall, R7's exact RCA).  Re-run when R7 lands g0-direct-fixed9. |
| **T2-16** direct https leg | `--mode direct --url https://…` | TLS/netlog + `[WIN] net fetch … status=200` | direct 200 + render | **GATED owner R7** — fetch bytes GREEN on-device (R1 g0); windowed direct render pending parser-completion. |
| **T2-17** guest-shell HTTP GET + DNS | runner `--shell-get` + `dnstst` | `ROB.WC` sha + `SOCK2TST …: PASS` + DHCP DNS | guest GET 200 prefix + DNS resolve | **GATED/partial owner V4** — WK-4c + N1 DNS receipts exist; merged-tree shell-GET leg scheduled in V4/soak. DNS serial evidence true (DHCP 10.0.2.3) in every run this dry. |
| **T2-18** 5-min/5-page soak + memory/time numbers | V4 runner | soak `ok=True`, per-page verdicts, memory high-water | clean soak + numbers | **GATED owner V4** — partial perf numbers already collected (PERF-NUMBERS.md); full soak + memory high-water = V4. |

## 3. Guest clock (G5b — T2-19)

| Row | What's checked | Required evidence | Pass criterion | Status |
|---|---|---|---|---|
| **T2-19** guest wall clock ≈ host (cert validity window) | on-device RTC/date vs host `date -u` | guest epoch (CLOCKPX probe → `/CLOCKPX.TXT` copy-back) + host epoch side-by-side | `|guest − host| ≤ 5 min` | **PASS** — runner clock-probe step added (lane runner copy `--clock-probe`; guest probe `::/CLOCKPX.BIN`, `continuation/fs-v2c/tools/clockprobe.c` built standalone, sha `b34db413…`).  Receipt `dry-clock/clock-probe.txt` + `dry-reader/clock-probe.txt`: host t0 `1791278229` / t1 `1791278262`; guest `[CLOCKPX] epoch=1791278242 sec; 2026-10-06 09:17:22 UTC Tue` → Δ = **13 s** (within the ±5 min window); boot-anchored realtime (hb_clock_realtime_ms, PL031 RTC 0x09010000) confirmed in sync with host.  Document-only: QEMU virt PL031 starts from host time; no fix needed. |

## 4. Evidence map (every row → path or gated-with-owner) — merged state

| Row | Evidence path (committed) | Or gated-with-owner |
|---|---|---|
| T2-01 | — | GATED: R7 (direct render incl. TLS interop re-check) |
| T2-02 | WK-4c (browser.md §11: CA-negative rc=77 ENFORCED) | merged-tree CA-negative re-run: R7/N2 |
| T2-03 | WK-4c 302→follow on-device | protocol-relative matrix: N2 |
| T2-04 | `fs-n1/evidence/RECEIPTS.md` + `gzip*/identity*` legs | — (N1 GREEN) |
| T2-05 | `fs-n1/evidence/bigbody*/` (1256646 B byte-exact) | — (N1 GREEN) |
| T2-06 | `fs-n1` wire logs (UA + Accept-Encoding) | — (N1 GREEN) |
| T2-07 | dry-reader (T2-13 evidence: load-ok + styled frame) | full-skin parse: R7 |
| T2-08 | — | GATED: R7 (full-skin) |
| T2-09 | `continuation/fs-v2c/receipts/serial-markers-dry-fixture.txt` (`[IMG1i]`/`[IMG1d]` PNG census) | JPEG sub-row: R7 full-skin |
| T2-10 | `continuation/fs-v2c/receipts/reader-post-render-1916x982.png` (glyph census 18055 dark px; vision-read) | — (PASS @ merged) |
| T2-11 | dry-fixture FIX05 load-ok (SVG-inclusive mix, 0 errors) | full-skin logo: R7 |
| T2-12 | dry-fixture/dry-reader serial `viewport render=1916x982` + glyph census | — (PASS @ merged) |
| T2-13 | `continuation/fs-v2c/receipts/report-dry-reader-v2.json` (verdict pass, load-ok) | — (PASSED) |
| T2-14 | `continuation/fs-v2c/receipts/report-dry-fixture-v2.json` (verdict pass 8/8) | — (PASSED; FIX02D/FIX05 now pass) |
| T2-15 | `continuation/fs-v2c/receipts/report-dry-full-v2.json` (fetch 415161 B persisted, no load-ok) | GATED: R7 (parser-completion) |
| T2-16 | R1 `g0-direct-fixed` fetch receipts | GATED: R7 (render side) |
| T2-17 | WK-4c + N1 DNS receipts | GATED: V4 (merged-tree shell-GET leg) |
| T2-18 | perf partial → `continuation/fs-v2c/receipts/PERF-NUMBERS.md` | GATED: V4 (soak + memory high-water) |
| T2-19 | **`continuation/fs-v2c/receipts/clock-probe-dry-clock.txt`** (+ dry-reader copy) — guest `1791278242` vs host `1791278229/1791278262`, Δ13 s | — (PASS; probe step in lane runner copy) |
| T2-20 | X1 `fs-x1` `4b611c1`/`efbc25e` | GATED: X1 retest in flight (wheel stretch) |
| T2-21 | X2 `fs-x2` `2f48639`/`d65e75a` (docs RESOLVED) | GATED: X2 |
| T2-22 | X3 CURED `408cb3c` (run 134) | — (PASSED, x64 leg) |
| T2-23 | X4 `fs-x4` WE2 (https 200, sha parity) | GATED: X4 windowed re-run |
| T2-24 | WN3 `wk6-live` receipts | GATED: WK-6 close |
| T2-25 | H1 `wk7-license/`; H2 docs wave | GATED: H1/H2 (merged) |

### 4a. V2c dry-run receipts (this session — merged state, instance v2c)

| Run | Mode | Instance | Port | Evdir | Result |
|---|---|---|---|---|---|
| DRY-1 | fixture | v2c | 8854 | `continuation/fs-v2c/receipts` | **8 pass / 0 gated / 0 fail — verdict pass** (report.json v2) |
| DRY-2 | reader | v2c | 8854 | `continuation/fs-v2c/receipts` | **1 pass / 0 gated / 0 fail — verdict pass** (+ post-render shot) |
| DRY-3 | full | v2c | 8854 | `continuation/fs-v2c/receipts` | **0 pass / 1 gated / 0 fail** (fetch OK, parser-completion) |
| DRY-4 | reader+clock | v2c | 8854 | `continuation/fs-v2c/receipts` | **1 pass / 0 gated / 0 fail** + T2-19 receipt |

DRY-1 per-row (`dry-fixture/report.json`): FIX01 **pass** (load-ok@7303 ms, FPC
0xab92ba47) · FIX02 pass (15253, 0xab92ba47) · **FIX02D pass** (22597,
0x1739d8d3 — was gated in V2's dry) · FIX03 pass (30177, 0x1739d8d3) · FIX04
pass (38056, 0x8f7f0f87) · **FIX05 pass** (45445, 0xe8700da6 — was gated) ·
HOME pass (52682, 0x44135d8c) · TALL pass (62688, 0x44135d8c).  0 FATAL, 0
open-fail, netlog 12 lines, 20 `[WIN] frame-times`, 3×6 IMG1i/IMG1d + 20 IMG1p
census, DHCP DNS serial evidence true.  Viewport native 1916×982 (scale 100%).
DRY-2 per-row (`dry-reader/report.json`): reader-Hobbyist_operating_system
**pass** (load-ok@11418 ms, FPC 0x91d24588 — styled article frame), netlog 4
lines, page_sha `eaa65cad…`; post-render screenshot legible article text
(headline "Hobbyist operating system" + lead paragraph), glyph census dark
18055 / black 13260 / nonwhite 103817 over the 1916×982 content rect.
DRY-4 T2-19 receipt: guest `1791278242` (09:17:22 UTC Tue) vs host
`1791278229`/`1791278262` → Δ 13 s ≤ 300 s **PASS**.

Instance hygiene: own sockets `/tmp/br-wc-ctrl-v2c.sock`(+qmp, serial), own
port-file `/tmp/wiki-proxy-port-v2c`, port 8854; runs sequential (one QEMU at
a time); teardown clean.  Concurrent fleet untouched (verify-build ib, R7
wbn, R4 wab, X6 wf1b all isolated).

## 5. Gap list (rows not yet demonstrable at merged state + why + owner)

1. **T2-15 full-skin, T2-08 CSS features, T2-16 direct render, T2-07
   full-skin parse** — one root cause remains: the main-document
   parser-completion stall (post-delivery; R1/R1c subresource+wathdog fixes are
   in and working — 415161 B full article fetched+persisted — but
   `readyState=Complete`/`load-ok` never fires for the full-skin leg).  R7
   owns the fix (parser/RunLoop pump, g0-direct-fixed9).  Until then every
   full-skin result is **gated**, not failed.
2. **T2-09 JPEG leg** — PNG decode+paint proven (fixture census); JPEG decoder
   merged but unexercised on-device this dry; exercise with R7's full-skin.
3. **T2-18 soak + memory high-water** — V4 lane (runner + report schema ready).
4. **T2-02/03/17 merged-tree re-runs** — direct-leg TLS/CA/redirect re-runs
   fold into R7's fixed9 + N2's matrix; shell-GET leg into V4.  Rubber-stamp
   bar: fetch bytes byte-exact on merged tree already (fixture/relay legs).
5. **Watchdog (R1c) regression signal** — watchdog/hung-fetch closed by R1c at
   merge (36/36 subresources); this dry shows 0 FATAL / 0 IDLESTUCK / all
   images `complete=1` across all four runs — no watchdog re-arm fired, which
   is the clean signal.  A deliberate hung-fetch negative test is a V4 soak
   item.

## 6. Permanence

T2-CHECKLIST.md lives at `tests/fixtures/browser/T2-CHECKLIST.md` (next to the
fixture README it owns) as proposed by V2; a copy stays under
`continuation/fs-v2c/` for this lane's evidence trail.  Runner clock-probe
extension proposal (small, additive) is in `continuation/fs-v2c/browser-md-s11-proposal.md`
for Integrator I.

## 7. Proposed browser.md §11 entry (for Integrator I to apply)

See `continuation/fs-v2c/browser-md-s11-proposal.md`.
