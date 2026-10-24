# WK7-uxpagetypes — page-type scroll + live document titles

Patch: `WK7-uxpagetypes.patch` (single file: `HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp`).
Validated read-only with `git apply --check` from `~/Documents/GitHub/webkit-hobbyos` → **OK**.
No `.h`/ABI changes: `WK5Chrome.h`/`WK5Bookmarks.h` untouched; `WK5ChromeState` stays trivially copyable (fixed char arrays).
The new state lives in `WK5TabState` (already uses `std::string`/`RefPtr` — internal driver state, not the chrome snapshot).

---

## (A) PageDown/PageUp scroll a pageful, not 40 px

### Root cause
`Wk5State::scrollPage(int dir)` (near line 1717) drives BOTH the wheel path and the
`WK5_KEY_PGUP`/`WK5_KEY_PGDN` key path through the same fixed `WK5_SCROLL_STEP = 40` (line 152).
Wheel ticks correctly scroll 40 px per notch, but the keyboard "page" keys inherit the same
40 px — one wheel notch, not a pageful. `snprintf(buf, ... "window.scrollBy(0, %d)"...)` and
`view->scrollBy({0, dir * WK5_SCROLL_STEP})` both use the constant. (The `buf` is dead code in
the original; kept as-is.)

### Evidence (`/Users/sarah/.hermes/cache/scratch/wklane-agentc-s3/serial.log`)

```
79514 [WIN] wheel btn=5 dir=down
79522 [WIN] scroll pos=40            # wheel tick 1 -> +40
79538 [WIN] scroll pos=80            # tick 2 -> +40
79563 [WIN] scroll pos=120           # tick 3 -> +40
...
[WIN] key=pgdn action=scroll-down
[WIN] scrollPage dir=1
[WIN] scroll pos=160                # PageDown from 120 -> only +40 (one wheel notch)
...
79794 [WIN] scroll pos=120           # wheel-up tick 1 -> -40
79839 [WIN] scroll pos=80            # wheel-up tick 2 -> -40
79854 [WIN] scroll pos=40            # wheel-up tick 3 -> -40
```

