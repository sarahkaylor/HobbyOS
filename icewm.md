# IceWM on HobbyOS — secondary desktop environment (plan v1, 2026-10-30)

Status: **v1, executing.** Interfaces frozen in §4 (M0). Owner: the swarm coordinator. Lanes in §7.
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

## 8. Fix log (newest first)
- 2026-10-30 v1: plan written; swarm started.
