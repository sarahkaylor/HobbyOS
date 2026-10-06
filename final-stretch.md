# Browser Final-Stretch Plan — to a modern, usable browser on Wikipedia-class sites (v1.1)

Status: **proposed — planning only; this document changes no code.** v1 independently audited
2026-10-05 evening (findings answered; see §11). Ground snapshot:
2026-10-05 ~22:00 UTC, OS main `5111de4` (10 commits past the frozen review copy `b8e69a9`),
WebKit fork tip `4536f622cc` (13 commits past `befd84e008`). `browser.md` remains the program
record (§6 milestone boxes, §11 fix log); **this document owns the endgame**: the defined work
tracks that take the program from tonight's integrated address-bar/Wikipedia milestone to a
browser that honestly works on Wikipedia-class sites — executed by parallel agent lanes.

Audience: agent sessions + maintainers. Read §0, §4 and §5 first; §5.2 (frozen interfaces) is
binding. Every current-state fact below is anchored to a tree path, commit, or evidence file
(timestamped 2026-10-05); anything unverifiable is flagged explicitly.

---

## 0. TL;DR and how to use this document

**Where we are (one paragraph).** The windowed WebKit browser runs on-device on ARM: Apps-menu
launch (F1 keyboard path too), a persistent address bar + GO button, back/forward/reload/wheel/
close, fixture pages render, and as of tonight **a real Wikipedia page loads through the address
bar and paints** (reader-mode route on the host proxy; frame `0xdabdfbc5`, `session.ok=True`).
The two blockers behind that milestone were root-caused and fixed this evening: the merged-tree
empty-DOM regression (self-pumped loop dropped the RunLoop substitute-data handoff — fixed by
synchronous delivery under `OS(HOBBYOS)`) and the guest TCP stall (v1 stack never ACKed and
advertised a fixed 2048-B window — fixed with per-segment ACK + a real receive window, OS main
`477c5c4`, unit-arm 302/0). Direct-HTTPS fetch from the guest is *partially* proven (small
direct transfers green in WB receipts; the merged-tree **full-page direct** path still needs one
decisive re-test — Open Question #1). What's still missing for the bar: **resolution & fidelity** (fixed 320×240 surface scaled ~3.2×; no letterforms today — §2.1), full-page (full-skin)
rendering and readable text, images, a gzip/large-body completeness pass, an acceptance run +
soak, and the entire x64 leg (input, liveness, paint, runtime net).

**The endgame in one line:** five parallel tracks (R render/readability, N net completeness,
X x64 parity, V verification machine, H hardening/docs) close out `browser.md`'s WK-6/WK-7
against the T2 checklist in §1, with both arches.

**How to use:** each lane finds itself in §4 in under a minute (owns / depends / first tasks /
DoD / evidence / skills). §5.1 maps the waves; §5.2 freezes the interfaces that make parallel
work safe. §8 is the concrete first dispatch wave. Gates are run by the Integrator on the
merged tree — a lane never ticks a gate it did not run. Evidence rule (§6.4): no "done" without
raw, executed, on-device output (pixel readback + DOM/probe values + per-request network log).

---

## 1. The bar: what "works on sites like Wikipedia" means (verified 2026-10-05)

From ~40 live probes against `en.wikipedia.org` on 2026-10-05 UTC (rerunnable scripts + raw
outputs archived in `05-acceptance-research.md` §7 and `probes/` — see §C):

| Tier | Definition | Role |
|---|---|---|
| **T0 — fixture ladder** | The six fixtures FIX01/FIX02/FIX02D/FIX03/FIX04/FIX05 (static / img / img-dataURL / tables / form / DOM; fork `continuation/wk3/fixtures/`) render deterministically on-device; FPC checksums stable | regression floor (largely green already) |
| **T1 — static wiki mirror over HTTP** | A wiki-shaped page served from `10.0.2.2` (host): full render incl. server-rendered TOC + ≥2 images over the wire; scroll/links/back | proves the pipeline without the live edge |
| **T2 — LIVE `en.wikipedia.org` article** | Load a real article over HTTPS; headline + lead + nav + server-rendered TOC + ≥2 images render **at window-native resolution with legible text (L0→L2 ladder below)**; follow an in-content link, back, scroll, fragment jump; search submit works; 5-min/5-page soak clean; memory/time recorded; guest clock verified against host (±minutes — cert validity depends on it). **This is the release bar.** Prefer **direct** connection; proxy-assisted reader-mode may serve as a *documented interim milestone only*, with the direct path tracked as an open gate (see OQ-1). | THE requirement |
| **T3 — JS stretch** | `client-js` flip + `mw.config` values observed via probe output; search suggestions; collapsed-section toggles. Slow-but-working accepted (no JIT) | post-bar stretch |

**Minimal capability set for T2** (each an acceptance-test row):

- **MUST:** TLS1.2+ with ECDHE+AES-GCM; **ECDSA certificate verification** (P-256/P-384,
  SHA-256/384 — Wikimedia certs are ECDSA-only; chain via ISRG Root X1/X2) + SNI; correct wall
  clock (90-day certs → validity window matters); **gzip decode** (Wikimedia serves gzip only —
  no brotli; main page 260 KB → 51.7 KB, article 2.26 MB → 361 KB); HTTP/1.1 + 301/302 incl.
  protocol-relative `Location`; a non-empty descriptive User-Agent (empty UA → 403); HTML5
  parse (UTF-8); CSS custom properties + flexbox + media queries + calc/min/max; JPEG + PNG
  decoders; broad UTF-8 font fallback.
- **SHOULD:** SVG render (site logos); basic CSS grid.
- **OPT:** WebP (only advertise `image/webp` if decode is wired), brotli/zstd, HTTP/2.
- **NOT needed for T2:** cookies (no-cookie reading works), JS (read/nav/TOC/search-submit are
  server-rendered). No ECH; h3 not advertised.

**Gotchas to keep in the test matrix** (from the probes): gzip-only WMF policy; ECDSA-only certs;
90-day cert lifetimes ⇒ clock correctness is a hard prerequisite; protocol-relative redirects;
`mw-data:`/`data:` URL tolerance (must not blank the page); images served from
`thumb.wikimedia.org` with query strings; WebP negotiation is explicit-Accept-only; empty UA 403;
page scale 1.8–2.3 MB decoded — memory budget matters.

**Fidelity ladder (added v1.1 — from the desktop-reference comparison, §2.1):** **L0** = today (styled boxes on the fixed 320×240 surface scaled ~3.2×; no letterforms); **L1** = window-native render + legible text on the reader-mode page (R5+R2) — the minimum before "usable" is claimed; **L2** = full-skin article renders (title/body/≥2 images/no horizontal overflow) at native resolution (R1-g1+R3+R5) — the visual half of T2. Not typographic parity (risk F2 stands).

**Extended acceptance (still scheduled — the program's original bar):** Google Search + CNN per
`browser.md` §8.3 (server-side blockers now fixed; in-page text-input wiring `ed8b9e17e4` landed).
Keep as WK-6 close-out after T2.

---

## 2. Where we are (grounded snapshot)

**Tips at snapshot:** OS main `5111de4` · fork `4536f622cc` · frozen review copy
`~/Documents/GitHub/HobbyOS-review` @ `b8e69a9` (10 behind; refresh per §B).

| Milestone | Status | Evidence pointer |
|---|---|---|
| WK-0/1/1.5/2 build+JSC+binary link | done | fork `HobbyOS/PORT_STATE.md`, `WK1-M3-EVIDENCE.md`, `WK2-EVIDENCE.md` |
| WK-3 headless render | done (doc fragmentation: tip `WK3-REPORT.json` says BLOCKED; closeout unmerged on `browser/l8-wk3x64` @ `92ba80f85d`) | `wk3/WK3-EVIDENCE.md`; ARM 6/6 runs 94–100; x64 runs 26–28 |
| WK-4 networking | **ARM gate GREEN** (WK-4c: HTTP 200 / 302→follow / HTTPS 200 / cookie round-trip / CA-negative rc=77) | `WK4C-EVIDENCE.md`; merged `90d3112a0c` |
| WK-5 UI shell | **done + address bar/GO landed tonight**; render-regression fix merged | `run-WK5-4.log`; WA-REPORT; fork `49f28a3bb5` |
| WK-6 acceptance | **milestone: Wikipedia loads via address bar** (reader-mode proxied; `0xdabdfbc5`); direct full-page + images + soak + Google/CNN open | fork `4536f622cc`; `wk-addrbar/evidence/final-reader/`; `wk6-live/`; OS `5111de4` §11 |
| WK-7 hardening | crash recovery + auto-respawn GREEN; license package `[wip]`; x64 matrix `[gated]` | `WK7-EVIDENCE-MATRIX.md` |

**Open blockers at snapshot** (each with owner-track):

1. **Direct-vs-relay truth (OQ-1).** WB receipts: direct https robots 200/28,275 B + 5/5 real
   fetches; §11 note says "DIRECT https … still hits the mbedTLS↔edge interop (documented; the
   proxied/reader path is the deliverable)". One decisive re-test on the merged tree must
   resolve which artifact is current → **R1 gate 0**.
2. **Text quality.** Reader page parses (title+body+links+inputs) but "text glyphs are
   shell-font blobs" (§11 wording; WA-REPORT: "shell font stack renders styled boxes, not raw
   text") — the font backend is `/USE-FONT` opt-in; glyph rendering was proven at WK-3 (165 glyph
   pixels). v1.1 re-read (updated vision model): in the windowed final-reader frame the page area shows **no letterforms at all** (styled bars only); chrome glyphs are blurred/clipped. → **R2**.
