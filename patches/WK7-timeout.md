# WK7-timeout — stale load-timeout watchdog fires on unrelated later loads

**Driver:** `HobbyOS/WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp`
**Patch (this dir):** `WK7-timeout.patch` — 6 insertions, 0 deletions, no other lines touched.
**Validated read-only:** `git apply --check` passes against the driver's working tree in
`webkit-hobbyos` (HEAD `61b0995f6c`, which already carries the unrelated fs-r16 census edits).
**Scope:** watchdog budget reset only; the watchdog itself, the 1800 s wall budget, the 30 s
stall gate-probe, and the one-shot log flags are all unchanged.

---

## Root cause

`m_start` is a **single-stamped boot clock**, never re-stamped on navigation:

| file:line | kind | role |
|---|---|---|
| `WK5WindowDriver.cpp:853` | **write** | `, m_start(MonotonicTime::now())` — WK5State ctor initializer. The ONLY set site. |
| `WK5WindowDriver.cpp:934` | decl | `MonotonicTime m_start;` |
| `WK5WindowDriver.cpp:3442` | read | `[WIN] load-ok ... ms=%lld` = `(now - m_start).milliseconds()` |
| `WK5WindowDriver.cpp:3541` | read | **watchdog**: `auto loadElapsed = (MonotonicTime::now() - m_start).seconds();` |
| `WK5WindowDriver.cpp:3542-3544` | read | `if (loadElapsed > 1800.0 && !m_loadTimeoutLogged) { [WIN] FAIL load-timeout ...` |

`grep -n m_start` returns exactly those four lines: the member is written **once** (at
construction / tab-0 boot) and thereafter only read. Every navigation entry point resets
`m_loadTicks`/`m_stallTicks` but **never** re-stamps `m_start`, so the 1800 s watchdog counter
keeps counting from boot. On a lane that has been up longer than 1800 s, the *next* healthy
navigation inherits a budget that was stamped at a *previous* load; once cumulative wall time
crosses 1800 s the watchdog emits a spurious `[WIN] FAIL load-timeout` and the load-of-record
is stubbed — a dead budget from an earlier navigation declaring an unrelated later load dead.
The same stale clock also inflates every later `load-ok ms` receipt (see evidence).

## Exact set/reset sites — all entry points, before the fix

The complete set of `m_phase = Phase::Load` **write** sites is the complete set of load-start
entry points; **none of them touched `m_start`** before this patch:

| entry | how it starts a load | `m_phase = Phase::Load` at | fixed by patch at |
|---|---|---|---|
| new tab (`newTab`) | then `navigateByName("HOME.HTM")` (:1308) | `:1302` | `:1306` |
| GO commit | `handleChromeHit` GO (:1174) → `commitAddress` (:3266) → `navigateByName` | `:2474` | `:2479` |
| address-field commit | `:1906` → `commitAddress` → `navigateByName` | `:2474` | `:2479` |
| bookmark nav | `handleChromeHit` chip (:1183) → `navigateByName` | `:2474` | `:2479` |
| engine-nav / link click | `engineLoop` dispatch (:3717) → `navigateByName` | `:2474` | `:2479` |
| open-first-result / query result | `:2784` / `:2837` → `navigateByName` | `:2474` | `:2479` |
| URL dispatch (shared) | `loadForHistory` (called by navigateByName-URL, reload, back, forward) | `:2513` | `:2519` |
| reload | `reload` :2914 → `loadForHistory` | `:2915` | `:2922` |
| back | `back` :2930 → `loadForHistory` | `:2931` | `:2939` |
| forward | `forward` :2949 → `loadForHistory` | `:2950` | `:2959` |

(Line numbers are the current working-tree numbers — post-fs-r16. Patch hunks use those same
numbers: 1306, 2479, 2519, 2922, 2939, 2959.)

## Evidence — `wklane-agentb-s2/serial.log`

`/Users/sarah/.hermes/cache/scratch/wklane-agentb-s2/serial.log` is an 18 s happy-path lane
that performs **5 independent navigations** on one tab. Its `load-ok ms` receipts are all
measured against the boot stamp and climb monotonically — they would be small and resetting
per navigation if `m_start` were re-stamped:

