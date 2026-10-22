# WK7-repaint: blank viewport on bookmark-chip revisit of a heavy page

Scope: single file `HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp` (driver only;
`DrawingAreaHobbyOS.h` / `WK5Chrome.*` untouched). Companion patch: `WK7-repaint.patch`
(delta of 18 added / 14 moved / 5 removed lines). No build, no qemu, no repo edit — the
webkit-hobbyos working tree was restored byte-identical to HEAD after generation;
validated read-only with `git apply --check`.

---

## 1. Confirmed root-cause chain (exact serial evidence)

Evidence: `wklane-agente-s5/serial.log` (Cat page visit via the bookmark chip, second
time, with the serialized single-worker subresource queue). All line numbers are the
serial file's line numbers.

1. **The nav path itself is the difference.** The bookmark chip click lands in
   `handleUiClick` → `HitKind::Bookmark` (WK5WindowDriver.cpp:1180-1185), which calls
   `navigateByName(...)` and then **`break`s into the shared fall-through
   `setTitlePrompt(); m_chromeDirty = true; bumpSettle(); paintAndBlit();`
   (lines 1200-1203). Every *other* nav entry point returns before that line: the
   omnibox GO button calls `commitAddress()` which `return`s (line 1175), and link
   clicks are dispatched by the engine loop directly to `navigateByName` (line 3717)
   with no ui-click fall-through. This is why the probe
   (`tools/wk_probe_repaint.py`) shows the omnibox re-navigation repaints after ~17 s —
   it never commits a mid-parse blank.
   `serial.log:80756` `[WIN] bookmark nav idx=0 url=https://en.wikipedia.org/wiki/Cat`
   `serial.log:80757` `[WIN] nav name=https://en.wikipedia.org/wiki/Cat`
   `serial.log:80803` `[WIN] net fetch url=... ok=1 status=200 bytes=1553689`
   `serial.log:80839` `[WIN] base-injected effective=... (1553736 B)`
   `serial.log:80840` `[WINP] frame-load-begin url=...`

2. **The load is far from finished when the fall-through paints.** For the 1.55 MB
   article the synchronous `FrameLoader::load` returns while the parser is still gated
   on stylesheets/scripts and the subresource queue is draining serially through the
   one pool worker:
   `serial.log:158086` `[WINP] frame-load-end ms=7859`
   `serial.log:158254` `[COMP] wedge-dump reqC=3 pending=60:`
   `serial.log:158371` `[COMP] wedge-dump reqC=9 pending=73:`
   (worker picks are ~60-280 ms apart: `serial.log:158309` `[WINP] worker-pick token=27 ...
   gap=60ms`, `158466` `... token=36 ... gap=280ms`; `[COMP] checkCompleted` around it
   shows `parsing=1`).

3. **The fall-through paint of the mid-parse document is all-zero, and it overwrites the
   last real frame.** `paintAndBlit()` runs `updateLayoutAndStyleIfNeededRecursive()`
   on a document that has no laid-out render tree yet; the ImageBuffer comes back
   transparent-black. The frame is still *published* (`m_frame = pixels`, `m_hasFrame =
   true`, blitted) — there is no guard at the commit site:
   `serial.log:158155` `[WIN] frame view=1916x934 nonZero=0 checksum=0xd5e71245`
   `serial.log:158157` `[WIN] frame-times render=88ms blit=12ms total=112ms`
   `serial.log:158159` `[IMG1p] view=1916x934 colors=1 dom=0x000000 foreign=0`
   The blit composites the transparent frame over white → the white viewport in
   `shots/06-back-on-cat.png`. Contrast: the first Cat visit paints real content:
   `serial.log:79243` `[WIN] frame view=1916x934 nonZero=7131805 checksum=0x0b2ae832`
   and example.com `80293` `nonZero=7155895`.

4. **Nothing repaints over it during the ~17 s serialized drain.** The engine paints
   in the Load phase only at `loaded || stalledPaint` (WK5WindowDriver.cpp:3416); a
   long parse legitimately produces no further frames, so the committed blank (plus
   UI-thread reblits of it, `serial.log:158143/158144/158156` `[WIN] reblit cached ...`)
   is what the window shows until the drain ends.

5. **When the drain finally ends, the load-complete paint is gated behind the whole
   census chain.** At `loaded || stalledPaint` the code emits load-ok, then runs the
   *four* JS censuses (doc-title / links / inputs / buttons) plus `imgCensus()` over the
   full DOM, and only then sets `Phase::Stable` and calls `paintAndBlit()` (the old
   lines 3524-3528). The census is heavy on this page (3691 `<a>` links) and runs on
   the same engine thread while 60+ pool workers are still finishing. The recovery
   paint never reached the window before the run window closed:
   `serial.log:158461` `[WIN] load-paint-early rt=1 parsing=0 reqC=0 stall=12`
   `serial.log:158462` `[WIN] load-ok url=https://en.wikipedia.org/wiki/Cat ms=45357`
   `serial.log:158473` `[WIN] doc title=Cat - Wikipedia|1731538|12|3691|59`  ← **last
   line of the serial log**; no `links`/`inputs`/`buttons`, no `[IMG1p]` with real
   colors, no post-load `[WIN] frame` ever lands (screenshot at +2.5 s is blank).

    So the reset is conditional exactly as observed: the blank is *committed by an
    ungated paint during parse*, and the *recovery* is *gated behind the census
    draining*. The WK5PaintSignal epoch machinery is healthy (it fired in earlier
    visits: `79289` `page-repaint epoch=31`, `80299` `epoch=40`, `80703` `epoch=58`) —
    it is not the defect, just proof of where Stable repaints normally come from.

