# Doom as an unmodified Linux ELF on HobbyOS — plan and evidence log

Status: in progress. Branch `doom-elf` (integration), lanes `doom/*`.
Owner: parent session (integration) + five lanes.

## 1. The ask, restated as a testable claim

Build **id Software's Doom for Linux** (`https://github.com/id-software/doom`,
`linuxdoom-1.10`) as a normal x86_64 Linux binary, and run **that binary** on
HobbyOS (intel/x86_64) — no source port, no HobbyOS backend inside the game.
Doom renders into an X11 window served by HobbyOS's own X server (XHOBBY, the
X.Org 21.1 port + hobbyos DDX) on the HobbyOS framebuffer, under QEMU.

"Unmodified" means: the game code and its X11 backend are upstream's. The only
concessions are build-level and are listed in §5.

## 2. Architecture

    DOOM.ELF  (static-PIE musl x86_64, static libX11/libXext, upstream X11 code)
       │  syscall traps (rax = Linux nr)
       │        └─ kernel: linux_compat_dispatch() → subsystem handler tables
       │           (fd / mem / proc / core), each writing tf->regs[0]
       │  X11 protocol over AF_UNIX "/tmp/.X11-unix/X0"
       ▼
    XHOBBY.BIN (native HobbyOS X.Org server, hobbyos DDX)
       │  shadow framebuffer → SYS_MAP_FB + SYS_FLUSH_FB_RECTS
       ▼
    QEMU window / QMP screendump  ← evidence

Launch path: kernel starts `XSESSION.BIN` (DESKTOP=icewm) → it starts
`XHOBBY.BIN`, then (no-WM or IceWM mode) spawns the programs listed in
`/XSTART.TXT`, which the disk build fills with `/DOOM.ELF -iwad /DOOM1.WAD`.

## 3. Why this shape (facts measured on this tree, 2026-11-02)

| fact | evidence |
|---|---|
| intel guest boots and runs a static-PIE musl ELF | `t4_w0_dispatch.sh`: `v2 loader: PID=6 entry=0x10000011B8 ... abi=0x1` |
| compat layer existed only as a number-translation switch | `src/kernel/arch/x64/trap.c` (GB-014) |
| named AF_UNIX stream sockets exist in the kernel | `src/kernel/unix.c`, `SYS_CONNECT_ADDR`/`SYS_BIND_ADDR`/`SYS_LISTEN`/`SYS_ACCEPT` (IceWM M1) |
| a real X.Org server already runs in-guest | `third_party/x11/build/intel/bin/XHOBBY.BIN` (2.7 MB), `run_icewm_test.py` |
| MIT-SHM is disabled in that server | `third_party/x11/build-server.sh`: `-Dmitshm=false` (Doom then takes the `XPutImage` path) |
| the DDX screen is depth 24 TrueColor only | `ddx/hobbyos/hbscreen.c:` `miSetVisualTypesAndMasks(24, …)` |
| upstream Doom **requires** depth-8 PseudoColor | `linuxdoom-1.10/i_video.c`: `XMatchVisualInfo(…, 8, PseudoColor, …)` else `I_Error("xdoom currently only supports 256-color PseudoColor screens")` |
| image slot is 128 MiB, so a 3 MB static binary is not a size problem | `src/include/vm.h`: `USER_IMG_SIZE 0x08000000` |
| Doom's build needs two mechanical fixes on a 2026 toolchain | `#include <errnos.h>` (typo for `<errno.h>`), and `(int)"literal"` static initializers in `m_misc.c` that no modern C compiler accepts; `-lnsl` no longer exists |

## 4. Work breakdown

