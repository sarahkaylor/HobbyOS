# L88 — where a RimWorld frame actually goes under IceWM/HBHOST

Lane `l88-fps`, worktree `~/hobbyos-rw-lanes/l88-fps` (branch `rw-l88-fps`,
seeded at the `rimworld` tip `d8a06ca3` = L87).

The brief: *measure* the frame time on an animated screen before optimising,
then fix the dominant stage and prove it with before/after ms/frame plus a
pixel-identical static-frame comparison. Three boots produced data: one at
1920x1080 (instrument validation, and it measured the wrong surface size — §7)
and the A/B pair at 1280x720 that this report is built on.

**Headline.** The app's own present path is **0.56 ms of a 963 ms frame**
(0.06 %). The dominant stage we own is on the *host* side, not the app's:
HBHOST's `XPutImage` into XHOBBY moved a 1280x720 frame at **5.9 MB/s —
620 ms per frame** — and it is not a copy cost (the same 3.7 MB memcpy in the
same process takes 0.39 ms, 9.5 GB/s). It is the AF_UNIX transport: a 64 KiB
ring means **57 writer-blocking refills per frame**. Raising the ring to
512 KiB (7 refills) took the same blit to **135 ms — 4.6x faster, 27.3 MB/s**,
with the screen byte-identical (0 / 942 080 pixels differ, §5).

---

## 1. The workload, and how it is driven

World generation's rotating planet, reached by
`Ignore -> New colony -> Crashlanded -> Next -> Cassandra Classic -> Next ->
Generate` with `tools/rw/rw_l86_input.py` (`RW_L86=1`), and measured in two
45 s windows right after the `Generate` click.

L87 could not OCR `Generate` at 1280x720; L88 fixes the lookup: the footer is
one full-width brown strip, so the band alone (bottom 56 rows, right half
first) is cropped and OCR'd at **4x** instead of the page-wide 2x pass. The
row that proves the click target:

```
[L88] click generate=(1388,744) how=ocr4:Generate:half
```

The driver's menu acceptance was also fixed: it required an exact `New colony`
and the 2x OCR reads that button as `newcoeny`, so the menu sat unclaimed for
200 s in the first run. `story generator` / `Official wiki` (same page,
unambiguous) now accept it.

Every measurement below is on the same screen, same phase, same seed flow,
`RW_SCREEN=1280x720 RW_WM_HONOUR=1` (the game's surface is exactly 1280x720;
`[XSW] w=1280 h=720 gx=0 gy=16`, X screen 1920x1080).

## 2. The instruments (new in this lane)

`[HBT]` / `[HBTS]` in HBHOST (`src/user/hbhost/hbhost.c`): the guest's
`CLOCK_MONOTONIC` ticks at 1 ms (skill rule 33), so the timing base is the TSC
calibrated against `sysinfo(1)` once per process (`[HBT] clock
cyc_per_ms=2994303 (2994 MHz)`). Per blit: the rect, the cycles inside
`XPutImage`, the cycles inside the sampled checksum. Every 2 s: blits/s,
mean+max `XPutImage`, Mpx/s, `busy_permille` (share of the window inside
`XPutImage`) and `copy_us` — a reference memcpy of the same rect in the same
process, which is what separates "our copy" from "the wire".

`[HBT] probe`: on a frame whose sampled checksum stopped changing, HBHOST does
an `XGetImage` of the content rect and compares it with the surface pixel by
pixel, then dumps both. `window_vs_surface_diff=0` is the host's blit proven
faithful (validated in run 1: three probes, `px=921600`, all `diff=0`).

`tools/rw/l88-stages.py` joins `[XSWB]` (the compat libX11's per-32-present
batch), `[VGLB]` (the virgl winsys) and `[HBTS]` into the stage table.

## 3. The stage table — 1280x720, world generation

Same window in both builds (`[XSWB] n=512/544`), app-side row quoted verbatim
from run A:

