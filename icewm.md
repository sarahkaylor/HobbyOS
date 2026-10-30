# IceWM on HobbyOS — the default desktop environment (plan v1, 2026-10-30)

Status: **v1 integrated (2026-10-30)** on `icewm-port`: M0-M6 done on intel, M7 done on ARM (TCG boot + same
checklist). Interfaces frozen in §4 (M0). Owner: the swarm coordinator. Lanes in §7. Status table: §3a.
Goal: `make run DESKTOP=icewm` boots HobbyOS into a real X11 session (an X.Org-derived X server
with a HobbyOS DDX), managed by an unmodified-as-possible **IceWM** (https://github.com/bbidulock/icewm),
with a taskbar, start menu, and at least one real X client app. Plain `make run` (the
`DESKTOP=hobby` default) must not change at all: same binaries, same disk contents, same behaviour.

## 1. Why this shape (selection record)

IceWM is a **window manager**: it needs `SubstructureRedirect` on the root, `ReparentWindow`,
properties/atoms (ICCCM + EWMH), grabs, cursors, and other *processes'* windows. HobbyOS today has:

| Have (anchored) | Gap |
|---|---|
| `src/user/x11/` — a client-side Xlib *shim* on the desktop's pixel-window protocol (README: "one window per process", no server) | No X server, no wire protocol, no multi-client windows → cannot host a WM |
| AF_UNIX `socketpair` + SCM_RIGHTS + `poll` (`src/kernel/unix.c`, docs/browser/p4-ipc-design.md) | **No bind/listen/accept/connect-by-name** (p4 §2.4 D4: pathless only; a 16-entry name table was sketched there, not built) |
| `SYS_MAP_FB` / `SYS_FLUSH_FB_RECTS` / `SYS_FB_IOCTL` (1920x1080 B8G8R8A8, docs/linux-abi.md §9) | — |
| evdev records on the native track: `SYS_INPUT_IOCTL` / `SYS_INPUT_READ` (`src/kernel/input_node.c`) | x86 vmmouse absolute pointer may only feed desktop.c — **verify** it reaches the evdev node |
| libc, libc++ (`-fno-exceptions`, RTTI available), threads, fork/exec/waitpid, signals, mmap/memfd | Misc libc gaps for X.Org code — found by compiling |
| Cross-build recipes for C deps (`~/webkit-hobbyos/HobbyOS/scripts/wk2-libs-cross.sh`, `toolchain-hobbyos.cmake`); vendored freetype, zlib, libpng, libjpeg-turbo, expat? (**verify**), DejaVu fonts (`third_party/fonts/dejavu-2.37`) | No meson cross file yet |

Options considered for the X server:

| Option | Verdict |
|---|---|
| **X.Org xserver (21.1.x) with a new DDX `hw/hobbyos` derived from `hw/vfb` (Xvfb)** — dix/mi/fb/Render/Composite/Damage/XFixes/RandR come for free; the DDX is ~1-2 kLOC (fb mmap + flush rects + evdev input) | **PICK.** Protocol correctness is the biggest risk for a WM; reusing dix removes it. |
| From-scratch mini X server speaking the subset IceWM uses | Rejected: IceWM hard-requires Xrender/Xcomposite/Xdamage/Xfixes/Xcursor client libs and exercises a wide protocol surface; correctness debt would be unbounded. Fallback only if the xserver port stalls > 2 integration rounds. |
| Kdrive/Xfbdev | Rejected: removed upstream (Xfbdev gone since 1.20/21.1). |
| Extend the in-process shim | Rejected: cannot manage other processes' windows. |
| Run Linux musl binaries under the x86 compat layer | Rejected: x86-only (ARM is primary; two-arch parity rule). |

IceWM build pins (its `src/CMakeLists.txt`): REQUIRED x11, xext, xfixes, xdamage, xcomposite, xcursor,
xrender. We choose: `CONFIG_XPM=on CONFIG_LIBPNG=on CONFIG_LIBJPEG=on CONFIG_IMLIB2=off
CONFIG_GDK_PIXBUF_XLIB=off CONFIG_LIBRSVG=off CONFIG_NANOSVG=off CONFIG_I18N=off ENABLE_NLS=off
CONFIG_SESSION=off CONFIG_XRANDR=on CONFIG_XFREETYPE=on (Xft) CONFIG_COREFONTS=on (fallback)
HAVE_XRES=off`, no sound (icesound off), no fribidi, no Xinerama required (build it anyway, cheap).
IceWM uses `throw/catch` only in `icesh.cc`, `apppstatus.cc`, `fdomenu.cc` — patch or drop those
(libc++ is `-fno-exceptions`); 17 `dynamic_cast/typeid` sites need RTTI (`-frtti`; libcxx.a carries
the RTTI closure, l3-rtti lane).

## 2. Architecture

```
 kernel (KERNEL_DESKTOP_ICEWM)  ──launches──▶  XSESSION.BIN  (native C session launcher, src/user/x11sess/)
                                                 ├─ spawns XHOBBY.BIN :0 -nolisten tcp ...  (X server)
                                                 ├─ waits until "/tmp/.X11-unix/X0" accepts
                                                 ├─ spawns ICEWMBG.BIN, ICEWM.BIN (DISPLAY=:0)
                                                 └─ optionally a demo client (XEYESR.BIN ...)
 XHOBBY.BIN = X.Org dix/mi/fb/render/composite/damage/xfixes/randr + hw/hobbyos DDX
   output: SYS_MAP_FB shadow → SYS_FLUSH_FB_RECTS on damage (block handler)
   input : SYS_INPUT_READ evdev (keyboard + rel/abs pointer) → dix; keymap from a host-precompiled .xkm
   fonts : libXfont2 built-ins ("fixed", "cursor") — no font files needed for core fonts
 clients link statically: libX11 (libxcb, Xau), Xext, Xrender, Xfixes, Xdamage, Xcomposite,
   Xcursor, Xrandr, Xinerama, Xpm, Xft + fontconfig + expat + freetype
 transport: AF_UNIX stream, name "/tmp/.X11-unix/X0" (path form AND abstract form both accepted, §4.1)
```

The existing HobbyOS desktop (`DESKTOP.BIN`) is **not launched** in the IceWM session — the X server
owns the framebuffer and input. Existing shim-based apps (XCALC/XEYES/...) are untouched; real-Xlib
rebuilds get new names (§4.3).

## 3. Milestones and gates

| M | What | Gate (runnable) | Evidence |
|---|---|---|---|
| M0 | This doc; frozen interfaces §4; lane worktrees | doc committed on `icewm-port` | commit |
| M1 | Named AF_UNIX in kernel + libc | kernel `unix_test_suite` named-socket cases pass (`make ARCH=intel MODE=unit_tests run` per hobbyos-run-tests); host tests green | test log lines |
| M2 | Cross-built X client stack (both arches) | `third_party/x11/build.sh --arch arm|intel --stage clientlibs` exits 0; `nm` shows `XOpenDisplay`, `XftDrawStringUtf8`; a smoke client links | build log + `ls prefix/lib` |
| M3 | XHOBBY.BIN builds (both arches) | `build.sh --stage server` exits 0; flat .BIN produced | build log, size |
| M4 | IceWM builds (both arches) | `build.sh --stage icewm` → ICEWM.BIN, ICEWMBG.BIN | build log |
| M5 | Session + build integration | `make ARCH=intel DESKTOP=icewm disk.img` puts all bins+data on disk; `make ARCH=intel disk.img` (default) is byte-for-byte unchanged in its file list | `mdir` listings both ways |
| M6 | **Boots to IceWM on intel** | `make run ARCH=intel DESKTOP=icewm` (and the headless test target) shows IceWM taskbar + start menu; clicking start opens the menu; a client window can be launched, moved, focused, closed | QMP screenshots + serial log |
| M7 | ARM parity | same on `ARCH=arm` (TCG; slow). If host QEMU aarch64 is broken (module mismatch, see notes) the ARM *build* gate still applies and the boot is recorded as blocked | build log / screenshots |

## 3a. Status (integration, 2026-10-30)

| M | Status | Evidence (under `~/hobbyos-icewm-lanes/evidence/`) |
|---|---|---|
| M1 | done (lane kern) | unit tests 379/379 intel + arm after integration (`unit-tests-{intel,arm}.log`) |
| M2 | done (xlibs-core, xlibs-ext) | `build-intel-shared*.log`, `build-arm-shared.log` |
| M3 | done (xserver) | same build logs (XHOBBY.BIN both arches) |
| M4 | done (icewm) | same build logs (ICEWM/ICEWMBG/ICESH/ICEWMHNT + XEYESR/XCALCR) |
| M5 | done | `default-{main,integ}-intel.mdir.txt`: same 911 file names; size deltas all explained (§8); kernel compile flags identical (`kernel-flags-*`) |
| M6 | done, intel | `m6-intel-headless-*` (QMP) and `m6-intel-gtk-*` (GTK window + host XTEST): taskbar, start-menu click, XCalc from menu, typing reaches XCalc, title-bar drag, click-to-focus, close via Kill Client, XEyes from toolbar, Logout -> clean server exit -> DESKTOP.BIN fallback; `make icewm_test` PASS; a bounded `make run DESKTOP=icewm` reached `[XSESS] session running` (`make-run-intel-icewm.log`); a fresh worktree at HEAD built the whole X stack in-tree (default build dir) and passed `run_icewm_test.py` (`fresh-intel-icewm-build.log`, `icewm_test_intel_fresh.log`) |
| M7 | done, arm (TCG) | `m7-arm-*`: the same checklist, all steps pass; host qemu-system-aarch64 ran fine headless |

Known gaps: see §8 "open items".

## 4. Frozen interfaces (single writer each)

### 4.1 Named AF_UNIX (writer: lane `kern`)
- `socket(AF_UNIX, SOCK_STREAM[|SOCK_NONBLOCK|SOCK_CLOEXEC], 0)`, then `bind`, `listen(backlog)`,
  `accept`, `accept4(flags)`, `connect`, `getsockname`, `getpeername`, `shutdown(how)`,
  `getsockopt(SO_ERROR/SO_TYPE/SO_PEERCRED-zeroed)`; Linux `struct sockaddr_un` layout (family u16 +
  108-byte path). Names live in a **kernel name table, never the filesystem** (FAT16 has no sockets):
  both path names (`/tmp/.X11-unix/X0`) and abstract names (`sun_path[0]=='\0'`) are accepted and are
  distinct keys. `unlink()` of a socket path is a harmless no-op/ENOENT. Closing the listener frees the
  name. Accept queue ≥ 16; total pairs raised from 16 to ≥ 128 (X: one connection per client + server).
- Nonblocking connect/accept, `poll`/`select` readiness on listeners (POLLIN = pending connection) and
  EOF/HUP on peer close must behave like Linux. `writev`/`readv`, `sendmsg/recvmsg` on these sockets.
- Syscall rows: appended after the current top (`SYS_MAX` = 95 in `src/include/syscall.h`); the `kern`
  lane owns the numbers; everyone else uses the POSIX libc names only. Libc wrappers + `<sys/socket.h>`,
  `<sys/un.h>` live in `src/libc` / `src/libc/include`.

### 4.2 Build layout (writer: lane `xlibs`, the `third_party/x11/` tree)
- `third_party/x11/` — pinned upstream tarballs + `.sha256` (house style: tarball committed or
  fetched-and-verified by `fetch.sh`; record URL + sha in `third_party/x11/README.md`), `patches/<pkg>/*.patch`,
  `cross/{meson-arm.ini,meson-intel.ini,toolchain-hobbyos-c.cmake}`, and `build.sh`:
  `build.sh --arch arm|intel [--prefix DIR] [--stage clientlibs|server|icewm|all] [-j N]`.
- Default prefix `third_party/x11/build/<arch>/prefix` (gitignored). Lanes share
  `~/hobbyos-icewm-lanes/prefix/<arch>` via `--prefix` so nobody rebuilds others' stages.
- Final flat binaries land in `third_party/x11/build/<arch>/bin/` with their on-disk 8.3 names (§4.3);
  data in `third_party/x11/build/<arch>/disk/` mirroring the disk tree (§4.4). The `server` stage is
  written by lane `xserver`, the `icewm` stage by lane `icewm` (each owns its own function/section of
  build.sh, or a sourced `build-<stage>.sh`, to keep merges trivial).
- Program link model = the repo's: static, `-T src/user/linker.ld` (v2 base), crt0.o + libc.a
  (+ libcxx.a for C++), `objcopy -O binary`. Large binaries are fine (WebKit precedent).

### 4.3 On-disk binaries (FAT 8.3 uppercase, disk root)
`XSESSION.BIN` (session launcher, `session` lane), `XHOBBY.BIN` (server), `ICEWM.BIN`, `ICEWMBG.BIN`,
optional `ICEWMHNT.BIN`/`ICESH.BIN`; real-Xlib demo clients `XEYESR.BIN`, `XCALCR.BIN` (the existing shim
apps' sources rebuilt against real libX11 — owned by `session` lane, built via build.sh `clients` sub-stage).

### 4.4 On-disk data tree (`DESKTOP=icewm` only)
- `/X11/US.XKM` — keymap compiled on the host (`xkbcomp -xkm` of evdev rules, pc105/us); the DDX loads
  it directly instead of exec'ing xkbcomp. `/X11/FONTS/` DejaVu TTFs + `/X11/FONTS.CNF` (fontconfig,
  cache dir `/TMP/FCCACHE` or caching disabled). Fontconfig must be told the config path via
  `FONTCONFIG_FILE` env from XSESSION.
- `/ICEWM/` = IceWM LIBDIR/CFGDIR (themes trimmed to the default + 1, `preferences`, `menu`,
  `toolbar`, `programs` listing HobbyOS X clients). If FAT LFN is not supported by the kernel, IceWM
  paths/theme names must be patched to 8.3 — **verify LFN first** (`lfn_test` exists).
- `/TMP/` exists (X sockets are not files, but xtrans `mkdir`s `/tmp/.X11-unix`; make it succeed or patch).

### 4.5 Build knob (writer: lane `session`)
- `DESKTOP ?= hobby` in the Makefile. `DESKTOP=icewm` ⇒ kernel `-DKERNEL_DESKTOP_ICEWM` (main.c launches
  `XSESSION.BIN` instead of `DESKTOP.BIN` in the default desktop branch), disk.img gets §4.3/§4.4
  content, a stamp file (like `MODE_FILE`) forces rebuild on knob change. `make run DESKTOP=icewm`
  builds the X stack from source if `third_party/x11/build/<arch>/bin/` is missing (calls build.sh).
  `X11_BUILD_DIR` override lets lanes point at a shared build. Optional test target
  `make icewm_test` / `run_icewm_test.py` (headless, QMP screenshots, pixel checks).
- `DESKTOP=hobby` path: zero diff in disk contents and kernel behaviour.

## 5. Constraints every lane honours
- Kernel code: hobbyos-kernel-constraints skill (no unaligned access on ARM, no SIMD, `-mgeneral-regs-only`).
- **30-second deadlock law: a hang is a defect.** Always run QEMU with a timeout; stop only your own VM.
- ARM is primary: every stage must build for `arm` and `intel`. Iterate on intel (KVM, fast).
- Local commits only, on your lane branch. **Never push, never open PRs.** Trailer:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Never edit `~/Documents/GitHub/HobbyOS` (the user's main checkout) or other lanes' worktrees.
- Upstream sources: vendored byte-identical from pinned tarballs; all changes as `patches/` files.

## 6. Risks / verify items
- xserver os/ layer: `getpwuid`, `uname`, `gethostname`, `sigaction`, `setitimer` (use `-dumbSched`),
  input thread (build with `-Dinput_thread=false`), MIT-SHM (off), XDMCP/XCSECURITY (off), `xkbcomp`
  exec (replaced by `.xkm` load), `/dev/urandom`.
- meson cross-compiling to a bare `*-none-elf` triple: use `system = 'linux'`-ish cross props only if
  needed for feature tests; prefer explicit `-D` and patched `meson.build` checks.
- libxcb codegen needs host python + xcb-proto (host-side only).
- IceWM runtime: `fork/execvp` of menu commands (does HobbyOS have `/bin/sh`? SH.BIN — map
  `execvp("sh", "-c", ...)` or patch IceWM's launcher), `/proc` reads (tray CPU/mem/net applets — disable),
  `gettimeofday`, `setlocale`, `iconv` (I18N off).
- Performance: fb shadow + damage flush; 1920x1080x32 on TCG ARM will be slow — acceptable for M7.
- Socket pool sizing (p4: 16 pairs) — raised in §4.1.

## 7. Lanes

Root: `~/hobbyos-icewm-lanes/` — `integ/` = integration worktree (branch `icewm-port`), `lanes/<lane>/os`
= your worktree (branch `lane/<lane>`, created from `icewm-port`), `prefix/<arch>` shared install prefix,
`tools/` VM helpers copied from the browser swarm (`boot.sh <lane> <arch> [gtk] [disk]`, `stop.sh`,
`q.py` QMP shot/click/type/key, `x.py`/`xvfb.sh` for GTK+XTEST). Run dir: `lanes/<lane>/run-<arch>/`.

| Lane | Owns | Depends on | DoD |
|---|---|---|---|
| `kern` | `src/kernel/unix*.c`, `fs.c` socket paths, `src/include/syscall.h` rows, `src/libc` socket wrappers + headers, kernel/host tests | — | M1 gate, both arches build |
| `xlibs` | `third_party/x11/` scaffold, cross files, `build.sh` clientlibs stage, libc gap fixes needed by client libs (non-socket), fontconfig/Xft/Xpm | — (codes against §4.1 POSIX names) | M2 gate both arches; libs in shared prefix |
| `xserver` | build.sh `server` stage, `third_party/x11/patches/xserver/`, the `hw/hobbyos` DDX source (`third_party/x11/ddx/`), pixman/libXfont2/fontenc, `.xkm` generation | `xlibs` (proto/xtrans/Xau in prefix) | M3 gate both arches |
| `icewm` | build.sh `icewm` stage, `patches/icewm/`, `/ICEWM/` data tree (themes, prefs, menu) | `xlibs` | M4 gate both arches |
| `session` | `src/user/x11sess/` XSESSION.BIN, Makefile `DESKTOP` knob + disk rules, `src/kernel/main.c` launch branch, real-Xlib demo clients, `run_icewm_test.py`, README section | — (stub-tolerant: missing X bins = clear error) | M5 gate (with stubs), default build unchanged |
| `integ` | merging lanes into `icewm-port`, end-to-end bring-up fixes, M6/M7 | all | M6 (+M7 or recorded blocker) |

## 9. Native HobbyOS apps under IceWM: HBHOST + the virtual display (lane apps, 2026-10-30)

Goal: every program the default desktop offers (its Apps / right-click menu) launches from
IceWM's start menu and works in a normal IceWM-managed window.

Why it needs more than menu entries: native apps don't speak X.  Text apps print text/ANSI
on stdout and read the desktop's escape-encoded input on stdin; pixel apps (the shim-Xlib
XCALC/XEYES/ANTFARM/XNIBBLER, BROWSER) map the framebuffer (SYS_MAP_FB) and paint their
content rect; the games map it, paint the whole screen and also read raw input through
SYS_GET_EVENTS -- the same kernel ring XHOBBY's DDX drains.  Run directly they would
scribble over the X screen and steal the server's input.

**Kernel: per-process virtual display** (`src/kernel/vdisplay.c`, `src/include/vdisplay.h`,
row `SYS_VDISPLAY` = 103, both arches).  `VDISP_OP_ATTACH(memfd)` attaches a private
screen-sized surface (same geometry as the real fb, so SYS_FB_IOCTL is unchanged) and a
private input queue to the caller; children spawned/forked afterwards inherit it (PCB
fields `vdisp_id/gen`, `vdisp_own/gen`, appended at the end of `struct process`).  For an
attached process SYS_MAP_FB maps the memfd at the usual USER_FB slot (`vm_map_fb_obj`),
SYS_FLUSH_FB(_RECTS) record a damage union + sequence number and are paced to ~60/s
(VDISP_FRAME_MS; a virtual present is otherwise free and loop-paced games ran unbounded),
FBIOBLANK is a no-op, and SYS_GET_EVENTS/SYS_INPUT_READ read the private queue, which the
host fills with `VDISP_OP_INJECT`; `VDISP_OP_STATUS` returns damage/flush/map/read
counters.  A display dies with its owner (generation check; slot reclaimed on the next
attach) or on DETACH.  Unattached processes -- XHOBBY, the default desktop -- are untouched.
Tests: kernel `vdisplay_test_suite`, host `vdisplay_test_run`.

**HBHOST.BIN** (`src/user/hbhost/`, real libX11; built by build.sh's icewm `hbhost`
sub-step, which `make` re-runs when the shared desktop sources change):
`HBHOST.BIN <PROGRAM.BIN> [args]` attaches a virtual display, spawns the app with
stdin/stdout pipes and plays the desktop for that one window.  `hbhost_desk.c` compiles
the default desktop's own `desktop.c` (unchanged; `main` renamed, `_start` compiled out by
`DESKTOP_TEST_WRAPPER`) plus `window.c`/`graphics.c`, so the escape parser, terminal mode,
pixel-mode geometry/expose messages, menus, key byte mapping (+ `ESC [ K` mods), mouse
P/G/R (cells or pixels), wheel, `ESC [ T` tracking and `ESC [ D` are the desktop's code
paths, painting into the virtual surface.  Its one window sits at (-2,-18) so the desktop
title bar and side frame are off-surface (IceWM draws the frame); the menu bar is the top
16 rows of the X window, dropdowns are drawn with Xlib.  `hbhost.c` blits damaged surface
rects into the X window with XPutImage (no MIT-SHM), maps X events to the desktop's
encodings (X keycode - 8 = evdev code), sets WM_NAME/_NET_WM_NAME from `ESC ] T`, resizes
to a pixel app's `ESC ] X` preference, and turns `ESC ] R` into another HBHOST window.  A
fullscreen framebuffer app (maps + flushes without `ESC ] X`: the games) switches to a game
view: the whole surface scaled into the window (960x540 to start, resizable).  Keys: presses
go to stdin in the desktop's encoding (under the desktop the desktop "usually wins" the raw
ring and every game drains stdin for it); key releases and byte-less presses (Ctrl/Alt/
Shift) are injected into the private raw queue when the app reads it (DOOM's key-up).
WM_DELETE_WINDOW / F4: pixel and game windows get `ESC [ D` and 250 ms before the kill, text
windows are killed at once (the desktop's rules); the app's exit closes the window.

**Menu** (`src/user/x11sess/gen-icewm-menu.sh`, run by `stage-disk.sh` at `disk.img` time):
`/ICEWM/menu` = Apps (`pinned_apps[]` read out of desktop.c, in that order), Games
(`pinned_games[]` + `/GAMES`), All programs (every other root `.BIN` on that disk, sorted,
submenus of 24), then the real-X XEyes/XCalc; `/ICEWM/toolbar` = CONSOLE, FILES, XEyes,
XCalc.  Only files present on the disk are listed; X-stack binaries are left out.  (The
default desktop itself shows pinned apps in directory order, not `pinned_apps[]` order.)

Verified (intel, KVM; screenshots under `~/hobbyos-icewm-lanes/evidence/apps/`): all 16 Apps
entries launch and render; CALC computes 2+3 by clicks; FILES lists, navigates and its
menu-bar `Go > Root` works; CONSOLE runs `ls`, `ls /home`, `HELLO.BIN`, `seq`; NANO edits
(terminal mode); CLOCK/SYSMON/HEX/TASKS/FIND/DIFF/NOTES/UNIT render; XCALC computes via
clicks; XEYES follows the pointer; ANTFARM animates around the pointer; BROWSER renders
HOME.HTM, follows a link and scrolls with the wheel.  Games: PONG, MILLIPED, STARDUST, NES
(game view, keys move/act), XNIBBLER and XCRYPT (pixel/terminal windows, keys work); DOOM
starts and exits "No IWAD file was found" (no WAD on the disk, same as the default
desktop).  After the games exit IceWM keeps the mouse and keyboard.  ARM (TCG): boots to
IceWM, CONSOLE runs `ls /home`, XCALC computes 2 ENTER 3 + by clicks.
`run_icewm_test.py` now launches CONSOLE and XCALC from the start menu (PASS, intel).

Known limits: text apps open 800x560 (resizable; the desktop tiles them full-screen);
`ESC [ T` tracking reports window-relative coordinates clamped at 0 (pointer left/above
the window reads as 0); the game view scales with nearest-neighbour; a game window may need
a click before IceWM gives it the keyboard; the first start-menu open takes a few seconds
(fontconfig scan, pre-existing).

## 10. IceWM becomes the default desktop (2026-10-30)

After hands-on testing, IceWM replaced the native desktop as the default for `make run`.
- `Makefile`: `DESKTOP ?= icewm` when `MODE=desktop` (the interactive boot), `DESKTOP ?= hobby`
  for every other `MODE`. The original desktop is `make run DESKTOP=hobby`.
- Test tiers keep their old behaviour: `test`, `unit_tests`, `desktop_test`, `apps_test`,
  `pong_test`, `filedialog_test`, `soak`, `selfhost` etc. recurse with their own `MODE`, so they
  resolve to `hobby` and never need the X stack (CI unchanged).
- The QMP harnesses that drive the native desktop through `make run` (`run_{antfarm,games,nes,
  console,xeyes,files_nav,xcalc,desktop_apps,nano}_test.py`, `capture_screenshots.py`,
  `tools/evidence_rig.py`, `tools/perf_game_cycle.py`) pass `DESKTOP=hobby` explicitly.
- `DESKTOP.BIN` stays on every disk: XSESSION.BIN still falls back to it, and IceWM's Logout
  still drops to it.
- The §0 rule "plain `make run` must not change" is retired; `DESKTOP=hobby` is now the
  byte-identical-to-before configuration.

## 8. Fix log (newest first)
- 2026-10-30 lane apps (branch `icewm-apps`, §9):
  - **virtual display + HBHOST.BIN + generated start menu**: see §9.
  - **kernel (x64) waitpid lost wake-up** (pre-existing, also on the default desktop with the
    unmodified kernel): schedule() re-saves a WAIT_CHILD caller's context from the live frame
    and `reap_deliver_locked` matched the wanted pid in `context[0]`, which on x64 became rax
    = the syscall number; a parent that parked before its child exited was never woken (the
    shell hung after its first external command).  The frame's return register now carries
    the wanted pid too (ARM's x0 already did).
  - **kernel (x64) CPL0 resume on an unmaterialized user-stack page -> triple fault**: the x64
    resume runs on the user stack (frame at sp-320, RSP at sp-616, a C call from there);
    exec'd images with a bigger environment (the IceWM session's) and forked children (IceWM's
    fork+exec of HBHOST) could start on a demand-zero page -> #PF at CPL0 with no usable
    stack -> #DF -> reset (TCG `-d int`: `v=0e cpl=0 SP=17fffff000 CR2=17ffffeff8`).  The exec
    path now pre-faults the stack top like spawn (GB-068), and schedule() vm_touch()es the
    page window below a user SP before every x64 user resume.
  - **intel boot**: Limine's multiboot1 loader panicked "Cannot allocate elf sections" once
    the kernel's DWARF grew past what it can place (any kernel growth hits it); `disk.img`
    now boots `obj/<arch>/hobbyos.boot.elf` (`--strip-debug`, same loadable segments) and
    `hobbyos.elf` keeps the symbols.
  - unit tests 383/383 intel, 379/379 arm (+ the 4 vdisplay tests); host `vdisplay_test_run`.
  - open: the default desktop lists DESKTOP=icewm's X binaries (pre-existing); text apps'
    initial size is fixed; DOOM needs a WAD on the disk to show anything.
- 2026-10-30 integ (M5-M7), all on `icewm-port`:
  - merged lane/kern, xlibs-core, xlibs-ext, xserver, icewm, session (no conflicts); removed the
    transitional `cross/compat` socket shims and XSESSION's inline-asm waitpid.
  - **kernel (x64)**: `sys_waitpid`/`sys_wait4` rewrote the user trap frame's regs[1..3]; regs[1] is the
    caller's rbx, so every waitpid returned (and every blocking wait resumed) with rbx = &status.  Now
    `process_wait_args()` takes explicit args and the blocking status pointer lives in a new PCB field
    `wait_status_va` (not context[1]).
  - **kernel (both arches)**: `sys_input_read`, `sys_input_ioctl`, `sys_fb_ioctl` treated
    `sys_user_range()==1` (OK) as failure.  Fixed; the DDX keeps its GET_EVENTS fallback.
  - **XSESSION stdio** (root cause of the "IceWM hangs after Kill" and the freeze when dragging a title
    bar): XSESSION started with no fds, so IceWM's X socket became fd 2 and its stderr warnings went into
    the X protocol stream.  XSESSION now pins fd 0 to /dev/null and fds 1/2 to a pipe it relays to the
    serial console as `[X] ...`; resets 0-2 before the DESKTOP.BIN fallback.  Pipes ignore O_NONBLOCK on
    HobbyOS, so the relay reads once per readable poll.
  - **libc**: `sigaction` honoured a caller's garbage `sa_restorer` (X.Org's `OsSignal` leaves it
    uninitialised), so XHOBBY's SIGTERM handler "returned" into a random function.  Now Linux semantics:
    the libc trampoline unless `SA_RESTORER` is set.
  - **DDX**: `hbCloseScreen` freed the screen pixmap before shadow's CloseScreen unregistered its damage
    (crash in `DamageUnregister` on every server exit).  Server now exits 0 at logout.
  - **build.sh**: XHOBBY/IceWM final-link stamps now include a hash of crt0.o/libc.a/libcxx.a/linker.ld,
    so libc fixes relink them.
  - **tests**: `run_icewm_test.py` real-stack mode checks the taskbar, a start-menu click, and no X
    process memory fault.  The "click lands at (0,0)" seen by lane icewm was the test driver: on intel the
    vmmouse reports QEMU's absolute position, so drive with absolute events (`q.py abs`), not PS/2 `goto`.
  - default build: same file list as main; binaries grew +848 B (kern's libc socket member), +16 B more
    in the 6 sigaction users, the kernel +24 KB; kernel compile flags identical.
  - open items: FAT readdir ignores the VFAT lower-case flag (IceWM patched around it); IceWM's upstream
    `FMONB` default font string lacks a comma (harmless warning); XEYESR/XCALCR lack WM_DELETE_WINDOW, so
    close goes through IceWM's Kill dialog; XCALCR.BIN was seen holding IceWM's X socket after
    fork/exec (before the stdio fix; whether libxcb marks its fd close-on-exec here is unverified); the HobbyOS Apps menu lists the X binaries after a fallback;
    xtrans warns that /tmp/.X11-unix is not mode 1777 (FAT has no modes).
- 2026-10-30 v1: plan written; swarm started.
