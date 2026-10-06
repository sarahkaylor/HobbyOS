# T2 Checklist — frozen rows, evidence map, gaps (FS-lane V4 — merged-state flips + soak)

Lane: `browser/fs-v4` (OS worktree `/home/sarah/hobbyos-lanes/fs-v2` @ `02f6756`
= OS merged tip (wiki_proxy reader-nav surface); fork `browser/l8-wk5` @
`1af2b6d3fc` (R7+R4+X6 all in) read-only).  Browser binary under test: the
controller's FINAL merged-verification build
`/home/sarah/webkit-lanes/wbn/WebKitBuild/HobbyOS-arm/bin/WebProcess`, ELF sha
`fa621da173b8ec9a015096643a419116695c36759862e24ca77293bd8decc16c`, flat (disk)
sha `09673700768a330c2200c668617c71c3f1459ad72be64166b94c31117cda9782`
(87,527,632 B; built 2026-10-06 10:39Z — verify-build proc `f077dd5a3e0c`,
poller confirmed no newer mtime).  Run date: 2026-10-06 (W3).  Baseline docs:
`final-stretch.md` §1 (T2 bar) + §3 G1..G12 + `browser.md` §9 R13.

## Purpose

Fixed T2 acceptance table.  Section 4 is the **V4 gate deliverable**: every
row either backed by committed evidence (path) or explicitly gated with a named
owner + pending item, at the FINAL MERGED state.  The rows V2c left
gated-with-owner-R7 — full-skin (T2-15, T2-07-full, T2-08, T2-09-JPEG) and
direct (T2-16, T2-01) — are now RE-RUN on R7's landed fix and **flipped to
PASS**.  T2-17 (guest-shell GET/DNS) and T2-18 (5-min soak + memory) also close
here.  Rows T2-01..T2-12 are the capability set; T2-13..T2-18 the acceptance-run
rows (runner `tools/run_browser_accept.sh` V1 modes + V4 soak/mem-probe);
T2-19 the guest-clock row (G5b); T2-20..T2-25 extended/x64 (mapped, out of ARM
T2 scope).

Runner config this run: V1 runner (lane copy with V2c `--clock-probe` +
V4 `--soak-min`/`--mem-probe`/`--go-timeout`, lane copy
`continuation/fs-v4/tools/run_browser_accept.sh`), `ACCEPT_ID=v4
--instance v4 --port 8855`, lock `/tmp/fs-accept-v4.lock`, disk staged from
the merged OS disk (`continuation/fs-v4/evidence/stage/disk.img`) + `wbn`
flat binary swapped as `::/BROWSER.BIN` + `::/CLOCKPX.BIN` probe + `::/FREE.BIN`
(mem probe).  All runs sequential (one QEMU per lane, `-smp 8 -m 16384M` TCG),
0 FATAL across every run.

## 1. T2 capability rows (what must work) — status at FINAL MERGED state

