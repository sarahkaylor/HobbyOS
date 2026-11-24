# SWARM-REPORT-WP-PTR — the dropped X11 button press: root cause, red/green, and the re-measurement

Branch `nv/wp-ptr-x11`, worktree `wp-ptr`. Round 4 of the NV swarm. This lane's
job was the one blocker round 3 left behind: a compat X11 client saw pointer
motion and keys, but **not mouse button presses**, so RimWorld's Steam modal
could not be dismissed with a click and N1's fps was measured on a modal.

Short form: the defect is **one XID choice in the compat `libX11`** — input was
delivered to the window that owned the *desktop surface* (Unity's
override-redirect container, which selects `StructureNotifyMask` only) instead
of the window that had *selected* the event (Unity's GL window,
`0x62c07f`, which includes `ButtonPressMask`). It is fixed, the gate that
reproduces it without the game is red on the old bytes and green on the new
ones, and on r1 the Steam modal now comes off with three presses: the frame
after it is RimWorld's **main menu** (verified by looking at it), presenting at
a **median 51.02 fps** over 150 s of the guest's own clock. **N1's fps gate
(≥30 fps median on the menu) is met; input is no longer a blocker; audio is
still not reachable and is not claimed.**

## 1. Root cause (MEASURED, and where the code disagreed with its own comment)

`src/user/xcompat/libx11.c`, `input_target()`, before this branch:

```c
/* Input goes to the window that owns the visible surface (d->surface) ...
 * Unity creates an override-redirect container after its GL window and selects
 * no input mask on it, while the GL window (the reparented child) keeps
 * KeyPress/Button/Motion.  A real server delivers to the window under the
 * pointer; here the reparented child is it. */
static Window input_target(Display *d, long *mask) {
  Window w = d->surface ? d->surface : d->win.id;
  ...
```

The comment states the rule and the body does the opposite: `d->surface` is the
CONTAINER (it is set by `XMapWindow` on every map — `d->surface = w` — and
Unity maps the container last). `mouse_event()` then asks
`if (sel & ButtonPressMask)` for that XID, and the container's selection is
`StructureNotifyMask` only, so the test is false and the press is dropped.

The two masks are not guessed: they are in this tree's own launch trace from a
real RimWorld run (`docs/rimworld-launch-log.md` §17.4 and §28.5/§29.1):

```
[XC] XCreateWindow id=2097154
[XC] XSelectInput mask=0x0x62c07f          <- the GL window: ButtonPressMask is in it
[XC] XCreateWindow parent=1 geom=1920x1080 <- the container
[XC] XSelectInput id=<container> mask=0x20000 (StructureNotify)   <- only this
[XC] XMapWindow   id=<container> -> ESC ] X 1920;1080 -> ESC ] G 2;34;1916;1018
```

Why the *shape* was "motion works, buttons do not" rather than "input is dead":
Unity polls `XQueryPointer` for the cursor, and this shim answers that from the
desktop's tracking report (`ESC [ T`), which is not mask-filtered. Buttons have
no polling equivalent — they only arrive as `ButtonPress`, which was filtered
out. That is the signature the round-3 report described, and it is not a dead
device: the whole device path was intact (see §2).

## 2. The device path was never the problem (MEASURED)

Before touching any code I measured the layers under the shim on the build box
(c4, QEMU 10.2.1, the r1 device set `-M q35,vmport=on -device virtio-tablet-pci`
plus `virtio-gpu-gl-pci`) with bounded rows added at each layer — they are kept,
because they are what named the layer:

| layer | row | what it showed |
|---|---|---|
| QEMU → guest | `[VMW] st=0x…20 x=… y=… z=0` | the vmmouse packet DOES carry the left-button bit (`0x20`) for a QMP `btn=left` |
| guest decode | `[BTN] raw value=1 mx=242 my=195 win=0` | `BTN_LEFT` reaches the desktop as `EV_KEY/BTN_LEFT` |
| desktop → client | `[BTN] fwd win=0 x=240 y=161` | the desktop forwards `ESC [ P 240;161;1` to the window |
| shim mask test | `[XE] BTNin 80;240;161;1` then `[XE] BTNsel 163917;4;4;1` | the press arrives at the shim, and `sel & ButtonPressMask` is what decides |

`qmp query-mice` on that boot: `vmmouse` `current=true absolute=true`,
`QEMU PS/2 Mouse` `current=false`, `QEMU Virtio Tablet` `current=false` — i.e.
QEMU routed the pointer to the vmmouse, and (measured, not inferred) a QMP
`btn` event survives the whole path to a compat X11 client. **So the blocker
was never evdev, not the vmmouse decode, not the desktop.** It was the shim's
choice of XID.

