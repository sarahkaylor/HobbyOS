# L82 — the present path's own cost: the sampled checksum, the single flush, the resync cadence

Lane `l82-present`, worktree `~/hobbyos-rw-lanes/l82-present` (branch
`rw-l82-present`, seeded at the merged tip `d374e6ca`).

The question this lane was given: **66-72 ms of a 96-100 ms frame is the
present path** (L80 §3). Cut what is *ours* in it — the per-frame surface
checksum, the double flush, the `dmg_resync` — then re-measure with the same
instruments and report the menu's fps over 120 s at 1916x1018 and 1280x720.

Two of the three premises did not survive measurement, and the third one did:

| premise | verdict |
|---|---|
| the per-frame checksum costs 6-7 ms/frame | **confirmed, and fixed.** 7.3 ms/frame of `self_avg`, i.e. a two-pass 7.8 MB scan every 10th present. Now one row in 8 is hashed in one pass: **~0.9 ms/frame**. Same-tree A/B (one constant): `self_avg` 2 062 → 1 250 us in the same batch |
| there is a double flush (`st_context_flush` then virgl's `fence_finish`) | **already gone** in the seeded tip (L80's `supports_fences = 0` → NULL fence → no `fence_finish`). Measured: one winsys present per swap (`[VGLB] n` == `[XSW] n`) and exactly one submit inside `flush_frontbuffer` (the 88-byte damage blit). Nothing to remove |
| the 8.3 MB `dmg_resync` every 64th present is waste | **it is noise**: 8.3 MB/64 = 130 KiB/present, ~0.03 ms at the winsys' own 0.25 ms/MiB. Widened to 256 anyway (a latency knob, not a perf one) |
| the winsys present has avoidable work | **no — and that is the finding.** `[VGLB] thumb_avg` *is* `pres_avg` (8.8 of 9.4 ms at the menu) with `idle=99 %`: the readback of the 64x64 thumbnail is ordered after the client's frame on the same context, so it waits for the **host to finish rendering Unity's frame**. It is Unity's render time seen from our side |

The menu, 120 s window, same phase in all three runs (guest t=90-210 s; see §4
for why this window and what the neighbouring one says):

| | 1916x1018 before | 1916x1018 after | 1280x720 after |
|---|---:|---:|---:|
| fps | 9.97 | **10.96** | **11.05** |
| ms/frame | 100.3 | **91.2** | **90.5** |
| `XcPresentMark` (`self_avg`) | 7.84 ms | **5.41 ms** | 7.41 ms |
| of which the checksum (in-run A/B) | ~7.3 | **~0.9** | ~0.9 |
| our present path (`self` + `swap`) | 43.1 ms | **12.9 ms** | 14.9 ms |
| winsys present (`[VGLB] pres`, mostly waiting on the host) | 11.0 | 9.4 | 7.2 |
| engine between swaps (`gap`) | 33.1 | 44.3 | 53.3 |

