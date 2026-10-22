# WK7 — HobbyOS WebKit browser: scenario sweep, bug fixes, verification

Scope: boot HobbyOS (ARM), launch the browser from the apps menu, and drive
real sites through QMP input (mouse + keyboard). The pre-existing `tools/`
harness (`wk_lane.py`, `wk_scenario.py`, `wk_s0_google.py`) boots a parallel
lane per scenario (clonefile disk.img, own QMP socket + serial log), so
scenarios run concurrently.

## Scenario suite (original + 6 more)

| ID | Scenario (steps) | Site(s) | Ordered by |
|----|------------------|---------|------------|
| S0 | baseline: type address → GO → Google renders | google.com | original |
| S1 | render example.com, click its link, Back, then a Wikipedia in-article link | example.com, iana.org, en.wikipedia.org/wiki/Cat | original+extended |
| S2 | render HN, click a story, Back, Forward | news.ycombinator.com | new |
| S3 | render long article, wheel-down, PageDown, wheel-up, each scroll repaints | en.wikipedia.org/wiki/Cat | new |
| S4 | render Cat, open tab 2 → example.com, tab switch, tab close | en.wikipedia.org, example.com | new |
| S5 | load Cat, star bookmark, → example.com, reload, return via bookmark chip, assert non-blank repaint | en.wikipedia.org/wiki/Cat, example.com | new |
| S6 | DDG html form, focus field, type, Enter → results; then unresolvable host → error page | html.duckduckgo.com | new |

## Bugs found (12 candidates → 7 fixed)

| Fix | Root cause | Verified by |
|-----|-----------|-------------|
| B9 stale-load-timeout watchdog | `m_start` stamped only in the ctor, never reset on later navigations → spurious `FAIL load-timeout` on unrelated loads. Reset at all 6 load-start sites (navigateByName, loadForHistory, reload, back, forward, newTab). | S1/S2/S3/S4/S5/S6: 0 `FAIL load-timeout` |
| B-blank bookmark return (repaint) | bookmark/uiclick nav path painted an all-zero frame over the last real one during the serialized subresource drain, holding a blank window. Guard: never commit a parse-time blank (`frame-skip-blank`); repaint inside the load-ok tick before the censuses. | S5 (real `nonZero>0` frame on return) |
| B-no-error-notice | failed nav (TLS/DNS/HTTP error) silently kept the old page. Now renders an in-browser error page (`about:error`): `[WIN] errpage ... class=dns|tls|http`, omnibox keeps the attempted URL, doc title `Can't reach this page`; `open-fail` no longer pre-empts the settle. | S6 |
| B-form-post | Enter on a method=post form submitted the bare action URL with the body dropped (GET-only fetch path) → empty page. Fold the urlencoded POST body into the action URL's query (and use `originalRequest()` — the authoritative form-submission request). NOTE: `equalLettersIgnoringASCIICase()` mis-evaluates in this fork (false for a byte-exact `POST`, hex `504f5354`, len 4) — method check is a plain ASCII CI compare. | S6: `url=…/html/?q=hobby+os&b=`, 31,828 B, 42 result links |
| B-census visibility | link census listed hidden/off-viewport anchors; clicks at their rects hit BODY/TOC. Census now skips zero-size / off-viewport / occluded anchors (elementFromPoint check, cap 20). | S1 (3691 links, in-article link click), S2 |
| B-census truncation | long census lines truncated in a 512 B emit buffer (garbled `[WIN] links` tails) + CString lifetime hazard. Buffer 2048 B; hoist `utf8()` CStrings for whole `wk5emit`. | S1/S3/S4 (full `links`/`doc title` parse) |
| B-serial tearing / kernel splice | `[WIN]` lines spliced mid-byte by kernel `[ALIVE-SET]` output. Driver emits one logical line as ONE `print_console`. (Kernel whole-line `uart_lock` was tried and REVERTED — see below.) | S2/S3/S4 (untorn lines) |

## Residual / pre-existing bugs (documented, not caused by this session)

1. S2 forward leg: after a successful item fetch on the HN story (bytes=276226,
   ok=1), the guest wedges system-wide (all BROWSER.BIN tasks `st=9`, kernel
   `[WAITPID] ECHILD`/IDLESTUCK storm) and no `load-ok` follows. **Proven
   pre-existing**: the pristine pre-fix binary fails S2 the same way (even
   earlier, at the story-link click). Repro 4/4 across fixed + pristine images.
   Likely a guest scheduler/network-pool robustness bug on repeated heavy-page
   navigation; out of scope of the 7 fixes.
2. Kernel `console_write_user` whole-line `uart_lock` (irqsave) change was
   reverted: it did not fix the S2 wedge and adds long IRQ-masked windows on
   big census lines under emulated UART. Serial atomicity is instead handled
   driver-side (single `print_console` per line).
3. `webkit-hobbyos/HobbyOS/scripts/wk2-libs-cross.sh` line ~331 has a baked
   dev-machine fallback path (`$HOME/hobbyos-lanes/l5-fonts/...`) used when
   `third_party/harfbuzz-14.5.0/src` is not extracted — extract the committed
   tarball before `--build-deps`.
4. `tools/cross-arm-wk2-libs.sh` uses GNU `sed -i` (fails on macOS BSD sed) —
   patched to portable `sed -i.bak`. curl on the bare toolchain mis-detects
   `HAVE_UNISTD_H=0` → `altsvc.c` needs an explicit `#include <unistd.h>`.

## Cumulative verification (final image)

Every scenario run on the SAME final rebuilt disk.img (all 7 fixes; no debug
markers in the binary):

| Scenario | Result | Notes |
|----------|--------|-------|
| S1 (example/iana/Wikipedia) | **5/5 PASS** | census click at stated rect → iana.org; Back → example.com; Wikipedia `/wiki/Main_Page` clickable. 0 `FAIL load-timeout`. |
| S2 (HN story + back/forward) | 4/5 — residual pre-existing bug | see below. 0 driver-side `FAIL load-timeout`. |
| S3 (long article scroll) | **5/5 PASS** | wheel/PageDown/PageUp + per-gesture repaints (distinct checksums). |
| S4 (tabs) | **5/5 PASS** | open/switch/close tabs, all frames non-blank. |
| S5 (bookmark → return repaint) | **6/6 PASS** | transient pre-load blank correctly skipped (`frame-skip-blank` ×1), real `nonZero=7,131,805` repaint, 0 blank `[IMG1p]`. |
| S6 (DDG form + error page) | **5/5 PASS** | Enter folds `?q=hobby+os&b=`, 31,828 B results fetch, 42 result links; bad host → `errpage class=dns` → `load-ok url=about:error` → `doc title=Can't reach this page`, painted. |

Known residual (pre-existing, proven by pristine-binary A/B — the unpatched
browser fails S2 the same way and earlier): S2's Forward leg reproducibly
wedges the guest after a successful heavy item fetch (`frame-load-end` then
DESKTOP.BIN `[WAITPID] ECHILD` busy-spin ×4.4M + 49× `[IDLESTUCK]`, all
BROWSER.BIN tasks sleeping; no `load-ok`, no `FAIL load-timeout`). This is a
guest scheduler / network-pool robustness bug on repeated heavy-page
navigation, not one of the 7 fixed issues. Kernel whole-line `uart_lock`
(irqsave) did not fix it and was reverted.
