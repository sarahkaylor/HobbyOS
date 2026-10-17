# Linux x86_64 ABI contract for HobbyOS ELF-compat (GB-010, v1)

Status: **v1 — DRAFT, frozen at the M1 gate** (gaming.md §2.2 #3). Owner: Coordinator (DEEP).
Scope: what an **unmodified static-musl x86_64 Linux binary** may rely on when it runs on
HobbyOS under the compat layer. Anything not written here is not promised, and a binary that
depends on it is out of scope until this document gains the row.

**Rule 1 — numbers are measured, never remembered.** Every constant, struct offset and errno
value in the *measured* sections below is emitted by `tools/linux-abi/probe_abi.c` on a real
Linux x86_64 host and spliced in by `tools/linux-abi/gen_abi_doc.py`. Regenerate with
`python3 tools/linux-abi/gen_abi_doc.py`; verify in CI with `--check` (on a Linux x86_64 runner)
so drift fails loudly. If you need a number that is not here, add it to the probe and re-run —
do not type it into this file.

**Rule 2 — the differential test is the enforcement.** A row is only "locked" when the T4
differential harness (GB-007/GB-015) asserts HobbyOS against a Linux box for that row. Rows
below are marked `locked` only when such an assertion exists; everything else is `stated`.

**Rule 3 — musl never runs on HobbyOS.** musl is compiled by a normal `x86_64-linux-musl`
cross. HobbyOS must satisfy what musl-generated code *calls* — syscall numbers, errno values,
struct layouts, signal frames — not emulate musl.

---

## 1. Process startup contract

A static-ELF process is entered with:

- **Registers:** `%rdi = argc`, `%rsi = argv`, `%rdx = envp` (musl's `_start` reads these; the
  kernel does **not** create an AT_* aux vector block for them — musl's `_start` walks `envp`
  to find `AT_*` entries itself, so the aux vector still must be appended after the environment
  and `%rsi/%rdx` must point at contiguous, NUL-terminated `char *[]` arrays).
- **Stack:** 16-byte aligned at entry, `%rsp` pointing at `argc` (i.e. the same value as `%rdi`).
- **Aux vector** (`Elf64_auxv_t` = two `uint64_t`s, key then value), appended after `envp`'s
  NULL, terminated by `AT_NULL`. Keys that must be present for musl/SDL to start:
  `AT_ENTRY`, `AT_PHDR`, `AT_PHNUM`, `AT_PAGESZ`, `AT_RANDOM` (16 bytes of entropy — musl's
  stack-guard and `AT_RANDOM`-derived pointers), `AT_UID/AT_EUID/AT_GID/AT_EGID`, `AT_HWCAP`,
  `AT_CLKTCK`, `AT_EXECFN` (a path string; SDL and some games read `/proc/self/exe` too).
- **fd table:** 0,1,2 = console/stdout/stderr (the serial console), plus whatever the launcher
  passed. `O_CLOEXEC` is honoured across `execve`.
- **`/proc/self/exe`** must resolve to the loaded path (musl's `dlsym`-free apps and SDL read it);
  `/proc/self/maps`, `/proc/self/stat`, `/proc/self/status`, `/sys/class/...` stubs are M1/M2 work
  (GB-020) — until they exist, a binary that needs them fails here, not in the syscall layer.

## 2. Syscall entry and return contract

- Keep the **native** HobbyOS syscall convention untouched. The compat layer is entered only for
  a process marked `PROC_FLAG_LINUX_ABI` (set by the ELF loader for `ET_EXEC` images), so x86_64
  `syscall` traps from such a process route to the Linux translation table instead of the native
  chain. No native ABI change, no ARM change (gaming.md §2.3 two-arch parity rule).
- **Args:** `%rax` = Linux syscall number, args in `%rdi, %rsi, %rdx, %r10, %r8, %r9`.
  HobbyOS's x64 trap handler already mirrors this scatter for the native path (rdi/rsi/rdx/r10),
  so the shim reads the same saved frame.
- **Return:** the *translated* syscall returns a Linux-style result: `>= 0` on success,
  `-errno` on failure (a small negative number in `[-4095, -1]`). The shim must **not** return
  HobbyOS's `-1`-with-separate-errno form to compat processes, and must not let HobbyOS's
  internal "would block" marker (`-2`, see `src/include/syscall.h`) escape — `-2` is
  `-ENOENT` in Linux and a leaked marker is a silent data corruption.
- **Blocking:** when a shim blocks (read on an empty pipe, `futex` wait, `nanosleep`), it must
  return through the normal scheduler path so the process is descheduled — never spin. The
  30-second deadlock law applies unchanged: **a hang is a defect.**
- **`syscall` rewind:** a restarted syscall re-executes the 2-byte `syscall` instruction; the
  x64 trap path already implements this rewind for the native ABI and the compat entry inherits it.
- **`-mgeneral-regs-only` still applies** to all compat kernel code, and the compat layer adds no
  new FP/SIMD use (musl's userland owns the FPU, not the kernel).

## 3. Measured constants

Values below come from the probe. `S_IFMT`, `O_*`, `PROT_*`, `MAP_*`, `POLL*`, `F_*`, `W*`,
`SIG*`, `SA_*`, `TC*`, `TIOC*`, `FBIO*`, `EVIOC*`, `DT_*`, `EV_*`, `KEY_*`, `ABS_*`, `REL_*`,
`RLIMIT_NOFILE` and the widths (`sizeof(long)`, page size) are all asserted by the M1 T4 suite
(GB-015) before any dependent ticket lands.

<!-- PROBE:constants -->
```text
O_RDONLY=0 O_WRONLY=1 O_RDWR=2 O_CREAT=0x40 O_TRUNC=0x200 O_APPEND=0x400 O_NONBLOCK=0x800 O_DIRECTORY=0x10000
AT_FDCWD=-100  SEEK_SET=0 SEEK_CUR=1 SEEK_END=2
S_IFMT=0170000 S_IFREG=0100000 S_IFDIR=040000 S_IFCHR=020000 S_IFLNK=0120000 S_ISUID=04000
PROT_READ=0x1 PROT_WRITE=0x2 PROT_EXEC=0x4 MAP_PRIVATE=0x2 MAP_SHARED=0x1 MAP_ANONYMOUS=0x20 MAP_FIXED=0x10
CLOCK_REALTIME=0 CLOCK_MONOTONIC=1 O_CLOEXEC=0x80000
POLLIN=0x1 POLLOUT=0x4 POLLERR=0x8 POLLHUP=0x10
F_GETFL=3 F_SETFL=4 F_GETLK=5 F_SETLK=6 F_SETLKW=7 F_DUPFD=0
WIFEXITED=1 WIFSIGNALED=1 WEXITSTATUS(7<<8)=7 WTERMSIG(9)=9
SIG_BLOCK=0 SIG_UNBLOCK=1 SIG_SETMASK=2 SA_SIGINFO=0x4 SA_RESTART=0x10000000 SA_RESTORER=0x4000000
SIGCHLD=17 SIGSEGV=11 SIGINT=2 SIGWINCH=28 SIGKILL=9 SIGSTOP=19
WNOHANG=1  ARCH_SET_FS=0x1002  EPOLLIN=0x1 EPOLLOUT=0x4 EPOLLET=0x80000000
TCGETS=0x5401 TIOCGWINSZ=0x5413 FIONREAD=0x541b FIONBIO=0x5421
FBIOGET_VSCREENINFO=0x4600 FBIOGET_FSCREENINFO=0x4602 FBIOBLANK=0x4611
EVIOCGVERSION=0x80044501 EVIOCGBIT(0,..)=0x80004520 EVIOCGNAME=0x80004506
RLIMIT_NOFILE=7  DT_REG=8 DT_DIR=4 DT_CHR=2
NOTE: syscall numbers below come from the kernel headers, not libc
```
<!-- /PROBE -->

### 3a. ELF / aux-vector constants

<!-- PROBE:elf-constants -->
```text
AT_NULL=0 AT_PHDR=3 AT_PHNUM=5 AT_PAGESZ=6 AT_ENTRY=9 AT_BASE=7 AT_EXECFN=31 AT_RANDOM=25 AT_HWCAP=16 AT_SYSINFO_EHDR=33
R_X86_64_RELATIVE=8 R_X86_64_JUMP_SLOT=7 R_X86_64_GLOB_DAT=6
```
<!-- /PROBE -->

## 4. Syscall numbers (x86_64)

The table below is the authoritative number source for the translation table (GB-014). The
implemented subset is tracked in `src/include/linux_syscall.h`; **a number absent from that
header must return `-ENOSYS`**, never fall through to a native handler.

Ordering of M1 work (gaming.md §2.2 #2): W0 corpus first —
`read, write, open, close, lseek, fstat, stat, brk, mmap, munmap, mprotect, exit_group,
writev, uname, getrandom, arch_prctl, rt_sigaction, rt_sigprocmask, futex, clone, execve,
wait4, gettimeofday, nanosleep, clock_gettime, poll, select, ioctl(TIOCGWINSZ/TCGETS),
getdents64, readlink, getcwd, chdir, fcntl` — because that is what static musl pulls in for a
`hello`/`cat`/`sh` (the W0 corpus of GB-024). Everything else is demand-driven by the
weight-class ladder (§1.2) and gets a row when its class demands it.

<!-- PROBE:syscalls -->
```text
  read                   0
  write                  1
  open                   2
  close                  3
  stat                   4
  fstat                  5
  lstat                  6
  poll                   7
  lseek                  8
  mmap                   9
  mprotect               10
  munmap                 11
  brk                    12
  rt_sigaction           13
  rt_sigprocmask         14
  rt_sigreturn           15
  ioctl                  16
  pread64                17
  pwrite64               18
  readv                  19
  writev                 20
  access                 21
  pipe                   22
  select                 23
  sched_yield            24
  mremap                 25
  msync                  26
  mincore                27
  madvise                28
  dup                    32
  dup2                   33
  nanosleep              35
  getitimer              36
  getpid                 39
  socket                 41
  connect                42
  accept                 43
  sendto                 44
  recvfrom               45
  clone                  56
  fork                   57
  vfork                  58
  execve                 59
  exit                   60
  wait4                  61
  kill                   62
  uname                  63
  fcntl                  72
  fsync                  74
  getcwd                 79
  chdir                  80
  rename                 82
  mkdir                  83
  rmdir                  84
  creat                  85
  link                   86
  unlink                 87
  readlink               89
  chmod                  90
  fchmod                 91
  umask                  95
  gettimeofday           96
  getrlimit              97
  getrusage              98
  sysinfo                99
  times                  100
  getuid                 102
  getgid                 104
  geteuid                107
  getegid                108
  setpgid                109
  getppid                110
  arch_prctl             158
  gettid                 186
  futex                  202
  sched_setaffinity      203
  sched_getaffinity      204
  fadvise64              221
  exit_group             231
  epoll_wait             232
  epoll_ctl              233
  epoll_create           213
  epoll_create1          291
  tgkill                 234
  openat                 257
  mkdirat                258
  fstatat                262
  unlinkat               263
  renameat               264
  readlinkat             267
  fchmodat               268
  faccessat              269
  pselect6               270
  ppoll                  271
  set_robust_list        273
  splice                 275
  tee                    276
  utimensat              280
  signalfd               282
  eventfd                284
  eventfd2               290
  epoll_pwait            281
  dup3                   292
  pipe2                  293
  inotify_init1          294
  preadv                 295
  pwritev                296
  prlimit64              302
  dup3                   292
  clock_gettime          228
  clock_getres           229
  clock_nanosleep        230
  getrandom              318
  memfd_create           319
  statx                  332
  rseq                   334
  clone3                 435
  close_range            436
  faccessat2             439
  process_madvise        440
```
<!-- /PROBE -->

## 5. errno values

musl stores `errno` itself (its own TLS slot, per-thread), and its `__syscall_ret` maps a
negative kernel return straight through: **the kernel must return Linux's errno *numbers***.
HobbyOS's own `src/include/errno.h` already uses Linux numbering for the shared subset, but the
compat layer gets its own header so a HobbyOS-only extension value can never leak into a
compat process. `EWOULDBLOCK` and `EAGAIN` are the same number (11) — a shim returning "try
again" must use 11; `ENOTSUP` and `EOPNOTSUPP` share 95.

<!-- PROBE:errno -->
```text
  EPERM        = 1
  ENOENT       = 2
  ESRCH        = 3
  EINTR        = 4
  EIO          = 5
  ENXIO        = 6
  E2BIG        = 7
  ENOEXEC      = 8
  EBADF        = 9
  ECHILD       = 10
  EAGAIN       = 11
  ENOMEM       = 12
  EACCES       = 13
  EFAULT       = 14
  EBUSY        = 16
  EEXIST       = 17
  EXDEV        = 18
  ENODEV       = 19
  ENOTDIR      = 20
  EISDIR       = 21
  EINVAL       = 22
  ENFILE       = 23
  EMFILE       = 24
  ENOTTY       = 25
  ETXTBSY      = 26
  EFBIG        = 27
  ENOSPC       = 28
  ESPIPE       = 29
  EROFS        = 30
  EMLINK       = 31
  EPIPE        = 32
  EDOM         = 33
  ERANGE       = 34
  ENAMETOOLONG = 36
  ENOSYS       = 38
  ENOTEMPTY    = 39
  ELOOP        = 40
  EOVERFLOW    = 75
  EMSGSIZE     = 90
  EPROTOTYPE   = 91
  ENOPROTOOPT  = 92
  EPROTONOSUPPORT = 93
  EAFNOSUPPORT = 97
  EADDRINUSE   = 98
  EADDRNOTAVAIL = 99
  ENETDOWN     = 100
  ENETUNREACH  = 101
  ECONNABORTED = 103
  ECONNRESET   = 104
  ENOBUFS      = 105
  EISCONN      = 106
  ENOTCONN     = 107
  ETIMEDOUT    = 110
  ECONNREFUSED = 111
  EHOSTUNREACH = 113
  EALREADY     = 114
  EINPROGRESS  = 115
  EDESTADDRREQ = 89
  ENOTSOCK     = 88
  ENOTSUP      = 95
  EWOULDBLOCK  = 11
  EDEADLK      = 35
  ENOLCK       = 37
```
<!-- /PROBE -->

## 6. Struct layouts

Field order, offsets and sizes must match exactly; musl passes these by pointer straight to the
kernel. Notably `struct stat` (144 B on x86_64, **not** the asm-generic layout), `struct
sigaction` (152 B with the kernel's `sa_restorer` at offset 144), `ucontext_t` (968 B;
`uc_mcontext.gregs` is a 23-entry `greg_t[23]` array at offset 40, and `uc_mcontext.fpregs` is a
pointer), `struct termios` (60 B, `c_cc` is 19 bytes at offset 17), and `struct epoll_event`
(**packed**, 12 B — `events` first, `data` at offset 4). Getting a layout wrong does not fail
loudly: it corrupts a struct the guest then trusts.

<!-- PROBE:structs -->
```text
sizeof(struct stat)                         = 144
  st_dev                                     = 0
  st_ino                                     = 8
  st_nlink                                   = 16
  st_mode                                    = 24
  st_uid                                     = 28
  st_gid                                     = 32
  st_rdev                                    = 40
  st_size                                    = 48
  st_blksize                                 = 56
  st_blocks                                  = 64
  st_atim                                    = 72
  st_mtim                                    = 88
  st_ctim                                    = 104
sizeof(struct statx)                        = 256
  stx_mask                                   = 0
  stx_size                                   = 40
  stx_mode                                   = 28
sizeof(struct sigaction)                    = 152
  sa_handler                                 = 0
  sa_mask                                    = 8
  sa_flags                                   = 136
  sa_restorer                                = 144
sizeof(ucontext_t) = 968
  uc_flags                                   = 0
  uc_link                                    = 8
  uc_stack                                   = 16
  uc_mcontext                                = 40
  uc_sigmask                                 = 296
sizeof(mcontext_t)                          = 256
  gregs                                      = 0
  fpregs                                     = 184
  (RDI idx)                                  = 8
  (RSI idx)                                  = 9
  (RDX idx)                                  = 12
  (RAX idx)                                  = 13
  (RSP idx)                                  = 15
  (RIP idx)                                  = 16
sizeof(stack_t)                             = 24
SS_ONSTACK=1 SS_DISABLE=2 MINSIGSTKSZ=13504 SIGSTKSZ=13504
sizeof(struct termios)                      = 60
  c_iflag                                    = 0
  c_oflag                                    = 4
  c_cflag                                    = 8
  c_lflag                                    = 12
  c_line                                     = 16
  c_cc                                       = 17
  c_ispeed                                   = 52
  c_ospeed                                   = 56
sizeof(struct winsize)                      = 8
  ws_row                                     = 0
  ws_col                                     = 2
  ws_xpixel                                  = 4
  ws_ypixel                                  = 6
sizeof(struct epoll_event)                  = 12
  data                                       = 4
  events                                     = 0
sizeof(struct pollfd)                       = 8
  fd                                         = 0
  events                                     = 4
  revents                                    = 6
sizeof(struct iovec)                        = 16
  iov_base                                   = 0
  iov_len                                    = 8
sizeof(struct timespec)                     = 16
  tv_sec                                     = 0
  tv_nsec                                    = 8
sizeof(struct timeval)                      = 16
  tv_sec                                     = 0
  tv_usec                                    = 8
sizeof(struct timezone)                     = 8
sizeof(struct itimerval)                    = 32
sizeof(struct rusage)                       = 144
sizeof(struct rlimit)                       = 16
sizeof(struct sysinfo)                      = 112
sizeof(struct dirent)                       = 280
  d_ino                                      = 0
  d_off                                      = 8
  d_reclen                                   = 16
  d_type                                     = 18
  d_name                                     = 19
sizeof(struct dirent64)                     = 280
  d_ino                                      = 0
  d_off                                      = 8
  d_reclen                                   = 16
  d_type                                     = 18
  d_name                                     = 19
sizeof(struct utsname)                      = 390
  sysname                                    = 0
  nodename                                   = 65
  release                                    = 130
  version                                    = 195
  machine                                    = 260
  domainname                                 = 325
sizeof(struct flock)                        = 32
  l_type                                     = 0
  l_whence                                   = 2
  l_start                                    = 8
  l_len                                      = 16
  l_pid                                      = 24
sizeof(stack_t)                             = 24
sizeof(struct fb_var_screeninfo)            = 160
  xres                                       = 0
  yres                                       = 4
  bits_per_pixel                             = 24
  xoffset                                    = 16
  red                                        = 32
  green                                      = 44
  blue                                       = 56
  transp                                     = 68
  activate                                   = 84
  vmode                                      = 132
  rotate                                     = 136
sizeof(struct fb_fix_screeninfo)            = 80
  smem_start                                 = 16
  smem_len                                   = 24
  line_length                                = 48
  type                                       = 28
  visual                                     = 36
FB_TYPE_PACKED_PIXELS=0 FB_VISUAL_TRUECOLOR=2 FB_ACTIVATE_NOW=0 FB_VMODE_NONINTERLACED=0
sizeof(struct input_event)                  = 24
  time                                       = 0
  type                                       = 16
  code                                       = 18
  value                                      = 20
sizeof(struct input_absinfo)                = 24
EV_SYN=0 EV_KEY=1 EV_REL=2 EV_ABS=3 EV_MSC=4 EV_LED=17 EV_REP=20
BTN_LEFT=0x110 BTN_RIGHT=0x111 BTN_MIDDLE=0x112
ABS_X=0x0 ABS_Y=0x1 REL_X=0x0 REL_Y=0x1 REL_WHEEL=0x8
KEY_A=30 KEY_ENTER=28 KEY_ESC=1 KEY_SPACE=57
```
<!-- /PROBE -->

## 7. TLS ABI

- x86_64: `arch_prctl(ARCH_SET_FS, addr)` sets the `%fs` base for the calling thread; musl keeps
  `errno`, the stack guard and `__thread` variables there. `ARCH_GET_FS` must read it back.
  The kernel must **save/restore the FS base per thread on every context switch** (GB-022) — the
  ABI is per-thread, not per-process.
- `%gs` is *not* used by musl on x86_64 (pthread internals use `%fs`); a program that sets it
  keeps it, but the layer makes no promises about `%gs` beyond preserving the value.
- ARM never enters this path: the compat track is x86_64-only (gaming.md §2.1) and its ABI
  (`TPIDR_EL0`) belongs to the native side, untouched.
- `set_thread_area`/`rdfsbase`-style legacy paths are **not** implemented — musl static uses
  `arch_prctl` only.

## 8. Signals

- `rt_sigaction(sig, act, oldact, 8)` with the 152-byte `struct sigaction`; `sa_flags` must
  carry `SA_RESTORER` + `sa_restorer` through to delivery (the restorer is how the guest returns
  from a handler; a dropped restorer is an instant crash).
- Delivery builds a real frame on the guest stack: the return address is the restorer, below it
  the saved `ucontext_t`, and the handler runs on the guest's `sigaltstack` when `SA_ONSTACK` is
  set. `rt_sigreturn` restores from that frame. `rt_sigprocmask` (how = SIG_BLOCK/SIG_UNBLOCK/
  SIG_SETMASK, 8-byte `sigset_t`) must be per-thread.
- Unblockable: `SIGKILL`, `SIGSTOP` (they are not maskable and cannot be caught).
- `SIGSEGV` from a genuine fault must reach the guest handler with a sane `uc_mcontext`
  (at minimum `gregs[REG_RIP]`, `[REG_RSP]`, `[REG_RAX]`); otherwise debuggers and SDL's
  crash paths get garbage.

## 9. Compat device nodes (§2.2 #5)

| node | backing | ABI asserted |
|---|---|---|
| `/dev/fb0` | mmap'd HobbyOS framebuffer | mmap offsets/size, `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` (see the probe's `fb_*` layouts), `FB_ACTIVATE_NOW` |

The framebuffer the mmap window + virtio-gpu 2D path expose is a fixed `1920x1080`
**32bpp packed B8G8R8A8** surface: byte0=blue, byte1=green, byte2=red, byte3=alpha
(blue at bits 0..7 of the little-endian frame word), stride 7680 B/row, smem
1920*1080*4. The concrete packing + pixel/row offset math are pinned by
`src/include/fb_pixel.h` + `fb_pixel_test_host` (T1; the level below `fb_ioctl`
's struct-field checks).
| `/dev/input/event0..N` | virtio-input | `struct input_event` (see probe), `EVIOCGVERSION`, `EVIOCGNAME`, `EVIOCGBIT`, `EVIOCGABS`, non-blocking reads |
| gamepad/joystick (`event*` or `js*` when GB-032 lands) | evdev gamepad node | gamepad `BTN_*`/`ABS_*` event codes (GB-042; `linux_joystick.h`, locked vs `<linux/input.h>` by `joypad_test_host`) |

The `/dev/input/event*` node (GB-027) is exposed on the native track as two syscalls,
`SYS_INPUT_IOCTL` (93) + `SYS_INPUT_READ` (94) in `src/kernel/input_node.c`
(`src/include/input_node.h`), fed by the shared `virtio_input` ring + the PS/2→evdev
decoder (`src/kernel/evdev.c`). `SYS_INPUT_IOCTL` answers `EVIOCGVERSION` (→ `0x010001`),
`EVIOCGNAME` (→ `"hobbyos-input"`), `EVIOCGBIT` (event-type / key / abs / rel bitmaps) and
`EVIOCGABS` (axis 0..32767) against real Linux `<linux/input.h>`; `SYS_INPUT_READ` drains
the ring into non-blocking 24-byte `input_event` records. The device-free logic is pinned
by `input_node_test_host` (T1/T4 differential) + the kernel `input_node_test_suite`.
| `/dev/null`, `/dev/zero`, `/dev/urandom` | kernel shims | `read`, `mmap` (zero page) |
| `/dev/tty` | console | `TCGETS`/`TCSETS`, `TIOCGWINSZ`, `isatty()` true |

`uname` for compat processes reports a Linux-shaped identity (§2.2 #4, GB-016): `sysname` =
`Linux`, `release` = a fixed string baked into `linux_compat.c` (SDL's version gates parse it),
`machine` = `x86_64`, `nodename` = the HobbyOS hostname. **Frozen at the M1 gate.**

## 10. What is deliberately NOT promised

- Dynamic linking (`PT_INTERP`, glibc `ld.so`, `dlopen`) — Program-2 (W5).
- X11/XCB, EGL/GL, Vulkan — Program-2 (`VULKAN` epic). SDL must be built with its **fbcon +
  evdev** drivers and no X11 (GB-031).
- `/sys` and a full `/proc` — stubs only (GB-020); a binary that shells out to `lspci` or parses
  `/proc/cpuinfo` in full is out of scope this program.
- `io_uring`, `perf_event_open`, `bpf`, `userfaultfd`, `inotify` (beyond what SDL polls), real
  `epoll` on arbitrary fd types (the `select`/`poll` path is first, §2.2).
- Floating-point state beyond what the existing FPU context switch already preserves.
- `clone3`, `pidfd_*`, `clone` with namespace flags (rejected with `-EINVAL`, not silently
  accepted — silently accepting a namespace flag is worse than refusing it).

## 11. Struct/syscall rows the M1 differential suite must lock first (GB-015)

1. `write`/`writev` byte-exactness + partial-write semantics on a pipe.
2. `read` EOF (`0`), `EAGAIN` for `O_NONBLOCK`, `EINTR` for a signal during a blocking read.
3. `open`/`openat` `O_CREAT|O_EXCL` → `EEXIST`; missing file → `ENOENT`; `O_DIRECTORY` on a
   file → `ENOTDIR`.
4. `brk` growth + shrink return values; `mmap(MAP_ANONYMOUS)` alignment/zeroing; `munmap` of a
   partial range; `mprotect` on a non-mapped page → `ENOMEM`.
5. `fstat`/`stat` field-by-field against the measurd `struct stat` (size, mode, ino, nlink, times).
6. `arch_prctl(ARCH_SET_FS)` round-trip, and per-thread `errno` isolation across a `clone`.
7. `getdents64` record sizes/`d_off`/`d_type` (`.`, `..` included) on the FAT16 root.
8. `uname`, `getrandom` (non-zero, correct length), `clock_gettime(CLOCK_MONOTONIC)` monotonicity.
9. Signal delivery round-trip: handler runs, `sa_restorer` returns, `rt_sigreturn` restores
   registers, `SA_RESTART` restarts an interrupted `read`.
10. `execve` + `wait4` exit-status encoding (`WIFEXITED`/`WEXITSTATUS` on 0..255).

## 12. Open items / decisions

- `AT_RANDOM` quality: seed from the existing kernel entropy pool (net.c) — confirm it is
  available before the timer starts.
- Whether `uname.release` should mimic a specific kernel version for SDL's version gate.
- `/proc/self/exe` implementation strategy (symlink in the FAT16-backed VFS vs. a compat-only
  read hook) — affects GB-020 and the launcher (GB-039).