| lane | owns | deliverable |
|---|---|---|
| parent (here) | shared contract (`linux_compat.[ch]`, trap dispatch, probe harness in the Makefile + `tools/gate/t3_linux_probe.sh`), `/XSTART.TXT` + `XSESS_NO_WM` in `xsession.c`, disk staging, evidence rig, merges | the integration |
| `doom-build` | `tools/linux-doom/**` | `DOOM10.ELF` (id linuxdoom) + `DOOMGEN.ELF` (doomgeneric stock xlib, risk-reducer) + `DOOM1.WAD`, each verified under Xvfb on the host |
| `compat-fd` | `src/kernel/linux_compat_fd.c`, `src/user/linux_probe_fd.c` | sockets (AF_UNIX), poll/select, ioctl(FIONBIO/FIONREAD), fcntl, dup/dup2/dup3 |
| `compat-mem` | `src/kernel/linux_compat_mem.c`, `src/user/linux_probe_mem.c` | mmap (anon + file, MAP_FIXED), brk, mprotect, munmap, mremap/madvise/msync |
| `compat-proc` | `src/kernel/linux_compat_proc.c`, `src/user/linux_probe_proc.c` | rt_sigaction/procmask/return, stat/fstat/newfstatat, readlink(at), getrlimit/prlimit64, times/sysinfo, set_tid_address |
| `xdepth8` | `third_party/x11/ddx/hobbyos/**`, `third_party/x11/build-server.sh`, `src/user/x11/apps/xdepth8/**` | a depth-8 PseudoColor screen mode (`HB_XDEPTH=8`) with a palette present path, verified by a real 8-bit X client + screendump pixel check |

Rule: **lanes never edit a file another lane owns.** The dispatcher reaches each
lane's table through a weak symbol, so landing order does not matter and no lane
edits `trap.c`, `linux_compat.c`, the Makefile or `docs/linux-abi.md`.

## 5. Concessions (build-level, not a port)

1. `errnos.h` → provided by a shim include directory (`-I tools/linux-doom/shim`
   with a one-line `errnos.h` that includes `<errno.h>`). No source edit.
2. **LP64 fixes** — `tools/linux-doom/patches/doom10-lp64-toolchain.patch`, 18
   hunks in 6 files, applied to a COPY of a pristine clone. The published 1.10
   source does not build correctly on a 64-bit target: `maptexture_t`'s
   `void *columndirectory` is a 32-bit on-disk field (every texture read
   garbage), pointer arrays sized `num*4`, `(int)` casts on string pointers in
   the config-default table (which no modern compiler accepts), a `char*`/`int`
   default-value table, and a missing NULL sentinel on `sprnames[]` that only
   worked because ILP32 layout happened to leave a zero word after the array.
   No game logic is changed; each hunk is a word-size correction.  This is the
   same class of fix any 64-bit Linux build of 1993 Doom needs.
3. `-std=gnu89 -fcommon` (the code is K&R-era C with tentative definitions).
4. `-lnsl` dropped (merged into glibc/musl long ago).
5. Static link (`-static-pie`), because the compat track is static-ELF only.
6. Audio: `/dev/dsp` does not exist, so Doom prints "Could not open /dev/dsp"
   and continues without sound (OSS probe is silent-safe by design, GB-029).

## 6. Gate ladder

1. Host: each Doom binary runs under `Xvfb :99 -screen 0 640x480x8` and a
   screenshot is non-blank (doom-build lane).
2. Kernel: `./run_unit_tests_intel.sh` 397/0, `tools/gate/t3_linux_probe.sh hello`
   → `[LP] done rc=0` (already green on the freeze commit).
3. Per-lane probe: `tools/gate/t3_linux_probe.sh <name>` → `[LP] done rc=0`.
4. X: `run_icewm_test.py --arch intel` stays green (24-bit path), and the new
   depth-8 client renders known pixels at depth 8.
5. Integration: boot the icewm disk with `/XSTART.TXT` → DOOM.ELF; screendump
   must show Doom's title screen and a demo frame (raycaster signature: many
   colours, horizon band, >200k non-black pixels), with the compat console free
   of `[LCOMPAT] ENOSYS` for the numbers Doom uses.

## 7. Log

