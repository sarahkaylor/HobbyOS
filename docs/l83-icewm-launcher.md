# Running RimWorld from the IceWM Start menu (L83)

IceWM is the HobbyOS desktop. RimWorld starts from **Start → Apps → RimWorld**,
a first-class menu entry, and the game runs in a real X window with IceWM
decorations and taskbar button. There is no other supported way to start it.

That entry is a *hosted* program:

```
prog "RimWorld" app HBHOST.BIN RWLAUNCH.BIN
```

`RWLAUNCH.BIN` is a native HobbyOS binary, but it is not a normal native app:
it mounts the NFS export, execs the glibc-2.31 AD-9 launcher and dlopens
`UnityPlayer.so`. The Unity client's compat `libX11` has no X server behind it —
it writes `ESC ] X <w>;<h>~` to stdout and reads `ESC ] G …` from stdin — so
something must answer that request and own the pixels. Under IceWM that
something is **HBHOST.BIN**: it attaches a private 1920×1080 virtual display for
its child, plays the desktop's window protocol for that one window (it compiles
`desktop.c` itself), and blits the surface into an X window. A raw
`app RWLAUNCH.BIN` entry therefore has no display at all, which is why
`gen-icewm-menu.sh` lists RimWorld only as a hosted entry and excludes
`RWLAUNCH.BIN` from **All programs**.

The menu row is emitted at disk-build time by
`src/user/x11sess/gen-icewm-menu.sh` (called from
`src/user/x11sess/stage-disk.sh`) whenever `RWLAUNCH.BIN` is on the disk, and at
the top of `menu "Apps"` — before the pinned apps.

## 1. The command

```sh
export HOME=/home/sarah
cd ~/Documents/GitHub/HobbyOS-rimworld        # or any worktree of this branch

RW_QEMU_GL=1 RW_GLMODE=virgl \
RW_SCREEN=1280x720 RW_WM_HONOUR=1 \
  tools/rw/run-rimworld-desktop.sh --run-timeout 1000
```

What that does, in order: pins glibc 2.31, builds
`MODE=desktop DESKTOP=icewm` (the default) kernel + `disk.img`, rebuilds the
xcompat DSOs and the Mesa GL frontend, builds the AD-9 launcher and the native
`rwlaunch`, stages `/LIB64`, `/RWDSO`, `/RWMANAGED`, `/NFSEXPORT`, `/GLMODE`,
`/RWSCREEN`, `/WMSIZE` and `RWLAUNCH.BIN`, **regenerates `/ICEWM/menu`** so the
RimWorld entry exists on a disk whose menu was generated before the payload
landed, boots the GL device (`virtio-gpu-gl-pci`, `egl-headless`), and then
clicks **Start → Apps → RimWorld** through QMP.

`RW_SCREEN=1280x720 RW_WM_HONOUR=1` is what makes it a 1280×720 game window
rather than a full-screen one: the pair tells the compat X lib that the
client's whole world is 1280×720, so Unity's borderless-fullscreen container
asks HBHOST for that instead of for the 1920×1080 screen. Drop the pair and the
same command still works, but HBHOST grants the largest rect that fits the
IceWM work area (~1916×1014) and the game renders ~2.2× the pixels — slower,
and the window fills the screen.

Boot + click happen in the first ~60 s. The game's engine banner and
`GfxDevice` row follow a few minutes later; what it reaches after that, and
where it stopped on the last run, is §5.

Useful switches:

| variable | default | meaning |
|---|---|---|
| `RW_DESKTOP` | `icewm` | `hobby` is a compatibility shim for old evidence only (§8) |
| `RW_LAUNCH_VIA` | `icewm-menu` | `icewm-menu` = Start → Apps → label; `none` = boot, leave the menu to a human |
| `RW_ICEWM_LAUNCH` | `RimWorld` | Start-menu label to click |
| `RW_QEMU_GL` | `0` | `1` = virtio-gpu-gl + egl-headless + GL screenshots from guest RAM |
| `RW_GLMODE` | `softpipe` | `virgl` for the GPU path |
| `RW_SCREEN` | `1280x720` | pixel size the client asks HBHOST for (`/RWSCREEN`) |
| `RW_SKIP_BUILD` | `0` | `1` reuses `hobbyos.elf`/`disk.img` and the built launcher, re-stages the payload |
| `RW_SKIP_EXPORT` | `0` | `1` skips `tools/rw/nfs-export.sh sync` |
| `RW_X11_BUILD_DIR` | `third_party/x11/build/intel` | prebuilt X stack (else `third_party/x11/build.sh --stage all` runs) |

`RW_SCREEN` alone changes what the launcher asks Unity for; `/WMSIZE` (set by
`RW_WM_HONOUR=1`) is what makes the compat X lib report that as the client's
screen, so a borderless-fullscreen container asks for it instead of the real
1920×1080. Use the pair.

