# Build-tools audit: GNU make / mtools / dosfstools pins + portability audits + sh matrix

Lane L4 of the HobbyOS self-hosting program (selfhost.md W-D/W-E, AD-7/AD-8).
Scope: pins + per-tool portability audits vs the HobbyOS sysroot, the sh
capability matrix for running make recipes in-OS, and the `Makefile.hobbyos`
sketch + recommendations. Nothing here ships to the OS; these feed the wave-2
port lanes (make-port, mtools-port, mkfs-port) and M3 (build driver).

House style: `docs/gnu-ports.md` — ports are transcription jobs into `src/`
(GPL header kept, gnulib/autotools scaffolding replaced by the sysroot +
small local helpers); autotools is NOT used in-tree. All three tools are
GPL-3+ and compatible with that policy.

---

## 1. Pins

Verified 2026-10-14 at lane time (facts drift — the lane brief's guesses were
re-checked, not trusted). All tarballs downloaded to `third_party/_staging/`
(gitignored) and hashed locally; `_staging` also holds the extracted trees
used for the audits below.

| Tool | Version | Upstream URL (tarball) | sha256 (computed locally) | tag→commit | Release date | License |
|---|---|---|---|---|---|---|
| GNU make | **4.4.1** (latest stable; 4.4.2 does not exist) | https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz | `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3` | tag `4.4.1` → `6dd575465372c7d8c22d9b35b977c1c6c63a0b6b` (`git ls-remote https://git.savannah.gnu.org/git/make.git`) | 2023-02-26 (NEWS + ftp listing) | GPL-3+ (COPYING v3) |
| mtools (mcopy/mmd subset) | **4.0.49** (latest stable) | https://ftp.gnu.org/gnu/mtools/mtools-4.0.49.tar.gz | `10cd1111da87bf2400a380c1639a6cba8bfb937a24f9c51f5f88d393ae5f6f76` | **N/A — upstream VCS is SVN, not git** (see note) | 2025-06-14 (GNU announcement, info-mtools 2025-06) | GPL-3+ (COPYING v3) |
| dosfstools (mkfs.fat) | **4.2** (latest stable) | https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz | `64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527` | tag `v4.2` → `bba5a12447991cc1be9c33855c6c68e89b9ab29c` (`git ls-remote https://github.com/dosfstools/dosfstools.git`); GitHub release v4.2 published 2021-01-31 | 2021-01-31 | GPL-3+ (COPYING v3) |

Cross-verification of the hashes:

- make 4.4.1: computed sha256 matches the long-published value; tarball size
  2,348,200 B. `.sig` present on ftp.gnu.org (GPG, not verified here).
- mtools 4.0.49: computed sha1 `b9f90c51a91cab19b5c470daff6f18cfb5740473`
  matches the SHA-1 published in the GNU release announcement
  (info-mtools / info-gnu, 2025-06-14). `.sums` file is not published on
  ftp.gnu.org for this tarball.
- dosfstools 4.2: the GitHub release asset accompanies the signed
  `dosfstools-4.2.tar.gz.sig`; project CONFIRMS GPL-3+ in README.

**mtools tag→commit note.** The sandbox-canonical `git ls-remote
https://git.savannah.gnu.org/git/mtools.git` returns "repository not found":
mtools is one of the last GNU packages whose upstream VCS is **SVN**
(savannah `svn://svn.savannah.gnu.org/mtools/`, browsable at
`https://svn.savannah.gnu.org/viewvc/mtools/` — the viewvc instance was
503 at lane time). There is no upstream git repo, so there is no git
tag→commit to record. Options recorded for the port lane:

1. Integrity gate = the tarball + its sha256 (+ published SHA-1), which is
   exactly the GNU-release provenance we actually need.
2. If a git history is wanted for the port work, the Debian packaging
   mirror `https://salsa.debian.org/debian/mtools.git` tracks upstream
   (tags there are `debian/*` packaging tags, not upstream); treat it as a
   convenience reference only, never as provenance.

## 2. Port surface summary (per tool) — see §3–§5 for the full audits