- 2026-11-02 **lane `doom-build` merged**: `tools/linux-doom/` with two
  one-command builds, a hash-pinned Alpine/musl X11 sysroot, the LP64 patch above,
  and host evidence.  Binaries: `doom10.elf` (id linuxdoom-1.10, 1 569 504 B,
  `eff32985…`) and `doomgen.elf` (doomgeneric stock X11 backend, 1 555 424 B),
  both `Type: DYN (Position-Independent Executable file)`, `NEEDED` count 0,
  `ldd` -> statically linked.  Host frames on Xvfb: doom10 E1M1 **158** distinct
  colours, doomgen E1M1 **155**, and a two-frame demo diff of 66.8 % of pixels —
  the raycaster is running, not a frozen first frame.

- 2026-11-02 **compat-fd merged** (sockets/poll/fcntl/dup on the native unix
  engine): its probe passes in the merged tree —
  `socketpair / sendto_recvfrom / poll_accepted / sendmsg_recvmsg / accept4 /
  shutdown_eof / poll_hup` all `ok`, `[LP] done rc=0`.
- 2026-11-02 **the X11 transport works from a Linux-ABI process** (the milestone
  this whole design rests on). `src/user/linux_probe_x11.c` in-guest, via
  `/XSTART.TXT` under `XSESS_NO_WM=1`:
  `connect ok` -> `setup status=1 body=236 bytes` ->
  `release=12101025 vendor=The X.Org Foundation roots=1` ->
  `screen0 root=0x00000041 depth=24 ok` -> `GetInputFocus reply ok`
  -> `[LP] done rc=0`.  A compat process speaks X11 to HobbyOS's own X.Org
  server; nothing about that path is emulated.
- 2026-11-02 first real Doom ELF in the guest: loads (666 pages, `abi=0x1`),
  runs, and names its first wall — `[LCOMPAT] ENOSYS nr=21` (`access`), which is
  why it reports `W_InitFiles: no files found`.
- 2026-11-02 second wall, same method: `[LCOMPAT] ENOSYS nr=8` (lseek) — Doom's
  `fseek` did nothing, so it read the WAD directory from offset 0 and reported
  `W_GetNumForName: PNAMES not found!`.  `src/kernel/linux_compat_misc.c` adds
  lseek/readv/pread/pwrite + the path ops; `src/user/linux_probe_wad.c` walks the
  IWAD directory both raw and through musl stdio and now matches the host
  exactly (1264 lumps, PNAMES idx 106, TEXTURE1 present).
- 2026-11-02 **Doom's whole init chain now runs in the guest**: sound server
  probe fails as designed, `D_CheckNetGame` reports
  `startskill 2 deathmatch: 0 startmap: 1 startepisode: 1 / player 1 of 1`,
  and then the last wall is the one predicted at plan time:
  `Error: xdoom currently only supports 256-color PseudoColor screens`.
  Nothing else in the compat layer stands between Doom and a frame.

- 2026-11-02 freeze: dispatcher + tracer + probe harness; `gettimeofday`
  64-bit fix; blocking-syscall return-value fix (store before `schedule`);
  intel unit tests 397/0; `hello` probe green in-guest.
- 2026-11-02 `xsession`: `/XSTART.TXT` autostart + `XSESS_NO_WM` switch.
- 2026-11-02 X path baseline: `run_icewm_test.py --arch intel --no-apps
  --settle 20` => PASS (XHOBBY + IceWM + XEYESR under a QMP harness), and the
  same session with `XSESS_NO_WM=1` runs a real Xlib client (`/XEYESR.BIN`) that
  paints 3% of the screen — so "client -> server -> framebuffer" works without a
  window manager.  (An earlier run failed at `--settle 8`: the start-menu click
  fired before the WM was ready; not a code defect.)
- 2026-11-02 startup block: real argv + envp from the PCB blobs.  Session
  launch of the probe measures `[X] [LP] argv argc=3 argv0=/DOOM.ELF` and
  `[X] [LP] env DISPLAY=:0` — the two things upstream Doom reads (its `-iwad`
  argument and the X display name).  First cut wrote argv before envp, guest
  read argv[0]==NULL and faulted at VA=0x1000; the ordering is pinned in the
  helper's comment.
