# WK-7 browser chrome UI — Chrome-like chrome (nav buttons, omnibox, bookmarks) + tabs (phase 2)

**Opened 2026-10-11 (controller).** User request: add modern browser UI to the
HobbyOS web browser — Forward / Back / Reload buttons, bookmarks —
mimicking Chrome's layout (general sense); then, after that lands, tab
support.  The work is split into parallel tracks (wave 1 below) per the
user's explicit direction.

## Ground truth (verified 2026-10-11, fork `browser/rp-f` @ `9a7b77ffb9`)

- The browser UI is the fork's `WK5WindowDriver` — one ~2.8k-line TU
  (`HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp`) running inside the
  WebProcess binary.  It draws its own chrome band (36 px: omnibox + GO) into
  the raw fb above the page blit, and already HAS `back()/forward()/reload()`
  + driver-side URL history (`m_history/m_histIdx`), F5/F7/F8/arrow keys,
  click→nav (link hit-test → `navigateByName`).
- Threading: the WebKit engine loop runs on the process MAIN thread
  (RunLoop-affine — do not move); the UI/protocol thread owns input decode +
  cached reblits, reading a chrome/state SNAPSHOT copied under `m_dispMu`.
  Any new chrome state rides the same discipline: trivially-copyable struct,
  copied under the mutex; no WebKit calls off-main.
- WM input: mouse arrives CONTENT-relative (0,0 = window content top-left);
  content origin/size via `[WIN] geom x y w h` (ESC ] G).  Left-click only
  drives UI today (kind 0 btn 1); wheel = btn 4/5.
- Build/run economics: **x64-KVM is the fast loop** (boot ≈3 s, fixture loads
  seconds): `ninja -C WebKitBuild/HobbyOS-intel WebProcess` (warm ≈1–2 min) →
  OS worktree `make ARCH=intel disk.img BROWSER_BIN=<ELF>` → QEMU-KVM q35,
  runner pattern = `~/hobbyos-lanes/perf-x64/tools/run_x64_browser.sh`
  (read-only template).  ARM-TCG = final receipt only (slow).  The shared
  cross-prefixes (`~/webkit-hobbyos-wk2/**`) are PROTECTED.

## Frozen design v1 (the two headers are the binding contract)

- `HobbyOS/WebKit/WebProcess/hobbyos/WK5Chrome.h` — geometry constants,
  `WK5ChromeState` (trivially copyable snapshot), `ChromeLayout`,
  `wk5ChromeLayout()` (pure: same input → same boxes for paint AND hit),
  `wk5ChromeHitTest()`, `wk5ChromePaint()` (CLAMPED to the content rect —
  confinement rule, docs/browser/render-pipeline.md), icon ids +
  `wk5DrawIcon()`, palette.  FREESTANDING / host-compilable.
- `HobbyOS/WebKit/WebProcess/hobbyos/WK5Bookmarks.h` — bookmark store
  (cap 24), find/toggle/remove, title derivation (`bkTitleFromUrl`, reused
  for tab-strip titles), persistence `bkSave`/`bkLoad` ("url<TAB>title" lines,
  guest path `/BOOKMARKS.TXT`, best-effort).  FREESTANDING / host-compilable.

Layout (window-content-relative; boxes `[x0,x1) x [y0,y1)`):

| Band | rows | content |
|---|---|---|
| Tab strip | [0,26) | tab chips (title + close box [x1-18,x1-2) x [5,21)), "+" new-tab box |
| Toolbar | [26,60) | back [6,32), fwd [34,60), reload [62,88), omnibox [94, cw-38) x [30,56), star [cw-36, cw-10) x [30,56) |
| Bookmarks bar | [60,84) | chips for first ≤8 bookmarks, [6+cw..] y [63,81) |
| Page | [84, ch) | (was [36, ch)) — blits/math follow the macro |

`WK5_CHROME_H` becomes `WK5UI::CHROME_H` (= 84); every existing page-geometry
expression keeps working through the single macro.  Markers that print
chrome/painted-rect numbers REFLECT 84 from now on — runners must derive from
the constants, never hardcode 36.