```
[XSWB] n=512 t=810829 frames=32 dt_avg=962906 dt_max=2643000 self_avg=562
       self_max=9000 put_avg=218 flush_avg=343 blit_px=921600 blit_n=32
       blit=1280x720+0+16 row_us=4000 w=1280 h=720
```

| stage | ms/frame | % of frame | where it is measured |
|---|---:|---:|---|
| everything between presents (Unity's world generation + render + the winsys readback, not separated — see §7) | 962.3 | 99.94 % | `dt_avg` − `self_avg` |
| shim: `XcPresentMark` self (put + flush + checksum) | 0.56 | 0.06 % | `self_avg` |
| — shadow → `/dev/fb0` blit | 0.34 | 0.04 % | `flush_avg` |
| — `XPutImage` copy into the shim's shadow | 0.22 | 0.02 % | `put_avg` |
| — sampled checksum (instrument, 1 row in 8) | 0.13 | 0.01 % | `crc_avg` |
| HBHOST `XPutImage` → XHOBBY, **parallel process** | **620 (before)** → **135 (after)** | 64 % → 14 % of a frame period | `[HBTS] put_avg_us` |
| HBHOST's own memcpy of the same 3.7 MB | 0.39 | 0.04 % | `[HBTS] copy_us` |
| app's damage per presented frame | 921 600 px = the whole surface | 100 % | `[XSWB] blit_px`, `[HBT] rect` |
| `[XSWB]` row's own cost (instrument) | 4.0 ms per 32 frames | 0.013 % | `row_us` |

The app's present path is **not** the frame. Neither is the shim, nor the
copy HBHOST makes. What is expensive is the bytes over the X connection.

Instrument cost, measured rather than assumed: the `[XSWB]` batch row's own
write is `row_us=4000-5000` (4-5 ms per 32 presents). The per-present `[XSW]`
row is 1 in 10 presents (`PRES_ROW_EVERY`), so ~3 of a 32-frame batch carry one,
and its own write is inside the frame it measures — the same order as the batch
row's, i.e. <= 15 ms of a 963 ms frame (< 1.6 %). L82 measured the loaded case
much worse (20-50 ms, up to 1.6 s on a congested console), which is why
`PRES_ROW_EVERY` is overridable.

## 4. The optimisation: the X transport's ring was 64 KiB

Measured, same phase, rows with blits in `t=700..1000 s`:

| | run A (64 KiB ring) | run B (512 KiB ring) | ratio |
|---|---:|---:|---:|
| `XPutImage` per 1280x720 frame, median | **619.8 ms** | **135.1 ms** | **4.59x** |
| p25 / p75 | 599.0 / 669.8 ms | 119.8 / 139.8 ms | — |
| min / max | 564.0 / 3 019.7 ms | 89.2 / 183.7 ms | — |
| mean over the window's blits | 766.7 ms | 129.4 ms | 5.9x |
| effective rate at the median | 5.9 MB/s | 27.3 MB/s | 4.6x |
| windows / blits in the window | 87 / 165 | 52 / 82 | — |
| `[HBTS] busy_permille` (share of wall time in `XPutImage`) | 595 | 108 | 5.5x |
| reference memcpy of the same rect (`copy_us`) | 388 µs | 385 µs | unchanged |
| app-side frame (`dt_avg`) | 962.9 ms | 962.2 ms | unchanged |
| app-side present path (`self_avg`) | 0.56 ms | 0.41 ms | unchanged (noise) |

The distributions do not overlap: run A's *fastest* blit (564 ms) is 3x run
B's *slowest* (184 ms). The memcpy reference is identical in both runs (388 vs
387 us), so the 4.6x is entirely the connection, not the guest's memory
bandwidth.

Model that fits both runs (`T = c·bytes + d·refills`, 3.7 MB/frame):
`d ≈ 8.4 ms` per ring refill, `c ≈ 30 µs/KiB` (33 MB/s of per-byte cost). A
64 KiB ring needs 57 refills (≈ 480 ms of the 620); 512 KiB needs 7. That is
also why the earlier 800x560 paint blit cost 304 ms in run 1 (28 refills) —
11 ms per refill at a different rect size.