## 3. The fix (one function, at the cause)

`input_target(Display *d, long *mask, long need)` now takes the class of event
it is being asked about and:

1. returns the surface XID when the surface selected that class (unchanged
   behaviour for every client that does select it);
2. otherwise returns the best **mapped** XID that did select it — a descendant
   of the surface first (that is the window under the pointer), then any other
   mapped XID, later-created winning a tie;
3. with no such window, returns the surface unchanged (a client that selected
   nothing still receives nothing, as on a real server).

`key_event()` passes `KeyPressMask`, `mouse_event()` passes the class it builds
(`ButtonPressMask` / `ButtonReleaseMask` / `PointerMotionMask`). A/B knob:
`make XCOMPAT_NO_PTR_FIX=1 xcompat_dsos` builds the pre-fix body
(`-DXCOMPAT_INPUT_SURFACE_ONLY`) from the same source, so the red row is
reproducible.

## 4. Red/green, without the game (MEASURED on c4)

`tests/fixtures/ptr/xptr.c` is an ordinary glibc Xlib client that builds the
*two-XID shape*: a child that selects `ButtonPress|Release|Motion|Key|Exposure|
StructureNotify` is mapped first, then a container that selects
`StructureNotify` only is mapped last (so it becomes the surface). It prints one
row per event class and exits 3 if no `ButtonPress` reached it.
`tools/gate/xptr_click.py` boots the same desktop, launches it from the Apps
menu with real key events, moves the pointer to the client's own content rect,
sends three press/release pairs over QMP, and grades the rows.

```
$ rm obj/xcompat/libX11.so.6
$ XCOMPAT_NO_PTR_FIX=1 XPTR_SKIP_OS=1 ./tools/gate/xptr_click.py
[XP] up shape=container+child container=0x200001 child=0x200002
[XP] geom gx=2 gy=34 gw=480 gh=320 cx=242 cy=194
[INFO] motion rows after abs=0 after rel=0
[XE] BTNin 80;240;161;1
[XE] BTNsel 131072;4;0;1          <- sel=0x20000, need=ButtonPressMask, result 0
[XE] BTNin 82;240;161;1
[XE] BTNsel 131072;8;0;1
[XP] done presses=0 releases=0 motions=0 exposes=1 to_container=0 rc=3
xptr: FAIL — no button event reached the client
```

```
$ rm obj/xcompat/libX11.so.6        # same tree, default (fixed) build
$ XPTR_SKIP_OS=1 ./tools/gate/xptr_click.py
[XP] up shape=container+child container=0x200001 child=0x200002
[XE] BTNsel 163917;4;4;1          <- sel is now the child's 0x2804d
[XP] ButtonPress n=1 button=1 x=240 y=161 win=0x200002 state=0x0
[XE] BTNsel 163917;8;8;1
[XP] ButtonRelease n=1 button=1 x=240 y=161 win=0x200002
[XP] done presses=3 releases=3 motions=3 exposes=1 to_container=0 rc=0
xptr: PASS — a compat X11 client received ButtonPress/ButtonRelease
```

Control row: `tests/fixtures/ptr/xptr.c` also builds a single-window shape
(`XP_FLAT`, one XID selecting everything), which is the same client without the
variable under test. It was **not** run this round: the flag is an environment
variable and the desktop launches the client with its own environment, so
exercising it needs a staged marker file — the two runs that were run differ
only in the window shape, and the red one is the shape the game builds.

## 5. Gates

```
intel: Tests run: 583   Tests failed: 0      (TOOLS_SKIP=1 ./run_unit_tests_intel.sh)
ARM per-TU: obj/arm/virtio_input.o, obj/arm/desktop.o   compile clean
```

## 6. The re-measurement on r1 (MEASURED — the menu, not a modal)

Same host, same image as round 3 except `/LIB64/libX11.so.6`, same boot line
(`egl-headless,gl=on` + `virtio-gpu-gl-pci`, host llvmpipe; the 4090 is still
vfio-bound to VM 300 and was not touched). Full rows: `docs/w4-ptr-r1-evidence.txt`.

| t (s) | event | presented CRC |
|---|---|---|
| 64 | loading art | `0xd0cb6fbecb358d2c` |
| 707 | load advances | `0x4ab25f59f342f8b3` |
| 915 | **Steam modal** (the same CRC round 3 recorded) | `0x0e40610827f1e4d3` |
| 940 | **3 presses** at (485,588) → **dismissed** | `0xcb920d0f8c49a201` |
| 948..1100 | RimWorld's **main menu**, sampled 150 s | `0xcb920d0f8c49a201` |