Behavior contract (phase 1):
- Back/Forward/Reload buttons → existing `back()/forward()/reload()`; disabled
  state = `canBack/canForward` (dim glyphs; click on disabled emits ui-click
  but performs no navigation).
- Star → `bkToggle` current page (§filled gold when bookmarked); best-effort
  save + `[WIN] bookmarks save rc=…` marker.
- Bookmarks bar chips → `navigateByName(chip.url)`; `[WIN] bookmark nav`.
- Bookmarks loaded once at startup (`[WIN] bookmarks load n=…`).
- Tab strip renders `tabCount` chips (phase 1: exactly 1, active); "+"/"×"/
  tab clicks emit `[WIN] ui-click …` and are otherwise inert until phase 2.
- Omnibox: click focuses (existing addr focus behavior/markers), typing/Enter/
  ESC unchanged from WA; caret + text from the snapshot (addrActive/urlBuf).
- GO button is REMOVED (Chrome layout; Enter commits).

## Markers delta (runners!)

- KEPT byte-compatible: `[WIN] load-ok`, `frame`, `painted rect`, `frame-times`,
  `geom`, `close ok`, `exit rc`, `reblit cached`, `viewport render=`,
  `nav name=`, `back target=`, `forward target=`, `reload target=`,
  `url-entry=`, `url-prompt`, `addr click x=%d y=%d focus`,
  `click x=.. y=.. px=.. py=..`, `link -> `.
- REMOVED: `[WIN] go click x=.. y=..`, `[WIN] addr-focus via go` (GO button gone).
- ADDED: `[WIN] ui-click kind=<back|forward|reload|star|omni|tab|closetab|newtab|bm> [i=%d]`;
  `[WIN] bookmark add url=… title=…` / `del url=…` / `nav idx=%d url=…`;
  `[WIN] bookmarks load n=%d skipped=%d`; `[WIN] bookmarks save rc=%d n=%d`.
  Phase 2 (land with tabs): `[WIN] tab new idx=%d`, `[WIN] tab switch idx=%d`,
  `[WIN] tab close idx=%d`.

## Lanes (one writer per file; wave 1)

| Lane | Tree / branch | Owns (exclusive) | Deliverable |
|---|---|---|---|
| **ui-chrome** | fork worktree `~/webkit-lanes/ui-chrome`, br `browser/ui-chrome` | `WK5Chrome.cpp` (new), `WK5Font8x8.h` (+icon table at end), `HobbyOS/tools/ui-host/**`, `HobbyOS/continuation/ui-chrome/**` | painter + hit test + icons; host harness renders 6 PNG states incl. confinement proof |
| **ui-bm** | fork worktree `~/webkit-lanes/ui-bm`, br `browser/ui-bm` | `WK5Bookmarks.cpp` (new), `HobbyOS/tools/ui-host/bookmarks_test.cpp` + run script, `HobbyOS/continuation/ui-bm/**` | bookmark store + persistence + host tests |
| **ui-run** | OS worktree `~/hobbyos-lanes/ui-run`, br `browser/ui-run` | `tools/ui/**` + evidence | x64-KVM harness (boot→launch→URL→screenshot→markers→report) + frozen-geometry click helper + baseline run on the pinned pre-change binary |
| **controller** | fork `~/webkit-hobbyos`, br `browser/ui` (integration) | `WK5WindowDriver.cpp`, `PlatformHobbyOS.cmake`, temp stubs, merges, final receipts | working end-to-end build; acceptance receipts |

Integration flow: lanes commit on their branches → controller merges
(file-level `git checkout <branch> -- <paths>` for lane-owned files), rebuilds
`WebKitBuild/HobbyOS-intel`, re-runs the ui-run harness.  Lane branches never
push; controller records.

## Acceptance receipts (wave 1; x64-KVM; screenshots vision-verified)

1. Boot → launch → HOME loaded: chrome v2 visible (strip + toolbar + bookmarks
   bar) at content rows 0–84; page below; `[WIN] load-ok` present.