## 2. By hand: build the disk, then boot it

```sh
export HOME=/home/sarah
cd ~/Documents/GitHub/HobbyOS-rimworld

# 1. the image (first time also builds the X stack + IceWM)
TOOLS_SKIP=1 make -j8 ARCH=intel MODE=desktop DESKTOP=icewm hobbyos.elf disk.img

# 2. stage the game payload and boot (this is what the harness automates)
RW_SKIP_BUILD=1 RW_LAUNCH_VIA=none \
  tools/rw/run-rimworld-desktop.sh --run-timeout 900
```

With `RW_LAUNCH_VIA=none` the harness boots, screenshots and stops driving; the
VM stays up until `--run-timeout` and the Start menu is yours:

* **Start** is the leftmost button of the taskbar along the bottom edge.
* **Apps** is the first submenu; **RimWorld** is its first row.
* The window that appears is HBHOST's; its taskbar button is titled `RWLAUNCH`
  until Unity sets a title, and its `[HBHOST] RWLAUNCH.BIN pid=… window=0x…`
  row shows on the serial console.

A plain `make run` (nothing else) boots IceWM but does **not** have the game on
the disk: the payload is staged by the harness, not by the disk recipe.

## 3. Windowed vs headless

Both boot the same disk; only QEMU's display changes.

**Windowed** (watch it live, keyboard/mouse into the guest):

```sh
RW_LAUNCH_VIA=none tools/rw/run-rimworld-desktop.sh --run-timeout 1800
```

`make run`'s default `QEMU_DISPLAY=gtk` gives a QEMU window; the Start menu and
the game window are driven with the host mouse/keyboard. This is the only mode
where you can play; expect ~1 fps (§5).

**Headless** (automated; what the harness does):

```sh
RW_QEMU_GL=1 RW_GLMODE=virgl tools/rw/run-rimworld-desktop.sh --run-timeout 700
```

