# Doom runs on HobbyOS — as an unmodified Linux ELF

Status: **working**, 2026-11-02.  Two Linux x86_64 binaries run on HobbyOS with no
HobbyOS-specific code in them: id Software's `linuxdoom-1.10` and doomgeneric's
stock X11 backend.  They load through HobbyOS's compat ELF loader, find their IWAD
on FAT16, speak the X11 wire protocol to HobbyOS's own X.Org server over
`/tmp/.X11-unix/X0`, and their pixels reach the HobbyOS framebuffer.

```
$ make doom_elf_test          # id linuxdoom-1.10 at depth 8 (PseudoColor)
[DOOMELF] PASS: Doom ELF ran under X and produced a frame
[DOOMELF] best=/tmp/hobbyos_doom_elf/shot-00.ppm nonblack=0.031 colours=232

$ DOOM_XDEPTH=8 python3 tools/doom_elf/run_doom_elf_test.py \
      --elf obj/doom-linux/doom10.elf --wad obj/doom-linux/DOOM1.WAD \
      --start-line "/DOOM.ELF -warp 1 1"        # straight into E1M1
[DOOMELF] PASS: Doom ELF ran under X and produced a frame

$ make doom_elf_test_gen      # doomgeneric, stock backend, depth 24
[DOOMELF] PASS: Doom ELF ran under X and produced a frame
```

## Running it from the desktop (make run -> Games -> Doom)

The desktop screen (`:0.0`) is 24-bit TrueColor, and linuxdoom-1.10 needs a
depth-8 PseudoColor screen.  A window cannot supply that: X requires a window's
depth to equal its parent's, so an 8-bit window under a 24-bit root is a
`BadMatch`, not a DDX limitation.  XFree86 solved the same problem with a second
*screen*, and so does this port:

| screen | depth | what runs there |
|---|---|---|
| `:0.0` | 24 TrueColor | XSESSION + IceWM + the 24-bit apps (unchanged) |
| `:0.1` | 8 PseudoColor | attended only while an 8-bit client has a mapped window |