**The menu was on screen and was looked at.** Two `pmemsave` frames dumped in
the same pass are byte-identical (sha256 `3563b2c1…31e08069`), so it is not a
torn capture: *Load game / New colony / Options / Mods / Credits / Quit to OS /
Language*, "a story generator by Tynan Sylvester", version 1.5.4104 rev435, no
warning text. The pre-press frame is the Steam modal ("Could not initialize
Steam API … Ignore / Quit"), i.e. the dismissal is a real state change and not
a hover CRC (the press is judged against an off-button baseline taken the same
way before and after — the trap round 3 hit).

**fps on the menu** (guest `[XSW]` counter, Δn/Δt between consecutive rows,
median over intervals; 150 s window starting 5 s after the dismissal):

```
median 51.02   p10 49.02   p90 52.08   min 43.86   max 53.19
7600 presents over 149 848 ms of the guest's CLOCK_MONOTONIC  (50.72 fps mean)
```

The modal itself measured 51.02 (p10 49.02, p90 52.08) over the 28 s it was up,
and the whole run 51.02 — as expected: both screens are the same 720p present
loop over llvmpipe. **Rung N1's fps gate (≥30 fps median on RimWorld's own
menu) is met: 51.02 against 30.** Input is no longer a blocker: a press
dismisses the modal in one round of three. Audio is still not reachable (no
backend; `docs/audio.md` GB-029) and is not claimed.

**A first attempt is worth recording as a harness lesson, not a result:** run A
armed its press on "CRC unchanged for 20 s", pressed at t=601 s and dumped the
frame — which turned out to be the *loading art*, not the modal (the game was
still loading its 1 209 def XMLs). The CRC change that followed was the loader
advancing. The rule this run used instead: press only when the picture differs
from the first presented one, has been stable for 20 s, **and** is still being
presented at >20 fps (round 3's load→menu transition stalls are ~10 presents in
160 s). That fired once, on the modal, at t=940 s.

## 7. What is NOT verified

- **The 4090 as a renderer of anything.** r1 still has no host driver for it
  (vfio → VM 300); this run is virgl over host llvmpipe, exactly round 3's
  stack. Nothing here measured the card.
- **Audio.** No backend; nothing audible was produced or verified.
- A long colony session, world generation, or any gameplay beyond the menu.
- The extra `[XSW]` stream noted in `docs/w4-ptr-r1-evidence.txt` is
  unexplained. It carries the loader-art CRC, its rows are excluded from every
  window, and the measured frame is verified independently by the byte-identical
  pair.
- The r1 configuration is still not persistent (`modprobe vgem`, Mesa packages).
- The r1 poll command in the work order (`pgrep -c qemu-system-x86_64`) cannot
  report what it is asked for: the name is longer than pgrep's 15-character
  match window (it warns and matches nothing), and with `-f` it matches the
  invoking shell. This lane used `pgrep -af '[q]emu-system-x86_64'` and took two
  quiet readings two minutes apart (22:22:44 and 22:24:49, both 0) before
  starting.

## 8. Files

| path | what |
|---|---|
| `src/user/xcompat/libx11.c` | the fix: `input_target(d, &sel, need)` + the A/B `#ifdef` |
| `src/user/xcompat/Makefile.inc` | `XCOMPAT_NO_PTR_FIX=1` builds the pre-fix routing |
| `src/kernel/virtio_input.c` | `[VMW]` bounded trace of vmmouse packets (instrument) |
| `src/user/desktop.c` | `[BTN]` bounded trace of the BTN_LEFT decision (instrument) |
| `tests/fixtures/ptr/xptr.c` | the glibc Xlib client that builds Unity's two-XID shape |
| `tools/gate/xptr_click.py` | the gate: boot, launch, click, grade (red/green) |
| `docs/w4-ptr-r1-evidence.txt` | the r1 run: frames, CRCs, fps windows, the anomaly |
| `SWARM-REPORT-WP-PTR.md` | this report |

## 9. Next step

The fps gate is met on llvmpipe; the number the owner actually wants is the
card's. That is the same decision round 3 named and this lane did not take: the
4090 must leave VM 300 and be bound to a host driver before virgl (or Venus, or
`nv` in a guest) can render with it. On the input side the next honest check is
the same run with the *menu* clicked through (New colony → world gen) to prove
button delivery inside gameplay, not only on the modal.