`-display egl-headless,gl=on`, QMP drives input, screenshots come from guest
physical memory (`pmemsave` of the kernel's `framebuffer`, `--fb-phys`), and the
ladder + `Player.log` are printed at the end. Without GL
(`RW_QEMU_GL=0`, the 2D device) QMP `screendump` works and the run is slower.

## 4. What the automated driver does

`tools/rw/rw_desktop_drive.py --launch-via icewm-menu`:

1. waits for `[XSESS] spawned /ICEWM.BIN`, then polls screenshots until the
   taskbar is painted (it is mapped after IceWM's first layout pass and font
   scan — seconds, not milliseconds);
2. finds the taskbar strip **in the screenshot** (the band of rows at the bottom
   that differ from the desktop), clicks its left end and logs
   `[ICEWM] click start=(x,y)`;
3. waits for the menu to be drawn (first open scans the font set), reads it with
   tesseract, clicks the `Apps` row and logs `[ICEWM] click apps=(x,y)`;
4. hovers/opens the submenu, reads the rows back, clicks the entry by its label
   and logs `[ICEWM] click rimworld=(x,y)`, then waits for the
   `[HBHOST] RWLAUNCH.BIN pid=…` row.

Every coordinate is discovered from a real screenshot; the OCR rows are printed
so the click can be audited against what was on screen. If tesseract is absent
the driver falls back to the generated menu file's own row order and arrow keys.

## 5. What was verified, and where it stops (L83, 2026-11-07)

Run: `RW_DESKTOP=icewm RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720
RW_WM_HONOUR=1 RW_LAUNCH_VIA=icewm-menu` (headless, virgl).
Screenshots: `obj/l83-evidence/`, serial: `obj/rw-desktop/serial-desktop.log`,
game log: `obj/rw-desktop/Player.log`.

| what | row / evidence |
|---|---|
| the entry exists, top of Apps | `/ICEWM/menu`: `prog "RimWorld" app HBHOST.BIN RWLAUNCH.BIN` (once; absent from All programs) |
| the menu shows it | `obj/l83-evidence/02-apps-menu-rimworld.png`; OCR: `xeyes … RimWorld BROWSER CONSOLE …` |
| the driver found and clicked it | `[ICEWM] taskbar rows y=1054..1079`, `[ICEWM] click start=(14,1066)`, `[ICEWM] click apps=(31,836)`, `[ICEWM] click rimworld=(225,730)` |
| HBHOST hosts the game | serial: `[X] [HBHOST] RWLAUNCH.BIN pid=5 window=0x800002` |
| the window is IceWM-managed | `obj/l83-evidence/03-hbhost-window.png`: title bar `RWLAUNCH`, taskbar button, 1280×736 client area |
| content rect granted | `Player.log`: `[XE] MAPGEOM ready=1 … x=0 y=16 w=1280 h=720` |
| the game is really running | `Initialize engine version: 2022.3.35f1`, `GfxDevice: creating device client; threaded=1`, `[RWMAIN] PlayerMain=… calling` |
| frames are produced | `[XSW] n=3870 swap=3870 … w=1280 h=720` (≈1 fps) |
| the window closes cleanly on exit | serial: `[RW] rwlaunch: done` → `[X] [HBHOST] RWLAUNCH.BIN exited, closing window` |

**Where it stops.** At ~360 s of presents the game's `Loading.AsyncRead`
thread takes a SIGSEGV:

```
[KERNEL] pid=30 (Loading.AsyncRe) memory fault VA=0x2F9D63 PC=0x108282D060
         rw=w in=HOLE region=NONE rsp=0x112B805800 … -> killed
[SIGDUMP] SIGSEGV code=1 addr=0x2f9d63 live=25 created=91 tid=30
[RW] rwlaunch: waitpid=7 status=24832
```

The GPU side was healthy at that moment — `[VIRGL3D] stats … peak_res=716
caps=4096/131072 peak_mib=145`, zero `res_attach refused` rows — so this is a
game-side pointer fault in the async loader (a new ladder, not the window path
and not a GL resource wall).

**Second defect, still open:** during the run the HBHOST window stayed on the
text tail it had before the pixel window arrived, while the client counted
3 870 presents at 1280×720 (`changed=0` for most of them, `put=17`,
`blit_n=0`). The app's frames reach the shim's shadow and its reported blit
rect (`blit=1280x720+0+16`) but the window never updates after the resize.
Instrumenting `blit_rect` against the virtual display's damage in HBHOST is
the next step; the launcher and the menu path are independent of it.

## 6. Caveats

* **Speed.** ≈1 fps at 1280×720 on virgl (`[XSW] n=3870 swap=3870` over
  ~360 s); 1920×1080-class surfaces are ~2.2× the pixels and slower. The first
  frames are HBHOST's text window showing Unity's `[UnityMemory]` rows until
  the client asks for its pixel window.
* **One QEMU per `disk.img`.** The image is a single raw file with the game's
  `Player.log` written into it; two boots of the same worktree corrupt each
  other. Stop a run with `fuser -k disk.img` (never `pkill -f qemu-system` — it
  would kill other lanes).
* **NFS.** The game data is served read-only from `/srv/nfs/rimworld-<lane>`
  (`tools/rw/lane.sh`); the guest mounts exactly the path in `/NFSEXPORT`. A
  stale export or a second lane pointed at the same directory is the classic
  "wrong binary/wrong data" failure; `tools/rw/nfs-export.sh sync` re-exports.
* **Window size.** The client asks for 1280×720 (`/RWSCREEN`, `RW_SCREEN`);
  HBHOST resizes its X window to that plus its 18 px menu bar and IceWM
  decorates it. `/WMSIZE` is a DESKTOP.BIN-only switch and does nothing here —
  HBHOST always honours the client's request.
* **GL mode.** `/GLMODE` is `softpipe` unless `RW_GLMODE=virgl`; a virgl run
  needs `RW_QEMU_GL=1`, otherwise the GL device is absent and the client falls
  back or refuses.
* **Steam modal.** "Could not initialize Steam API" appears during loading and
  clears itself in ~30 s; no click is required.
* **Evidence lives on the host:** `obj/rw-desktop/shots/*.png`,
  `obj/rw-desktop/serial-desktop.log`, `obj/rw-desktop/Player.log` (copied out
  of `::/RW_HOME/Player.log`).

## 7. Why the entry is hosted (HBHOST contract)

HBHOST is not a shim around the old desktop: IceWM's menu uses it for **every**
native app (`HBHOST.BIN <NAME.BIN>`), and the game is just the largest client it
hosts. For a pixel client HBHOST:

* attaches a memfd the size of the screen as a virtual display (`SYS_VDISPLAY`);
* answers the app's `ESC ] X <w>;<h>~` request by resizing its X window to
  `<w>×(<h>+18)` and switching that window to pixel mode;
* blits the damaged rectangle of the private surface with `XPutImage`;
* forwards keys as the desktop's own byte encodings (and raw events for a game),
  mouse presses with window-local coordinates, and `WM_DELETE_WINDOW` as the
  desktop's `ESC [ D ~`.

A window-size or resize defect in that path is an HBHOST bug, not a desktop
bug: HBHOST outlives `DESKTOP=hobby`.

## 8. Deletion checklist: `DESKTOP=hobby`

`RW_DESKTOP` still accepts `hobby` so old evidence can be reproduced, but that
desktop is being removed. Nothing below is to be *fixed* — it is the list of
places that name, build, boot or assume the native desktop, so the removal is a
checklist rather than an excavation.

**Harness / driver**

| file:line | what it does |
|---|---|
| `tools/rw/run-rimworld-desktop.sh:87` | `RW_DESKTOP` default (`icewm`), `hobby` accepted (89-92) |
| `tools/rw/run-rimworld-desktop.sh:96-105` | `RW_LAUNCH_VIA` default per desktop + validation |
| `tools/rw/run-rimworld-desktop.sh:168` | build: `MODE=desktop DESKTOP="$RW_DESKTOP"` |
| `tools/rw/run-rimworld-desktop.sh:563` | boot: `MODE=desktop DESKTOP="$RW_DESKTOP"` |
| `tools/rw/run-rimworld-desktop.sh:585-587` | `--desktop hobby --launch-via hobby-menu --app RWLAUNCH.BIN` drive args |
| `tools/rw/rw_desktop_drive.py:472` | `--desktop` choices (`hobby`, `icewm`) |
| `tools/rw/rw_desktop_drive.py:521-522` | `launch_via` default = `hobby-menu` for a hobby disk |
| `tools/rw/rw_desktop_drive.py:555-566` | the `[MENU] count=` wait (the only hobby-only entry point) |
| `tools/rw/rw_desktop_drive.py:589-596` | the F1/End/Up×N/Enter menu walk |
| `tools/rw/rw_desktop_drive.py:143` | `menu_map()` — parses the native desktop's `[MENU] i=NAME` dump |

**Gates and tests that pass `DESKTOP=hobby` explicitly**

| file:line | what it is |
|---|---|
| `tools/gate/m1_virgl.py:76` | build args |
| `tools/gate/m2_virgl.py:167`, `:392` | build / run |
| `tools/gate/m3_virgl.py:148`, `:375` | build / run |
| `tools/gate/m3_window_probe.py:163`, `:328` | build / run (the ESC ] X host is DESKTOP.BIN) |
| `tools/gate/m3_x11_client.py:132`, `:293` | the M3 gate: `[MENU]` + F1 menu walk |
| `tools/gate/m4_gl_probe.py:193`, `:328` | the M4 GL gate |
| `tools/gate/m7_perf.py:172`, `:370` | perf harness |