`XHOBBY.BIN` creates `:0.1` whenever `:0.0` is 24-bit; the screen that owns the
framebuffer is polled from the server's block handler, so while the game's window
is up the 24-bit screen's shadow updates are dropped, and when it goes away the
desktop's shadow is presented in full again (measured: `present -> screen 0`,
pointer back at 960,540).  The game is handed the pointer (a warp *plus* a real
motion event — the warp alone does not update the sprite's window) and a keyboard
grab, because with a WM on `:0.0` focus can never stay on `:0.1`.

The Games submenu carries two rows:

```
prog "Doom (Linux ELF)" app /DOOML.BIN            <- id's linuxdoom-1.10, :0.1
prog "DOOM"             app HBHOST.BIN DOOM.BIN   <- native doomgeneric port, :0.0
```

`/DOOML.BIN` sets `DISPLAY=:0.1` (via `spawn_ex`, because `setenv`+`spawn` does
not carry the environment blob to the child) and hands over to `/DOOM.ELF`.
F10 quits back to the desktop.

## What the screenshots show

| file | binary | what |
|---|---|---|
| `doom10-title-660x400.png` | linuxdoom-1.10 | the shareware title screen — DOOM logo, space marine, `id SOFTWARE ©1993`, the `PROVIDED BY id FREE OF CHARGE` footer |
| `doom10-fullscreen.png` | linuxdoom-1.10 | the same title in the 1920×1080 HobbyOS framebuffer (320×200 client window, 3.1 % of the screen) |
| `doom10-e1m1-640x400.png` | linuxdoom-1.10 | **gameplay** — launched with `-warp 1 1`: E1M1's blue-floored room, pillars, a health bonus, the pistol, and the full status bar (`AMMO 50`, `HEALTH 100 %`, `ARMS`, `ARMOR 0 %`, `BULL 50/200`, `SHEL/ROKT/CELL`) |
| `doom10-e1m1-fullscreen.png` | linuxdoom-1.10 | the same gameplay frame in the whole framebuffer |
| `doom10-e1m1-start-room.png` | linuxdoom-1.10 | E1M1 first room after `-warp 1 1` (barrel, corpse, pistol) |
| `doom10-e1m1-outdoors-firing.png` | linuxdoom-1.10 | **driven by input**: two 900 ms forward holds walked the player out of the start room — shotgun mid-fire, muzzle flash, AMMO 48 |
| `doom10-e1m1-corridor.png` | linuxdoom-1.10 | deeper in E1M1 (`-skill 3`), rusted corridor |
| `doom10-e1m1-desktop-context.png` | linuxdoom-1.10 | the whole 1920×1080 framebuffer: a 320×200 client window at (0,0), no WM |
| `desktop-doom-elf-960x600.png` | linuxdoom-1.10 | **launched from the desktop**: `/DOOML.BIN` -> `:0.1`, Doom's title on the 24-bit machine |
| `desktop-doom-elf-fullscreen.png` | linuxdoom-1.10 | the same frame in the 1920x1080 framebuffer (0.0309 = a 320x200 window) |
| `desktop-back-after-quit.png` | — | F10 in the game returns the IceWM desktop, taskbar and clock intact |
| `desktop-native-doom.png` | native `DOOM.BIN` | the *native* port's row in the same menu, in an IceWM window via HBHOST |
| `doomgen-e1m1-640x400.png` | doomgeneric | E1M1 gameplay: corridor, HUD (`AMMO 34 / HEALTH 82 % / ARMOR 94 %`), Doomguy face, `YOU GOT THE SHOTGUN!` |
| `doomgen-e1m1-fullscreen.png` | doomgeneric | the same frame in the full framebuffer |

Both were captured by `tools/doom_elf/run_doom_elf_test.py` from the QMP
`screendump` of the live guest framebuffer — not from a host X server, not
reconstructed.

## The two binaries

Both are `Type: DYN (Position-Independent Executable file)`, `statically linked`,
zero `NEEDED` entries (musl static-PIE from an Alpine sysroot), with
`EI_OSABI = SYSV` as upstream Linux binaries are:

| binary | bytes | sha256 |
|---|---|---|
| `doom10.elf` — id linuxdoom-1.10 | 1 569 504 | `eff329853a…` |
| `doomgen.elf` — doomgeneric stock X11 | 1 555 424 | `1916d2f810…` |
| `DOOM1.WAD` — shareware IWAD | 4 196 020 | `1d7d43be50…` |

Builds: `tools/linux-doom/build-doom10.sh`, `build-doomgen.sh` (host-only; see
`tools/linux-doom/README.md`).  No HobbyOS header, symbol or library is involved.

**Honest concession:** `linuxdoom-1.10` as published does not build correctly for a
64-bit target.  `patches/doom10-lp64-toolchain.patch` (18 hunks, 6 files) fixes
word-size bugs only — an on-disk `maptexture_t` field declared `void *`, pointer
arrays allocated `n*4`, `(int)` casts on string pointers in a static initializer
table, and a missing NULL sentinel that ILP32 layout happened to supply.  Game
logic is untouched.  doomgeneric needed no patch at all.

## What had to be built in the OS

1. **A Linux-ABI syscall layer with a real dispatch table.**  Handlers live in
   per-subsystem files behind weak symbol tables (`linux_compat_fd/mem/proc/misc`),
   so several people add handlers without touching one switch.  Every unhandled
   call is named on the console: `[LCOMPAT] ENOSYS nr=21 name=/DOOM.ELF pid=5
   rip=0x10000AD7D2`.
2. **A correct startup block.**  argc/argv/envp plus auxv (`AT_PAGESZ`, `AT_PHDR`,
   `AT_PHNUM`, `AT_ENTRY`, `AT_RANDOM`, `AT_EXECFN`) laid out by address, so
   `argv[0]` lands at `%rsp+8` with `%rsp` 16-aligned.  Without the environment
   Doom never sees `DOOMWADDIR`; with a mis-aligned block libXau exits with
   "Please set $HOME".
3. **A file layer with seeking.**  `lseek`/`readv`/`pread`/`fstat`/`stat`/
   `access`/`readlink`/`getdents64` over the existing HobbyOS fd table — Doom's
   `W_AddFile` seeks to the end for the size and back to 12 for the lump
   directory.
4. **Memory and process ABI.**  `mmap` (file-backed and anonymous), `brk`, `mremap`,
   `mprotect`, `madvise`, `msync`, `mincore`, `memfd_create`; `getpid`/`gettid`/
   `getppid`, credentials, `arch_prctl`, `rlimit`, `sysinfo`, `sigaction`,
   `rt_sigreturn`, `kill`, `setitimer`.
5. **Sockets.**  The X connection is a `AF_UNIX` stream socket: `socketpair`,
   `sendto`/`recvfrom`, `sendmsg`/`recvmsg`, `accept4`, `shutdown`, `poll` — built
   on HobbyOS's existing unix engine rather than a second implementation.
6. **A depth-8 PseudoColor screen in the X.Org port** (`HB_XDEPTH=8`), because
   `linuxdoom-1.10` calls `XMatchVisualInfo(dpy, screen, 8, PseudoColor, &vi)` and
   refuses to start otherwise.  Depth 8 required its own pixel path in the DDX: one
   byte per pixel expanded through the colormap at present time.
7. **Colormap installation on window realization.**  Doom never calls
   `XInstallColormap`; it creates a private `AllocAll` colormap, stores 256 cells
   and lets the server install it.  With no window manager and PointerRoot focus
   nothing did, so every index went through the default 256-cell map and the frame
   came out as a handful of `0x55`/`0xAA` colours.  The DDX now installs a realized
   window's own colormap at depth 8 (and only at depth 8 — the depth-24 path is
   untouched).

