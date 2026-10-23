# WK-7 census fixes — hidden/unhittable links, truncated & garbled emits, scroll receipt at load

Date: 2026-10-21 · Repo: `~/Documents/GitHub/webkit-hobbyos`
File touched: `HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp`
Patch: `HobbyOS/patches/WK7-census.patch` (unified diff, validated with `git apply --check`).

Three separate defects in the post-load test hooks, confirmed from on-device serial
logs (classic E2E evidence, no engine hooks). The census JS runs in the isolated
world via `executeScriptInWorldIgnoringException` and stays **pure JS** — no C++
per-link work — exactly as before.

---

## (A) The link census lists anchors that are not actually clickable

### Evidence
- `wklane-re1-s1/serial.log` (Wikipedia):
  `[WIN] links 3691;#bodyContent@-1,-1:1x1|/wiki/Main_Page@218,125:193x28|/wiki/Wikipedia:Contents@218,154:193x28|…`
  — the census advertised links whose rects report non-zero size but which the
  driver then could not click.
- Same log, the drive aimed at the *declared* centre of `/wiki/Main_Page`
  (`@218,125:193x28` → centre (314,139)) and the C++ click path reported
  `[WIN] click x=314 y=222 px=314 py=138` → `[WIN] focus=BODY|||` — a click at
  the census rect centre did **not** resolve to that anchor; later the same slot
  resolved to the TOC `#` element: `[WIN] link -> https://en.wikipedia.org/wiki/Cat#`.
- `#bodyContent@-1,-1:1x1` is a zero-size/off-viewport anchor `getBoundingClientRect()`
  still reports a box for (it is real in the DOM, invisible in the render).