2. Click star → `[WIN] bookmark add`; star gold; bar gains a chip (screenshot).
3. Click reload → `[WIN] reload target=HOME.HTM`.
4. Type `TALL.HTM` (F2) → loads; click star → 2nd bookmark; click back →
   `[WIN] back target=HOME.HTM`; click forward → `[WIN] forward target=TALL.HTM`.
5. Click bookmark chip 0 → `[WIN] bookmark nav` → HOME re-loads.
6. Click on omnibox → addr focus; typing updates field text (existing markers).
7. Confinement: no chrome pixels outside the content rect (screenshot crop).
8. Regression: fixture acceptance leg still green; `close ok`/`exit rc=0`.
9. ARM: build `WebKitBuild/HobbyOS-arm-wk5` + one fixture run screenshot.

## Phase 2 — tabs (design intent; freezes when wave 1 lands)

Per-tab state in the driver (`current`, history stack + index, title).  Engine
options, in preference order:
- **T1 page pool**: one `WebPage` per tab (`WebPage::create` with fresh
  pageID/group ids) — bounded SPIKE first; per-tab cached frame for instant
  switch, repaint on activation.  Unknown port assumptions (singletons) —
  verify before committing.
- **T2 fallback**: single `WebPage`; switch = `loadForHistory(tab.current)`
  (re-load on switch; scroll/JS state lost — documented cost).
UI (`+`, `×`, chip click → switch) identical under both.  Receipts: new tab,
switch (content changes), close, per-tab history (back after switch returns
the TAB'S previous URL, not the global history).

## Run economics (all lanes)

- One QEMU per disk image; never two QEMUs on one disk.  Kill only by saved
  PID / owner-cwd-scoped shims — never bare `pkill -f`.
- Host-only lanes (ui-chrome, ui-bm) do NOT build WebKit and do NOT boot QEMU
  — their loop is seconds (host g++).
- QEMU lanes: fixture pages only in-loop; bounded waits; detached QEMU + PID
  file + short polls; checkpoint-commit every ~20 min.

## Status — COMPLETE (2026-10-11)

Wave 1 (chrome/bookmarks) + wave 2 (tabs) landed on fork branch `browser/ui`;
evidence `continuation/ui-ctl/{,wave2/}`; host tests 107/107 via
`HobbyOS/tests/run_wk5ui_host_test.sh`.  Phase 2 shipped as **T1 (page
pool)**: one `WebPage` per tab (`wk5CreatePage()`), per-tab history +
stashed frames; closed pages park in `m_tabGraveyard` — never destructed
(WebPage teardown untested on this port; bounded by the 8-tab cap).  Tab v1
semantics: only the active tab loads (inactive tabs keep their DOM);
switching adopts the cached frame (`tab switch idx=.. url=..`), then the
adopted tab stays Stable until interaction.  Acceptance receipts in
`continuation/ui-ctl/wave2/README.md`.  Known cosmetic residuals: chip close
box and nav glyphs are text glyphs (`x`, `<`, `>`, `R`); the 8×8 icon
bitmaps exist in `WK5Font8x8.h` (host-tested) and are not yet wired into the
paint.  ARM leg not re-run this wave (x64-KVM acceptance path per plan).

## Palette re-theme (2026-10-14)

The `WK5Chrome.h` palette block was converted from the original dark set to
the **Chrome-light** set — same constant names, geometry, hit boxes and
marker contract; values mirror the OS palette
`src/user_include/graphics/theme.h` (exact Google hexes: frame/tab-strip
`#DEE1E6`, toolbar/active-tab/bookmark bar `#FFFFFF`, omnibox `#F1F3F4` with
a `#DADCE0` border, accent `#1A73E8`; two constants removed/changed by
design: no green active-tab top accent, blue bookmarked star).  Fork branch
`browser/ui-theme` @ `2952eed95d` (off `browser/ui`); host tests still
107/107; the x64-KVM chrome-band sampler passes on both receipts
(browser-only and combined-look with the light OS chrome).  Evidence:
`continuation/ui-theme/` (REPORT.md, PNGs, `check_chrome_band.py`).  The
geometry/viewport/marker contract above is unchanged.
