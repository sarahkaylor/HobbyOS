# L84 — a hosted pixel app repaints its HBHOST window

Lane `l84-blit`, worktree `~/hobbyos-rw-lanes/l84-blit` (branch `rw-l84-blit`,
seeded at the `rimworld` tip that includes L83).

The defect L83 recorded: RimWorld starts from IceWM's Start menu inside
HBHOST's X window and presents (`[XSW] n=3870 swap=3870`), but the window keeps
showing the text tail it had before the pixel window arrived — the launcher's
environment dump (`memorysetup-…=2097152 …`) — and never shows a game pixel.

Two separate causes, both fixed here:

1. **The hosted client never painted HBHOST's surface.** `vm.c` mapped
   `/dev/fb0` to the real scanout for every process. A compat (Linux-ABI) X
   client reaches the framebuffer by mmap'ing `/dev/fb0` — the native path
   (`SYS_MAP_FB`) is not used — so under HBHOST the Unity player painted the X
   server's framebuffer while HBHOST blitted the private surface it had
   attached to the process group. The two never met: the surface only ever
   contained what the desktop had drawn (the text tail).
2. **The text tail was never dropped on the pixel-mode switch.** The desktop
   stops painting at the menu bar once a window is `pixel_mode`, and an app
   only repaints what it is asked to, so any content pixels the app did not
   overwrite stayed on screen (measured: the env dump stayed in the window's
   top-left corner while the game's loading art filled the rest).

## 1. The fix

| file | change |
|---|---|
| `src/kernel/vm.c` | in the `/dev/fb0` mmap branch, if the caller's process group is vdisplay-attached (`vdisp_attached()`), return `vdisp_map_fb()` — the same mapping `sys_map_fb` installs for an attached native process. The hosted client and HBHOST now share one object; the real scanout is untouched. |
| `src/user/hbhost/hbhost_desk.c` | `hd_pixel_enter()` / `pixel_clear_content()`: clear the window's content rect on the pixel-mode switch and whenever the content rect changes size, and queue a full repair (`ESC [ E ~`). The chrome (menu bar) is repainted by the desktop; the content belongs to the app from then on. |
| `src/user/hbhost/hbhost.c` | `[HB]`/`[HBS]` rows: one line per blit decision (kind, rect, window, pixel/game state, and a sampled checksum of the source rect), one summary line every 2 s, plus rows for the pixel-mode switch, `ConfigureNotify`, `hd_set_size`, the repair requests sent, the app's `ESC ] F` messages and the absolute window geometry. `blit_rect()` takes a kind so a blit can be attributed. |

Nothing here blits blindly: every repaint still comes from a decision — the
app's `ESC ] F` present message (`restamp`), a vdisplay surface flush (`dmg`,
for native apps), an X `Expose`, a chrome paint, or the one clear at the mode
switch/resize.

## 2. Before / after, in rows

Baseline run (this lane, instrumentation on, no fix): the client reported
3 870 presents and the window stayed on the text tail.

```
[HB] pixel mode -> 1 t=66579 win=800x560 mapped=1 flushed=0 menu=-1
[HB] blit kind=restamp rect=0,16,1280,720 win=1280x736 pix=1 game=0 flush=0 dmg=0 restamp=1 crc=… nz=2560
[HBS] t=67576 vd=491 flush=0 dmg=0 restamp=1 paint=6 blit=11 empty=0 px=4405760 full=0 dmgblit=0 restampblit=1 paintblit=10 game=0 pix=1 win=1280x736
[HBS] t=122526 vd=2582 flush=0 dmg=0 restamp=3 paint=6 blit=13 empty=0 px=6248960 … restampblit=3 …
```

`mapped=1` is HBHOST's own mapping: the client never mapped the private
surface, and the one restamp blit per ~30 s copied the text tail (`nz=2560`
sampled non-zero pixels = the dump). (Rows from run 1 of this lane — same
instrumentation, unfixed kernel; that run's serial log was overwritten by the
later boots, L83's screenshot `before-L83-window-text.png` is the pixel record
of the same state, and `docs/l83-icewm-launcher.md` §5 records the client's
`put=17`.)