## How it was verified

Every claim above comes from a run, not from reading code:

| gate | result |
|---|---|
| `make doom_elf_test` (linuxdoom, depth 8) | **PASS**, best frame 232 distinct colours, 3.1 % of screen covered (320×200 on 1920×1080) |
| `make doom_elf_test_gen` (doomgeneric, depth 24) | **PASS**, 185 colours, 12.3 % covered (640×400 window) |
| `bash tools/gate/t3_linux_probe.sh {hello,fd,mem,proc,x11,wad}` | all PASS in-guest, each a plain Linux ELF (`x11` runs through an X session: `connect ok`, `setup status=1 body=236`, `screen0 root=0x41 depth=24`, `GetInputFocus focus=0x1`) |
| `python3 src/user/x11/apps/xdepth8/run_xdepth8_test.py` | PASS — a plain client that maps with a private 256-cell colormap and never calls `XInstallColormap` gets its exact cells back (13 `[XD8]` rows) |
| `./run_unit_tests_intel.sh` | 397 run, 0 failed |
| `linux_abi_test_host` | 12 tables, 0 failed |
| `x11_lib_test_host` | 124 checks, 0 failed |
| `run_icewm_test.py --arch intel --no-apps --settle 20` | **PASS** — the default depth-24 desktop is unregressed |
| `tools/gate/t4_w0_dispatch.sh` | PASS — `W0PIE.BIN` carries `PROC_FLAG_LINUX_ABI` |
| `make run` → Games → **Doom (Linux ELF)** | PASS — Doom renders on `:0.1` (0.0309 coverage = 320x200, 232 colours), F10 returns the IceWM desktop, serial `present -> screen 0` |
| the same, on the merged tip (with the parallel GB-164/165 + GB-171 work) | unit tests 397/0, `t3_linux_probe.sh proc`/`mem` PASS, `make doom_elf_test` PASS, Doom on `:0.1` PASS, IceWM 24-bit gate PASS |

The harness stages every run onto a *copy* of the disk and boots the real
`intel` guest under QEMU/KVM; `docs/doom-elf/serial-doom10.txt` is the unedited
guest console for the title-screen run (it shows `adding //doom1.wad`,
`D_CheckNetGame … player 1 of 1`, `P_Init: Init Playloop state`, then the
`[LCOMPAT]` trace going quiet).

## Known gaps (not exercised by Doom)

- **No audio.**  `I_InitSound` finds no `/dev/dsp` and Doom runs silently (it does
  not fail).
- **No hardware-fault signals.**  A `SIGSEGV` on a bad write kills the compat
  process instead of delivering a Linux-shaped signal frame, because the frame
  engine has no Linux `ucontext_t`.  Explicit `kill`/`raise` delivery works.
- `rt_sigprocmask` round-trips but the delivery engine does not consult the mask;
  `sigaltstack` is stored only; `SA_SIGINFO` handlers get no `siginfo_t`.
- `ftruncate` is ENOSYS (memfd path only).

## Layout

```
src/include/linux_compat.h        dispatch contract (struct l_sysent, user copies)
src/kernel/linux_compat.c         dispatcher, core handlers, ENOSYS tracer
src/kernel/linux_compat_fd.c      files, sockets, poll, fcntl
src/kernel/linux_compat_mem.c     mmap/brk/mremap/madvise
src/kernel/linux_compat_proc.c    identity, stat, signals, rlimit, sysinfo
src/kernel/linux_compat_misc.c    clock, random, misc
src/kernel/program_loader.c       compat_stack_block(): argv/envp/auxv
src/user/linux_probe_*.c          the in-guest probes (also run on Linux)
tools/linux-doom/                 the two builds, the LP64 patch, host evidence
tools/doom_elf/run_doom_elf_test.py   the evidence rig
third_party/x11/ddx/hobbyos/hbscreen.c  HB_XDEPTH, depth-8 pixels, colormaps
docs/linux-abi.md                 the ABI reference (§13 maps contract to files)
docs/doom-linux-elf-plan.md       the plan this followed
```
