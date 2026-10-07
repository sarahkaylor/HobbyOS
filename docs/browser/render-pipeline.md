# Render-pipeline workstream — window-confined drawing + wall-clock rendering performance

**Opened 2026-10-07 (controller).** Two user-reported problems on the merged browser tip:

1. **The browser does not respect the WM's layout decisions** — it can (and does) paint
   outside its designated window area. Wanted: a user-mode *render sandbox* — a layer
   every app's pixels go through so drawing is confined to the window's content rect by
   construction ("not literal sandboxing … preferably only in user-mode").
2. **Rendering is slow to the wall clock** — seconds to minutes to render a site under
   TCG. Wanted: strong wall-clock improvement, incl. **multiple UI threads** and using
   the OS's **lightweight threads** so UI aspects are not blocked on each other.

## Ground truth (from the 2026-10-07 recon; verify before editing)

**Bleed surface (why apps can draw outside their window).** Pixel-mode apps own a raw
slice of the shared framebuffer and draw with no confinement:
- `WK5WindowDriver.cpp` (fork) writes the mapped fb directly — `wk5FillRect` /
  `wk5DrawText` clamp only to *screen* bounds; `blitCachedFrame()` writes rows at
  `fb32 + (pageY+yy)*1920 + m_gx` with **no right-edge clamp** (stride bleed if
  geometry is stale); `paintChrome` likewise.
- The WM (OS `src/user/desktop.c` + `graphics/window.c`) *carves* content rects out of
  its own passes and sends repair requests — it trusts the app. Nothing clamps app writes.
- The X11 lib (`src/user/x11/`) already draws into a **client-side shadow** with
  clip masks, flushed only at the window's content rect — the browser driver is the
  outlier, not the norm.

**Perf cost map (per `paintAndBlit()` in the fork driver).** One "repaint" today does:
`updateLayoutAndStyleIfNeededRecursive` → `m_page->setSize` (only when viewport changes;
**minutes-long** on the 5.7 MB page under TCG — the R15 stall) → ImageBuffer create →
`paintContents` (Skia software raster, 1916×982 ≈ 1.88 Mpx) → `copyNativeImage` +
`readPixels` (full-frame copies) → **checksum pass** (1.88 M fnv) → **color census pass**
(`[IMG1p]`, runs on *every* paint, evidence-only) → `m_frame = pixels` (copy) →
`blitCachedFrame()` (nearest-neighbour convert loop + chrome + **full-screen `flush_fb()`**
— a 1920×1080 present even when damage is one row). The driver loop is **single-threaded**
(`tickLoop()` = `tick(); sched_yield();`): during fetch/render/setSize the pump starves —
that is the documented input-starvation / close-under-load limitation family.

**What already exists (do not re-build):**
- Kernel **user threads + futex-lite + pthreads** (P1, `docs/browser/p1-threads-design.md`;
  proven by `THRD_T`): PCB-slot threads sharing the leader AS, preemptive RR (10 ms),
  `SYS_THREAD_CREATE/FUTEX/THREAD_EXIT/SET_TLS` (72–75), full pthread surface in libc.
- **WTF POSIX threading wired for HobbyOS** (`Source/WTF/wtf/PlatformHobbyOS.cmake` uses
  `posix/ThreadingPOSIX.cpp` over the in-libc pthreads).
- **`flush_fb_rects()` / `SYS_FLUSH_FB_RECTS` (89)** — damage-rect present (GX), also
  `flush_fb_rect()` single-rect helper (OS `src/user/libc.c:564`).
- WK5 perf markers already in serial: `[WIN] frame-times render=… blit=… total=…`,
  `[WIN] viewport apply begin/end ms=`, `[WIN] reblit cached …`, `[WIN] painted rect=…`.
- R5 TCG knob `WK5_RENDER_SCALE` (+ `::/RENDER-SCALE` marker) renders at scale and
  upscales at blit; 1.0 = quality default.

## Design 1 — `wsurf`: the window-surface layer (frozen v1 interface)

A small user-mode library (OS repo) that every app pixel goes through. Drawing is in
**window-local coordinates**; every primitive clamps to `[0,cw)×[0,ch)` and can never
write a pixel outside the content rect. Damage is tracked (screen-coords union) for
rect-based presents. The content rect comes from the WM's `ESC ] G` message (same source
the driver already reads).

```c
/* src/user_include/graphics/wsurf.h — frozen v1 (rp-w implements; rp-f vendors) */
struct wsurf {
  uint32_t *fb;       /* mapped framebuffer base */
  int fb_w, fb_h;     /* screen size (stride == fb_w) */
  int cx, cy, cw, ch; /* content rect in SCREEN coords — the whole sandbox */
};
void wsurf_init(struct wsurf *s, uint32_t *fb, int fb_w, int fb_h);
int  wsurf_set_rect(struct wsurf *s, int x, int y, int w, int h); /* clamp; 1 if changed */
/* All draws: window-local (0,0)=content top-left; clamped; never touch outside. */
void wsurf_fill(struct wsurf *s, int x, int y, int w, int h, uint32_t color);
void wsurf_frame(struct wsurf *s, int x, int y, int w, int h, uint32_t color);
void wsurf_put(struct wsurf *s, int x, int y, uint32_t color);
uint32_t wsurf_get(struct wsurf *s, int x, int y);
void wsurf_blit(struct wsurf *s, int x, int y, int w, int h,
                const uint32_t *src, int src_stride);
void wsurf_blit_scaled(struct wsurf *s, int x, int y, int w, int h,
                       const uint32_t *src, int sw, int sh);
int  wsurf_text8x8(struct wsurf *s, int x, int y, const char *text, int max_x,
                   uint32_t fg, uint32_t bg);  /* returns pen-x (local) */
/* Damage (screen coords) union of everything written since last take. */
int  wsurf_take_damage(struct wsurf *s, int out[4]);  /* 1 if nonempty, clears */
```

