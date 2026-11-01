# X server for HobbyOS (stage `server`, XHOBBY.BIN)

Lane `xserver` of the IceWM port (`icewm.md` milestone M3). It builds X.Org
xserver 21.1.x with one new DDX, `hw/hobbyos`, which draws to the HobbyOS
framebuffer and reads HobbyOS evdev input.

```
bash third_party/x11/build.sh --arch arm|intel --prefix <prefix> --stage server [-j 8]
  -> build/<arch>/bin/XHOBBY.BIN        flat binary (the disk-root program)
     build/<arch>/sym/XHOBBY.elf        same image with symbols (not for the disk)
     build/<arch>/disk/X11/US.XKM       keymap, compiled on the host
```

The `clientlibs` stage must already be in the prefix, because the build
needs xorgproto, xtrans, libXau, and libX11/libxcb for libxkbfile. The host
needs `meson`, `ninja`, `xkbcomp` and xkeyboard-config: on Debian/Ubuntu
that is `x11-xkb-utils` and `xkb-data`. Set `XKB_CONFIG_ROOT` if
xkeyboard-config is not installed in `/usr/share/X11/xkb`.

## Layout

| Path | What |
|---|---|
| `build-server.sh` | Stage recipe. Deps go into the shared prefix. The server goes into the per-lane build dir, cached by `build/<arch>/.stamp-xserver`. |
| `ddx/hobbyos/` | The DDX source (`hbscreen.c` output + DDX hooks, `hbinput.c` input, `meson.build`). It is copied to `hw/hobbyos` in the extracted tree. |
| `ddx/include/` | Header overlays, on `-I` for the server stage only: O_NOFOLLOW, F_DUPFD, SIGXCPU/SIGSYS/..., M_PI, `ffs` via `<string.h>`, `setlinebuf`, MAXPATHLEN, `caddr_t`, a stub `<net/if.h>`, and prototypes for fork/execl/set*id/setpgid/getpgrp. |
| `ddx/compat/hbxcompat.c` | Weak libc gap fillers, linked only into XHOBBY.BIN: `hypot`, `acos`, `cbrt`, a line-based `fscanf`, `execl`, `set*id`, `setpgid`, `getpgrp`. The repo libc and the default build are untouched. |
| `ddx/libsha1/` | A tiny SHA-1 behind the `libsha1` API (`-Dsha1=libsha1`), used for Render glyph hashing. A host self-test is built with `-DLIBSHA1_SELFTEST`. |
| `ddx/keymap/us.xkb` | The keymap source (evdev + pc105 + us). |
| `patches/xserver/` | 0001 meson (`-Dhobbyos`, no BUSFAULT, no XTRANS_SEND_FDS). 0002 os/ (si_addr, F_DUPFD, utsname, no lock file, stderr through the DDX). 0003 xkb (load `/X11/US.XKM`, no xkbcomp, no rules files). |

## Server configuration

- **DDX:** only `hw/hobbyos`. Xorg, Xvfb, Xnest, Xephyr, glamor, GLX, DRI1/2/3,
  XDMCP, xcsecurity, MIT-SHM, Xv/XvMC and DGA are all off.
- **Input:** `input_thread=false`.
- **Listening:** unix only. `listen_local=true` is required: without
  LOCALCONN and abstract sockets, xtrans treats "unix" as an alias, and the
  real `/tmp/.X11-unix/X<n>` listener is the transport named "local".
- **Extensions:** Render, Composite, Damage, XFixes, RandR (one fixed
  output, "HOBBYFB"), XKB, XInput, XTEST, SHAPE, SYNC, DBE, RECORD, DPMS,
  Present, X-Resource, MIT-SCREEN-SAVER, XINERAMA, BIG-REQUESTS, XC-MISC.
- **Fonts:** the default font path is `built-ins`, served by libXfont2's
  compiled-in "fixed" and "cursor" fonts. No font files are needed.

## DDX behaviour

**Screen**
- Depth 24 at 32 bpp, TrueColor, masks `0xff0000/0xff00/0xff`. This is the
  same byte layout as the B8G8R8A8 framebuffer.
- The size comes from `FBIOGET_VSCREENINFO` (normally 1920x1080), and the
  pitch from `FBIOGET_FSCREENINFO`.

**Output**
- `fb` draws into a heap shadow buffer.
- miext/shadow collects the damage. From the server's block handler, the
  damaged boxes are copied into the `SYS_MAP_FB` mapping and presented with
  `SYS_FLUSH_FB_RECTS`, at most 32 rects per call.