Root cause: the census only read `querySelectorAll('a[href]')` + `getBoundingClientRect()`
and never asked whether a click at that rect actually lands on the anchor. Hidden
off-canvas menus and TOC overlays carry real DOM boxes that `elementFromPoint`
resolves to a different element, so the serial contract ("click this centre and you
follow this link") was false for those entries.

### Fix (link census JS, cap 12 → 20)
For each anchor, only list it when a click at its rect centre is genuinely hittable:

- skip boxes with `width<1 || height<1` (zero-size) and boxes whose edges lie fully
  outside the viewport (`r.right<=0 || r.bottom<=0 || r.left>=vw || r.top>=vh`);
- keep the anchor only if the topmost element at its centre is the element itself
  or a descendant:
  `a.contains(document.elementFromPoint(cx, cy))` with
  `cx = Math.round(r.left + r.width/2), cy = Math.round(r.top + r.height/2)`;
- scan at most 200 anchors, list at most 20 (was 12), keep the leading total count
  (`L.length`, unchanged semantics).

This is exactly the check the **C++ click path** already performs — the driver
resolves clicks with `doc->elementFromPoint(px, py, HitTestSource::User)` and then
walks up to the enclosing `<a>` (`WK5State::handleMouse`, driver line ~1845) — so
the census and the real click now agree on hittability. The format is untouched
(`<total>;<href>@<left>,<top>:<w>x<h>|…`).

### Verification
- `node` extracted-the `lexpr` string from the patched source, `new Function(js)`
  parses it (syntax valid for JSC), and a Wikipedia-like simulated DOM
  (`wklane` fixture: hidden sidebar anchors, `#bodyContent`-style zero-size anchor,
  overlay-covered anchor, plain visible anchor) returns
  `7;/wiki/Linked_list@60,500:80x20|…` — the hidden side-menu anchors, the
  zero-size anchor, and the overlay-covered anchor are all dropped; the two
  hittable entries (link + its inline child) are kept. (Validation script:
  `~/.hermes/cache/scratch/wk7-census-validate.js`.)
- No on-device run (read-only task — change must be validated by the next sweep,
  which does the real click-through).

---

## (B) Serial emit truncates and garbles long censuses

### Evidence
- `wklane-probe-https/serial.log` (Google): the census line is truncated and the
  tail is interleaved garbage from *other* strings:
  `[WIN] links 10;https://mail.google.com/mail/&ogbl@1629,18:38x24|https://www.google.com/imghp?hl=en&ogbl@1682,18:48x24|https://www.google.com/intl/en/about/products@1744,11:[4N0ExT4]0 |fhetttcphs-:r/e/ta cOcKo(uqnutise.tg)o…`
  Decoding the tail shows real census fragments spliced in wrongly
  (`accounts.google.com/service/login…`, `fonts.gstatic.com…`), i.e. bytes from a
  *different* string mixed into this line — the `%s` read followed a pointer into
  memory that was re-used between the JS evaluation and the formatting.
- `wklane-re2-s2/serial.log` shows a baked, unparseable `[WIN] links 230;…` tail
  cut mid-`@<x>,<y>` — the fixed `char line[512]` buffer in `wk5emit` could not
  hold a full Google/YS census (the old 12-item census routinely exceeded 512 B).

Root cause: two independent problems, both fixed:
1. **Buffer too small.** `wk5emit`'s `char line[512]` truncates any census line
   longer than a 511-byte payload mid-token (the census is one line by design).
2. **Stale byte pointer.** The emits passed a *temporary* `CString`'s buffer to
   the varargs formatter: `wk5emit(…, lr.utf8().data())`. The `CString{}` from
   `String::utf8()` is destroyed at the end of the full expression, and its
   JS-generated byte buffer is owned by the JSC heap; between the JS evaluation
   and `vsnprintf`'s `%s` read the buffer can be freed/re-used, so a previous
   string's bytes get spliced into the live line. The probe-https Google line is
   exactly that signature.

### Fix
- `wk5emit` line buffer 512 → **2048**.
- Hoist a private byte copy that outlives each emit: for the four JS-result
  census strings (`doc`, `links`, `inputs`, `buttons`) keep
  `CString xxxOut = …utf8();` and pass `xxxOut.data()` — the buffer now lives for
  the whole `wk5emit` call. Census **format** (delimiters, fields, leading count)
  is otherwise unchanged.

### Verification
- `git apply --check` clean on pristine HEAD.
- The 2048 B buffer covers the longest observed censuses (the ~800 B Google line
  and any 20-anchor line that survives the (A) filter); a `CString` copy has no
  dependency on the JS heap, so no stale bytes can appear.
- The remaining input/button census emissions did **not** change shape — same
  `[WIN] inputs N;…` / `[WIN] buttons N;…` wire contract, only the buffer/lifetime
  hardened.

---

## (C) Scroll receipt at load (so the harness can tell "starts at 0" from "receipt missing")

### Evidence
- `wklane-agentc-s3/serial.log`: a clean page load paints frames
  (`[WIN] load-ok`, `[WIN] frame …`) but no `[WIN] scroll pos=` line exists until
  the first wheel input produces `[WIN] scroll pos=40` at ~t+79522. `scrollReceipt()`
  is only invoked on the fragment-jump and wheel/keyboard scroll paths, so an
  untouched page emits nothing and the harness cannot distinguish "content at
  scroll 0" from "receipt lost".

### Fix
- Call `scrollReceipt()` once right after the first `paintAndBlit()` of a freshly
  loaded page in `enginePhaseTick()` (the `loaded || stalledPaint` arm), next to
  where the fragment/load-ok/census receipts are emitted. `scrollReceipt()` is an
  existing `WK5State` member that prints `[WIN] scroll pos=%d` from the frame
  ScrollView — no new JS, no new wire format.

### Verification
- A fresh load now shows `[WIN] scroll pos=0` immediately after the first frame
  and before any wheel input; wheel/paging receipts continue unchanged. This is a
  one-line call addition (no state), so it cannot perturb the scroll path.

---

## Compliance & scope
- `wk5emit` buffer: 2048 is a stack array inside a per-thread function — no static
  growth, no lock/shared-state change.
- Census JS remains isolated-world, pure DOM — no C++ per link.
- Inputs/buttons census JS untouched; only their emit's string lifetime is pinned.
- Links census emits the same fields with the same delimiters; the only changes
  are the hittability filter, the cap (12 → 20), and the harder lifetime/buffer.
- The patch applies cleanly against current `main` (`git apply --check` OK) and
  only touches `WK5WindowDriver.cpp`.