3. **Images / resource loader.** Windowed WebProcess has no network resource loader for
   subresources (`img-pending` documented at WK-3; F7). → **R3**.
4. **Net completeness.** gzip decode not yet verified on-device; large-body (>24 KB) behavior
   post-`477c5c4` needs receipts; correctness matrix (redirects, errors, UA/Host). → **N1/N2**.
5. **x64 leg.** (a) PS/2 mouse buttons (EV_KEY 0x110/0x111) never reach userland — no OS lane
   exists yet, F1 workaround only; (b) windowed white-paint RCA'd to the fork's
   `WK5WindowDriver` readback (OS exonerated) — verify whether the merged WA fix cures it;
   (c) `-smp 8` LOSTWAKE→#PF after ~46 s idle (workaround `-smp 1`); (d) TSC ~18× fast clock;
   (e) intel runtime net leg never run; NVMe loader re-measure owed. → **X1–X5**.
6. **Perf honesty.** The 4 KB reader slice took ~100 s wall (TCG, poll-pumped). Slow-but-working
   is accepted (`browser.md` §1.6) — record numbers, don't hide them.

**Verify current state before executing anything** (commands in §B): re-read both tips, diff
`browser.md` §11 since this snapshot, and re-run the quick gates below. If tips have moved, treat
this document's "current state" as historical and re-baseline first.

