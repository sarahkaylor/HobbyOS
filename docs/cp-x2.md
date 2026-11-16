# cp/x2 — Gate P4: Wine on X11, windows, input and the X surface (lane C4-x2, wave 4)

Plan: `docs/cyberpunk.md` §6 P4 and §7.2d (C4-x2 row).  Groundwork:
`docs/cp-x11.md` (C4-x: the real X client libraries and the audit of
`XHOBBY.BIN`; its §3.1 is this lane's work list).  Every number here was
measured on **strix** (Ryzen AI MAX+ 395, Zen 5, KVM), with up to four guests
running at once (§7.4 slots) unless a row says otherwise.

| What | Where |
|---|---|
| X libraries + FreeType (build, stage, audit) | `tools/cp/x11/build.sh` → `$CP_X11_WORK` (lane run: `~/cp/x11-build-x2`) |
| fetch only approved rows | `CP_X11_ONLY="libXi ..." tools/cp/x11/fetch.sh` |
| Wine with X11/FreeType/Vulkan | `CP_WINE_VARIANT=x tools/cp/build-wine.sh` → `~/cp/wine-build-x` |
| raw-input / mode / fullscreen / hold probe (PE) | `src/user/cp/rawinput.c`, `tools/cp/p4/build-rawinput.sh` |
| kernel REL probe (no X) | `tools/cp/p4/relprobe.c` + `relmouse.py` |
| in-OS gate | `tools/cp/p4/run-p4.sh` (build, export, stage) + `p4_gate.py` (boot, QMP, grade) |
| native launcher in the X session | `tools/cp/p4/winep4.c` (`/WINEP4.BIN` from `/XSTART.TXT`) |
| host reference (pinned chroot + private Xvfb) | `tools/cp/p4/host_ref.py` |
| evidence | `docs/cp-x2-evidence/` |

## 1. The second Wine build

`tools/cp/build-wine.sh` gains `CP_WINE_VARIANT=x`:

- its own work dir (default `~/cp/wine-build-x`); the console build in
  `~/cp/wine-build`, which other lanes use, is never touched;
- `--with-x --x-includes/--x-libraries` = C4-x's dev tree, its `pc/` after the
  sysroot's on `PKG_CONFIG_LIBDIR`, `-Wl,-rpath-link` for libxcb's deps;
- the P4 features are removed from `WITHOUT=` and **proved from `config.h`**
  (`configure-summary.txt`); a missing one fails the step unless named in
  `CP_WINE_ALLOW_MISSING`;
- `winevulkan` dlopens `libvulkan.so.1`, so configure only needs a library with
  that soname: a one-symbol stub in `$WORK/vkstub` (never staged) unless
  `CP_VULKAN_LIB_DIR` names a real loader;
- `CP_WINE_SYSROOT` reuses a glibc 2.31 + gcc-10 sysroot read-only;
- the audit adds a closure check: every `DT_NEEDED` **and every dlopen soname**
  (`SONAME_*` in config.h) resolves in {Wine stage} + {/CP/X11} + {glibc 2.31},
  `libvulkan.so.1` excepted (/CP/VK, C5-vk2) and `libodbc.so` (odbc32 fails soft).

`tools/cp/x11/build.sh` adds **libXcomposite** and **FreeType 2.14.3** (the
vendored tarball, bundled gzip, no png/bzip2/brotli/harfbuzz → `NEEDED libc`
only; staged in `/CP/X11/lib`, first on a Wine session's `LD_LIBRARY_PATH`).
Tarballs come from `fetch.sh`'s cache or, checked against
`third_party/x11/sums`, from the native stack's `third_party/x11/dl` and
`third_party/`.  libXdmcp/libxshmfence/libXi are reported SKIPPED when absent.
The audit runs under `LC_ALL=C`: under strix's `en_US.UTF-8`, `sort` ignores
`_` and `comm` rejected its input, so every symbol read as unresolved.

### 1.1 Configure summary and audit (strix; `docs/cp-x2-evidence/wine-x-build.txt`)

```
feature x          ON   #define SONAME_LIBX11 "libX11.so.6"
feature xext       ON   #define SONAME_LIBXEXT "libXext.so.6"
feature xinput     ON   #define SONAME_LIBXI "libXi.so.6"
feature xinput2    ON   #define HAVE_X11_EXTENSIONS_XINPUT2_H 1
feature xrandr     ON   #define SONAME_LIBXRANDR "libXrandr.so.2"
feature xrender    ON   #define SONAME_LIBXRENDER "libXrender.so.1"
feature xfixes     ON   #define SONAME_LIBXFIXES "libXfixes.so.3"
feature xcursor    ON   #define SONAME_LIBXCURSOR "libXcursor.so.1"
feature xinerama   ON   #define SONAME_LIBXINERAMA "libXinerama.so.1"
feature xcomposite ON   #define SONAME_LIBXCOMPOSITE "libXcomposite.so.1"
feature xshape     ON   #define HAVE_X11_EXTENSIONS_SHAPE_H 1
feature xshm       ON   #define HAVE_LIBXXSHM 1
feature freetype   ON   #define SONAME_LIBFREETYPE "libfreetype.so.6"
feature vulkan     ON   #define SONAME_LIBVULKAN "libvulkan.so.1"
audit: 30 ELF objects; max GLIBC_ symbol version required = GLIBC_2.29
audit: DT_NEEDED union: libc.so.6 libdl.so.2 libgcc_s.so.1 libm.so.6 libpthread.so.0 libresolv.so.2 libX11.so.6 libXext.so.6 ntdll.so win32u.so
audit: x closure: every DT_NEEDED and every dlopen soname resolves in {Wine stage} + {/CP/X11} + {glibc 2.31} (+ /CP/VK for vulkan): yes
```

X libraries (`x11-build.txt`): 39 shared objects, max GLIBC_2.27, closure yes;
xsmoke builds again (it needs libxshmfence).  The Wine stage is 204 MiB, 931
files; the build takes ~75 s at -j16 on strix.

## 2. Staging (`run-p4.sh`, `winep4.c`, Makefile block)

- One NFS export `/srv/nfs/cp-x2-p4*` holds `WINE/` (the X variant), `X11/`
  (the C4-x stage + FreeType) and `P4/` (probes), mounted at `/CP`; it is added
  and removed with `tools/cp/exports.sh` (§7.4).
- On a copy of `disk.img` (MODE=desktop, DESKTOP=icewm): glibc 2.31 in `/LIB64`
  + `/lib64/ld`, `/WINEP4.BIN`, `/XSTART.TXT`, and `/X11/XSESSION.ENV` +
  `HB_SECOND_SCREEN=0`.  XSESSION starts XHOBBY.BIN + IceWM, then WINEP4.BIN.
- Wine's environment (winep4.c): `DISPLAY=:0`,
  `LD_LIBRARY_PATH=/CP/X11/lib:/CP/WINE/lib:/LIB64:/CP/WINE/lib/wine/x86_64-unix`
  (/CP/X11/lib first: winex11 dlopens by bare soname and /LIB64 may hold the
  RimWorld libX11 shim), `WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d`,
  no audio driver built at all (AD-9).
- Makefile (separate block marked `cp/x2`, for the integrator): `make ARCH=intel
  cp-x11-disk [CP_X11_STAGE=...]` copies the /CP/X11 runtime tree onto
  `disk.img` for sessions without NFS; not a `disk.img` prerequisite.

**Explorer gets 10 s.**  win32u starts the desktop process and waits
`NtUserWaitForInputIdle(process, 10000)` (`dlls/win32u/winstation.c:877`); the
client then reads `GraphicsDriver` from the registry and, if explorer has not
written it yet, keeps the null driver ("nodrv_CreateWindow ... The explorer
process failed to start") for its lifetime.  In-guest, explorer + winex11 + the
X libraries over NFS take longer than that (diag1: the first nodrv is at serial
line 8754, before any NFS failure row; explorer itself then initialises
winex11).  `winep4.c` therefore starts a background `rawinput.exe hold` client
after `wineboot`, waits 90 s, and runs the GUI steps without quiescing between
them, so the desktop stays up.

## 3. DDX: one screen, RandR

**One screen.**  `HB_SECOND_SCREEN=0` never adds the depth-8 `:0.1`.  Unset
keeps the default (added when `:0.0` is depth 24); other values force it on.
With one screen, RandR's Xinerama shim and Composite are advertised again.
In-OS (diag1, `docs/cp-x2-evidence/diag1-excerpt.txt`):
`XOpenDisplay ok ... screens=1`, 19 extensions incl. `XINERAMA` and
`Composite`; winex11 logs `xinerama_init monitor 0x0: (0,0)-(1920,1080) work
(0,0)-(1920,1054)` and `XComposite is up and running`.

**RandR.**  Screen 0 offers its own size (preferred) plus every common size
that fits (in-OS: 12 modes at 1920x1080, from 1920x1080 down to 640x480),
synthetic 60 Hz timings, a 96 dpi physical size.  The shadow is allocated once
at the largest size; `RRSetScreenSize` changes the root window and the screen
pixmap (stride kept by passing `devKind` explicitly), `RRSetCrtcConfig` picks
the part on the glass, top-left aligned, 1:1, black around it.  Rotation, a
CRTC offset or an out-of-range size fail with RandR's own error.  winex11
selects `XRandR 1.4` for display settings.  The host reference (Xvfb, one
mode) shows Wine's clean-refusal path: `ChangeDisplaySettingsEx 1280x720
rc=-2` (BADMODE), restore OK.

## 4. winecfg and notepad

See §8 (results).

## 5. Relative motion: kernel → DDX → XI2 raw → WM_INPUT

- **Kernel** (`src/kernel/evdev.c`): every moving PS/2 packet also reports its
  raw counts as `EV_REL REL_X = dx`, `REL_Y = -dy` (evdev +y down), right after
  the ABS pair, one row per nonzero axis, never clamped.  QMP `input-send-event`
  "rel" events reach the guest through the PS/2 mouse (QEMU's only relative
  device), so they arrive this way.  Host golden `test_mouse_rel_raw` fails on
  the base decoder (46 checks, 5 failed) and passes after (46, 0).  In-OS
  (`relmouse-kernel.txt`): 10x(7,3) 5x(-4,-9) 40x(100,1) → `rel_x=4050
  rel_y=25`, ABS_X pinned at px 1919 — PASS.
- **DDX** (`hbinput.c`): a second slave pointer, "HobbyOS relative pointer",
  with two Relative, unbounded valuators (REL_X/REL_Y labels).  REL deltas are
  posted with `POINTER_RELATIVE` and no acceleration, so `XI_RawMotion` carries
  the exact deltas and the master's valuators 0/1 become `XIModeRelative` —
  what `winex11 update_relative_valuators` requires (it ignores raw motion on
  absolute valuators).  A packet with REL rows ignores its ABS rows and sends
  its buttons to the relative slave.  In-OS: `device 7 "HobbyOS relative
  pointer" slave-pointer valuators=2 (relative=2 absolute=0)`.
- **Wine** selects `XI_RawMotion` on the root only while the cursor is clipped
  (`ClipCursor` → `grab_clipping_window`), which is what a game does for
  mouselook.  The probe sums `WM_INPUT` with the cursor free and clipped; the
  gate's clip phase ends with 20 x rel(+40,+2) into the clip edge, so its sums
  (dx=850, dy=25) only come out exact from device deltas.

## 6. MIT-SHM (plan; not started)

Wine's winex11 uses **SysV** shm (`XShmCreateImage` + `shmget`/`shmat` +
`XShmAttach`); Mesa's WSI uses the fd form (`xcb_shm_attach_fd`).  Plan:
1. Kernel SysV shm: `shmget/shmat/shmdt/shmctl` (x86_64 Linux 29/30/67/31)
   for compat processes and native numbers for XHOBBY.BIN, backed by the
   existing memfd/`MAP_SHARED` VM object (coherent across processes, as
   KUSER_SHARED_DATA already proves), a global id table, `IPC_RMID` deferred
   to the last detach, `IPC_STAT` with `shm_segsz`/uid (xserver's `shm.c`
   checks both).  Unit test + linux probe row.
2. `-Dmitshm=true` in `third_party/x11/build-server.sh`; the DDX needs nothing.
3. fd form (SHM 1.2 `ShmAttachFd`, and DRI3 later): restore `XTRANS_SEND_FDS`
   (patch 0001), which needs the kernel's `K_IPC_CTRL_MAX` (512) to accept
   xtrans's 528-byte control buffer — truncate with `MSG_CTRUNC` as Linux does
   instead of rejecting.

## 7. Host reference (`host-reference.txt`)

The same scenario on the host (pinned chroot, private Xvfb :47, xdotool):
wineboot -i 8.6 s; free phase `dx=70 dy=30` exact.  With **no window manager**
the clip does not hold (Wine never logs `clipping to`, the cursor leaves the
window, `inside=0`), so the clip phase is only meaningful under IceWM in-OS.

## 8. Results

(filled from the final gate run)

## 9. Open items

- NFS: under four concurrent guests the guest logs `rpc: no reply from server`
  storms; a READ that gives up is zero-filled (C3-nfs2's fix).  Runs with those
  rows are VOID (`p4_gate.py` grades it).
- A FAT16 rename from user space hangs a CPU (C5-vk2's find); the lane's scratch
  tree carries the guard locally, uncommitted, until it lands on `cyberpunk`.