After the fix the *frame* is no longer mostly the present path: at the menu it
is `gap` 44 ms (Unity's own CPU) + ~9 ms of host render inside our probe +
~13 ms of shim/submit. **Unity's own ~30-50 ms is dominant — task 3's stop
condition — so this lane stops at the present path.**

**No run in this lane reached the menu with its UI drawn.** The game reaches the
menu *art* at ~present 530 and holds it, but the menu's widget area is a black
box (L80 §5's texture problem, now with 0 resource refusals), and after
~600 s the frame goes fully black and decays to 2-6 fps. That wall is not the
present path; §5 records it with its evidence instead of smoothing it over.

---

## 1. What changed, and why

| change | file | why |
|---|---|---|
| the surface checksum is **sampled** (one row in `XC_PRES_CRC_ROWS`, default 8) and computed in **one pass** — the visibility scan and the FNV share a loop, and x/y are carried instead of a per-pixel `%`/`/` | `src/user/xcompat/libx11.c` | it was two passes over 7.8 MB with a byte-at-a-time FNV and two 64-bit divisions per pixel, on every row (= every 10th present). `self_avg` carried 6-7 ms/frame of it |
| `XC_PRES_CRC_ROWS=0` compiles the checksum out, `=1` restores the old full scan | same | the probe is diagnostic instrumentation in the hot path; it must be removable without touching the code around it. `=1` is also the A/B control used below |
| the fb0 blit's coverage is **reported**: `blit_px` (pixels blitted per present), `blit_n` (presents that blitted at all), `blit=<w>x<h>+<x>+<y>` | same | task 1(d) — "the blit only covers the damaged rect" is a claim that needs a counter |
| `PRES_ROW_EVERY` is now overridable (`-DPRES_ROW_EVERY=64`) and its measured cost is documented | same | §3.3: the `[XSW]` row itself is 2-50 ms/present in the loaded phase and up to 1.6 s for a single row when the console is congested — and it is *inside* `self_avg` |
| `dmg_resync` default 64 → **256** | `tools/mesa/virgl-hobbyos/virgl_hobbyos_winsys.c` | reviewed and measured: a latency/correctness knob (sub-cell damage safety net), ~0.03 ms/frame of transfer |
| nothing on the fence path | — | measured: the double flush is already gone (§2) |

Nothing was added to the steady-state hot path: the new fields are `+` on
counters, and the two `now()` reads they use were already taken.

---

## 2. The double flush is already gone — and how that is measured

L80's fix (`supports_fences = 0` → `vgl_submit_one` hands back a **NULL**
fence) means `XMesaSwapBuffers`' `if (fence) fence_finish(...)` never runs, so
virgl's `fence_finish` — whose first act is `virgl_flush_eq()` — is never
reached. Two independent counters say so on this build:

* `[VGLB] n=` (winsys presents) tracks `[XSW] n=` (swaps) one for one (9 376 vs
  9 380 in L80's runB; the same 1:1 in every log of this lane). A double
  present would show 2x.
* `[VGLB] sub_avg=1 sub_b_avg=0 KiB` — exactly one submit inside
  `flush_frontbuffer`, and it is the 88-byte damage blit, not a second flush of
  the frame's stream.

`st_manager_flush_frontbuffer()` (reached from `ST_FLUSH_FRONT`) does call
`screen->flush_frontbuffer` for `ST_ATTACHMENT_FRONT_LEFT` — but a
double-buffered GLX drawable has no FRONT_LEFT texture, so
`xmesa_st_framebuffer_display()` returns before the winsys is entered. The one
present is `xmesa_swap_st_framebuffer` → BACK_LEFT. So L80's 21-24 ms "fence"
bucket was the present *plus* the extra `virgl_flush_eq`; in the seeded tip it
is the present alone. **Task 1(b) is a no-op on this tree**, and that is a
result, not a miss.

---

## 3. The budget, before and after

Same three instruments as L80 (`[XSWB]`/`[XSWL]`/`[VGLB]`, `/XPROF` + `/GLPROF`,
32-present batches), same guest (intel, KVM, virgl, egl-headless), same
1916x1018 content rect (fb0 rect `1916x1018+2+34` inside a 1920x1080
drawable). **Before** = *the same tree* with `XC_PRES_CRC_ROWS=1`, i.e. L80's
checksum and nothing else different; the "L80 runD" column is the previous
lane's own recorded run, to show that this tree reproduces it.

Whole-run medians (`tools/rw/l80-budget.py`), loading-to-menu phase:

| term | L80 runD | L82 before (`=1`) | L82 after (`=8`) |
|---|---:|---:|---:|
| `dt` frame period | 88.0 ms | 81.8 | 65.0-97.0\* |
| `st_context_flush(ST_FLUSH_FRONT)` | 26.2 | 26.1 | 3.5\* |
| `fence_finish` + frontend present | 17.5 | 15.9 | 3.8\* |
| winsys present (`[VGLB] pres`) | 12.4 | 13.1 | 9.4 |
| `XcPresentMark` (`self`) | 9.8 | **9.66** | **1.25-1.34** (quiet) |
| of which the checksum | ~7.3 | **7.32** | **~0.85** |
| of which the `[XSW]` row (`row_us`/32) | 2.1 | 2.34 | 0.41-2.0 |
| `XPutImage` (`put`) | 0 | 0 | 0 |
| damage idle | 98 % | **100 %** | **99-100 %** |
| readbacks/frame | 1.0 (269 KiB) | 1.0 (16 KiB) | 1.0 (16 KiB) |

\* **The frame-level terms in the `after` column are not comparable across
runs, and this report does not take credit for them.** `st_context_flush`
moved 26.1 → 3.5 ms with *identical* submit code and `gap` moved the other way
by more: the host changed between runs. A sibling lane ran its own 2100 s
in-game soak throughout, and the guest's console/FAT writes were 10-20x more
expensive in the later runs (`row_us` 64 → 107 ms per *batch*, and single rows
of 1.6 s in the congested phase). Only the terms measured *inside one run* are
comparable, and those are what the claims rest on:

**(a) Same tree, one constant, same epoch.** Both runs' second 32-present batch
(`n=64`, right after layout, both quiet):

| | before (`=1`) | after (`=8`) |
|---|---:|---:|
| `self_avg` | 2 062 us | **1 250 us** |
| `row_us`/32 (the instrument's own I/O) | 531 us | 406 us |
| shim net of its own I/O | 1 531 us | **844 us** |

**(b) Whole run, same tree.** `self_avg` 9.66 ms (before) vs 1.25-1.34 ms in
the after run's quiet batches — an 7-8x cut, which is exactly the 1/8 sampling.

**(c) The hash did not change meaning.** The full-scan build's CRC for the
early menu frame is `0x193b8beb63959a9a`, byte-identical to what L80's own
single-pass *unsampled* hash produced for the same frame — the cross-check that
the one-pass rewrite is faithful. `nz`/`crc_tot` now describe the sampled rows
(`nz=245248/245248` at 1916x1018, `115200/115200` at 1280x720).

### 3.1 The winsys present: `thumb_avg` *is* `pres_avg`

`[VGLB]` on every run of this lane: `pres_avg` = `thumb_avg` (8.8 of 9.4 ms at
1916x1018, 5.8 of 7.2 ms at 1280x720), `copy_avg` 0.0-0.2, `disp_avg` 0.0-0.7,
`idle` 97-100 %, `boxes=0`. When the damage probe says "nothing changed" the
present moves no pixels — and still costs that, because `dmg_thumb_grab()`
submits a full-surface blit into the 64x64 thumbnail and then **reads the
thumbnail back**, and that transfer is ordered after the client's frame on the
same context. The wait is the host finishing the frame. Reading the whole
surface back instead (M2's path) measures the same order of cost, which is why
the damage machinery is roughly break-even here and why nothing in it is worth
micro-optimising: the lever is the transport (direct scanout), not this file.
That also explains §4's headline: the fps is the *same* at 1280x720 and
1916x1018 — the present path is round-trip bound, not fill bound.

### 3.2 The fb0 blit does cover only the damaged rect

`blit_px`/`blit_n`/`blit=` from the same rows: on an idle present `blit_n=0`
and `blit_px=0` — nothing is copied to fb0 at all. On a present that carries
pixels, `blit=1916x1018+2+34` (or `1280x720+2+34`): the client's own
`XPutImage` rect clipped to the content area inside the drawable.
`shadow_blit()` clips the dirty rect to the content rect and then to the screen
rect before copying, so a partial `XPutImage` blits a partial rect; Unity's
`XPutImage` is full-surface, which is why the blit is full-surface on those
presents — not because the shim inflates it.

### 3.3 The instrument is a term (named, measured, and now tunable)

`self_avg` includes the `[XSW]` row: `pres_row()` runs *before* the batch's
`now` is taken. That row goes to fd 2 (Player.log, a FAT file) and to
`/dev/console` (a polled UART) inside the present it measures. Measured on
this lane: 2.0-2.3 ms/present at `PRES_ROW_EVERY=10` on a quiet host, 20-50
ms/present when the game logs heavily, and up to **1.6 s for a single row** in
the congested phase (`self_avg=163 750 us` with `flush_avg=0`, i.e. all of it
the row). L80 already flagged this at 46-147 ms/row; it is worse at the menu
because Unity's own logging saturates the same FAT file. The constant is now
overridable (`-DPRES_ROW_EVERY=64`) for a measurement run that does not need
the CRC ladder; the `[XSWB]`/`[XSWL]`/`[VGLB]` batch rows are unaffected.

---

## 4. fps at the menu, 120 s, two sizes

`tools/rw/l82-menu-fps.py --from-t <t> --window 120` — timed by the guest's own
CLOCK_MONOTONIC (`t=` on every `[XSW]` row). `--from-t` matters: `l74-sizes.py
--window N` measures the *first* N seconds, which is the load, not the menu.

Two 120 s windows are reported because one window is not stable on this host
(the sibling lane's soak moves whole phases; see §3's footnote):

| size | build | surface (drawable) | fb0 rect | t=90-210 s | t=210-330 s |
|---|---|---|---:|---:|---:|
| 1916x1018 | before (`CRC_ROWS=1`) | 1920x1080 | 1916x1018+2+34 | **9.97 fps** (100.3 ms) | 9.39 fps (106.5 ms) |
| 1916x1018 | after (`CRC_ROWS=8`) | 1920x1080 | 1916x1018+2+34 | **10.96 fps** (91.2 ms) | 9.94 fps (100.6 ms) |
| 1280x720 | after | 1280x720 | 1280x720+2+34 | **11.05 fps** (90.5 ms) | 6.95 fps (143.8 ms)\* |

\* the 720p second window contains a 4.75 fps patch (a host stall, the same
signature as the 1916 window's 3.22 fps patch): 30 s buckets
`11.06 6.37 2.54 8.35`. The first window's buckets are flat
(`11.82 10.59 11.03 10.45`), which is why it is the headline.

Presents in the headline window: 1 190 (before), 1 300 (after 1916), 1 320
(after 720). Fill: 24.4 / 26.2 / 8.5 Mpx/s — the 720p run moves a quarter of
the pixels for the same frame rate, so **the present path is latency-bound,
not bandwidth-bound** (the same conclusion as §3.1, from a different
measurement).

The menu is identified two ways and they agree: the guest CRC settles
(`changed=0` for hundreds of consecutive rows) and the driver's own
screenshots show the game's menu art (planet + starfield) with the menu's
widget area drawn as a black box — `obj/l82/shots-after-A-1916/09_running.png`
and `obj/l82/shots-after-B-1280/05_running.png`, the latter 44 % of the desktop
area, i.e. a real 1280x720 window.

**On "the menu takes 533-549 s" (L78).** That is a *present-count* landmark,
not a wall-clock one: L78 reached the menu at ~present 530 while running at
1 fps. On this tip 530 presents happen at **t≈60-90 s**, and the menu is
already up in the t=90-210 window measured above. The 533-549 s figure must
not be used as a wall-clock target for a faster build.

---

## 5. The wall in front of the fps number (not the present path)

Two different walls, one per build, both recorded rather than smoothed over:

* **before (full scan): presents stop at t=462 s** (`n=4224`) with the process
  alive; `[WATCHDOG] console silent for more than 20s` and an `[IDLESTUCK]`
  dump that repeats **815 times** over the stall, every slot parked in a futex
  wait (`THRS live=34`, main thread `st=3`, `rip` in libc, `futex=0xF0...`).
  That reproduces L80 runD exactly (4 250 presents, stall at t=466 s) — it is a
  pre-existing deadlock, not this lane's code, and the present path was 100 %
  idle when it happened.
* **after (sampled): no stall** — 6 410 presents to t=891 s — but the frame
  goes **black** at ~t=600-640 s (`nz=0/245248`) and decays from 11-18 fps to
  2-6 fps (`dt_avg` 420 ms at the end), ending on a static black frame.

So the fps numbers above are the menu *art* at 10-11 fps, not a menu with its
buttons drawn. The next wall for the menu is (a) the UI area rendering as a
black box and (b) the decay to black/fps-collapse at ~600 s — neither is the
present path's cost, and both need the texture/UI and threading work this lane
does not own.

---

## 6. Gates

| gate | result | log |
|---|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **435 run, 0 failed — UNIT TESTS PASSED** | `obj/l82/units.log` |
| `tools/gate/m2_virgl.py` | **PASS** — both drivers from one DSO, pixel-exact on GL and `/dev/fb0`; `p = 1.02 us/draw`, **`F = 6.35 ms/frame`** (L80: 0.70 / 5.95) | `obj/l82/gate-m2.log` |
| `tools/gate/m3_virgl.py` | **PASS, 8/8** — every run virgl, damage vs full **pixel-IDENTICAL**, every frame on `/dev/fb0`; plan row **98.04 fps** at 1916x1018 (L80: 89.42) | `obj/l82/gate-m3.log` |
| `tools/gate/m4_gl_probe.py` | **PASS** — libGL rendered a GL 3.3 core triangle and presented it to a window | `obj/l82/gate-m4.log` |

(Run sequentially, each in its own boot; the unit tier was run with no desktop
harness up, and `fuser disk.img` was empty between every run.)

`m3` is the gate that would have caught a broken damage path: it compares the
damage-rect present against the full-surface present **pixel for pixel** on the
same frames, and both are identical on this tree, so the `dmg_resync` change
(64 → 256) did not let a stale rect through in the gate's workload.

---

## 7. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l82-present

# one measured run to the menu (per-lane NFS export, virgl, probe on)
RW_QEMU_GL=1 RW_GLMODE=virgl RW_GLDMG=1 RW_PRESENT_PROF=1 RW_SHOT_EVERY=300 \
  tools/rw/run-rimworld-desktop.sh --timeout 480 --run-timeout 1150

# the budget, in the window you name
python3 tools/rw/l80-budget.py obj/rw-desktop/serial-desktop.log --from-t 90 --to-t 210

# fps at the menu for the same window, timed by the guest clock
python3 tools/rw/l82-menu-fps.py obj/rw-desktop/serial-desktop.log --from-t 90 --window 120

# the same tree with L80's full-scan checksum (the A/B control)
sed -i 's/^#define XC_PRES_CRC_ROWS 8$/#define XC_PRES_CRC_ROWS 1/' \
  src/user/xcompat/libx11.c && make -s ARCH=intel xcompat_dsos

# 1280x720 at the menu: the honour path, or the window keeps the tile size
RW_SCREEN=1280x720 RW_WM_HONOUR=1 ... tools/rw/run-rimworld-desktop.sh
```