**Other tools / tests**

| file:line | what it is |
|---|---|
| `run_xeyes_test.py:248` | boots `DESKTOP=hobby` to drive XEyes |
| `run_nano_test.py:286`, `:339` | boots `DESKTOP=hobby` |
| `run_console_test.py:136` | boots `DESKTOP=hobby` |
| `capture_screenshots_hobby.py:2-6`, `:139` | screenshots of the native desktop |
| `tools/perf_game_cycle.py:245` | game-cycle perf, `DESKTOP=hobby` |
| `tools/evidence_rig.py:304` | evidence rig, `DESKTOP=hobby` |
| `tools/rw/run-guest-walk.sh:36` | guest walk build, `DESKTOP=hobby` |
| `tools/rw/rw_flow.py` (`CLOCK = (1780,1030)` at `:47`) | front-end click map in the native desktop's full-screen 1920×1080 frame; under IceWM the game lives in a window, so these coordinates need the window-rect transform |

**Build system / kernel**

| file:line | what it does |
|---|---|
| `Makefile:106` | `DESKTOP ?= icewm` for `MODE=desktop` (`:108` keeps `hobby` for test modes) |
| `Makefile:252` | `DESKTOP` validation regex `hobby|icewm` |
| `Makefile:447-449` | `DESKTOP=icewm` → `-DKERNEL_DESKTOP_ICEWM` (else the kernel launches DESKTOP.BIN) |
| `Makefile:2414-2416` | `ifeq ($(DESKTOP),icewm)` → HBHOST.BIN is a disk prerequisite |
| `Makefile:2422-2424` | `ifeq ($(DESKTOP),icewm)` → XSESSION + the X stack are disk prerequisites |
| `Makefile:2765-2768` | the `stage-disk.sh` call inside the disk recipe (icewm only) |
| `Makefile:2393` | an error message that suggests `make DESKTOP=hobby run` as the fallback |
| `src/user/x11sess/stage-disk.sh:6` | comment: "called … after every DESKTOP=hobby file is already on the disk" |
| `src/kernel/main.c` | the `KERNEL_DESKTOP_ICEWM` split: `DESKTOP.BIN` vs `XSESSION.BIN` |

**Docs**

| file:line | what it says |
|---|---|
| `README.md:125`, `:146`, `:195-206` | `DESKTOP=hobby` described as an option |
| `icewm.md:74`, `:86` | M6 history: the two desktop sessions |
| `docs/rimworld-m3-xclient.md:17` | "the same session the M3/M4 gates use" (`DESKTOP=hobby`) |
| `docs/rimworld-launch-log.md:685` | the ladder's build row still says `MODE=desktop DESKTOP=hobby` |
| `files.md:178`, `:219` | the file inventory's hobby/icewm split |