Enforcement envelope (documented honestly): this is **platform-layer confinement**
(user-mode, per the user's preference) — official code paths cannot draw outside.
Kernel/MMU-enforced mapping isolation is a documented future option, explicitly out of
scope here. The WM's existing repair/expose machinery remains the recovery net.

Adoption: the browser driver (rp-f) replaces every raw fb write with `wsurf_*` calls
bound to its current `]G` content rect. X11 stays as-is (already shadow-confined).

## Design 2 — wall-clock rendering + UI threads

Perf levers, cheapest-first (each one measured against the same fixtures):
1. **Rect presents**: replace the driver's full-screen `flush_fb()` with
   `flush_fb_rects()` over (wsurf damage ∪ chrome ∪ repair) — interface exists.
2. **Census gating**: `[IMG1p]`/`[IMG1h]` become marker-gated (`::/WK5-CENSUS`, staged by
   gate runners) instead of running on every paint; gate runs keep byte-identical receipts.
3. **Skip redundant commit**: when a settle repaint's frame is checksum-identical and no
   WM repair is pending, skip blit+flush (keep the render — it is what formats the checksum).
4. **UI-thread split**: a dedicated pthread owns stdin + WM protocol + cached-frame
   reblits + close; a second ("engine") thread owns all WebKit/WebCore work (load/render/
   setSize) behind a small mutex+condvar queue. Goal receipts: close-within-grace under
   heavy load, input ACKed during render/setSize, re-tile catch-up during a load — the
   existing limitation family, eliminated.
5. **Parallel blit**: row-split the convert/scale loop across N pthreads (measure N=2/4).
6. (Later/optional) deeper: fused checksum+convert pass, readback-copy reduction,
   `updateLayoutAndStyleIfNeededRecursive` audit.

Constraints: all serial markers the runners parse stay byte-compatible
(`load-ok`, `frame`, `painted rect`, `frame-times`, `geom`, `close ok`, `exit rc`,
`viewport apply begin/end`, `reblit cached`), and the acceptance quartet stays green at
every checkpoint.

## Lanes (parallel; one writer per file)

| Lane | Repo/tree | Owns | Deliverable |
|---|---|---|---|
| **rp-w** | OS `~/hobbyos-lanes/rp-w` (branch `browser/rp-w`) | new `wsurf.{h,c}` + host/device tests + Makefile wiring + doc touch-ups | tested wsurf layer, frozen semantics |
| **rp-h** | OS `~/hobbyos-lanes/rp-h` (branch `browser/rp-h`) | `tools/render_pipeline/**`, docs baselines | repro of issue 1 + perf/responsiveness harness + baselines |
| **rp-f** | fork `~/webkit-hobbyos` (branch `browser/rp-f`, off `l8-wk5` @ `cdabe011af`) | `WK5WindowDriver.cpp` + vendored wsurf copy | driver: clip adoption → perf wins → UI-thread split → parallel blit; receipts |
| controller | OS main (docs/merges) + integration | `docs/browser/render-pipeline.md`, browser.md §12, gates | merged + verified end state |

Future lanes (as needed): rp-f2 continuation (new worktree), rp-p engine-side perf
experiments, rp-g final gate battery.

## Gates

- Per lane: its own gate (named in each brief) — committed, evidence-linked.
- **Integration gate (controller)**: fixture acceptance quartet green on the merged tip;
  tile-bleed probe shows zero out-of-rect pixels; perf table shows the measured deltas
  (before/after, same fixtures, same box); responsive-close probe green under load;
  regressions: `make host_tests`, unit-arm, the standard browser acceptance modes.

## Run economics (all lanes)

- One QEMU per disk image; assemble per-run disks (wk5 pattern); never two QEMUs on one
  disk; drive via QMP; fixture pages only in-loop (HOME fast, TALL ~7–9 s load);
  live-site runs are for the final gate only.
- Serial-marker economics: bounded steps, `--go-timeout` flags, detached QEMU + PID file
  + short polls; kill only by saved PID or owner-cwd-scoped shims (never bare `pkill -f`).
- `~/webkit-hobbyos-wk2/**` cross-prefixes are PROTECTED: never rm/clean/rebuild ad hoc.
- The OS repo's `rm` is approval-gated → `touch src/kernel/main.c` forces relink+fresh disk.
- `/tmp` is inode-capped (~1M): clean per-run scratch; keep big trees on the workstation disk.