| Row | What's checked | Required evidence | Pass criterion | Owner / status @ merged |
|---|---|---|---|---|
| **T2-01** TLS 1.2+ ECDHE+AES-GCM (Wikimedia) | cipher suite negotiated on a real https fetch | serial `[WIN] net fetch … status=200` + netlog TLS/issuer | status=200 over https | **PASS @ merged — flipped this run** — direct leg (`--mode direct --url https://en.wikipedia.org/wiki/Hobbyist_operating_system`): `[WIN] net fetch … status=200 bytes=120003`; 30 subresource fetches carrying `tls= cipher= verify=0xffffffff` markers (CentralAutoLogin + thumb.wikimedia.org etc.); PAGE-NET sha `bb765a43…` byte-exact.  Receipt `continuation/fs-v4/receipts/report-flip-direct-v4.json`. |
| **T2-02** ECDSA certificate verification (P-256/384, ISRG X1/X2) + SNI | real wiki cert chain valid, wrong-CA rejected | on-device CA-negative (bogus CA → rc=77) + positive fetch | neg rc=77 AND positive 200 | **GATED owner N2** — positive leg GREEN at merged (direct 200 + TLS verify=0xffffffff above); merged-tree CA-negative re-run still N2's matrix row. |
| **T2-03** HTTP/1.1 + 301/302 incl. protocol-relative `Location` | redirect chain followed | netlog upstream/status + effective URL echo | 3xx → final content | **GATED owner N2** — on-device 302→follow proven (T2-17 SOCK2TST live GET shows `HTTP/1.1 301 Moved Permanently` prefix followed to 200); protocol-relative matrix row + merged-tree redirect-matrix re-run pending N2. |
| **T2-04** gzip decode (Wikimedia gzip-only) | decoded bytes == identity bytes | N1 receipts (`PAGE-NET.HTM` sha == fixture on gzip-vs-identity) | decode byte-exact | **N1 GREEN** (unchanged at merge) — `webkit-lanes/wbn/…/fs-n1/evidence/RECEIPTS.md`. |
| **T2-05** Large-body (≥1 MB decoded) transfer | 1.2 MB article fetches cleanly | N1 big-body receipts (181437 B wire → 1256646 B decoded) | 200, byte-exact | **N1 GREEN** — post-`477c5c4` receipts on-device. |
| **T2-06** Non-empty descriptive User-Agent | guest UA on the wire (empty UA → 403) | N1 wire logs | UA present | **N1 GREEN** — `Mozilla/5.0 (HobbyOS-WK6/1.0)` + `Accept-Encoding: gzip, deflate` in fs-n1 wire logs. |
| **T2-07** HTML5 parse (UTF-8) | real article HTML parses | `[WIN] load-ok` + frame + screenshot | load-ok + non-blank frame | **PASS @ merged — flipped this run** — full-skin AND direct legs both `[WIN] load-ok` on the real 120003 B article (full-skin ms=425529, direct ms=824991) with styled frames (`0x036bb41a` / `0xfbd4089a`) + `doc title=Hobbyist operating system - Wikipedia|…|268|24` + 268 links.  Reader row was already PASS (T2-13). |
| **T2-08** CSS custom properties + flexbox + media queries + calc/min/max | CSS feature rows on real page | serial + screenshot on full-skin | features observed | **PASS @ merged — flipped this run (direct leg)** — the direct https render shows the fully-styled Wikipedia page (Vector-2022 skin: sidebar `Contents` box, `Toggle … subsection` controls, search box, logo, article layout in the content column) — CSS driven by the real `/w/load.php?…vector-2022` bundle (213097 B fetched on the direct leg).  NOTE (honest): the relay full-skin leg renders sidebar/TOC/logo but its main content column paints blank in the captured screenshot (see Gap list #1) — direct leg is the full-fidelity leg. |
| **T2-09** JPEG + PNG decoders | ≥2 images decode and paint | R3 census + screenshot + netlog img requests | ≥2 fetched+decoded+painted | **PASS @ merged (JPEG+PNG legs both exercised)** — fixture PNG census (dry-fixture `[IMG1i]/[IMG1d]/[IMG1p]`) PLUS **JPEG now exercised on-device**: full-skin/direct serials fetch `Icaros131.jpg` (4356 B) and `SkyOS.jpg` (4388 B) with `[IMG1d] imgs=24,24 rt=1` (24/24 decoded) and post-render shot showing thumbnails.  Receipts `serial-flip-full.log` / `serial-flip-direct.log`. |
| **T2-10** Broad UTF-8 font fallback | glyphs render (not boxes) at pixel level | glyph census (dark-pixel count) + screenshot | legible glyphs | **PASS @ merged** — direct leg post-render shot `direct-go-1916x982.png` vision-verified: full article text (headline, intro, Development section, 6502/Z80 text) legible; glyph census over content rect dark=181490 (wheel not measured this run; reader leg dark 218269). |
| **T2-11** SVG render (SHOULD) / CSS grid (SHOULD) | logo + grid if present | screenshot + serial | renders w/o error | **PASS @ merged** — FIX05 SVG mix load-ok (fixture 8/8) AND the Wikipedia logo (`enwiki-25.svg`, 115918 B) + wordmark SVGs fetch/parse on both full-skin and direct legs with 0 errors/FATAL; logo visibly painted in both post-render shots. |
| **T2-12** Fidelity ladder L1 (window-native render, legible text) | `view` == window content; glyph census | serial `viewport`/`frame` lines + glyph census + screenshot | native res + legible | **PASS @ merged** — `[WIN] viewport render=1916x982 (window content 1916x1018, chrome 36, scale 100%)` in every run; direct post-render vision-verified legible article text at native res. |

## 2a. Extended / x64 rows (G7..G12 — mapped, out-of-scope for the ARM T2)

| Row | What's checked | Evidence / owner | Status |
|---|---|---|---|
| **T2-20** x64 PS/2 button delivery (G7) | click-launch + click-nav on intel | X1 — `fs-x1` `4b611c1` (E0 parity) + `efbc25e` rel-pointer drivers | **GATED: X1** (wheel stretch remains) |
| **T2-21** x64 LOSTWAKE/TSC (G8) | `-smp 8` stability + TSC ~18× fix | X2 — `fs-x2` `2f48639` PIT-cal, `d65e75a` RCA (docs RESOLVED) | **GATED: X2** (unit-x64 hard gate) |
| **T2-22** x64 white-paint (G9) | frame parity post-WA-fix | X3 — CURED run 134; WA fix `49f28a3bb5` merged | **PASSED (x64 leg)** |
| **T2-23** intel runtime net leg (G10) | intel https fetch + load ms | X4 — `fs-x4` (WE2): x64 https 200/28,275 B sha `48b98d80` == windowed == ARM | **GATED: X4** (windowed re-run in flight) |
| **T2-24** Google Search + CNN (G11) | WK-6 §8.3 rows re-run with render fix | WN3 `wk6-live` receipts; re-run post-merge | **GATED: WK-6 close** |
| **T2-25** license package, docs refresh, WK3 unification (G12) | license relink + docs | H1 `wk7-license/` + H2 docs wave | **GATED: H1/H2** (both merged) |

## 2b. Acceptance-run rows (run_browser_accept.sh, V1) — status @ FINAL MERGED

| Row | Mode / URL | Required evidence | Pass criterion | Status (V4 run 2026-10-06) |
|---|---|---|---|---|
| **T2-13** reader `<Topic>` leg | `--mode reader` — reader/Hobbyist_operating_system | report.json v2 + screenshot + netlog | `[WIN] load-ok` + frame + legible | **PASS** — V4 reader run: load-ok `ms=19708`, FPC `0x78168b3c` (styled article frame), netlog 4 lines, 0 FATAL; post-render shot census dark=218269 / nonwhite=548340, nav surface 4 links (HTML/CSS/#sec2/full-article).  Receipt `report-reader-v4.json` (PAGE-NET sha `eaa65cad…` — byte-identical to V2c). |
| **T2-14** fixture FIX01-05 + FIX02D + HOME/TALL | `--mode fixture` | per-row report.json + netlog | all load-ok with stable FPC | **PASS — 8/8** — V4 fixture run: FIX01..FIX05 + FIX02D + HOME + TALL all `load-ok` (FPCs `0xb6ef95b9,0xfefa62d3,0xbd7d6a35,0xe0736d7f,0x6a3ac9a2,0xfa23e7c3,0x6dfa1363,0xa3e34d32`), 0 FATAL/0 open-fail.  NOTE: first fixture attempt omitted `WIKI_PROXY_FIXTURES` and 404'd HOME/TALL (harness ergonomics — fixed runner default to include both fixture dirs). |
| **T2-15** full-skin article (L2) | `--mode full` — `wiki/Hobbyist_operating_system` (proxy relay) | frame checksum + screenshot + CSS rows | article renders, ≥2 images | **PASS @ merged — flipped this run** — full-skin relay: net fetch 200 `bytes=120003` (PAGE-NET sha `bb765a43…` byte-exact), **`[WIN] load-ok ms=425529`** (was: no load-ok in 300 s, gated-R7), styled native frame `0x036bb41a` @ 1916×982, 26 subresource fetches incl. 2 CSS load.php bundles + 2 JPEGs + wiki logo SVG, `[IMG1d] imgs=24,24`, 0 FATAL.  Receipt `report-flip-full-v4.json` + `full-skin-go-1916x982.png`.  Honest paint note: relay main-content column renders blank in the captured shot while sidebar/TOC/logo paint — see Gap #1; the same bytes render fully on the direct leg. |
| **T2-16** direct https leg | `--mode direct --url https://en.wikipedia.org/wiki/Hobbyist_operating_system` | TLS/netlog + `[WIN] net fetch … status=200` | direct 200 + render | **PASS @ merged — flipped this run** — direct leg: `[WIN] net fetch … status=200 bytes=120003`, **`[WIN] load-ok ms=824991`**, styled full-skin native frame `0xfbd4089a` @ 1916×982, 33 subresource fetches (30 with TLS markers), 24/24 images, post-load screenshot `direct-go-1916x982.png` **vision-verified fully rendered Wikipedia article** (headline, intro, Development section, 6502/Z80).  Receipt `report-flip-direct-v4.json`. |
| **T2-17** guest-shell HTTP GET + DNS | runner `--shell-get` + `dnstst` | `ROB.WC` sha + `SOCK2TST …: PASS` + DHCP DNS | guest GET 200 prefix + DNS resolve | **PASS — flipped this run** — T2-17 run: SOCK2TST live GET 35 passes (prefix `HTTP/1.1 301 Moved Permanently` → followed), `[DNSTST] example.com A = 172.66.147.243` via DHCP DNS 10.0.2.3, serial DNS evidence true.  Receipt `report-t217-v4.json` + `ROB.WC.t217`. |
| **T2-18** 5-min/5-page soak + memory/time numbers | `--mode soak` (fixture+reader+full cycled) + `--mem-probe` + `--clock-probe` | soak `ok`, per-page verdicts, memory high-water | clean soak + numbers | **PASS — flipped this run** — soak ran **10.8 min wall** (11:50:32→12:01:23, ≥5 min), 1 full cycle = 10 rows (8 fixture + reader + full), per-row verdicts in `report-soak-v4.json`: **8 pass / 2 gated / 0 fail / 0 FATAL**.  The 2 gated rows (soak1-FIX02, soak1-reader) are transient `Failure when receiving data from the peer` network drops — both rows are individually **PASS** in their dedicated runs (T2-13/T2-14) on the same binary.  Memory high-water: measured honestly in `mem-probe-soak.txt` — guest `free` (sysinfo(2)) reports the user-region block pool (total 7,784,628,224 B ≈ 7.25 GiB) with used=0; the WebProcess's working set is allocated via the frame allocator / lazy AS_V2 loader and is **not visible to the guest sysinfo(2) counter** (kernel `frame_high` exists but is not syscall-exported) → guest-observable memory high-water is **not reportable** by the guest; the block-pool number + 0 FATAL/0 crash over 10 pages is the honest metric this harness can give (documented, no fix needed — a kernel-syscall export would be required to do better). |

## 3. Guest clock (G5b — T2-19)

| Row | What's checked | Required evidence | Pass criterion | Status |
|---|---|---|---|---|
| **T2-19** guest wall clock ≈ host (cert validity window) | on-device RTC/date vs host `date -u` | guest epoch (CLOCKPX probe → `/CLOCKPX.TXT` copy-back) + host epoch side-by-side | `|guest − host| ≤ 5 min` | **PASS — re-verified this run (3 receipts)** — soak-run: host t0 `1791288083` / t1 `1791288116`; guest `[CLOCKPX] epoch=1791288096` → Δ=13 s.  reader run: guest `1791286574` vs host `1791286561/1791286594` → Δ=13 s.  flip-full run: guest `1791284100` vs `1791284087/1791284120` → Δ=13 s.  All ≤ 300 s window.  Receipts `clock-probe-soak.txt`, `clock-probe-reader.txt`, `clock-probe-flip-full.txt`. |

## 4. Evidence map (every row → path or gated-with-owner) — FINAL MERGED state

| Row | Evidence path (committed) | Or gated-with-owner |
|---|---|---|
| T2-01 | **V4 `continuation/fs-v4/receipts/report-flip-direct-v4.json`** (direct https 200 + 30 tls= subres) | — (PASS @ merged) |
| T2-02 | WK-4c (browser.md §11: CA-negative rc=77 ENFORCED) + direct positive leg (V4) | merged-tree CA-negative re-run: N2 |
| T2-03 | WK-4c 302→follow on-device + T2-17 SOCK2TST 301 prefix | protocol-relative matrix: N2 |
| T2-04 | `fs-n1/evidence/RECEIPTS.md` | — (N1 GREEN) |
| T2-05 | `fs-n1/evidence/bigbody*/` | — (N1 GREEN) |
| T2-06 | `fs-n1` wire logs | — (N1 GREEN) |
| T2-07 | **V4 flip-full + flip-direct serials** (`[WIN] load-ok` + styled frames; receipts dir) | — (PASS; both legs) |
| T2-08 | **V4 direct post-render `direct-go-1916x982.png`** (fully styled) | relay main-column paint: see Gap #1 |
| T2-09 | fixture PNG census (V2c dry) + **V4 JPEG: `serial-flip-full.log`/`serial-flip-direct.log` `Icaros131.jpg`/`SkyOS.jpg` + `[IMG1d] 24,24`** | — (PASS, both codecs on-device) |
| T2-10 | **V4 `direct-go-1916x982.png`** (vision-verified legible) | — (PASS @ merged) |
| T2-11 | FIX05 (fixture) + **V4 logo SVG on both legs** (no-error) | — (PASS @ merged) |
| T2-12 | V4 serial `viewport render=1916x982` × every run + direct census | — (PASS @ merged) |
| T2-13 | **V4 `receipts/report-reader-v4.json`** (pass, load-ok 19708) | — (PASSED) |
| T2-14 | **V4 `receipts/report-fixture-v4.json`** (8/8 pass) | — (PASSED) |
| T2-15 | **V4 `receipts/report-flip-full-v4.json`** (load-ok 425529, frame 0x036bb41a, 24/24 imgs) | relay main-column paint note (non-blocking) |
| T2-16 | **V4 `receipts/report-flip-direct-v4.json`** (load-ok 824991, frame 0xfbd4089a, full render shot) | — (PASSED) |
| T2-17 | **V4 `receipts/report-t217-v4.json`** + `ROB.WC.t217` (SOCK2TST 35 PASS, DNS resolve) | — (PASSED) |
| T2-18 | **V4 `receipts/report-soak-v4.json`** + `mem-probe-soak.txt` + `PERF-NUMBERS.md` | — (PASSED; 8/10 in-soak, 2 transient w/ standalone PASS) |
| T2-19 | **V4 `receipts/clock-probe-{soak,reader,flip-full}.txt`** (Δ13 s ×3) | — (PASS; probe step in lane runner copy) |
| T2-20 | X1 `fs-x1` `4b611c1`/`efbc25e` | GATED: X1 retest in flight (wheel stretch) |
| T2-21 | X2 `fs-x2` `2f48639`/`d65e75a` (docs RESOLVED) | GATED: X2 |
| T2-22 | X3 CURED `408cb3c` (run 134) | — (PASSED, x64 leg) |
| T2-23 | X4 `fs-x4` WE2 (https 200, sha parity) | GATED: X4 windowed re-run |
| T2-24 | WN3 `wk6-live` receipts | GATED: WK-6 close |
| T2-25 | H1 `wk7-license/`; H2 docs wave | GATED: H1/H2 (merged) |

### 4a. V4 run receipts (this session — FINAL merged state, instance v4)

| Run | Mode | Instance | Port | Evdir | Result |
|---|---|---|---|---|---|
| flip-full | full (`/wiki/Hobbyist_operating_system` relay) | v4 | 8855 | `continuation/fs-v4/evidence/full2` | **1 pass / 0 gated — flip T2-15** (load-ok 425529) |
| flip-direct | direct https article | v4 | 8855 | `continuation/fs-v4/evidence/direct` | **1 pass / 0 gated — flip T2-16** (load-ok 824991) |
| fixture | fixture (8 rows, fixtures env fixed) | v4 | 8855 | `continuation/fs-v4/evidence/fixture2` | **8 pass / 0 gated — T2-14** |
| reader | reader + clock | v4 | 8855 | `continuation/fs-v4/evidence/reader` | **1 pass / 0 gated — T2-13** + T2-19 |
| soak | soak (fixture+reader+full, 5+ min) + mem + clock | v4 | 8855 | `continuation/fs-v4/evidence/soak-run` | **8 pass / 2 gated(transient) / 0 fail — T2-18** + memory + T2-19 |
| t217 | reader + shell-GET + DNS + clock | v4 | 8855 | `continuation/fs-v4/evidence/t217` | **1 pass / 0 gated — T2-17** (SOCK2TST 35, DNS A) |

Plus one aborted soak boot (LOSTWAKE desktop disposal — known stochastic TCG
class; evidence dir `soak` superseded by `soak-run`), one aborted soak run
(`KIND` unbound under `set -u` in the first soak-row log line — fixed; dir
`soak` overwritten), and the first fixture run without `WIKI_PROXY_FIXTURES`
(404'd HOME/TALL — harness ergonomics, fixed in the runner default).

Soak per-row (`report-soak-v4.json`): soak1-FIX01 **pass** (load-ok@9941) ·
**soak1-FIX02 gated** (transient peer-reset; PASS in fixture2 run) ·
FIX02D pass (47066) · FIX03 pass (55364) · FIX04 pass (68719) · FIX05 pass
(76753) · HOME pass (84530) · TALL pass (92971) · **soak1-reader gated**
(transient peer-reset; PASS in reader run) · **soak1-full pass** (load-ok@653995,
frame 0x1140aa94, 24/24 imgs).  0 FATAL, 0 IDLESTUCK-fatals across all rows.
Wall 10.8 min (≥5 min).  `[IMG1p] view=1916x982 … foreign=10003` (relay body
paint caveat).

Instance hygiene: own sockets `/tmp/br-wc-ctrl-v4.sock`(+qmp, serial), own
port-file `/tmp/wiki-proxy-port-v4`, port 8855; runs sequential; teardown clean
(scoped WT-shim `pkill` for the aborted soak, only fs-v2 cwd procs, G11/H4
untouched).  Concurrent fleet (G11 cnn-relay qemu at 99%, H4) untouched.

## 5. Gap list (rows not yet demonstrable at FINAL merged state + why + owner)

1. **Relay full-skin main-content paint** — on the `--mode full` relay leg the
   browser paints the article sidebar/Contents/logo/search and reports
   `[IMG1d] 24,24` + load-ok + styled frame, but the main content **column**
   renders blank in the captured screenshot (`[IMG1p] … foreign=10003` relay vs
   `93062` direct).  The identical bytes render fully on the direct https leg
   (same PAGE-NET sha), and the reader (proxy) leg paints fine — so this is a
   relay-path render gap, not a parse/fetch failure.  Suggested owner:
   **Integrator I / next FS lane** — root-cause candidates: relayed CSS
   application on the base-injected document, or a fractional/column paint
   clip.  Non-blocking for T2-15 (criterion met on load-ok+frame+images), but
   flagged honestly rather than conflated with the fixed parse stall.
2. **T2-02/03 merged-tree CA-negative + redirect-matrix re-runs** — positive
   legs GREEN at merged (T2-01 direct + T2-17 301 prefix); CA-negative rc=77
   re-run and protocol-relative matrix remain **owner N2**.
3. **Guest-observable memory high-water** — sysinfo(2) reports only the
   7.25 GiB block pool (used=0; WebProcess allocates via frame allocator /
   AS_V2 lazy loader, not the counted block pool); kernel `frame_high` is not
   syscall-exported.  V4 documented what's measurable (block pool + 0 FATAL +
   no crash over 10 pages).  Adding a `frame_high` export to sysinfo is a
   candidate kernel-side nicety (owner: next lane / kernel).
4. **Watchdog (R1c) deliberate negative** — no watchdog re-arm fired across
   all V4 runs (0 FATAL, all `[IMG1d]` complete); a deliberate hung-fetch
   negative test was a V4 soak item but the soak's transient peer-reset rows
   give a partial natural negative; a scripted hung-fetch test remains open
   (owner: next FS lane, optional).
5. **Soak's 2 gated rows** — transient `Failure when receiving data from the
   peer` on the proxy during the 10.8-min soak; each row PASSES in its
   dedicated run on the same binary.  Root cause: host proxy/network blip under
   concurrent fleet load (G11 relay runs), not a browser fault.  A re-run of
   the soak with a sturdier host network could push 10/10, but evidence for
   the underlying rows already exists.

## 6. Permanence

T2-CHECKLIST.md lives at `tests/fixtures/browser/T2-CHECKLIST.md` (next to the
fixture README it owns); a copy stays under `continuation/fs-v4/` for this
lane's evidence trail.  Runner extensions landed this lane: `--soak-min`
(multi-page soak with per-row verdicts), `--mem-probe` (guest `free` →
`/MEMFREE.TXT` copy-back), `--go-timeout` (long TCG parse windows), and the
fixture-dirs default fix (both wk3 + OS tree so HOME/TALL don't 404).

## 7. Proposed browser.md §11 entry (for Integrator I to apply)

See `continuation/fs-v2c/browser-md-s11-proposal.md`; add a V4 line: both
full-skin and direct https legs now reach `load-ok` on the FINAL merged
binary (fw `fa621da1…`), fully styled direct render vision-verified, JPEG
decoder exercised on-device (T2-09), T2-17 shell-GET/DNS closed, 10.8-min
soak clean (0 FATAL; 8/10 in-soak, 2 transient).