```
274:[WIN] script-enable=1
275:[WIN] page id=8 viewport=1916x962 seq=1          <- tab-0 boot; m_start stamped HERE (ctor)
277:[WIN] nav name=HOME.HTM
404:[WIN] load-ok url=HOME.HTM ms=308                <- nav #0: 308 ms since BOOT
1167:[WIN] nav name=https://news.ycombinator.com     <- nav #1 (GO/address commit)
5417:[WIN] load-ok url=https://news.ycombinator.com ms=6946      <- still elapsed since BOOT
5546:[WIN] nav name=https://news.ycombinator.com/    <- nav #2 (engine-nav story link click)
9732:[WIN] load-ok url=https://news.ycombinator.com/ ms=11054    <- elapsed since BOOT
9823:[WIN] back target=https://news.ycombinator.com  <- nav #3 (back)
13954:[WIN] load-ok url=https://news.ycombinator.com ms=13150    <- elapsed since BOOT
14032:[WIN] forward target=https://news.ycombinator.com/  <- nav #4 (forward)
18191:[WIN] load-ok url=https://news.ycombinator.com/ ms=14612   <- elapsed since BOOT
```

`308 → 6946 → 11054 → 13150 → 14612` is elapsed-**since-boot**, not per-navigation: five
separate loads (fixture HOME.HTM, two HN pages, a back, a forward) report ever-growing
milliseconds although each was seconds long. This is direct serial proof that `m_start` is
never re-stamped after construction.

About the FAIL itself: no literal `[WIN] FAIL load-timeout` / `[WIN] load-cancel` line appears
in this serial — the lane only lived 18 s, far below the 1800 s watchdog (and 30 s gate-probe)
windows, and the sweep's S2 scenario passed 5/5. The spurious FAIL was observed on long-lived
lanes; this short serial carries the *mechanism* evidence (monotonic-from-boot budget across
later navigations), which is precisely the input the watchdog consumes at
`WK5WindowDriver.cpp:3541`. A lane that stays up > 1800 s and then navigates once more fires
`FAIL load-timeout` against the stale boot stamp even though the new load is healthy.

## Fix — per-navigation timeout budget reset at EVERY navigation entry point

Add one line at each of the 6 load-start sites, alongside the existing per-load counter
resets (`m_loadTicks = 0; m_stallTicks = 0;`):

```cpp
m_start = MonotonicTime::now();
```

That makes the invariant: **every place that arms a new Load phase also re-arms the watchdog
budget.** `navigateByName` (stamps GO, address, bookmark, engine-nav/link-click, new-tab-home,
startup) and `loadForHistory` (stamps URL dispatch plus reload/back/forward) alone would cover
all paths; the stamp is additionally placed at reload/back/forward/newTab themselves so each
entry point is self-contained and auditable (the reload/back/forward double-stamp is
microseconds apart and harmless).

Deliberately unchanged (per HARD RULE — minimal, touch nothing else):
- watchdog math (`1800.0` wall budget, `m_loadTimeoutLogged` one-shot) — now per-load by construction;
- 30 s `gate-probe` (`loadElapsed > 30.0 && !m_stallGateLogged`) — now correctly 30 s after the load STARTS;
- `load-ok ms` receipt (:3442) — now a truthful per-navigation duration (side benefit);
- the conc/emit buffers, census JS, fs-r16 edits — untouched.

## Verification

1. **Apply check** — `git apply --check` (in `webkit-hobbyos`) on `WK7-timeout.patch`: passes
   read-only, working tree unchanged. Done.
2. **Long-lived lane** (the real acceptance test): boot once, then drive several pages over
   the tab's lifetime — GO/address commit, an engine-nav link click, back, forward, a bookmark
   nav, plus a second tab — and let the lane run **past 1800 s total wall time**. Assert:
   - no `[WIN] FAIL load-timeout` and no `[WIN] load-cancel` fires after any later navigation that itself reaches `load-ok`;
   - every `[WIN] load-ok ... ms=N` after the first reports a small per-navigation N (resets per nav), not a boot-accumulated value.
3. **30 s test-mode guarantees preserved**: the stall `gate-probe` still fires 30 s into a
   load that is actually stalled (now anchored to its own start), and the 1800 s watchdog still
   fires for a genuinely stuck first load; both remain one-shot logs.
4. Sanity: first page's `load-ok ms` stays boot-anchored (tab-0 stamp is still the ctor), so
   existing S1/S2 assertions on the first-load receipt are unaffected.