- When there are more than 128 boxes, the server presents their extents
  instead.

**Input**
- Input is drained from a block handler, because the input node has no fd.
  The handler caps the server's sleep at `-pollms` ms (10 by default), or
  0 when it has just queued events.
- Keys: keycode = evdev code + 8. Autorepeat records (value 2) are dropped,
  because XKB generates its own repeats.
- Buttons: BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE and BTN_EXTRA map to
  buttons 1, 2, 3, 8 and 9. BTN_TOUCH maps to button 1.
- Wheel: REL_WHEEL up/down maps to buttons 4/5. REL_HWHEEL maps to 6/7.
- Pointer position: ABS 0..32767 is scaled to the screen, and REL_X/REL_Y
  move the same position. Motion is coalesced per SYN packet and flushed
  before each button event.
- **Kernel defect workaround:** `SYS_INPUT_READ` returns `-EFAULT` for
  valid buffers. `sys_input_read` in `src/kernel/arch/{x64,arm}/trap.c`
  tests `if (sys_user_range(..))`, but that function returns 1 when the
  range is OK. On the first `-EFAULT`, the DDX switches for good to
  `SYS_GET_EVENTS`, which is the desktop's call and drains the same ring.
  Once the kernel is fixed, the 24-byte evdev path is used again
  automatically.

**stdio**
- In `OsVendorInit`, before any socket exists, the server pins fds 0-2 to
  `/dev/null`.
- This matters because a HobbyOS process spawned without std fds still
  gets console output from `write(1/2)`, through the kernel's fallback for
  unassigned fds. Without the pinning, `accept()` would hand fds 0-2 to
  clients, and `ErrorF()` would write into a client's protocol stream.
- With 0-2 pinned, `os/log.c` sends stderr text to the console through
  `hbConsoleWrite()` (`SYS_WRITE_CONSOLE`).

**No lock file** (FAT16 has no `link()`).

## Command line (XSESSION.BIN)

```
XHOBBY.BIN :0 -nolisten tcp -noreset -dumbSched [-s 0] [-retro] [-verbose 3]
```

- `-noreset` is required, because the session's readiness probe connects
  and then closes.
- `-dumbSched` is harmless. HAVE_SETITIMER is false, so there is no SIGALRM
  scheduler in any case.
- `-s 0` turns off the 10-minute screen saver. Blanking is a no-op either
  way.
- `-retro` draws the classic root weave, which is useful for tests.
- `-verbose n` sets the Xorg-style stderr verbosity (default 0).

DDX-specific options:

| Option | Meaning |
|---|---|
| `-screen WxH` | Screen size. |
| `+/-render` | Turn RENDER on or off. |
| `-xkm FILE` | Keymap file to load (default `/X11/US.XKM`). |
| `-pollms N` | Input poll interval while idle. |
| `-verbose [N]` | stderr log verbosity. |

## Pins (2026-10-30)

| Package | Version | sha256 | Check |
|---|---|---|---|
| xorg-server | 21.1.25 | `6ad4e3c7b59a309b32c92e5c15ce1267110f9e13f1ac78da62361a628da1a0eb` | gpg: good signature, Peter Hutterer |
| pixman | 0.46.4 | `a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239` | matches the sha512 published at cairographics.org/releases |
| libXfont2 | 2.0.9 | `f042a370666815e7b941e9b7019024755bd1c6c2954afbfa515af378251799e2` | gpg: good signature, Peter Hutterer |
| libfontenc | 1.1.9 | `9d8392705cb10803d5fe1d27d236cbab3f664e26841ce01916bbbe430cf273e2` | gpg: good signature, Alan Coopersmith |
| libxkbfile | 1.2.0 | `7f71884e5faf56fb0e823f3848599cf9b5a9afce51c90982baeb64f635233ebf` | gpg: good signature, Alan Coopersmith |

All tarballs come from `https://www.x.org/archive/individual/`. libxcvt is not
needed, because only Xorg uses it.

## Known limits

- **One process, 64 fds.** `MAX_OPEN_FDS` is 64, so roughly 60 clients can
  connect at once.
- **Kernel recvmsg is stricter than Linux.** It rejects `controllen > 512`
  (`K_IPC_CTRL_MAX`), where Linux would truncate. The server works around
  this by not using fd passing. libxcb's 80-byte buffer fits within the
  limit.
- **Harmless warning.** xtrans warns "Mode of /tmp/.X11-unix should be set
  to 1777", because FAT has no mode bits.