Change: `src/include/unix.h`, `K_UNIX_RING 65536 -> 524288` (2 x 512 KiB per
connected pair, 256 contiguous frames from the frame pool). Flow control is
unchanged (`c->used >= K_UNIX_RING` still blocks the writer); the kernel's own
`unix_test_suite` exercises the ring-full boundary at the new size and passes
(§6).

What it buys the user: the hosted window's **display ceiling** — one frame per
`XPutImage` — goes from **1.61 fps to 7.40 fps** (at the medians), and the lag
between the app presenting a frame and the window showing it goes from
**0.62 s to 0.14 s**.
The app's own frame rate is untouched, because the host runs on another vCPU
and the app never waits for it (`-smp 8`; the app's `ESC ] F` is a 20-byte
pipe write, and HBHOST's `[HBTS] blits` never exceeds the present rate).

## 5. Pixel identity — the image did not change

1. **Across the two builds, on a static frame.** The storyteller page is
   static: two shots 3 s apart differ by 0 px in *each* run (`storyteller_after_
   Cassandra_Classic` vs `storyteller_settle`), so it is a legitimate static
   frame to compare across builds.
   Client-area crops `(332,24)-(1612,760)` of the same page, run A vs run B:

   | screenshot | differing pixels |
   |---|---:|
   | `adv_storyteller_probe` (page as it arrives) | **0 / 942 080** |
   | `storyteller_settle` (after the radio clicks, 3 s later) | **0 / 942 080** |
   | `adv_scenario_probe` (previous page) | **0 / 942 080** |
   | `adv_worldopts_probe` (next page, has a rotating globe preview + a random seed) | 980 (expected — animated/seeded) |

   The 0-pixel pairs are the whole path: app → HBHOST → XHOBBY → scanout.
2. **In-guest, host blit vs source surface.** Run 1's `[HBT] probe` (three
   times, `rect=0,16,1280x720`, `px=921600`):
   `window_vs_surface_diff=0` — `XGetImage` of the window equals the shared
   surface byte for byte.
3. The damage-rect change (§8) was proven a no-op on this workload by the
   measurement itself: `[HB] blit kind=restamp rect=0,16,1280,720` in both
   runs, `[XSWB] blit_px=921600` — RimWorld damages the whole surface on every
   presented frame.

Reproduce: `python3 tools/rw/l88-diff-shots.py obj/l88/runA-shots
obj/rw-desktop/shots` (same crop, same tags).

## 6. Gates

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **Tests run 436, Tests failed 0,
UNIT TESTS PASSED** (the count is runtime-dependent — several suites loop over
the CPU count and the process table — the integrator records 437; the failure
count is the gate and it is 0).

## 7. What did not work / what is still open

* **The first run measured the wrong surface.** `RW_SCREEN` is the marker that
  sets the *game's* resolution (`/RWSCREEN` -> `-screen-width/-screen-height`);
  the X screen is 1920x1080 either way. With `RW_SCREEN=1920x1080` the game
  renders 1920x1080 (`[XSW] w=1920 h=1080`), not the 1280x720 the brief asks
  for. Both runs here use `RW_SCREEN=1280x720`, and the surface is 1280x720.
* **`[VGLB]`/`[VGLP]` never appeared**, with `/GLPROF` staged and `/XPROF`
  working (`[XSWB]` rows are present). So the winsys' own split — the 64x64
  damage probe, the boxed readback, the readback→XImage copy — is *not* in the
  table above; "everything between presents" includes it. The shim's [XSWB]
  numbers bound it from the outside (`self_avg` = 0.56 ms), but the readback's
  own cost is unmeasured in this lane.
* **No like-for-like comparison with the pre-HBHOST 14.24 fps exists.** That
  number was the present rate of a mostly-static loading screen through a
  different present path; this lane's 963 ms/frame is world generation with
  changing pixels. The only honest cross-path statement is the one the stage
  table makes: what HBHOST adds to a *displayed* frame was 620 ms and is now
  135 ms.