Same rows after the fix (`obj/l84/serial-run5.log`):

```
[HB] pixel mode -> 1 t=70654 win=800x560 mapped=3 flushed=0 menu=-1
[HB] blit kind=clear rect=0,16,800,544 win=800x560 pix=1 game=0 flush=0 dmg=0 restamp=0 crc=0xfcc0dd12993e6583 nz=0
[HB] blit kind=restamp rect=0,16,1280,720 win=1280x736 pix=1 game=0 flush=0 dmg=0 restamp=1 crc=0x1b542b92b8424b83 nz=0
[HB] blit kind=restamp rect=0,16,1280,720 win=1280x736 pix=1 game=0 flush=0 dmg=0 restamp=2 crc=0xd4cdaac200b4e7bf nz=115200
[HBS] t=114535 vd=2727 flush=0 dmg=0 restamp=2 paint=6 blit=13 empty=0 px=5762560 full=0 dmgblit=0 restampblit=2 paintblit=11 game=0 pix=1 win=1280x736
```

* `mapped=3` — the client mapped the private surface (HBHOST's own map + the
  client's /dev/fb0 maps) instead of the scanout.
* `kind=clear … nz=0` — the text tail is dropped at the switch.
* `restamp=1 crc=0x1b54… nz=0` — the app's first present into the surface is a
  black frame; it is the app's frame, no longer the dump.
* `restamp=2 crc=0xd4cd… nz=115200` — the app's next present is the game's
  loading art (every sampled pixel non-zero). The checksum changes because the
  pixels changed, and the blit is the app's own `ESC ] F` present message.

The app's side of the same run (`obj/l84/serial-run5.log`):

```
[XSW] n=3880 swap=3880 put=17 putok=17 flush=113 crc=0xc192388215c2ef10 changed=1 w=1280 h=720 gx=0 gy=16 t=356118 ready=1 nz=115200/115200 box=0,0,1279,712
```

`put=17` is the point: the shim only calls `XPutImage` when the GPU damage grid
says a frame actually changed, so 17 real presents over 3 880 swaps is the
game being static, not HBHOST dropping frames. HBHOST made 29 `XPutImage`
calls in that run (`[HBS] t=662562 … blit=29 … restampblit=18 paintblit=11`),
one per app present plus the paints; 15 of the 18 restamp rows carried the same
content checksum (`crc=0xd4cdaac2…`), i.e. one blit per distinct app frame and
a blit for every repeat — which is exactly the contract.

## 3. Pixels

Window: `[HB] geometry client=332,24,1280x736 screen=1920x1080` (the client area
inside IceWM's frame; the real window is the title bar above it).

Two frames 6 s apart (`RW_SHOT_EVERY=3`), cropped to the client area:

| pair | diff samples | bbox (window coords) | 32×32 cells |
|---|---:|---|---:|
| `13_running` → `15_running` | **230 399** (97.8 % of the 640×368 grid) | (0, 16, 1278, 734) | **920** |
| `12_running` → `14_running` | 230 399 | (0, 16, 1278, 734) | 920 |
| `98_running` → `99_running` | 117 sampled (428 at full res) | (600, 370, 686, 382) | 4 |
| `22_running` → `24_running` | 1 | (650, 378, 650, 378) | 1 |

Reproduce:

```sh
python3 tools/rw/l84-blit-diff.py --shots obj/l84/shots-run5 --gap 2
```

The first row is the app's second distinct frame landing in the window: shots
11–13 are black inside the client area (`distinct colours = 2`: black content +
the 16-row menu bar), shot 14 is the game's loading art (5 173 distinct colours).
The bbox starts at y=16 — the content rect's origin — so the change is the
app's content, not the menu bar or the title bar. 920 cells changed: a mouse
pointer is 1–2 cells, and the crop `(332,24)-(1612,760)` never includes the
taskbar or its clock (screen rows ≥ 1054).

The 98→99 row is the game *drawing text*: the loading line appears there — 428
pixels at full resolution, an 88×14 box at (600,369) — and later frames
re-render the same line with the next ellipsis. The last row is the loading
indicator blinking (a 1-pixel change at (650,378)): the app presenting a
mostly-identical frame, which HBHOST blits and the window shows.

OCR of the same crop (`tesseract`, window crop, 2× upscale):

* before pixel mode (`02_running`): `[UnityMemory] Configuration Parameters —
  Can be set up in boot.config`, `memorysetup-bucket-allocator-granularity=16`,
  … — the text tail.
* after (`13_running`, `15_running`): no text at all — the dump is gone; the
  window shows the RimWorld title-screen art (large ochre planet, two ships
  near its surface, starfield, two moons).
* the loading line at `99_running`, cropped to its own box and upscaled 8×
  (`obj/l84/ocr-loading-defs.png`): **`Loading defs..`** — the game's own
  loading progress text, rendered by the app into the window. The frame before
  it (`98_running`, same crop, `obj/l84/ocr-before-98.png`) has no text there.

Evidence files:

| file | what |
|---|---|
| `obj/l84/before-L83-window-text.png` | L83's screenshot of the defect (window showing the env dump) |
| `obj/l84/frame-a-13-black.png`, `obj/l84/frame-b-15-art.png` | the two frames at full client resolution |
| `obj/l84/diff-13-15.png` | their difference |
| `obj/l84/diff-98-99-text.png` | the 428-pixel text appearance at frame 99 |
| `obj/l84/ocr-loading-defs.png`, `obj/l84/ocr-before-98.png` | the loading line, and the same box one frame earlier |
| `obj/l84/shots-run5/` | all 146 frames of the evidence run |
| `obj/l84/serial-run5.log` | the serial log with the `[HB]`, `[HBS]` and `[XSW]` rows |
| `obj/l84/run5.out` | the harness's own log (click rows, milestones) |

## 4. A second boot, same result

A second, independent boot (`obj/l84/run6.out`, `--run-timeout 900`, same
command) repeats the sequence: 20 restamp blits by t=605 s
(`[HBS] t=604966 … restamp=20 blit=31`), two of them with source checksums this
run had not produced before (0x4363f3…, 0x3f76b4…) on top of the black first
frame and the repeated 0xd4cdaac2…, and the loading line OCRs cleanly at frame
116 (`obj/l84/ocr-loading-defs-run6.png`, window coords (925,388)-(1040,410)
upscaled 8×):

```
Loading defs...
```

That boot ended at ~530 s in the block device's own controller reset
(`[BLKQ] controller reset #1`), the environmental flake L75 recorded — not in
the window path; every frame before it was in the window.

## 5. How to reproduce

```sh
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l84-blit
RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1 RW_SHOT_EVERY=3 \
  tools/rw/run-rimworld-desktop.sh --run-timeout 900
python3 tools/rw/l84-blit-diff.py --shots obj/l84/shots-run5 --gap 2
```

Read `[HB] geometry` for the client rect and `[HB] blit` for the decisions; the
window is the client rect from that row.

## 6. What is still open

* **The game's own wall is unchanged.** Unity presents only when its GPU
  damage grid reports a change — 17 real presents over 3 880 swaps in the
  evidence run, most of them the loading indicator and the loading line's
  ellipsis — and never reaches the menu; L83's `Loading.AsyncRead` SIGSEGV is a
  separate ladder. HBHOST now shows every frame the app produces.
* **A compat client's presents arrive as `ESC ] F`, not as surface damage.**
  `flush=0 dmg=0` on every `[HBS]` row is correct for a compat app (it has no
  `SYS_FLUSH_FB`); native pixel apps still come through the `dmg` path.
* **The clear is a full-content memset per mode switch/resize** (3.7 MB at
  1280×720). It runs once per event, not per frame.