| Tool | LOC (core sources) | VCS | Port model | Key syscall surface | verdict |
|---|---|---|---|---|---|
| make 4.4.1 | ~36 src/*.c (≈28k C incl. VMS/Amiga/guile we drop) | git (savannah) | transcription of ~22 core files + own getopt; replace gnulib-ish local portability layer | fork/exec/waitpid, pipe, signals, fcntl F_SETLK (lockf), glob, getcwd, isatty/termios, time | **PORTABLE with the known gap list in §3**; recommend `-j1` v1 |
| mtools 4.0.49 | ~60 C files (whole tree); subset = mcopy + mmd + shared libs | SVN (no git) | transcription of a ~12-file subset; VFAT LFN writing is REQUIRED (staging copy-in path) | open/read/write/lseek, stat/fstat, readdir/opendir (local dirs), termios (raw floppies — stub), ioctl(BLKGETSIZE64) — stub for image files | **PARTIAL by design** — subset + stubs enumerated in §4 |
| dosfstools 4.2 | src/mkfs.fat.c + boot.c (+ common/) — small | git (github) | transcription of ~4 sources; no ioctl needed if we implement the O_EXCL/SEEK_HOLE probe fallback | open(O_EXCL), lseek (SEEK_HOLE/SEEK_DATA probe — conditional), write, ftruncate/fsync, time | **EASIEST of the three** — §5 |


## 3. Portability audit — GNU make 4.4.1

Sources: `third_party/_staging/make-4.4.1/` (tarball pin §1). Method applied
to all three tools: grep the extracted tree for the OS-facing call surface,
then classify each call FULL / PARTIAL / MISSING against the sysroot
(`src/libc/include/**`, `src/libc/src/**`, `src/user/libc.c`,
`src/user_include/libc.h`) with file:line anchors.

### 3.1 Files that make up the port (transcription set)

make 4.4.1 src/ has 36 C files; the port transcribes the ~25 POSIX ones +
its own getopt + its bundled glob/fnmatch (moved to `lib/` in 4.4):

| File(s) | LOC | Notes |
|---|---|---|
| main.c read.c job.c function.c expand.c variable.c dir.c commands.c file.c misc.c output.c remake.c rule.c implicit.c default.c signame.c strcache.c vpath.c version.c hash.c ar.c arscan.c load.c loadapi.c posixos.c | ~29.8k | the port core (sizes above in §2) |
| getopt.c getopt1.c | 1207 | GNU getopt bundled — make does not need the sysroot copy |
| lib/glob.c lib/fnmatch.c | 1781 | bundled glob / fnmatch (4.4 moved them from src/glob/ to lib/); **transcribe both** — wildcard expansion + archive-member matching call glob()/fnmatch() unconditionally (read.c:3472, ar.c:209) |
| — dropped — | | amiga.c, guile.c (needs libguile — compile out, default), remote-cstms.c / remote-stub.c (remote-jobs, unused), vms*.c (VMS), vms_exit.c vms_export_symbol.c vms_progname.c vmsfunctions.c vmsify.c vmsjobs.c |

Port shape: no autotools. A hand-written `config.h` (house style — like the
textutils ports) selects: `HAVE_MKSTEMP 1`, `HAVE_FDOPEN 1`,
`HAVE_GETOPT_LONG 1`, `HAVE_STRNDUP 0` (make's own `xstrndup` fallback,
misc.c:326), `HAVE_UMASK 0` (make's own `umask`, misc.c:569), `HAVE_MEMRCHR 0`
(self fallback), `HAVE_GETLOADAVG 0`, `HAVE_GETRLIMIT 0` / `HAVE_SETRLIMIT 0`
(only stack re-sizing), `HAVE_PATHCONF 0` (falls back to PATH_MAX),
`HAVE_GETLOGIN 0` → *requires the getlogin() stub below*, `HAVE_PSELECT 0`,
NLS off (`--disable-nls` equivalent; `setlocale(LC_ALL,"")` tolerated by the
sysroot locale layer but make's `_()` maps to plain strings). **`-j1` v1**
makes `HAVE_SETITIMER`/jobserver machinery inert.

### 3.2 OS-facing surface (make binary, at build/run time)

| Call | Sysroot status | Anchor | Fix estimate |
|---|---|---|---|
| open read write close lseek | FULL | user libc.c:318,328,332; stat.c:78 SYS_LSEEK | — |
| stat fstat lstat | FULL | stat.c (SYS_STAT/FSTAT/LSTAT) | — |
| fork | FULL | libc.c:313 SYS_FORK | — |
| execv execve execvp | FULL | libc.c:752,759; execvp in stdlib.o | — (**execvpe** called only on z/OS branch, job.c:2599 — POSIX branch uses execvp; not needed) |
| pipe | FULL | libc.c:556 | — |
| waitpid wait | FULL | libc.c:698,703; SYS_WAITPID blocks (make's `reap_children(1,…)` blocking path) |
| kill | FULL | libc.c:311 SYS_KILL | — |
| signal sigaction sigprocmask | FULL | signal.h:106-120; P5 engine actually delivers SIGCHLD/SIGPIPE/SIGTERM (kernel process.c:425-462: SIGCHLD pending + wake on child exit; main.c:313 frame engine + SIGRETURN; procmask rows present) | verify in port lane: kill-delivery while the caller is blocked in waitpid/read |
| fcntl F_SETLK/F_SETLKW + flock | FULL | libc.c:368 (F_SETLKW = bounded-retry loop over the SYS_FCNTL record lock), flock at :391 | used only for `--output-sync` (default off) |
| getopt_long | FULL | getopt.c (+ make's own src/getopt.c) | — |
| getcwd | FULL | libc.c:673 SYS_GETCWD | — |
| getenv setenv putenv unsetenv | FULL | stdlib.o (stdlib.h:112-114) | putenv: function.c:1784, variable.c:2037 |
| mkstemp | FULL | stdlib.c:750 (mkstemps too); the insecure mktemp/tmpnam fallback is `#if !HAVE_MKSTEMP`'d out (misc.c:640,704) |
| fdopen | FULL | stdio.h:51 | |
| realpath | FULL | realpath.c:21 | |
| isatty | FULL | libc.c:640 | |
| getpwuid getpwnam | FULL | pwd.c:32,45 (synthetic single user, uid 0 "user") | for `~name` expansion |
| **opendir/readdir/closedir** | **MISSING (ENOSYS stubs)** | dirent.c:12-30 (all three return ENOSYS) | **S/M — wire to the existing `read_dir` syscall** (SYS_READ_DIR=15 → vfs_read_dir → fat16_read_dir, kernel-side COMPLETE at trap.c:1048/1729, fat16.c:985). make's dir cache (dir.c:613,733,802-808) reads every referenced directory: without it `file_exists_p` returns 0 for everything (dir.c:815), killing VPATH, `$(wildcard)`/glob, implicit `%`-rule matching and default-makefile discovery. Explicit-rule targets still resolve via `stat` (`name_mtime`, remake.c:52), so `make -f Makefile.hobbyos` with explicit lists mostly dodges it — but the fix is the make-port lane's first dependency. |
| **glob globfree** | MISSING in sysroot, **bundled** in make | lib/glob.c (+fnmatch.c); wired with make's own dir hooks (dir.c:1362-1366) | transcribe with the port; runtime still needs working readdir (row above) |
| **getlogin** | **MISSING** (no decl anywhere in sysroot) | read.c:3114 unconditional `getlogin()` in the `~` expansion path (`#if !defined(_AMIGA) && !defined(WINDOWS32)`) → **link error** | **S** — sysroot `getlogin()` returning getenv("LOGNAME")/"user", or a make-local stub |
| **alarm** | **MISSING** | posixos.c:580-599 compiles when SIGALRM defined (it is, signal.h:49) and calls `alarm(1/0)` — load-throttle path only (`set_alarm`) | **S** — sysroot `alarm()` stub returning 0 (load throttle then never fires; with `-j1` it is unused), or add a real timer |
| getloadavg | MISSING (no proc fs) | job.c:2138, graceful fallback (errno==0 → "unsupported", then /proc/loadavg) | none for v1 (no `-l`, no `-j`) |
| getrlimit setrlimit | MISSING | main.c:1212-1217,2815 (stack resize, `HAVE_GETRLIMIT`-guarded) | none (compile out) |
| umask ttyname pathconf | self-provided fallbacks | misc.c:569 (umask `#ifndef HAVE_UMASK`), misc.c:792 (ttyname, EMX only), misc.c:884 (pathconf) | none |
| strndup memrchr | MISSING | used by make's xstrndup (misc.c:332) + a fallback (misc.c:326 `#ifndef HAVE_STRNDUP`); memrchr self-provided | none (config `HAVE_STRNDUP 0`) |
| system popen | not on POSIX path | job.c:1515 is inside the `__MSDOS__`/DJGPP branch | none |
| mmap ioctl termios utime | not used by make core | grep empty for ioctl/termios/utime; mmap unused | none |

### 3.3 Shell usage, -j jobserver, output sync

- make runs every recipe as `$(SHELL) -c '...'` — with `SHELL=/TOOLS/SH.BIN`
  (frozen F3 interface) this is exactly the sh `-c` + file modes
  (sh.c:3346/3351). make's "unixy shell" detection (path contains `/`) passes.
- **`minimal -j1 v1` (recommended, matches AD-7/R5):** with `-j1` make never
  opens the jobserver pipe (jobserver = `-j>1` + shared fds), never takes the
  O_SYNC fcntl lock (only with `--output-sync`), and the load path is inert —
  the parent simply `reap_children(1,0)`, i.e. a **blocking waitpid**
  (job.c:1835,1966), which the sysroot supports. TCG is serial-bound anyway.
- **What `-j` will need later** (recorded, not v1): (a) jobserver pipe fds
  inherited across exec (pipe+dup2 full), (b) SIGCHLD delivery *while blocked*
  in `read()` on the jobserver pipe — make relies on the handler interrupting
  the read (job.c:599-619) or on pselect (missing in sysroot; poll/select
  exist) — verify EINTR/SA_RESTART semantics at port time, (c) per-child
  process-group handling for interrupt (make sends SIGINT to the process
  group of the job — kill(2) on a group id is a sysroot/kernel question),
  (d) stdout capture for output sync (pipe full).

### 3.4 Exact sysroot gaps (make)

| Gap | kind | size (estimate) | waves |
|---|---|---|---|
| dirent: wire opendir/readdir/closedir to the existing read_dir syscall | libc + kernel-verify (SYS_READ_DIR path exists) | **M** (readdir glue + per-call tests; reuse fat16_read_dir) | M1 (shared with sh `test -d` and mtools) |
| getlogin() | libc | **S** | make-port lane |
| alarm() | libc/kernel timer | **S** (stub acceptable v1) | make-port lane |
| glob → bundled (no sysroot work) | — | — | make-port lane transcribes lib/glob.c+fnmatch.c |
| (future) pselect / SA_RESTART read-interrupt for `-j` | kernel signal plumbing | **L** | post-v1 |

## 4. Portability audit — mtools 4.0.49 (mcopy + mmd subset)

### 4.1 Subset: file list + decisions

The whole tree is ~24.7k LOC over ~60 C files; every command links the same
shared library. The port takes **mcopy + mmd** plus the shared files they
pull; floppyd, xdf, scsi, mzip, mpartition, mformat, mbadblocks, minfo are
NOT ported (mkfs.fat covers formatting; mcopy on an image file only needs
plain file I/O on `-i IMG`).

| File | LOC | Role |
|---|---|---|
| mcopy.c | 726 | copy implementation (unix→dos = the staging/acceptance **copy-in path**) |
| mmd.c | 195 | make-directory main |
| mainloop.c | 618 | generic main (the `-i`/`-c` option plumbing) |
| directory.c fat.c vfat.c dirCache.c mk_direntry.c file.c stream.c buffer.c | 145/1029/912/374/721/812/136/414 | FAT16 FS engine on top of image streams |
| plain_io.c | 471 | plain-file image backend (lseek+read/write; its own pread/pwrite fn-pointers at stream.h:38-61 — **no OS pread/pwrite needed**) |
| init.c | 641 | drive/geometry init (for `-i IMG` reads geometry from the image BPB) |
| devices.c | 1225 | raw-floppy geometry/locking — **trim to ~200** for image mode (see stubs) |
| misc.c missFuncs.c charsetConv.c match.c nameclash.c tty.c privileges.c config.c | 303/572/462/167/…/238/215/961 | shared helpers; missFuncs provides its own `strndup`; charsetConv has a non-iconv codepage-table fallback (`#ifdef HAVE_ICONV_H` → own CP4xx/CP8xx tables, default CP850); config.c can be a minimal empty-config stub; tty.c is prompt/password code |

Total ≈ 11-12k LOC transcribed ≈ 8-9k after trims. **VFAT LFN writing is
kept** — vfat.c + mk_direntry.c long-name writers + charsetConv conversion
tables; the staging's mcopy-from-host path and the in-OS acceptance's
copy-in path both rely on long-name creation (AD-11 real source tree).

### 4.2 OS-facing surface (subset)

| Call | Sysroot status | Anchor | Fix estimate |
|---|---|---|---|
| open read write close lseek (pread/pwrite via its own stream layer) | FULL | libc.c:318-335; stat.c:78 | — |
| stat fstat | FULL | stat.c — image size probing | — |
| getopt | FULL | getopt.c; `-i` handling in mainloop | — |
| getpwuid | FULL | pwd.c:32 (synthetic) — for `~` in paths/config | — |
| gettimeofday / time | FULL | libc.c:835; time.c | timestamps |
| utime | FULL | stat.c (utime.h) — mcopy `-m` mtime pass-through |
| mkstemp | FULL | stdlib.c:750 |
| setlocale / nl_langinfo(CODESET) | PARTIAL — byte-only C locale | locale.c, langinfo.c; charsetConv.c:358 reads CODESET → "ANSI_X3.4-1968" → ASCII conversion path (adequate for the 8.3 + LFN-ASCII staging payloads; non-ASCII LFN chars use the internal CP850 table) | none for v1; document for wave 3 |
| strndup | MISSING but self-provided | missFuncs.c:47 (also config.c uses it — resolves via missFuncs) | none |
| **ioctl** | **MISSING** (no ioctl anywhere in sysroot) | devices.c:103-244 (GDGETA/GDSETA/floppy geometry), plain_io.c:224 (BLKFLSBUF — inside `#ifdef BLKFLSBUF`, so **compiled out** when the macro is undefined) | **S — one `ioctl()` stub returning -1/ENOTTY**; verify at port time that image-mode never reaches the floppy-geometry call sites (it doesn't: geometry comes from the image BPB) |
| **termios tcgetattr/tcsetattr** | **MISSING** (no termios.h) | tty.c (guarded `#ifdef HAVE_TERMIOS_H`) | **S — stub pair returning 0** (prompts fall back to plain read); only interactive "overwrite?"/password paths use them |
| **opendir/readdir** | **MISSING (ENOSYS)** | dirent.c:12-30 | same dirent fix as make — only needed for **`mcopy -s` local-directory recursion** (mcopy.c:300 unix_copydir); the disk recipe uses individual-file MCOPY lines (no `-s`), so v1 can skip it; wire it with the shared fix |
| fork/kill/signal/pipe | not in subset | floppyd only | — |
| flock/fcntl | raw-device lock paths only (devices.c loop/geometry locks); not used for images | — | — |
| system/popen | not in subset (mtype -t / floppyd) | — | — |

### 4.3 Stub list (every stub the port needs, flag each)

1. `ioctl()` → -1/ENOTTY (devices.c floppy paths; verify unreachable for
   `-i IMG`).
2. `tcgetattr()`/`tcsetattr()` → 0 (tty.c prompts; keep a `struct termios`
   typedef locally).
3. `opendir/readdir` — NOT a stub-once: use the shared real fix (or avoid
   `-s`).
4. BLKFLSBUF: never defined → plain_io's `file_discard` compiles to `return 0`.
5. config.c → minimal (no mtools.conf on the OS); `-i` + `-c` covers the
   recipes.

## 5. Portability audit — dosfstools 4.2 mkfs.fat

### 5.1 Files + size

`mkfs_fat_SOURCES = mkfs.fat.c + common.c (+charconv) + device_info.c`
(src/Makefile.am:40-44) — **4 files, ≈3.1k LOC** (mkfs.fat.c 1961, common.c
368, charconv.c 373, device_info.c 349). The boot-sector writer is **inside
mkfs.fat.c** (no src/boot.c in the link set; boot.c is fsck's). Smallest of
the three ports.

### 5.2 OS-facing surface

| Call | Sysroot status | Anchor | Fix estimate |
|---|---|---|---|
| open(O_EXCL\|O_RDWR) and open(O_EXCL\|O_RDWR\|O_CREAT,0666) | **FULL** — the kernel honors O_CREAT/O_EXCL/O_TRUNC POSIX-style | fs.c:103-158 (`!exists && !(O_CREAT)` → ENOENT; `exists && O_CREAT && O_EXCL` → EEXIST); fcntl.h:28-33 | — |
| lseek SEEK_SET (io.c wrappers) | FULL | stat.c:78 | — |
| **SEEK_HOLE / SEEK_DATA** | **NOT PRESENT in 4.2 at all** (whole-tree grep empty) | — | none — the brief's "check 4.2!" resolves to: 4.2 never probes holes; plain lseek+write only |
| write (block device_write) | FULL | libc.c:332 | |
| **ftruncate** | **FULL** | libc.c:403 SYS_FTRUNCATE (kernel resizes FAT16 files, row 33) | `-C` sizing at mkfs.fat.c:1879 |
| **fsync** | FULL | stat.c:173 (FAT16 writes synchronous → effectively a no-op barrier) | |
| fstat | FULL | stat.c — device_info.c:289 sizes + S_ISREG (regular file path); "discover size" never needs ioctl for a plain file | |
| getopt_long | FULL | getopt.c | |
| localtime / gmtime (volume date/label timestamps) | FULL (UTC-only, localtime==gmtime) | time.h:62-63, time.c | |
| errno/strerror/stdio | FULL | error.h, stdio.c | |
| malloc/calloc/realloc | FULL | |
| **ioctl** | MISSING | only in src/blkdev/blkdev.c (BLKGETSIZE64/BLKGETSIZE/FDGETPRM/DKIOC/DIOC) reachable for real block devices only; device_info uses fstat for regular files (device_info.c:289-295) | **S — `ioctl()` stub → -1/ENOTTY** (and compile OUT the `LOOP_GET_STATUS64` arm at device_info.c:246, a Linux-ism) |
| check_mount (read /proc/mounts) | host-only | mkfs.fat.c:1861 | replace with a no-op in the port (no mounts in-OS) |

### 5.3 What mkfs.fat writes for `-F 16` (boot/BPB compatibility note)

mkfs.fat 4.2 writes the standard DOS BPB (jmp+OEM, bytes/sector 512, sectors
per cluster, reserved sectors, FAT count, root entries, media 0xF8, FAT
size, total sectors, spt/heads, hidden sectors, total32 0, drive number,
extended boot sig, volume id/label, FAT32 FS type string vs FAT16 BS
"FAT16   ") plus the boot code and 0x55 0xAA. **"Byte-compatible-enough"**
for HobbyOS = the kernel's BPB-data-driven parser (fat16.c reads BPB; §3
snapshot) plus Limine/OVMF reading the FAT to find `/EFI/BOOT/BOOTAA64.EFI`
require: correct cluster/FAT math, 2 FATs, a root directory, valid reserved
sector count, and total size that the FAT16 cluster arithmetic accepts. It
is **NOT** byte-identical to the host's mkfs.fat: OEM string, volume-id RNG,
boot-code bytes and a few padding fields differ — nobody reads those on
this platform. The in-OS `-F 16` must be given an explicit size (`-C IMG
BLOCKS`) so cluster sizing lands in the allowed FAT16 range (≤65524
clusters): 256 MiB @ 512 B/sector → mkfs.fat auto-raises SPC (8 KiB
clusters / spc=16 at 256 MiB), same shape as today's host 1 GiB image
(larger clusters).

## 6. sh capability matrix — can our shell run make's recipes?

Method: (1) enumerate every construct appearing in the host Makefile's
recipe lines (`grep -nP '^\t'` — 100s of compile/link/copy lines plus a
handful of guard recipes), (2) read `src/user/sh.c` (3369 lines) for each,
(3) mark SUPPORTED / PARTIAL / ABSENT with a sh.c anchor and a fix estimate
where the acceptance path needs it. Recipes that are host-only (QEMU, scp,
ssh, python test harnesses, git, unzip, du) are column-separated as
`host-only (dropped in Makefile.hobbyos)`.

| construct | used by host Makefile recipes | sh.c status | evidence (sh.c anchor) | fix estimate |
|---|---|---|---|---|
| `mkdir -p DIR` | yes (≈120×, mostly `@mkdir -p obj/...`) | **PARTIAL** — mkdir is an EXTERNAL command (not a sh builtin) and `src/user/mkdir.c` ignores `-p` (it unlinks nothing / mkdirs every argv incl. "-p") | builtins list sh.c:2113 (no mkdir); mkdir.c:22; sh runs externals via pick_binary/execv sh.c:2051,2337 | **S** — teach MKDIR.BIN `-p` (multi-level parent create), or have staging precreate the dirs on the image (then recipes use bare `mkdir obj`) |
| `[ -f FILE ]` , `[ -n "$X" ]` , `[ -z "$X" ]`, `[ "A" != "B" ]`, `[ -e ]` | yes (`@test -f $@` guards, `if [ -f "$X" ]`, `.arch-stamp` compare) | **SUPPORTED** | test builtin `[` (sh.c:3083); test_unary -f/-n/-z/-e (sh.c:3047-3056 via file_exists open()); test_binary `=`/`!=`/`-eq`… (sh.c:~3070) | — |
| `[ -d DIR ]` | not in host recipes | **PARTIAL** — `test_unary -d` calls `opendir()` → ENOSYS stub → always false | sh.c test_unary -d → opendir; dirent.c:12 | same dirent fix (M) |
| if/then/elif/else/fi | yes (3 ifs, 2 elif/else guards) | **SUPPORTED** | header sh.c:8-11; parser AST | — |
| `&&` / `\|\|` / `;` separators | yes (fetch guard, arch-stamp line, multi-command lines) | **SUPPORTED** | header sh.c:10 | — |
| `{ list; }` block | yes (`|| { rm -f …; bash …/fetch.sh; test -f $@; }`) | **SUPPORTED** | header sh.c:9 (`{ list; }`) | — |
| redirections `>` `>>` | yes (`echo … > obj/x/limine.conf`, `printf … > file`) | **SUPPORTED** | header sh.c:10 | — |
| redirection `2>` / `2>&1` | host-only targeting `/dev/null` | **PARTIAL** — `2>` supported but there is **no `/dev/null`** on the FAT disk; sh's redirect open would create a file | sh redirs sh.c:10; open() semantics fs.c:103 | Makefile.hobbyos drops all `/dev/null` lines (host-only); if ever needed: precreate `/DEV/NULL` or sh-special-case |
| shell command substitution `$(cmd)` / backticks (recipes spell it `$$(cmd)`) | yes (`.arch-stamp` guard: `$$(cat .arch-stamp 2>/dev/null)`) | **SUPPORTED** | header sh.c:8 (`$(...)` and backticks) | Makefile.hobbyos may keep or simplify |
| `\` line continuation | yes (QEMU/ssh multi-line + HOST_RUN) | **SUPPORTED** | sh.c:499-500 backslash-newline | — |
| var assignment + expansion in recipes (`TO=""; …; $$X`, `$$src`) | yes (HOST_RUN, qemu detect) | **SUPPORTED** | header sh.c:6-7 | — |
| `$@` / positional (`$$@`) | host lines only | **SUPPORTED** | header sh.c:7 ($0..$9, $@, $*) | — |
| `command -v` | host-only (4×, qemu/timeout detection) | **PARTIAL** — no `command` builtin | builtins sh.c:2113 | drop (host-only); add `command` builtin only if a future recipe needs it (S) |
| `echo` / `printf` builtins | yes | **SUPPORTED** | builtins sh.c:2114 | — |
| `rm -f …` | host-only (clean target) | **PARTIAL** — RM.BIN iterates argv and `unlink()`s names including "-f" (rm.c:22) | rm.c:15-24 | **S** — RM.BIN `-f`/`-r` support, or make the clean target list files (`rm FILE…` acceptable since `unlink` ENOENT errors; better: fix RM.BIN) |
| `touch FILE` | 2 host lines | **SUPPORTED** | touch.c:23 open(O_WRONLY\|O_CREAT) | — |
| `cp`/`mv`/`ls`/`cat` | not in recipes | **SUPPORTED** (externals exist: CP/MV/LS/CAT BINs) | — | — |
| `$(shell …)` at makefile level | **4 uses** (makefile.scope, NOT recipe lines): `UNAME_S := $(shell uname -s)`, `CURRENT_MODE …`, `$(shell mkdir -p …)`, `CLANG_RESOURCE_INCLUDE := $(shell $(CC) -print-resource-dir …)` | n/a — makefile expansion, not sh | Makefile:6,131,133,155 | **DROPPED in Makefile.hobbyos** (brief's directive) — replacements: fixed ARM arch; mode file optional; MKFAT/OUT.IMG paths fixed; `-isystem /TOOLS/RESDIR/include` (AD-5) replaces -print-resource-dir |
| `for`/`while`/`case`/`until` | not in recipes | **SUPPORTED** (parser) | header sh.c:8-9 | — |
| functions `name() { }` | not in recipes | SUPPORTED | header sh.c:9 | — |
| heredocs `<<` | not in recipes | **ABSENT** (graceful: swallowed past delimiter, sh.c:394) | sh.c:394 | none (not needed) |
| background `&` + `wait` | not in recipes | SUPPORTED | header sh.c:10 | — |
| globbing `* ? [..]` for external args | not in recipes (make expands its own lists) | SUPPORTED | header sh.c:16 | — |
| **recipe execution contract** `SHELL -c cmd` | the one contract make needs | **SUPPORTED** — `sh -c script` runs sh_run_text; also `sh file` script mode; exit status returned (make sees rc via waitpid) | sh.c:3346-3351 | — |
| external command resolution | make will invoke `/TOOLS/*.BIN` absolute paths | **SUPPORTED for absolute paths** (pick_binary passes names containing `/` through verbatim, sh.c:2053-2070); bare names resolve cwd→`/` only (NO /TOOLS search, sh.c:3335 PATH="/") | sh.c:2051-2110 | Makefile.hobbyos MUST use absolute `/TOOLS/*.BIN` names — which the frozen F1 layout already requires; optionally add PATH support to sh later (S/M) |
| command line length | — | 2048 B/line, 1024 tokens — compile recipes are short | sh.c:39-46 | — |

### 6.1 Itemized sh-gap fix list (becomes M3 / wave-2 tasks)

1. **M** — sysroot dirent: wire `opendir/readdir/closedir` to the existing
   `read_dir` syscall (SYS_READ_DIR=15, kernel-complete). Unlocks: make's
   dir cache (VPATH/wildcards/implicit `%`), sh `test -d`, mtools `mcopy -s`.
2. **S** — MKDIR.BIN `-p` (multi-level) — or precreate dirs at staging so
   recipes only need single-level `mkdir`.
3. **S** — RM.BIN `-f` (`-r` for directories: unlink refuses dirs today).
4. **S** — getlogin() + alarm() stubs for make (see §3.4). Not sh itself.
5. (optional, S/M) — sh `command` builtin + PATH search for `/TOOLS` if
   recipes ever use bare names instead of absolute /TOOLS paths.
6. Nothing in this list blocks `make -f Makefile.hobbyos user` IF the sketch
   uses absolute /TOOLS paths, explicit file lists, no wildcards, no `-s`
   recursion and precreated dirs — the sh matrix proves the *core* recipe
   constructs (if/elif/[ ]/&&/||/;/redirs/echo/sh -c) all work today.

## 7. `Makefile.hobbyos` sketch + recommendations

### 7.1 Sketch (fenced here per the lane brief; the REAL file lands in wave 2)

Design constraints this sketch bakes in (all derived from §3–§6):

- **No `$(shell)`, no `$(wildcard)`, no implicit rules, no VPATH, no
  `%`-patterns in recipes** — every name explicit; make's dir-cache/glob
  paths stay out of the critical path (see §3.2 readdir row and §6 row 11).
- **Absolute `/TOOLS/*.BIN` commands** (sh has no PATH search for /TOOLS,
  §6 row "external command resolution").
- **One command per recipe line**, no `;`-chains, no `$$()` (may use
  `SHELL`-visible `$$` only where unavoidable), no `/dev/null` (does not
  exist on FAT), and newline-only continuations.
- **`SHELL=/TOOLS/SH.BIN`** (F3) — `sh -c` contract proven (sh.c:3346).
- **Stage markers** `[SELFHOST] phase=<name> rc=<n>` echoed by the heavy
  steps — the acceptance parser (M4 harness) greps them; the vocabulary:
  `phase=check-start/-done`, `phase=user-start/-done`,
  `phase=kernel-compile-start/-done`, `phase=kernel-link-start/-done`,
  `phase=disk-start/-done`, `phase=all-done`. Every recipe echoes its marker
  with `$$?` (make's `rc`) so the transcript shows failures in-band.
- **Flags verbatim from the host Makefile** (kernel Makefile:183, user
  :189, C++ :215/226) plus `-isystem /TOOLS/RESDIR/include` per AD-5 (the
  kernel build resolves `<stdint.h>` from the resource dir today; in-OS the
  resource dir is `/TOOLS/RESDIR/include`).
- **Frozen targets (F3)**: `check`, `user`, `kernel`, `disk`, `all`.

```makefile
# Makefile.hobbyos — in-OS build driver sketch (lane L4, wave-2 make port
# owns the real file).  Shell-conservative by design: SHELL=/TOOLS/SH.BIN,
# one command per line, no $(shell)/$(wildcard)/implicit rules; every file
# explicit.  Kernel source list (59 ARM TUs, measured on the lane worktree)
# is enumerated below; the make-port lane re-derives it from the host
# Makefile's C_SRCS/ASM_SRCS wildcards (Makefile:335-342) and freezes it.
SHELL       := /TOOLS/SH.BIN
CC          := /TOOLS/CLANG.BIN
LD          := /TOOLS/LLD.BIN
OBJCOPY     := /TOOLS/OBJCOPY.BIN
AR          := /TOOLS/LLVMAR.BIN
MKFSFAT     := /TOOLS/MKFSFAT.BIN
MMD         := /TOOLS/MMD.BIN
MCOPY       := /TOOLS/MCOPY.BIN
RESDIR      := /TOOLS/RESDIR/include

ARCH        := aarch64-none-elf
TARGET      := hobbyos.elf
BIN         := hobbyos.bin
OUTIMG      := /SRC/OUT.IMG
OBJDIR      := /SRC/obj
DISK_BLOCKS := 524288   # 256 MiB / 512 B  (F4: OUT.IMG <= 256 MiB; the
                        # formatting tool auto-raises the cluster size to
                        # keep FAT16 within <=65524 clusters)

# -- flags verbatim from the host Makefile -----------------------------
KERNEL_CFLAGS := -O2 -Wall -Wextra -g -Isrc/include \
                 --target=$(ARCH) -ffreestanding -mcpu=cortex-a53 \
                 -mgeneral-regs-only -isystem $(RESDIR)
USER_CFLAGS   := -O2 -Wall -Wextra -g -Isrc/user_include \
                 -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include \
                 --target=$(ARCH) -ffreestanding -mcpu=cortex-a53 \
                 -isystem $(RESDIR)
CXX_USER_FLAGS := $(USER_CFLAGS) -std=c++17 -fno-exceptions -fno-rtti -nostdinc++
# (kernel is C + asm only; the C++ policy flags are kept for the acceptance
#  user set, e.g. DESKTOP, if it links cheaply — AD-9.)

# -- kept source sets ---------------------------------------------------
# kernel: ALL 59 ARM C/asm (measured on the lane tree; the host ~71 count
# includes the x64 arch branch which is NOT in the ARM self-host build).
KERNEL_C := \
  src/kernel/main.c src/kernel/process.c src/kernel/program_loader.c \
  src/kernel/fat16.c src/kernel/fs.c src/kernel/vfs.c src/kernel/pipe.c \
  src/kernel/net.c src/kernel/dhcp.c src/kernel/virtio_blk.c \
  src/kernel/virtio_gpu.c src/kernel/virtio_gpu_x64.c \
  src/kernel/virtio_input.c src/kernel/virtio_net.c src/kernel/vm.c \
  src/kernel/time.c src/kernel/unit_test.c src/kernel/frame.c \
  src/kernel/nfs.c src/kernel/nfs_proto.c src/kernel/ipc_proto.c \
  src/kernel/iommu_vtd.c src/kernel/net_rdma.c src/kernel/unix.c \
  src/kernel/test_fw_cfg.c src/kernel/test_smp.c \
  src/kernel/fat16_test.c src/kernel/fs_test.c src/kernel/frame_test.c \
  src/kernel/locks_test.c src/kernel/math_test.c src/kernel/mmu_test.c \
  src/kernel/net_test.c src/kernel/net_rdma_test.c \
  src/kernel/nfs_test.c src/kernel/pipe_test.c src/kernel/process_test.c \
  src/kernel/program_loader_test.c src/kernel/time_test.c \
  src/kernel/timer_test.c src/kernel/trap_test.c src/kernel/unix_test.c \
  src/kernel/virtio_blk_test.c src/kernel/virtio_gpu_test.c \
  src/kernel/virtio_input_test.c src/kernel/virtio_net_test.c \
  src/kernel/vm_test.c \
  src/kernel/arch/arm/cpu.c src/kernel/arch/arm/gic.c \
  src/kernel/arch/arm/locks.c src/kernel/arch/arm/mmu.c \
  src/kernel/arch/arm/rtc.c src/kernel/arch/arm/smp.c \
  src/kernel/arch/arm/timer.c src/kernel/arch/arm/trap.c \
  src/kernel/arch/arm/uart.c
KERNEL_ASM := src/kernel/arch/arm/boot.s src/kernel/arch/arm/fpu.s \
              src/kernel/arch/arm/setjmp.s
# (the *_test.c files order as ordinary objects: only the boot list in
#  main.c has test-role logic; removing test TUs is an M4 trimming option.)
# The object rules below build into $(OBJDIR)/arm/; the make-port lane
# derives the full link list with $(patsubst src/kernel/%.c,$(OBJDIR)/arm/%.o,…)
# (flat per-TU names — arm/locks.c vs locks_test.c never collide).
KERNEL_C_SRC := $(KERNEL_C)   # + KERNEL_ASM handled by the .s pattern rule

# user: acceptance set AD-9 (HELLO SH LS CAT; DESKTOP if it links cheaply).
USER_C     := src/user/hello.c src/user/sh.c src/user/ls.c src/user/cat.c
USER_BINS  := HELLO.BIN SH.BIN LS.BIN CAT.BIN

# dropped groups (v1): browser, ICU/Skia/glib, python tiers, E2E, screenshots,
# x64 branch, libcxx (archive pre-built into the toolchain image), tests.

# -- pattern rule (one command per line, explicit, no dir scan) ---------
# NOTE on directory creation: `mkdir -p` is not -p-aware in-OS today (sh
# matrix row 1; MKDIR.BIN ignores -p, mkdir.c:22).  Two wave-2 options:
# (a) fix MKDIR.BIN -p; (b) pre-create every needed directory on the disk
# image at staging (mmd/mcopy) so recipes use single-level `mkdir
# $(OBJDIR)/arm` (the FAT mkdir syscall is one-level).  The sketch assumes
# (b) — staging pre-creates /SRC, /SRC/obj, /SRC/obj/arm, /SRC/obj/user.
$(OBJDIR)/arm/%.o: src/kernel/%.c
	/TOOLS/MKDIR.BIN $(OBJDIR)/arm
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@
	@echo "[SELFHOST] phase=kernel-compile rc=$$?"

$(OBJDIR)/arm/%.o: src/kernel/arch/arm/%.s
	/TOOLS/MKDIR.BIN $(OBJDIR)/arm
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@
	@echo "[SELFHOST] phase=kernel-ascompile rc=$$?"

# -- frozen targets (F3) ------------------------------------------------
check:                       # one-TU compile smoke (T3.1)
	@echo "[SELFHOST] phase=check-start"
	/TOOLS/MKDIR.BIN $(OBJDIR)/arm
	$(CC) $(KERNEL_CFLAGS) -c src/kernel/main.c -o $(OBJDIR)/arm/check.o
	@echo "[SELFHOST] phase=check-done rc=$$?"

user: $(OBJDIR)/arm/sh.o $(OBJDIR)/arm/ls.o $(OBJDIR)/arm/cat.o
	@echo "[SELFHOST] phase=user-start"
	$(CC) $(USER_CFLAGS) -c src/user/hello.c -o $(OBJDIR)/arm/hello.o
	$(LD) -T src/user/linker.ld -o $(OBJDIR)/arm/sh.elf \
	      $(OBJDIR)/arm/sh.o $(OBJDIR)/arm/hello.o
	$(OBJCOPY) -O binary $(OBJDIR)/arm/sh.elf $(OBJDIR)/arm/SH.BIN
	# ... one link+objcopy recipe per acceptance program; SH links against
	# the prebuilt libc archive (obj/arm/libc.a carried in the payload) ...
	@echo "[SELFHOST] phase=user-done rc=$$?"

kernel: $(OBJDIR)/arm/$(TARGET)
	@echo "[SELFHOST] phase=kernel-link-start"
	# `*.o` is a SHELL-side glob (sh globs via the read_dir syscall,
	# sh.c:1843-1879, which works today) — NOT make's $(wildcard).
	$(LD) -T src/linker.ld -o $(TARGET) $(OBJDIR)/arm/*.o
	$(OBJCOPY) -O binary $(TARGET) $(BIN)
	@echo "[SELFHOST] phase=kernel-link-done rc=$$?"

disk: kernel user
	@echo "[SELFHOST] phase=disk-start"
	/TOOLS/RM.BIN -f $(OUTIMG)              # -C refuses an existing file
	$(MKFSFAT) -F 16 -C $(OUTIMG) $(DISK_BLOCKS)   # sizes via truncate; no dd
	$(MMD) -i $(OUTIMG) ::/BOOT ::/TOOLS ::/SRC
	$(MCOPY) -i $(OUTIMG) $(BIN) ::/BOOT/HOBBYOS.BIN
	$(MCOPY) -i $(OUTIMG) bootloader/limine.conf ::/BOOT/limine.conf
	$(MCOPY) -i $(OUTIMG) bootloader/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
	$(MCOPY) -i $(OUTIMG) $(USER_BINS) ::/TOOLS/
	# staged source payload lives under /SRC (F2) — pre-placed by staging
	@echo "[SELFHOST] phase=disk-done rc=$$?"

all: check user kernel disk
	@echo "[SELFHOST] phase=all-done rc=$$?"
```

**`dd` replacement analysis (brief-mandated):** the host disk recipe sizes
`disk.img` with `dd if=/dev/zero bs=1M count=N` (zero-filled) before
formatting. In-OS, OUT.IMG ≤ 256 MiB and there is no `/dev/zero`: the
formatter's `-C` path uses `ftruncate()` — the kernel resizes FAT16 files
(ftruncate full) and the new region is implicitly zero on first read via
the cluster allocator (no consumer can read an unwritten cluster as data),
so **ftruncate-only is sufficient and recommended**: `MKFSFAT -C OUT.IMG
BLOCKS` costs one syscall and avoids a 256 MiB write loop. A zero-fill
fallback is the alternative if any consumer ever reads never-written
sectors; the FAT allocator guarantees that cannot happen for data clusters.
Open tradeoff recorded: the pre-FAT region (boot+FAT+root) is written by
the formatter itself, so the only "sparse" region is metadata that is
always materialized. `-C` requires the explicit `DISK_BLOCKS` (the tool
sizes by argument, not by file content).

### 7.2 Recommendations

1. **Build driver v1 = GNU make** (confirms AD-7): the port is
   self-contained (~33k LOC transcribable incl. glob; §3), the sh recipe
   contract is proven (sh.c `-c`), and every executor syscall (fork, exec,
   waitpid, pipe, signals) measured FULL. `hbuild` stays the dormant
   contingency; activation gate unchanged (M3 slips > 1 week → hbuild).
2. **One-command-per-line recipe policy** (baked into the sketch): with
   `sh -c` per recipe and no `command` builtin, multi-command `;`-chains
   and `$$()` command substitution only add `test`-matrix risk; recipe
   logic uses if/elif/fi + `[ ]` forms (all SUPPORTED).
3. **Serial `-j1` policy for v1** (AD-7/R5): documented in §3.3 — the
   signal-interrupt/jobserver machinery is not needed and TCG is
   serial-bound. Recorded as post-v1 work, not risk.
4. **Wave-2 lane lineup (exact)** from this audit:
   - **make-port**: transcribe §3.1's file set + config.h; land the
     dirent glue (SYS_READ_DIR wiring) + getlogin()/alarm() stubs first;
     gate = `make -f Makefile.hobbyos user` green in-OS (T3.2).
   - **mtools-port**: transcribe the §4.1 subset (mcopy+mmd, LFN writers
     kept); ioctl/termios stubs (§4.3); gate = the disk recipe's
     `MMD`+`MCOPY` copy-in round-trip in-OS (T4.2 helper).
   - **mkfs-port**: transcribe the 4 files (§5.1) + ioctl stub +
     check_mount no-op; gate = `MKFSFAT -F 16 -C OUT.IMG` producing a
     bootable OUT.IMG the kernel parser + Limine accept (T4.2).
   - The three lanes are file-disjoint (make: src/user/make*/ +
     Makefile.hobbyos; mtools: src/user/mtools*/; mkfs: src/user/dosfs*/)
     except the shared dirent/getlogin/alarm libc items — those are parent
     single-writer sysroot milestones (F6 protocol: one commit each, both
     trap.c for syscall changes; the dirent glue is libc-only).
5. **Open questions recorded for wave 2** (feed the make-port lane brief):
   (a) final kernel source list incl. whether to drop src/kernel/*_test.c
   TUs from the v1 kernel image (build-time only — they are ordinary TUs);
   (b) exact OUT.IMG size (this sketch uses 256 MiB / 524288 blocks; L5
   owns the final math); (c) whether MKDIR.BIN -p or staging-precreate
   wins (both work); (d) DESKTOP in the acceptance set (links libcxx —
   payload weight only).