### Fix
- `scrollPage` signature becomes `void scrollPage(int dir, bool pageStep = false)` (declaration gets the default; the wheel call site `scrollPage(dir)` is unchanged).
- When `pageStep` is true, the step is `max(1, (int)(0.9 * view->visibleHeight()))` — `visibleHeight()` is the `ScrollableArea` viewport height (the same source layout/scroll uses), so ~90% of the visible page. `ScrollView::scrollBy` already clamps to the document's scrollable range.
- Wheel (`scrollPage(dir)`) keeps exactly `WK5_SCROLL_STEP = 40`.
- Receipt: the existing `[WIN] scrollPage dir=%d` line is emitted unchanged (byte-identical, so the drive's prefix match is unaffected); a new `[WIN] scrollPage key-step=%d` receipt prints the actual keyboard step.
- `processKey` PGUP/PGDN call `scrollPage(±1, true)`.

### Verification
On a long page (e.g. `file:///TALL.HTM` or a tall article), with `WK5_VIEW_H` ~1018 the step is ~916 px:
```text
[WIN] key=pgdn action=scroll-down
[WIN] scrollPage dir=1
[WIN] scrollPage key-step=<~916>          # NEW receipt
[WIN] scroll pos=<~916>                    # moved >400 px, not 40
[WIN] scroll pos=<cur> after a second pgdn # continues scrolling "by the page"
```
And a single wheel tick still lands at `+40` (regression guard):
```text
[WIN] wheel btn=5 dir=down
[WIN] scroll pos=<prev+40>
```

---

## (B) Tabs and bookmarks show the live document title

### Root cause
Title derivation is entirely URL-based:
- `buildChromeState()` (line ~1113) fills `st.tabs[i].title` from `bkTitleFromUrl(m_tabs[i].current)` for every tab — never from the document.
- `toggleBookmark()` (line ~1213) builds the star's title from `bkTitleFromUrl(m_current)`, so a bookmark is recorded with the URL-derived label even though the loaded document has a real `<title>`.
The driver already *has* the live title (the load-time census reads it via JS and prints `[WIN] doc title=...`) but never persists it to the chrome/bookmark paths.

### Evidence (`/Users/sarah/.hermes/cache/scratch/wklane-agente-s5/serial.log`)
```
79188 [WIN] load-ok url=https://en.wikipedia.org/wiki/Cat ms=20244
79197 [WIN] doc title=Cat - Wikipedia|1731538|12|3691|59     # document title IS live
79407 [WIN] bookmark add url=https://en.wikipedia.org/wiki/Cat title=en.wikipedia.org/wiki   # BUG: URL-derived label
```

### Fix
- New per-tab field `std::string docTitle;` in `WK5TabState` (cleared to `""` — "not loaded").
- At the load-ok completion latch (`if (localFrame)` right after `[WIN] load-ok`), capture `localFrame->document()->title()` — a pure C++ read, no JS eval — into `activeTabState().docTitle` (only when non-empty), with a new `[WIN] doc-title store=%s` receipt. Clear-on-navigate: `navigateByName()` and `loadForHistory()` (the shared dispatch for back/forward/reload too) clear `docTitle` so a stale title never leaks into the next load; the tab falls back to the URL-derived label while loading.
- `buildChromeState()`: tab title = `docTitle` if non-empty, else `bkTitleFromUrl`, else `"New Tab"`.
- `toggleBookmark()`: passed title = active tab's `docTitle` if non-empty, else `bkTitleFromUrl`. `bkToggle` still falls back to `bkTitleFromUrl` for a null/empty title, so the fallback contract is unchanged.
- Title survives tab switches because it lives in `WK5TabState`, not in the stash frame — `stashActiveFrame()`/`restoreActiveFrame()` still move only the frame + checksum, and `switchTab`/`closeTab` both end in `setTitlePrompt()` → `buildChromeState()` which repaints the stored title.

### Verification
```text
[WIN] load-ok url=https://en.wikipedia.org/wiki/Cat ms=...
[WIN] doc-title store=Cat - Wikipedia                       # NEW receipt
[WIN] bookmark add url=https://en.wikipedia.org/wiki/Cat title=Cat - Wikipedia   # was en.wikipedia.org/wiki
```
Re-open the bookmark / view the tab chip and the label reads **"Cat - Wikipedia"**. Regression guards:
- Unloaded/fresh tab (no `docTitle`) still shows the URL-derived label; `New Tab` for `<HOME>`.
- Tab-chip width is still `clamp(8*len(title)+44, 88, 240)` (`WK5Chrome.cpp` `wk5ChromeLayout`, untouched) — only the title *string* differs, so geometry semantics are unchanged.

---

## Regressions to avoid
- **No ABI / title-field size change**: `WK5ChromeState` / `TabChip` / `WK5Bookmarks.h` untouched; titles still land in fixed `TAB_TITLE_MAX`/`BM_TITLE_MAX` buffers via `snprintf` (truncation, never overflow). `docTitle` is `std::string` in internal `WK5TabState` only.
- **Existing receipts unchanged**: `[WIN] scrollPage dir=%d` and the `[WIN] doc title=...` JS census line are byte-identical (drive parsers keep working); only additive `key-step=` / `doc-title store=` receipts were added.
- **Wheel semantics frozen**: wheel ticks remain exactly 40 px (`WK5_SCROLL_STEP`); only the keyboard page keys take the viewport-relative step.
- **No layout/geometry change**: all `WK5Chrome.h` constexprs and `wk5ChromeLayout` formula untouched; the render/viewport pipeline (`applyViewport`, `paintAndBlit`) is not touched by this patch.
- No JS eval added: the title capture is a direct `Document::title()` read, consistent with the port's "no JS for critical paths" policy.