## 2. The fix and why it cannot regress anything

Two minimal, provable changes in `WK5WindowDriver.cpp`:

- **M1 — never commit an all-blank frame while the parser is running** (`paintAndBlit`,
  right after the `[WIN] frame` receipt, before the commit critical section):
  if `nonZero == 0` and `localFrame->document()->parsing()`, emit
  `[WIN] frame-skip-blank nonZero=0 parsing=1 ...` and `return` without publishing.
  The last real frame stays cached and displayed through the whole drain; the load-ok
  paint (which forces layout) publishes the real page. No lock is touched — the guard
  runs before the `m_dispMu` critical section, reads only guarded live args, and a
  rendered-then-discarded buffer costs nothing later converges.

- **M2 — paint the styled frame at load-ok / stalledPaint *before* the census chain**
  (`enginePhaseTick`): move the `m_phase = Phase::Stable; m_stablePaints = 0;
  m_stableTicks = 0; bumpSettle(); paintAndBlit();` block to immediately after the
  `[WIN] load-ok` emit (restoring the documented fs-r7 "PAINT FIRST" contract), and
  delete the identical block at the end of the tick. The census receipts still follow
  in the same tick, after the frame is on screen; even if the census is slow or stalls
  on a heavy page, the real frame is already displayed.

Non-regression argument:

- **Fresh load via omnibox GO / link click** — those paths `return` before the
  line-1203 fall-through paint, so M1's guard is inactive for them; M2 only reorders
  within the load-ok tick (frame now precedes the census; every receipt — load-ok, doc
  title, links/inputs/buttons, IMG1p — is still emitted). The runner's `settle()` waits
  for `[WIN] load-ok` **and then** the post-load frame quiet window, which is unchanged.
- **example.com / other light pages** — parse completes in ~130 ms, so the old code's
  fall-through paint was already real content. New behavior: while `parsing()` is true
  the previous frame is kept instead of a blank (a sub-second, invisible difference);
  at load-ok M2 paints the real frame exactly as before but sooner in the tick. Receipt
  order changes (frame before doc title) but the set of receipts is identical.
- **Tab switch / close / new tab** — `switchTab`/`closeTab`/`newTab` have their own
  self-paint paths (lines 1340/1371) and return before the fall-through; neither M1 nor
  M2 touches them. The Stable-phase epoch repaint and settle-budget repaint loops
  (lines 3574-3599) are untouched. `DrawingAreaHobbyOS.h` epoch mechanism, `WK5Chrome`
  types, and the display lock are all untouched.

## 3. Verification plan (receipts to assert)

Re-run the S5 scenario (Cat → bookmark → example.com → reload → bookmark chip back):

1. **No blank frame is ever committed.** Between `[WIN] bookmark nav idx=0
   url=.../Cat` and `[WIN] load-ok url=.../Cat`, assert there is **no**
   `[IMG1p] ... colors=1 dom=0x000000 foreign=0`, and **no** `[WIN] frame ... nonZero=0`
   without an immediately following `[WIN] frame-skip-blank nonZero=0 parsing=1 rt=...`.
   (During the drain you now see the previous real frame reblitted: `[WIN] reblit cached
   view=1916x934 ...`.)
2. **Load-complete paint lands before the census.** Assert the receipt order
   `[WIN] load-ok url=... → [WIN] frame view=1916x934 nonZero≈7.1-7.2M checksum≠0xd5e71245
   → [WIN] doc title=... → [WIN] links ... → [WIN] inputs ... → [WIN] buttons ...`.
3. **The window ends real, not blank.** The last `[IMG1p]` after load-ok must have
   `colors > 1` and `foreign > 0` (real Cat render — cf. `79247` colors=256
   dom=0xffffff foreign=117348), and `shots/` at +2.5 s must be non-white.
4. **No blank on the light paths.** Fresh example.com and its reload must show zero
   `[WIN] frame-skip-blank` lines and keep `nonZero≈7155895` frames; omnibox re-nav to
   Cat must still repaint (as the probe already shows) and now also emit a single skip
   of any mid-parse blank instead of committing it.
5. **Stable machinery unchanged.** `[WIN] page-repaint epoch=N+1` still advances after
   load, and load-timeout/gate-probe receipts (`[WIN] FAIL load-timeout`, `gate-probe`)
   are unchanged.