**Do-not-restart (already done — verify, don't redo):** address bar + GO; synchronous
substitute-data delivery; TCP ACK/window fix on OS main; WK-4c net gate; crash
recovery/respawn; host proxy + E2E driver (`tools/wiki_proxy.py`, `run_browser_accept.sh`);
loader/demand-paging + prefetch work.

### 2.1 Desktop-Linux reference comparison (added v1.1, 2026-10-06)

**How the reference was captured.** Desktop Chrome (workstation), JS on,
`en.wikipedia.org/wiki/Hobbyist_operating_system`, at 1024×704 and 1920×1080 (plus a tall
capture). Wikipedia currently serves a US-fundraising banner and a CentralNotice to fresh
readers (both observed 2026-10-06); the saved reference shots are the banner-dismissed variants.
Artifacts: `~/.hermes/cache/scratch/final-stretch-research/09-desktop-reference/`
(`ref-check-b-clean.png`, `harness-hobbyist-1920x1080-clean.png`, `chrome-hobbyist-tall-1024x2400.png`,
computed-style facts); regeneration in §B.6.

**Reference facts.** 1024×704: content column 961 px; TOC rail present (200 px box, collapsible);
body 16 px sans (`sans-serif`); title serif (`"Linux Libertine", Georgia, …`) 28.8 px; links
`#3366cc`; 25 images (20 loaded; a 120 px thumbnail grid in "Examples"). 1920×1080: centered
column (content x=479 w=1220; article w≈948), TOC rail visible left (x=211 w=228); same
fonts/links/images; page height ≈ 3126 px.

**On-device re-read (updated vision model, 2026-10-06).** Window title `HobbyOS-Browser - http:`;
the address bar shows the reader URL as blurred glyphs (`Hobb ist_0_Netatiti_n_s sfew` for
`…/reader/Hobbyist_operating_system`); **the page area contains no letterforms at all** — one
mid-blue bar (the styled h1) and one light-blue bar over white. A pixel census agrees — the page area carries exactly three colors (`#ffffff`, `#3366cc`, `#e8f0fe`) and zero black pixels (`08-pixel-stats.txt`). The surface is a fixed **320×240**
WebPage viewport scaled ~3.2× into the 1020×670 paint rect (`final-reader/serial.log`:
`[WIN] created pref=320x240`, `viewport=320x240`, `painted rect=1020x670+36 at 2,70`). The reader
fetch took 49.4 s for 983 B (TCG; slow-but-working is accepted); the direct-https leg remains
`gated-expected-fail` (OQ-1).

| # | Aspect | Desktop reference | HobbyOS today | Fix |
|---|---|---|---|---|
| C1 | Render resolution | native 1:1, crisp AA | 320×240 scaled ~3.2× (blurred; blocks only) | **R5** (+ **R6** for the 1024×768 desktop itself) |
| C2 | Text | full text; serif title 28.8 px; 16 px sans body | no page letterforms; chrome glyphs blurred/clipped (descenders cut) | **R2** (+R5) |
| C3 | Page scope | full Vector skin (header/tabs/TOC rail/≈948–961 px column) | reader-mode minimal doc only | **R1-g1** |
| C4 | Images | 25 (20 loaded) incl. thumbnail grid | none (no subresource loader) | **R3** |
| C5 | Interaction | links/hover/scroll/TOC | addr-bar focus + GO + wheel; link-at-scale untested on the final run | **R4**, T2 |
| C6 | Perf | sub-second loads | reader 49.4 s / 983 B; fixtures 0.75–14 s (accepted-slow) | H3 records |

Direction (maintainer, 2026-10-06): much higher resolution for desktop + browser — hence **R5**
(window-native browser render) + **R6** (desktop display mode beyond the hard-coded 1024×768 —
`src/kernel/virtio_gpu.c:140`, `virtio_gpu_x64.c:131`).

---

## 3. Gap analysis → the endgame work inventory

To T2 on ARM:

- G1 Direct full-article fetch + render from the windowed browser (decision + receipts; possibly
  resolve residual mbedTLS↔edge interop for the full-skin page).
- G2 Readable text in content (font backend default-on; glyph-level verification).
- G2b Resolution & display fidelity: browser renders at window-native resolution (drop the fixed 320×240; legible text at real px sizes) → R5; OS desktop beyond the hard-coded 1024×768 → R6.
- G3 Network images (fetch + PNG/JPEG decode + WebP decision) in the windowed page.
- G4 Real-page navigation quality (scroll at 2 MB pages, fragment jump, link follow/back, form
  GET submit for search; address-bar polish optional).
- G5 gzip decode + large-body completeness receipts; net correctness matrix + OS-side net
  regression sweep over the `477c5c4` change.
- G5b guest clock correctness (TLS cert validity window): add a T2 receipt row (guest time vs
  host); fix path if wrong (N-lane); fold `browser.md` R13 clock tests into V2.
- G6 T2 acceptance run + receipts format + 5-min site soak; memory/startup numbers.

To x64 parity:

- G7 PS/2 button delivery (OS input); retest browser launch by click.
- G8 LOSTWAKE/TSC audit (`-smp 8` stability); unit-x64 green.
- G9 White-paint follow-through post-merge (fork); x64 frame parity.
- G10 intel runtime net leg + loader re-measure; x64 acceptance leg.

To extended acceptance / close-out:

- G11 Google Search + CNN re-run with render fix merged (WK-6 §8.3 rows).
- G12 License package; docs/registers refresh; WK3 report unification; stale wording retirement
  *after* the OQ-1 verdict.

---

## 4. Work tracks (the parallel execution design)

Five tracks + Integrator. Lane IDs: `R1..R6`, `N1..N2`, `X1..X5`, `V1..V5`, `H1..H4`.
A lane works only in its owned paths (worktree rule; one lane per worktree). All lanes: local
commits only, checkpoint ~30 min, strict JSON report + raw evidence, timebox ~4 h.

### Track R — Site render & readability (ARM; fork-heavy)

| Lane | Purpose | Owns | Depends |
|---|---|---|---|
| **R1** direct-net & fetch-path unification | One fetch path in the windowed shell (WB `WKNetFetch`/`AddrBarFetch` as default over the weak in-proc bridge); **gate 0: decisive direct-https re-test** (robots, main page, full article; classify every failure exactly); push the full-skin page through | fork `HobbyOS/WebKit/WebProcess/hobbyos/net/*`, `AddrBarFetch.h`, `WK5WindowDriver.cpp` (fetch integration), OS `tools/wiki_proxy.py` only if the relay route contract must change | merged tips; proxy running for the relay leg |
| **R2** text & font quality | Enable `HobbyOSFontManager` by default for the windowed binary; DejaVu glyph rendering verified at pixel level; font-matching gaps filled; HarfBuzz decision recorded (likely stays off for T2 — D-15) | fork font backend files + build defines; `HobbyOS/WebCore/platform/graphics/hobbyos/HobbyOSFontManager.cpp` and build flags | R1 build recipe (same relink cadence) |
| **R3** images & resource loader | Network `<img>` fetch + decode chain for the windowed page; PNG/JPEG first (libs vendored), WebP gated on decode wiring; image census markers | fork resource-loader wiring + `WK5WindowDriver` paint; no OS changes expected | R1 (fetch path), R2 (paint cadence) |
| **R4** real-page navigation polish | Link follow/back/forward at real page sizes; fragment jump; form GET submit (wiki search); scroll sanity; optional address-bar font upgrade | fork `WK5WindowDriver.cpp` nav/input sections | R1 |
| **R5** window-native render & resolution (browser) | Drop the fixed `pref=320×240`: render the WebPage at the window content size (real font px sizes; optional scale/supersample knob — TCG frame-budget note); owns the L1 checks | fork `WK5WindowDriver.cpp` viewport/pref + paint scaling (serialize per §5.3) | R2 (fonts) at gate time |
| **R6** desktop display mode & scaling (OS) | Raise the desktop beyond hard-coded 1024×768 (target 1920×1080 — OQ-8): mode constants, taskbar/WM layout, screenshot + test-coordinate updates; browser window ≈ full-res | OS `src/kernel/virtio_gpu*.c`, desktop layout constants; screenshot tooling | I (Makefile/disk/runners); coordinate X-lane boots |

**R gates:** R1-g0 = direct matrix receipts (re-run `wk-realsite/run_realsite.sh` + one
integrated-shell direct attempt; every failure classified) or written RCA + relay as gated
interim. R1-g1 = full-skin article bytes render in the windowed frame (structure + no crash; FPC
recorded; CSS feature rows var()/flex/media/calc confirmed).
R2-g = glyph evidence (dark-pixel census on content text + screenshot). R3-g = ≥2 images fetched
+ decoded + painted (census + fetch log). R4-g = T2 nav rows pass on the served page. **R5-g** = L1 checks: serial shows the render view == window content size (no upscale), legible-glyph census + screenshot. **R6-g** = desktop boots at the new mode with layout intact (screenshot; ARM first, x64 when green).
**Evidence:** serial markers, FPC/frame checksums, screenshots, per-request logs (proxy or edge).

### Track N — Net completeness & regression (OS + fork)

| Lane | Purpose | Owns | Depends |
|---|---|---|---|
| **N1** gzip + large-body completeness | Verify/decide gzip decode on-device (Wikimedia is gzip-only); enable if missing (curl+zlib wiring); large-body (≥1 MB decoded) transfer receipts post-`477c5c4` | OS build config for libcurl/zlib; fork net module config; no shared files without I | merged tips |
| **N2** HTTP correctness matrix | Redirect chains (protocol-relative), 4xx/5xx graceful page, DNS failure path, UA/Host correctness, cookie residual; fold into the acceptance harness rows | fork net module; `tools/wiki_proxy.py` test routes (coordinate with V1) | R1 (single fetch path) |

**N gates:** N1 = gzip receipts (decoded bytes == identity bytes for a known page) + big-body
receipt. N2 = matrix receipts (each row: request → status → handled description). Any OS-side
change runs the **net regression sweep**: `SOCK2TST`, `NETFIX`, `DNSTST`, unit-arm + unit-x64,
ARM wave 0-FAIL.

### Track X — x64 parity (OS + fork)

| Lane | Purpose | Owns | Depends |
|---|---|---|---|
| **X1** PS/2 button delivery (OS input) | Why `EV_KEY 0x110/0x111` never reach userland on x64; deliver buttons (wheel E0-extended as stretch); retest Apps-menu click → launch → click-nav | `src/kernel/arch/x64/**` input driver + desktop path; `run_acceptance.sh --arch intel --profile dryfix` retest | none (start now — it gates the x64 leg) |
| **X2** x64 liveness: LOSTWAKE/TSC | TSC calibration (~18× fast clock) audit; `-smp 8` desktop LOSTWAKE→#PF root cause; re-enable lost-wake semantics sane on x64 | kernel timer/process paths | none; coordinate with X1's boots |
| **X3** white-paint follow-through (fork) | Re-run x64 windowed after merged WA fix; if still white, fix `WK5WindowDriver` readback/paint for x64 (WD handoff note `docs/wd-x64-white-blit.md`) | fork `WK5WindowDriver.cpp` paint/readback | merged WA fix |
| **X4** intel runtime net + loader re-measure | Wire/run the x64 runtime networking leg (WE2 staging done, link green); NVMe loader timing post-coalescing | fork x64 runners; OS loader path if needed | X1/X3 (a bootable x64 windowed leg) |
| **X5** x64 acceptance leg | Full toolkit acceptance on intel; parity receipts vs ARM | toolkit runner | X1–X4 |

**X gates:** X1 = click-launch + click-nav receipts on-device (screenshots + serial). X2 =
bounded `-smp 8` stability receipt + unit-x64 0-fail. X3 = x64 frame checksum parity-family +
screenshot. X4 = x64 https fetch receipt + load ms. X5 = acceptance `report.json` pass|gated with
owner list. **x64 convention:** `unit-x64` (KVM) is the hard gate; the x64 wave stays
best-effort/no-new-signature.

### Track V — Verification machine & acceptance

| Lane | Purpose | Owns | Depends |
|---|---|---|---|
| **V1** harness unification | Extend `run_browser_accept.sh`/`br_e2e.py` + wk6 toolkit: modes (fixture / reader / direct / full), per-request network log capture, screenshot set, `report.json` v2 (strict; pass\|gated\|fail). One canonical acceptance runner for all lanes | OS `tools/` + fork `HobbyOS/continuation/wk4b/wk6/` runner (single writer: V1) | none (host-side; start now) |
| **V2** T2 protocol + receipts | Freeze the T2 checklist (rows from §1 + guest-clock row) + evidence format; reconcile fixture staging (fork `wk3/fixtures/` FIX01–05 vs OS `tests/fixtures/browser/` HOME/TALL); run T2 dry on the current tip; every row evidence-mapped or gated-with-owner | `tests/fixtures/browser/` refs + docs + toolkit checklist | V1; R/N lanes for rows |
| **V3** CI/battery wiring | Merged-tip battery recipe (host + units + ARM wave + desktop + accept subset) + hobytest runs (`hobbyos-ci.sh <tier> <new-label>`) per merge wave; record three-way verdicts (rc/marker/FAIL-count) | run commands only; receipts archive | none (can prep now); executed by I |
| **V4** site soak | 5-min multi-page Wikipedia soak through the acceptance runner; memory high-water, bytes, per-page verdicts | soak runner + receipt format | R1–R4 complete |
| **V5** (stretch) T3 JS probes | `client-js` flip + `mw.config` value probes via the eval path; document slow-no-JIT numbers | probe scripts | T2 close |

**V gates:** V1 = runner modes demonstrated (fixture leg re-runs green on merged tip). V2 = T2
table complete + dry run recorded. V3 = one recorded CI battery per merge wave. V4 = soak ok=True
with numbers. V5 = probe values (not screenshots) recorded — JS evidence rule.

### Track H — Hardening, compliance, docs

| Lane | Purpose | Owns | Depends |
|---|---|---|---|
| **H1** license package | Finish WK-7 license relink package (`[wip]` L2–L4: link-line capture, offer text, SPDX cross-check) | fork `license-*` artifacts; docs | — |
| **H2** docs & registers | `browser.md` §11 wave-6/7 entries + §6 boxes (+ §7 refresh if lanes change); fork `DECISIONS.md` D-16+, `PORT_PLAN` WK-3+ rows; `WK3-REPORT.json` unification (`92ba80f85d` merge); `docs/webkit.md` refresh; fork doc-drift batch (`docs/webkit.md:28`, README headers, superseded-file labels); retire stale wordings only after OQ-1 verdict | browser.md (§11) — **I merges edits**; fork docs single-writer H2 | merges land |
| **H3** perf & measurements | Record startup/first-frame, load-ms at fixture/reader/full tiers, memory high-water per acceptance run (slow-but-working numbers) | measurement docs + receipts | V4 |
| **H4** worktree/tooling hygiene | `wb1` locked worktree cleanup (recreate for any render lane); `wab` run7 driver fix (already fixed in `97ac3fa07c` — fold the fix-verify); prunable `/tmp` worktrees list (check before prune); fold tonight's deployed-skill backfills (`~/.hermes/skills/hobbyos/{hobbyos-build-and-run,hobbyos-run-tests}` — updated during copy verification) back into the repo `skills/` sources so `deploy.sh` stays the one-way source of truth; runner teardown hygiene (stop the proxy + remove the port file, reap QEMU after runs — a stale :8892/port-file was observed) | infra only | — |

### Integrator (I)

Single writer for: Makefile, disk recipe, test-wave lists, `browser.md` §11/§6, merged-tip
batteries, merges in wave order. Runs every gate on the merged tree; lanes propose diffs.
Maintains the §11 record and the acceptance receipts index.

---

## 5. Execution model

### 5.1 Waves (critical path)

| Wave | Lanes | Exit / checkpoint |
|---|---|---|
| **FS-W1** (start now) | **X1** (unblocks the whole x64 leg), **X2**, **R1** (g0 first), **N1**, **V1**, **H1**, **H2** start (non-conflicting parts) | R1-g0 direct verdict recorded; X1 first click receipts; V1 modes demonstrated; **CP-FS1: direct-path decision** |
| **FS-W2** | R2, R3, **R5**, **R6** (I coordinates the mode bump), X3, N2, V2, H2 | CP-FS2: readable full-page render (R2-g/R3-g/R5-g); white-paint verdict |
| **FS-W3** | R4 + **T2 dry run (ARM)** + V4 prep, X4, H3 | CP-FS3: T2 checklist dry (rows pass or named-gated); x64 runtime net receipt |
| **FS-W4** | T2 soak + x64 acceptance (X5), V5 (optional), G11 Google/CNN | CP-FS4: T2 receipts archived; x64 leg verdict |
| **FS-W5** | Extended acceptance (WK-6 close), H1/H2 finish, release-readiness review | WK-6/WK-7 checkboxes honest; docs done |

Dependency rule: a lane never starts its on-device tests before its dependency's gate passed on
the merged tree. Host-side work (V1, H1, harness prep) has no such constraint — it is the early
parallelism engine.

### 5.2 Frozen interfaces (binding)

| # | Interface | Owner (sole writer) | Change protocol |
|---|---|---|---|
| F-R1 | Windowed fetch contract (`fetchUrl`/`setFetchUrl`, `WKFetchSink`) | R1 (fork) | fork README note; consumers (R3, N2) adapt to the frozen text |
| F-R2 | Proxy route contract (`tools/wiki_proxy.py`: default port 8800 + `/tmp/wiki-proxy-port`; raw relay of any path to `https://en.wikipedia.org<path>`, `?head=N` prefix mode, `/reader/<Topic>` reader-mode route; upstream forced `Accept-Encoding: identity` — gzip decode is exercised only on the direct leg; one instance per host — `tools/run_browser_accept.sh` starts its own, do NOT double-start; lane-local relays on other ports are test scaffolding, not the contract) | V1 | single writer; route additions via diffs; guard test in V1 |
| F-R3 | TCP ACK/window semantics (post-`477c5c4`) | OS net (as landed) | any change re-runs the net regression sweep (N2 owns the sweep list) |
| F-R4 | Acceptance `report.json` schema + evidence format | V1 | versioned (`v2`); consumers: every track's gates |
| F-R5 | Desktop protocol additions | none expected this stretch (F1.8 stands); any new message = L4-style design note first | freeze before use |
| F-R6 | Makefile / disk recipe / test-wave lists / `browser.md` edits | **I only** | lanes propose diffs in reports |

### 5.3 File ownership map (collision dodge)

- Fork net module + windowed driver: one writer at a time **per file** — R1 owns
  `net/WKNetFetch.*` + fetch integration; R2 owns font backend + defines; R3 owns resource-loader
  wiring; R4 owns nav sections of `WK5WindowDriver.cpp` **after** R1/R3 land (serialize driver
  edits through merge order; prefer small, reviewable diffs; R5 owns the viewport/pref +
  paint-scaling sections, same serialize rule).
- OS kernel input: X1 (x64 input only); timer/process: X2; display mode/desktop layout: R6.
- `tools/wiki_proxy.py`: V1 (test routes) / WC-owner — coordinate; one writer.
- Evidence dirs: `<lane>/HobbyOS/continuation/<lane>/evidence/**` per lane — never touch another
  lane's.

### 5.4 Resource budget & do-not rules (from the infra survey)

- ≤3 heavy local lanes concurrently: **≤1 WebKit cross-build/relink + ≤2 QEMU runs**; if a build
  runs, ≤1 QEMU. Serialize: merges + merged-tip batteries, any shared `disk.img`/tree, `make
  test_intel` per tree, official CI batteries (fresh label per run), site-live runs (one proxy).
- **Never unscoped `pkill`** — 12 `run_*.py` harnesses embed it (unshimmed desktop/app runners are
  global-kill hazards). Use a cwd-scoped shim (`HobbyOS-review/.review-artifacts/shim/pkill`, or
  `~/.hermes/cache/scratch/gxverify-shim/pkill`) or `fuser -k <tree>/disk.img`.
- `/tmp` tmpfs is inode-capped (~1 M; rsync'd WebKit trees exhaust it) — use lane scratch, check
  `git worktree list` before pruning.
- OVMF: shared read-only code file; stale-ELF spin fixed by `touch src/kernel/main.c`; avoid intel
  *boots* from the review copy while other lanes share the OVMF file (intel compile-only is fine).
- One QEMU per `disk.img`; one `make` per tree at a time.

### 5.5 Integration protocol

As established: lanes commit to `browser/*` (OS) / `bw-*` (fork) worktree branches, checkpoint
~30 min, local-only commits; the Integrator merges in wave order, runs gates before advancing,
records §11. Fork rebases recorded in the fork README. Lane done-report: what changed, exact
commands, raw evidence, unchecked risks, proposed diffs.

---

## 6. Verification & gates

### 6.1 Tiers (local)

| Tier | Command | Pass criterion |
|---|---|---|
| Host golden | `make host_tests` | `ALL APPS SUITE TESTS PASSED` + `TEST EXIT: 0` |
| Kernel unit ARM / x64 | `./run_unit_tests.sh` / `./run_unit_tests_intel.sh` (+KVM) | `UNIT TESTS PASSED`; x64 = the hard x64 gate |
| In-OS wave ARM | `make test` | `System halt.` **and zero uppercase `FAIL` tokens** |
| In-OS wave x64 | `make test_intel` (KVM; 1800 s bound) | known-incomplete: unit-x64 green + **no new** signature |
| Desktop | `make desktop_test` | `SCREENSHOT_READY` + non-black screendump |
| Fork link | `HobbyOS/scripts/wk2-link-ci.sh --arch arm\|x64` | exit 0 = three binaries link |
| On-device fork runners | `run_wk3/4/5/7`, `drive_acceptance.py` | marker lines + FPC checksums (pixel readback rule) |
| Acceptance | `tools/run_browser_accept.sh` (self-proxies; V1 adds modes) | `report.json` `pass` (or `gated` w/ owner); pixel readback + net log |

### 6.2 Per-track gate matrix

- **Kernel/OS change** (X1, X2, N1-OS): focused on-device probe → host + unit-arm + unit-x64 →
  ARM wave (0 FAIL) → CI. 
- **Fork port code** (R1–R4, X3, X4): relink (`wk2-link-ci`/ninja) → on-device runner markers +
  checksums; 1 build at a time.
- **Integration** (I): merged-tip battery (host + units + ARM wave + desktop + accept subset) →
  `hobbyos-ci.sh all <new-label>` on hobytest.
- **Acceptance** (V2/V4, X5): toolkit + `run_browser_accept.sh` verdicts + JS probe values +
  soak receipts.

### 6.3 Acceptance run protocol (T2)

Every T2 row records: pixel readback (frame + screenshot PNG), DOM/probe output where
applicable, and a per-request network log (URL, status, TLS version, issuer, encodings, sizes).
Store under the lane's `evidence/` with a strict `report.json`. "Works" claims need the raw
lines; "slow" is acceptable, "silently degraded" is not. **Fidelity (v1.1):** L1/L2 rows (§1
ladder) = screenshot + glyph census + the serial's viewport/paint lines; "no upscale" means the
render `view` equals the window content size.

### 6.4 Evidence standards

As `browser.md` §8.4 + JS evidence rule (probe values, not screenshots), plus: no tick without a
run; every gate verdict = rc + marker + FAIL-token count; failures get fix-log entries with
diagnosis.

### 6.5 The frozen review copy

`~/Documents/GitHub/HobbyOS-review` (snapshot `b8e69a9`) is the stable research/build sandbox for
this plan and for sessions that need a non-moving tree. Verified state (see its `REVIEW-COPY.md`):
build + 1 GiB disk PASS; the unit tier carries one known tree-level red (`time_test.c:94` 256 MiB
bound vs the 1 GiB disk — fixed post-snapshot on main by `3313bcb`); wave reds were ambient-only
across two runs; desktop test passes via the scoped pkill shim; the disk's `BROWSER.BIN` defaults
to the fork build (`BROWSER_BIN=` to ship without it). Every chained tier triggers a full kernel
recompile + disk rebuild (~1–3 min on the 64-core box); don't `make -n` a tier (it runs).
**Refresh procedure** and isolation notes: §B.4 + §A.

---

## 7. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| F1 | Direct-edge TLS residual (mbedTLS↔edge) blocks the full-skin direct path | R1-g0 decides with receipts; relay/reader path stays as documented interim; edge interop gets a named owner if open |
| F2 | Render/readability scope creep ("make it pretty") | Bar is *legible + structured*, not typographic parity; H3 records numbers instead |
| F3 | Big-page memory (2.3 MB decoded page + the 86 MB-class WebProcess image; ELFs 139–147 MB, intel 182 MB) | Budgets recorded per run (V4/H3); demand-paging + prefetch already in; acceptance records high-water |
| F4 | x64 leg races the ARM work (shared integrator/CPU) | Budget §5.4; X1/X2 lanes start early but cap QEMU slots |
| F5 | Tip drift while this plan executes | This doc is a snapshot; §B re-baseline commands; §11 is the live record |
| F6 | Merge collisions among R1–R4 (same driver file) | §5.3 ownership + merge-order serialization; small diffs |
| F7 | Over-claiming "Wikipedia works" | T2 checklist + receipts gate the claim; proxy-interim explicitly labeled |
| F8 | Gzip/large-body surprises (Wikimedia gzip-only) | N1 receipts first; acceptance rows include encodings |
| F9 | Resource contention / pkill accidents | §5.4 do-not rules; shim; fresh-label CI |
| F10 | Stale docs mislead future sessions | H2 refresh is a wave deliverable, not an afterthought |

---

## 8. First dispatch wave (concrete, ordered)

Launch day-one (Integrator staggers to respect §5.4 — ≤2 QEMU lanes total **including R1's own ARM
boots**; ≤1 QEMU if a relink runs; host-side lanes unconstrained):

1. **X1 — x64 PS/2 button delivery** (OS lane; start first: it gates the x64 acceptance leg and
   has no dependencies). Brief: reproduce EV_KEY gap; fix delivery; on-device click-launch
   receipts; retest command in toolkit.
2. **R1 — direct-net & fetch unification** (fork lane): g0 direct matrix on merged tips — re-run
   WB's direct ladder (`HobbyOS/continuation/wk-realsite/run_realsite.sh`, the script behind the
   5/5 receipts) + one integrated-shell direct attempt (`tools/run_browser_accept.sh --url
   'https://en.wikipedia.org/…'`); wiki robots + Main_Page + a full article; classify every
   failure exactly. If direct green → flip the windowed default to direct and retest; else
   written RCA (windowed fetch path suspect: in-proc bridge vs WB `WKNetFetch`) + relay stays
   gated-documented; then full-skin page through the windowed frame.
3. **N1 — gzip + large-body** (OS/fork): receipts first; enable if missing; net sweep on any OS
   change.
4. **V1 — harness unification** (host-side): modes + net log + report v2; re-run fixture leg on
   the merged tip as demonstration.
5. **X2 — x64 LOSTWAKE/TSC audit** (OS kernel): TSC calibration + `-smp 8` crash; bounded
   receipts.
6. **H1 — license package** (host-side) continues.
7. **H2 — register hygiene** may start its non-conflicting parts (fork docs: DECISIONS/PORT_PLAN
   rows — H2 owns; browser.md edits wait for I).

FS-W2 additions (v1.1): **R5** (window-native render) + **R6** (desktop display mode) — the
resolution workstream from §2.1; R6 coordinates the mode bump with I.

Checkpoint **CP-FS1** after R1-g0 + X1 receipts: record the direct-path decision + x64 input
verdict into §11 and schedule FS-W2.

---

## 9. Open questions

1. **OQ-1 (critical):** Is direct HTTPS to `en.wikipedia.org` (full article) green on the merged
   tree after `477c5c4`? Receipts conflict (WB standalone: direct robots 200 + article fetches;
   §11 caveat: direct still interop-blocked; sharpest datapoint: the integrated final-reader
   run's `d-https` step was `gated-expected-fail` while WB's module passed → suspect the windowed
   fetch path). R1-g0 resolves; wording cleanup waits for the verdict.
2. **OQ-2:** Does the merged WA render fix cure the x64 white paint, or is a driver readback fix
   still needed? (X3 answers.)
3. **OQ-3:** gzip decode status on-device today (curl+zlib wiring)? (N1 answers.)
4. **OQ-4:** Is `-smp 8` x64 crash the same TSC/lost-wake class as the calibration defect, or a
   new signature? (X2 answers.)
5. **OQ-5:** HarfBuzz target build — needed for T2 (Latin text) or defer to WK-7 stretch?
   (R2 records the decision; default: defer, keep off.)
6. **OQ-6:** Wiki search submit path — does the current form GET wiring work without JS on the
   live page? (R4/V2 verify.)
7. **OQ-7:** Acceptable interim: reader-mode/proxied wiki as documented milestone while direct
   interop (if still open) gets its own track — maintainer review of this plan decides.
8. **OQ-8:** Desktop display-mode target for R6 (recommended 1920×1080) and R5 render strategy
   (1:1 first for TCG speed, supersample later?) — R5/R6 record the decision.

---

## Appendix A — Key paths & artifacts index

- **OS repo:** `~/Documents/GitHub/HobbyOS` (tip `5111de4`); frozen review copy
  `~/Documents/GitHub/HobbyOS-review` @ `b8e69a9` (+ `REVIEW-COPY.md` and `.review-artifacts/` —
  tier logs, scoped `shim/pkill`, desktop screendump).
- **Fork:** `~/webkit-hobbyos` (tip `4536f622cc`, branch `browser/l8-wk5`; port = ~111 overlay
  files under `HobbyOS/` + the `-DPORT=HobbyOS` bootstrap cmakes (`OptionsHobbyOS` +
  `Source/*/PlatformHobbyOS`)); lanes
  `~/webkit-lanes/*` (wab/wbn merged; `wb1` locked); per-fork `HobbyOS/continuation/**`. The
  browser binary itself is a fork artifact (flat objcopy → `::/BROWSER.BIN`); the OS side owns the
  menu/disk pipeline.
- **Proxy + drivers:** `tools/wiki_proxy.py` (+`/tmp/wiki-proxy-port`), `run_browser_accept.sh`,
  `br_e2e.py`; fork `HobbyOS/continuation/wk4b/wk6/run_acceptance.sh` + `drive_acceptance.py`.
- **Desktop reference (v1.1):** `~/.hermes/cache/scratch/final-stretch-research/09-desktop-reference/`
  — banner-free 1024/1920 captures + tall shot + computed-style facts (§B.6 regenerates).
- **Evidence anchors (tonight):** fork `HobbyOS/continuation/wk-addrbar/evidence/final-reader/`
  (screenshots + serial + report.json, `0xdabdfbc5`); `wk-realsite/ARM/run-REALSITE-*.log` +
  `RECEIPTS/**`; `wk6-live/evidence/**`; OS `docs/wd-x64-white-blit.md`.
- **CI:** `hobytest` (192.168.0.20) `~/bin/hobbyos-ci.sh <tier> [label] [N]`; logs
  `~/ci/logs/`. 
- **Skills (load per lane):** `hobbyos-build-and-run`, `hobbyos-run-tests`,
  `hobbyos-kernel-constraints` (X-lanes), `hobbyos-screenshot`/`hobbyos-gui-test` (V/acceptance),
  `lane-orchestration` (controller), `hobbyos-port-planning` (I).

## Appendix B — Regeneration / verification commands

```sh
# B.1 Tips + drift check
git -C ~/Documents/GitHub/HobbyOS        log -1 --format='%H %h %ad %s' --date=iso
git -C ~/webkit-hobbyos                  log -1 --format='%H %h %ad %s' --date=iso
git -C ~/Documents/GitHub/HobbyOS        log --oneline -20   # diff vs this snapshot (§11 top)
# B.2 Quick gates (in the target tree)
./run_unit_tests.sh ; ./run_unit_tests_intel.sh        # UNIT TESTS PASSED
timeout 1800 make test                                 # System halt + 0 FAIL tokens
timeout 900 make desktop_test                          # SCREENSHOT_READY, non-black dump
# B.3 Browser acceptance (ARM; the runner starts + owns its proxy — do NOT double-start)
bash tools/run_browser_accept.sh                       # default: drives /wiki/Main_Page via the proxy
#   variants: --url http://10.0.2.2:8800/reader/Hobbyist_operating_system --port N --evdir DIR
#   manual proxy only for bespoke drivers: python3 tools/wiki_proxy.py [--port N]
# B.4 Refresh the review copy (stable sandbox)
rsync -aH --exclude='.git/worktrees/' ~/Documents/GitHub/HobbyOS/ ~/Documents/GitHub/HobbyOS-review/
git -C ~/Documents/GitHub/HobbyOS-review status --porcelain   # normalize to HEAD if dirty
# B.5 Live-site posture re-probe (from the workstation; archive outputs)
curl -sv https://en.wikipedia.org/wiki/Main_Page -o /dev/null      # TLS ver, issuer, h2
curl -sI -H 'Accept-Encoding: gzip' https://en.wikipedia.org/wiki/Main_Page
# B.6 Desktop reference captures (workstation; §2.1)
google-chrome --headless=new --hide-scrollbars --force-device-scale-factor=1 \
  --window-size=1920,1080 --virtual-time-budget=8000 \
  --screenshot=<dir>/chrome-hobbyist-1920x1080.png \
  'https://en.wikipedia.org/wiki/Hobbyist_operating_system'
#   banner-free variants: capture via a CDP browser session, remove
#   [class*="frb"], [id*="fundraising"], [class*="fundraising"], #centralNotice, then re-shoot.
```

## Appendix C — Provenance of this plan

Produced 2026-10-05 evening from the frozen copy @ `b8e69a9` + read-only live-tree checks, with
parallel research passes (full artifacts under `~/.hermes/cache/scratch/final-stretch-research/`:
`04-webkit-ledger.md`, `05-acceptance-research.md` (+`probes/`, 119 files), `06-infra-lanes.md`,
plus the copy-verification / OS-state / fork-surface reports `01…03` when filed). Live-probe
numbers (TLS/encodings/sizes) are dated 2026-10-05 UTC. **v1.1 (2026-10-06):** desktop-reference
captures + a re-read of the final-reader evidence with the updated vision model; §2.1 added. This document is planning-only; it
changed no code. The planning session wrote ONLY this file (plus its review-copy artifacts).

## 11. Fix log (newest first)

- 2026-10-06 — **v1.1: desktop-reference comparison + resolution workstream.** Added §2.1
  (desktop-Linux reference vs on-device render; C1–C6 table) from new reference captures (1024/1920,
  banner-free variants) and a re-read of the final-reader evidence with the updated vision model
  (findings: no letterforms in the windowed page; 3-color/zero-black pixel census; chrome-glyph clipping; `320×240 → 1020×670`
  upscale confirmed in `final-reader/serial.log`). New: L0–L2 fidelity ladder (§1); lanes **R5**/**R6**
  (§4); gap G2b; wave/ownership/gate rows (§5–§6); OQ-8 (§9); §B.6. Direction (maintainer):
  much higher resolution for desktop + browser.
- 2026-10-05 — **v1 audit response.** Independent audit pass (~35 spot-checks; no blockers):
  corrected the accept-runner path + self-proxy behavior (§B.3/§6.1) and the F-R2 proxy contract
  (raw relay + `?head=N` + `/reader/<Topic>`; slice mode is lane-local scaffolding), fixed the
  wave-budget stagger + the R1-g0 command, added the guest-clock row (T2/V2), T0 fixture names,
  anchor corrections (text-blob quote, binary sizes, `-DPORT` bootstrap wording), and misc style
  fixes. Maintainer review still pending; OQ-7 asks the interim-path call.
- 2026-10-05 — **v1 created** (endgame plan from mid-WK-6 state; ground snapshot OS `5111de4` /
  fork `4536f622cc`).
