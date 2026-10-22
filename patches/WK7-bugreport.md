# WebKit Browser — E2E Test Sweep & Bug Report (WK-7)

Date: 2026-10-21 · Repos: `~/Documents/GitHub/HobbyOS` (OS/disk) and
`~/Documents/GitHub/webkit-hobbyos` (browser fork, `HobbyOS/WebKit/WebProcess/hobbyos/`)

## Method

Seven independent test lanes, each booting its own ARM QEMU desktop instance
(own disk clone, own QMP socket, own serial log) and driven **only** through
QMP-synthesized mouse/keyboard + screen captures — no direct engine hooks.
Every assertion comes from the guest's own serial receipts (`[WIN] …`) and the
framebuffer. Entry point: `python3 tools/wk_scenario.py --id S<n> --lane <x>`
(harness `tools/wk_lane.py`).

| Scenario | Story | Outcome |
|---|---|---|
| S0 | original acceptance: address bar → GO → google.com → click search box (mouse) → type "cat pictures" (keyboard) | **FAILED before fix** (HTTPS broken), **PASSES** once the CA bundle is on the disk |
| S1 | example.com → click in-page link (mouse) → chrome Back → return; then Wikipedia link-click attempt | pass (link+back work); the Wikipedia census click is unreliable (see B6) |
| S2 | news.ycombinator.com → click story link → Back → Forward | pass (link/back/forward all work with real story URLs) |
| S3 | Wikipedia long article → wheel scroll down → PageDown → wheel up; repaint checksum per step | pass (scroll works; PageDown step wrong — B3) |
| S4 | new tab (+), navigate 2nd tab, switch back, close tab | pass (tabs work; frame checksums + pixel diffs confirm visible switch) |
| S5 | star → bookmark, navigate away, reload, return via bookmark chip | pass mechanically, but re-visit paints blank — B1 |
| S6 | html.duckduckgo.com: click search field, type "hobby os", Enter, expect results; then bad-host error page | **FAIL**: Enter submits a bodyless GET — B2; bad host shows nothing — B4 |

Load times under TCG for reference: example.com ~5‑6 s, HN ~18‑30 s,
Wikipedia/Cat ~16‑45 s, Google ~36 s (first load).

## Bugs (severity-ordered)

### B1 — [P1] Re-navigating to a heavy page paints a blank viewport and stays blank
**Trigger:** after visiting a heavy page (Wikipedia/Cat), leaving it, then
returning via the bookmark chip (heavy page + a queue of ~70 serialized image
fetches at ~280 ms each).
**Observed (lane `agente-s5`):** during parse the driver paints an all-zero
frame `[WIN] frame view=1916x934 nonZero=0 checksum=0xd5e71245`,
`[IMG1p] colors=1 dom=0x000000 foreign=0`, then `[COMP] wedge-dump reqC=3
pending=60`; `[WIN] load-ok … ms=45357` follows but **no further frame is ever
painted** — screenshot `shots/06-back-on-cat.png` shows a white viewport with
the correct URL in the omnibox. Control experiment (`tools/wk_probe_repaint.py`)
shows the same page re-visited via the **omnibox** does repaint (~17 s), so the
blank is conditional on the navigateByName path and the subresource queue.
**Expected:** the page paints real content once parse/load completes.
**Suspected:** load-complete paint is gated behind subresources draining and/or
the post-load repaint signal is lost (see port trap: DrawingAreaHobbyOS
`setNeedsDisplay` must bump the WK5PaintSignal epoch the driver polls); an empty
frame is allowed to paint during parse.

### B2 — [P2] Form submission drops the input values (POST → bodyless GET)
**Trigger:** Enter in a page form whose method is POST (html.duckduckgo.com
search: `<form … action="/html/" method="post"><input name="q">`).
**Observed (lane `re6`):** field focused (`[WIN] focus=INPUT|q|…`), text typed
(`[WIN] page-value=hobby os`), Enter forwarded (`[WIN] page-key ch=0x0a
key=Enter`), then `[EDIT] kd-miss vk=13`, `[POL] self-driven auto-use
url=https://html.duckduckgo.com/html/`, `[WIN] engine-nav … claimed=1` — the
submission navigates to the **bare action URL with no `?q=`**, re-loading the
empty search page. The shell nav path (`wk5NetFetch`) only issues GET with no
body (`handle.enableHttpGetRequest()`).
**Expected:** the form values reach the server (GET-equivalent URL, or a real
POST body).

### B3 — [P2] PageDown scrolls 40 px, the same as one wheel notch
**Observed (lane `agentc-s3`):** wheel-down×3 → `scroll pos=120`; PageDown →
`pos=160` (only +40); wheel-up×3 → `pos=40`. `WK5_SCROLL_STEP` is 40 and
`scrollPage()` uses it for the keyboard page keys too.
**Expected:** PageDown moves roughly a viewport.