* **The menu/loading screens cannot supply an fps.** During the def load the
  app presents once per 196 s with `changed=0`; the 10-14 fps the earlier lanes
  quoted is Unity re-swapping an unchanged frame. Only world generation
  presents changing frames (~1/s, `changed=1`), so that is the workload used.
  At that rate the frame period (963 ms) is Unity generating a planet, and the
  present path is noise inside it.
* **The damage-rect carrying (§8) is a no-op for RimWorld**, measured, not
  assumed. It stays because it is the right contract for a host (cost
  proportional to what the app actually damaged) and because the coalescing fix
  it needed is a real bug fix — but it buys nothing here.
* **What 30 fps would need.** Not the present path: at 30 fps, 0.56 ms of shim
  work is 1.7 % of the budget. Two blockers, in order:
  1. Unity's own frame. Measured 963 ms/frame while generating, and no screen
     in the front end presents faster than ~2 fps of *changing* pixels.
  2. If the app ever does run at 30 fps, the host must put a 1280x720 frame on
     screen in <= 33 ms. It is at 135 ms (27 MB/s) after this lane. The
     remaining cost is per-byte (33 MB/s), not per-refill: another ring bump
     buys little (512 KiB -> 1 MiB models at ~15 %), and 1280x720x4 bytes at
     33 MB/s is 110 ms by itself. Getting under 33 ms needs either fewer bytes
     (the app must damage less than the whole surface — it does not today) or a
     zero-copy path into the server (MIT-SHM is **not** built into XHOBBY:
     `dix-config.h` has `#undef MITSHM`, checked, not assumed) or a faster
     per-byte path (the framebuffer write and the two socket copies are the
     candidates).

## 8. The other change: the app's present rect, carried through

`ESC ] F <x>;<y>;<w>;<h>` already names the rect the compat Xlib wrote into
the shared surface, but HBHOST re-blitted the whole content rect on every
present. L88 carries it through (`hd_take_present()` in `hbhost_desk.c`,
`desktop.c` unions rects that arrive before the host services them — two
presents between two services used to *replace* the pending rect and leave the
first frame's pixels stale). Measured effect on RimWorld: none (§5.3). Kept for
correctness and for the hosts/apps where damage is partial.

## 9. Commits (branch `rw-l88-fps`, not pushed)

| commit | what |
|---|---|
| `c18d5aea` | the present budget (`[HBT]`/`[HBTS]`), the `/HBTPROBE` static-frame probe, the stage-table and screenshot-diff tools, **and** the present-rect carrying (`hd_take_present()`, the main loop blits the named rect instead of the whole content — §8) |
| `c1b40e90` | `K_UNIX_RING` 64 KiB -> 512 KiB (§4) |
| `d0c1daf5` | `desktop.c`: union two present rects that arrive before service (§8) |
| `b259eaf5` | driver: 4x footer OCR, menu acceptance, `measure()` fps windows, `RW_HB_PROBE` |
| this one | `docs/l88-fps-report.md` |

## 10. Reproduce

```sh
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l88-fps
fuser -k disk.img; sleep 2
RW_L86=1 RW_L88_MEASURE=1 RW_L88_WINDOW=45 \
RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1 \
RW_PRESENT_PROF=1 RW_HB_PROBE=1 RW_WAIT_SECS=1900 \
  bash tools/rw/run-rimworld-desktop.sh --run-timeout 1700
python3 tools/rw/l88-stages.py obj/rw-desktop/serial-desktop.log --from 790000 --to 900000
python3 tools/rw/l88-diff-shots.py obj/l88/runA-shots obj/l88/runB-shots
```

Evidence kept: `obj/l88/runA-serial.log`, `obj/l88/runA-driver.log`,
`obj/l88/runA-shots/`, `obj/l88/runB-serial.log`, `obj/l88/runB-driver.log`,
`obj/l88/runB-shots/`.