### B4 — [P2] Failed navigation shows nothing (no error page, blank omnibox)
**Observed (lanes `s0-google`, `agentf-s6`):** on `[WIN] load open-fail` (TLS
verification without CA bundle; unresolvable host) the **previous page stays on
screen**, the omnibox is **empty**, and no error text exists anywhere in the
driver (screenshots `s0-after-fail.png`, `06-bad-domain.png`).
**Expected:** a real "can't reach this page" error document + the attempted URL
in the omnibox.

### B5 — [P2] Tab/bookmark labels come from the URL, not the page title
**Observed (lane `agente-s5`):** `[WIN] bookmark add
url=https://en.wikipedia.org/wiki/Cat title=en.wikipedia.org/wiki` while the
document title is "Cat - Wikipedia". `buildChromeState()`/`toggleBookmark()`
use only `bkTitleFromUrl()`.
**Expected:** document title once the page is loaded, URL-derived label only as
a fallback.

### B6 — [P3] Link census lists hidden anchors → suite mis-aims clicks
**Observed (lane `re1`):** Wikipedia Vector-2022's hidden main-menu anchors
(`/wiki/Main_Page` etc.) report rects at (218,125)…; clicking the stated rect
hits a TOC "Hide"/"#" element instead (`focus=BODY`,
`[WIN] link -> https://en.wikipedia.org/wiki/Cat#`). The census JS takes the
first 12 anchors in document order with no visibility/hit-test check.
**Expected:** census only lists anchors whose own center is the topmost element
there; raise the 12 cap; skip zero-size/off-viewport boxes.

### B7 — [P3] Serial emit line truncates and can garble census lines
The emit buffer is fixed 512 B (`wk5emit`): Wikipedia's census is cut mid-token
after ~5 links; on Google the links line is spliced with fragments of an older
string (stale/mis-synchronized buffer). The kernel `[ALIVE-SET]` scheduler dump
also splices into the middle of `[WIN]` lines (~229 occurrences in `agentc-s3`).

### B8 — [P1, environment] HTTPS dead from a clean build — no CA bundle on the disk
`::/CERTS/CA.PEM` exists on the OS design (`browser.md`) but **nothing in the**
**Makefile creates `/CERTS` or stages a bundle**, so every `https://` navigation
fails at certificate verification (`err=no-CA-bundle(/CERTS/CA.PEM missing;
verification will fail)SSL peer certificate …`). The user's original google.com
scenario cannot work until this is fixed.
**Fix (already staged in this sweep):** vendored the Mozilla root bundle
(`certs/cacert.pem`, 131 roots, validated against all five target sites) and
`make disk.img` now creates `::/CERTS` and copies it to `::/CERTS/CA.PEM`.

### B9 — [P2, observability] `load-ok ms=` is wall-clock since launch, not per-navigation
`m_start` is stamped once in the `WK5State` constructor (`WK5WindowDriver.cpp:853`)
and **never reset on navigation**; `load-ok ms=` (`:3442`) and the load-timeout
watchdog (`:3541`, `loadElapsed > 1800 s`) both read it. Observed (S2): identical
loads report monotonically rising 6946→14612 ms, and any navigation after ~30 min
uptime would immediately trip `FAIL load-timeout`. Fix = reset `m_start` at every
navigation entry (GO, back, forward, bookmark, engine-nav).

### B10 — [P2, perf] Every scroll paints the full frame twice
`scrollPage()` paints synchronously (`:1733`), then WebCore's own invalidation
fires a second identical Stable-branch repaint (`:3575`). Two 70–96 ms TCG renders
per notch. No fix this wave; documented.

### B11 — [Low] GO button is receipted `ui-click kind=none`; duplicate `m_stallTicks=0`
The chrome hit-name switch (`WK5Chrome.cpp` caller in `WK5WindowDriver.cpp:1130`)
has no `HitKind::Go` case; `back()/forward()/loadForHistory()` zero
`m_stallTicks` twice.

### B12 — [Low/cosmetic] Star is an asterisk glyph (colour-only state); long labels clip mid-glyph with no ellipsis
`wk5DrawIcon`/`ICON_STAR_OUT`/`ICON_STAR_FILL` exist but are never called.
`drawText` clips at the box edge mid-glyph.

## Evidence tree (all under `~/.hermes/cache/scratch/`)
`wklane-agente-s5/` (B1, B5) · `wklane-re6-*/` (B2) · `wklane-agentc-s3/` (B3,
B7) · `wklane-agentf-s6/` + `/tmp/s0-after-fail.png` (B4) · `wklane-re1-*/`
(B6) · `wklane-probe-https/` (B7, B8) · `wklane-probe-repaint/` (B1 control).
