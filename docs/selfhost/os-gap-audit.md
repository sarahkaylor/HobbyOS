# HobbyOS OS-runtime gap audit: can HobbyOS host clang/lld/llvm-ar/llvm-objcopy?

**Lane L2 (selfhost program).** Scope: every OS-facing call clang/lld/llvm-ar/llvm-objcopy
21.1.8 make through `llvm/lib/Support` when compiled as HobbyOS EL0 userland programs, checked
against the HobbyOS libc/syscall/header surface. Anchors are `file:line` on one or both sides.
LLVM paths are relative to the extracted source (`~/.hermes/cache/scratch/llvm-osaudit-src/`;
here abbreviated `llvm/…`, `clang/…`, `lld/…`). HobbyOS anchors are relative to this repo's
`src/` unless prefixed. **Read-only audit** — fixes are the W2-x work items in §12.

Config base for every status below: the wave-2 build sets the `llvm/Config/config.h` feature
flags listed in §13 (e.g. `HAVE_POSIX_SPAWN 0`, `HAVE_SIGALTSTACK 0`, `CLANG_HAVE_RLIMITS 0`),
targets `aarch64-none-elf` (baked default triple, no runtime `uname`), and runs with the
in-process cc1 default. Everything marked FULL still assumes a **v2** process image (8 MiB
main stack, `src/include/vm.h:58`; v1's 256 KiB stack is too small for cc1).

---

## §1 Summary

| Status | Count | Meaning |
|---|---|---|
| FULL | 66 | works unchanged on current HobbyOS |
| PARTIAL | 20 | reachable but degraded (graceful fallback or stub/no-op) |
| MISSING | 16 | compile- or link-time blocker, or hard runtime gap |
| NO-ACTION | 12 | looks scary, unreachable in this build config (§12b) |

**Top-10 riskiest seams (ranked):**

1. **Exec argv envelope too small — kernel, `src/include/process.h:112-113`**
   `HO_EXEC_MAX_ARGS 32` / `HO_EXEC_ARGV_LEN 256`. `sysconf(_SC_ARG_MAX)` reports 256
   (`src/libc/src/stat.c:195`). Every out-of-process toolchain job (linker, llvm-ar,
   assembler) carries at least `@resp -o out` plus a half-dozen direct flags; the cc1-style
   direct args of any real compile exceed 256 B. In-process cc1 (default) sidesteps it for the
   compiler itself, so the *immediate* smoke path works, but **no tool can be spawned with a
   realistic argv until this is raised** (≥4 KiB / ≥128 args). Single-writer kernel change.
2. **No temp file target — `llvm/lib/Support/Unix/Path.inc:1441-1462`**
   `system_temp_directory` = first of `TMPDIR/TMP/TEMP/TEMPDIR`, else `P_tmpdir` (`/tmp`,
   `src/libc/include/stdio.h:123`). On a clean HobbyOS disk `/tmp` does not exist and the env
   is empty → `Driver::GetTemporaryPath` (`clang/lib/Driver/Driver.cpp:6677`) and
   `TempFile::create` fail with `err_unable_to_make_temp`. Required whenever a compile+link or
   any out-of-process job runs. Fix is infra (a `/tmp` dir on the disk image and/or
   `TMPDIR`/`CLANG_TMPDIR` env from the launcher). **This is the first thing the smoke test
   hits if the disk image lacks `/tmp`.**
3. **File-backed mmap is unimplemented (correctly rejected)** —
   `sys_mmap` returns `-ENOTSUP` for non-anonymous (`src/kernel/process.c:2922-2923`) and for
   `fd >= 0` (`src/kernel/process.c:3004-3006`). LLVM degrades gracefully everywhere:
   `MemoryBuffer` falls back to a read loop (`llvm/lib/Support/MemoryBuffer.cpp:498-548`),
   `FileOutputBuffer` falls back to an in-memory buffer (`llvm/lib/Support/FileOutputBuffer.cpp:146-151`).
   But lld's link output then lives entirely in RAM (fine at our sizes) and the fallback is
   only safe *because* the kernel rejects file maps instead of silently mapping anonymous —
   keep that `-ENOTSUP`. VERIFY after first in-OS lld run.
4. **`wait4` missing — `llvm/lib/Support/Unix/Program.inc:418`** (`sys::wait4`),
   `using ::wait4` at `Program.inc:341`. Every `ExecuteAndWait` (each subprocess the driver
   starts) blocks on it. HobbyOS has only `waitpid` → one small shim (ignoring `rusage`).
5. **A cluster of missing C headers breaks the LLVM *build* before anything runs:**
   `float.h` (every TU), `setjmp.h` + `setjmp/longjmp` (`llvm/lib/Support/CrashRecoveryContext.cpp:17,77,421` —
   used by the in-process cc1 path itself!), `dlfcn.h` (included unconditionally by
   `llvm/lib/Support/Unix/Unix.h:37`), `sys/resource.h` (`rlimit`/`getrlimit`/`setrlimit` at
   `llvm/lib/Support/Unix/Process.inc:139-160`, `struct rusage`/`getrusage`),
   `sys/statvfs.h` (`statvfs`/`fstatvfs` — `Path.inc:563-580`, Linux spells
   `#define STATVFS statvfs` `Path.inc:75`). None exist in the sysroot today.
6. **Signal-constant/`sigaction` header gaps** — `SA_NODEFER` and `SA_ONSTACK` are used
   unguarded (`llvm/lib/Support/Unix/Signals.inc:321`) but absent from
   `src/libc/include/signal.h`; `pthread_sigmask` (`Process.inc:262`) absent;
   `alarm` (`Program.inc:406,432,455`; `Watchdog.inc:23,29`) absent. All one-line header/shims.
7. **Directory iteration is ENOSYS** — `src/libc/src/dirent.c:12-28` (opendir/readdir/closedir
   return `ENOSYS`) although the kernel already has `SYS_READ_DIR` (15). LLVM uses directory
   iteration only for GCC-toolchain probing and `-print-search-dirs` style scans
   (`Path.inc:894-931`); our config avoids those, but any future `-print-search-dirs` or tool
   scanning a `-L` dir hits it. Small user-space impl over `SYS_READ_DIR`.
8. **`isatty`/terminal surface is minimal** — `isatty` exists (`src/user/libc.c:640`) but
   `ioctl`/`TIOCGWINSZ` (`Process.inc:325`) and `tc{get,set}attr` are absent; llvm only probes
   columns, guarded by `HAVE_SYS_IOCTL_H` + `isatty` — set the config off and colors degrade
   to `COLUMNS`/`TERM` env. No tty is ever attached in the in-OS shell, so this is cosmetic.
9. **Crash diagnostics / signal *delivery* is notional** — handlers register fine
   (`Signals.inc:295-324`) but HobbyOS only ever invokes SIGCHLD/SIGPIPE/SIGTERM handlers
   (`src/libc/include/signal.h:67-75`); a cc1 fault kills the process without the "PLEASE
   submit a bug report" dump, and `CrashRecoveryContext::RunSafely` cannot recover from a
   real SEGV (its `sigsetjmp`-less design relies on delivery); temp-file cleanup on signal
   (`Signals.inc` `FileToRemoveList`) never runs. Not a blocker; a QA loss.
10. **In-OS `syscount`s are honest but small** — `_SC_OPEN_MAX 64`, `_SC_CHILD_MAX 64`,
    `_SC_ARG_MAX 256` (`src/libc/src/stat.c:195-215`). The command-line length math in
    `Program.inc:549-588` therefore forces response files at ~2 KB total argv; that is OK
    *only if* the response-file + temp infrastructure (§1.2) exists. cc1 `-cc1` in-process
    args bypass the blob entirely (no OS limit) ✓.

Verdict: the compiler *core* (cc1 in-process) is close to hostable once the M1 header/shims
land; the *process* part (driver spawning tools) and *storage* part (temp dir, argv) are the
load-bearing gaps, and both have clean work items documented below.

---

## §2 Filesystem surface

HobbyOS VFS: FAT16 + NFS (mount), single namespace, 8.3 names enforced in FAT, no symlinks,
no permission bits, no maintained timestamps. `SYS_* ` from `src/include/syscall.h`.

| Call | Used by | Status | HobbyOS anchor | Work item |
|---|---|---|---|---|
| open(O_CREAT/O_EXCL/O_TRUNC) | `Path.inc:1025` openFile; `Path.inc:1120` openNativeFile | FULL | SYS_OPEN (4); kernel honors O_CREAT/O_EXCL/O_TRUNC, `src/include/fcntl.h:5-8` | — |
| close | everywhere | FULL | SYS_CLOSE/SafelyClose | — |
| read / write (with EINTR retry) | `Path.inc:1192`, `raw_ostream.cpp` | FULL | SYS_READ/SYS_WRITE; EINTR never happens (no interrupting signals) | — |
| lseek + read (pread fallback) | `Path.inc:1218-1220` readNativeFileSlice | PARTIAL | SYS_LSEEK (29). Set `HAVE_PREAD 0` → lseek+read path; `pread` itself undeclared | config only |
| fstat | `Path.inc:766` status(FD); `MemoryBuffer.cpp:481` | FULL | SYS_FSTAT (31) | — |
| stat / lstat | `Path.inc:757` status(); `Signals.inc:160`; stat64 fields | PARTIAL | SYS_STAT (30); `src/libc/include/sys/stat.h:24-37` — no st_blocks/st_blksize fidelity, mtime via `#define st_mtime st_mtime_s` (sys/stat.h:22). Enough for S_IS* / size / mtime | — |
| access | `Path.inc:619`; can_execute `Path.inc:634` | PARTIAL | `src/libc/src/stat.c:120-125` — over stat(), mode ignored; X_OK accepts dirs (fine for executable probes) | — |
| realpath (incl. NULL-buffer) | `Path.inc:271,1164`; getprogpath `Path.inc:140` | FULL | `src/libc/src/realpath.c` (textual canon; no existence check — llvm only needs the name) | — |
| getcwd / chdir | `Path.inc:368,401` | FULL | SYS_GETCWD/SYS_CHDIR; `src/user/libc.c:673,682` | — |
| rename | `Path.inc:588`; lld `unlinkAsync` rename workaround `lld/Common/Filesystem.cpp:71-75` | FULL | SYS_RENAME (23) | — |
| unlink | `Path.inc:471` remove(); `Signals.inc:171` | PARTIAL | SYS_UNLINK (22) **refuses directories** (`src/user/libc.c` unlink → fat16_unlink; skill: dirs return -1); no rmdir | W2-14 (dirs) |
| mkdir | `Path.inc:416` (`::mkdir(p, Perms)` — **2 args**) | PARTIAL | sysroot has **no mkdir declaration**; user libc is 1-arg (`src/user/libc.c:590`); kernel SYS_MKDIR ignores mode | W2-7 (decl) |
| rmdir | `Path.inc` remove() on dirs | MISSING | no SYS_RMDIR | W2-14 |
| ftruncate | `Path.inc:594-597` resize_file (fallocate→ftruncate; here ftruncate) | FULL | SYS_FTRUNCATE (33), `src/include/fcntl.h:48-54` | — |
| fsync / fdatasync | P6.1 | FULL | declared `src/libc/include/unistd.h:52-54`, FAT writes are synchronous | — |
| symlink / readlink | create_link `Path.inc:433`; readlink `Path.inc:239,257,1155` | PARTIAL | `readlink` always EINVAL (`src/user/libc.c:645`); `symlink`/`link` **undeclared**; VFS has no symlinks (lstat=stat) | W2-7 (decls, stubs) |
| link (hard link) | create_hard_link `Path.inc:439`; `lld/ELF/Driver.cpp` — `--reproduce` archives only | MISSING | no `link` anywhere; only used by optional paths (reproduce) | W2-7 (stub) |
| pipe | driver child fd plumbing `Program.inc:102-116` (via open+dup2); HobbyOS-native | PARTIAL | sysroot **undeclared**; impl only in `src/user/libc.c:556` | W2-7 (sysroot decl+impl) |
| dup / dup2 | `Process.inc:248`, `Program.inc:110,283` | FULL | SYS_DUP (34)/SYS_DUP2 (35) | — |
| fcntl F_SETLK/F_SETLKW/F_SETFD/F_GETFD | `Path.inc:1235-1270` lockFile; FileLocker | FULL | SYS_FCNTL (68) + LP64 `struct flock` `src/include/fcntl.h:68-74` (P6.1) | — |
| flock | same | FULL | `src/user/libc.c:391` | — |
| utimes / futimens | `Path.inc:795-818` setLastAccessAndModificationTime | PARTIAL | `futimens` = fstat-validated no-op (`src/libc/src/stat.c:130-135`); `utime.h` exists; no maintained FAT mtimes | — |
| chmod / fchmod | `Path.inc:784,789` | FULL | no-ops (`src/libc/src/stat.c:94-104`) | — |
| fchown | changeFileOwnership `Path.inc:1338` | MISSING | undeclared — compile gap | W2-7 (stub) |
| statvfs / fstatvfs | is_local `Path.inc:563-580` (Linux: `#define STATVFS statvfs` `Path.inc:75`) | MISSING | no `sys/statvfs.h` — compile+link gap | W2-6 |
| opendir/readdir/closedir | `Path.inc:894-931` directory_iterator; `remove_directories` `Path.inc:1277-1310` | PARTIAL | ENOSYS stubs `src/libc/src/dirent.c:12-28`; kernel has SYS_READ_DIR (15) | W2-14 |
| TempFile::create / createTemporaryFile | clang driver `Driver.cpp:6677-6697`; `TempFile` create `Path.inc:1041-1080` (O_CREAT\|O_EXCL, 0600); response files `Driver.cpp:2198-2211`; cc1 output `CompilerInstance.cpp:901` | PARTIAL | needs an existing temp dir — see §1.2 | W2-9 (infra) |
| mkstemp / mkostemp | CompilerInstance PCH paths | FULL | `src/libc/include/stdlib.h:136-138` | — |
| getPathFromOpenFD | openFileForRead RealPath `Path.inc:1139-1171` | FULL (no procfs) | falls back to realpath (`Path.inc:1163-1165`) — no /proc needed | — |
| copy_file | `Path.inc:1492` (lld reproduce, objcopy `--keep-file-symbols`?) | PARTIAL | read/write loop; only used by optional paths | — |
| is_directory/exists/equivalent | everywhere | FULL | via stat S_IF* bits | — |

## §3 Process / exec

| Call | Used by | Status | HobbyOS anchor | Work item |
|---|---|---|---|---|
| fork | `Program.inc:261` Execute child path (used when `HAVE_POSIX_SPAWN 0`) | FULL | SYS_FORK (3) | — |
| execv / execve | `Program.inc:311-314` | PARTIAL | SYS_EXEC (40); `src/user/libc.c:752-759` — but **argv blob 256 B / 32 args** (`src/include/process.h:112-113`) | W2-10 (kernel) |
| execvp / execvpe | `src/libc/src/stdlib.c:476,522`; PATH search | FULL | present | — |
| posix_spawn | `Program.inc:242` | MISSING (avoided) | no `spawn.h`; **set HAVE_POSIX_SPAWN 0** → fork+exec path | config |
| wait / waitpid | `Program.inc:438` wait(); Compilation cleanup | FULL | SYS_WAITPID (39) + macros `src/libc/include/sys/wait.h` | — |
| wait4 | `Program.inc:341,418` | MISSING | no wait4 — **compile+link gap**, hot path | W2-5 |
| setsid | `Program.inc:297` (DetachProcess only) | MISSING | undeclared; only reachable with detach (never in driver) | W2-7 (stub) |
| kill / raise | `Process.inc` (mac) / `Program.inc:429` (SIGKILL on timeout) | PARTIAL | SYS_KILL (16) exists; raise over kill/`getpid` | — |
| getpid / getppid | `Process.inc:66`; `Threading.inc:215` | FULL | SYS_GETPID/GETPPID | — |
| getuid/geteuid/getgid/getegid/getgroups | P6.1; `Path.inc` | FULL | `src/libc/include/unistd.h:57-61` | — |
| getrlimit/setrlimit | `Process.inc:139-160` PreventCoreFiles (called by clang driver main path); `Program.inc:140-154` SetMemoryLimits; `cc1_main.cpp:86-103` (guard CLANG_HAVE_RLIMITS) | MISSING | no `sys/resource.h` — compile+link gap; PreventCoreFiles runs at startup | W2-4 (header+stubs) |
| getrusage | `Process.inc:53` (HAVE_GETRUSAGE); `Program.inc:413-467` Wait stats | MISSING | absent; config 0 for the `#if` path, but `rusage` struct still needed by wait4 path | W2-4/W2-5 |
| _Exit / exit / _exit | `Process.inc:433`; `Program.inc:321` | PARTIAL | `_exit` ✓ (`unistd.h:96`); `_Exit` implemented (`stdlib.c:767`) but **undeclared in stdlib.h** | W2-7 |
| alarm | `Program.inc:406,432,455`; `Watchdog.inc:23,29` | MISSING | undeclared; only used when a child timeout is set (driver passes secondsToWait=0, Job.cpp:379-385) → stub returning 0 is correct (wait blocks forever = no timeout) | W2-7 |
| sysconf(_SC_ARG_MAX/_SC_NPROCESSORS_ONLN/_SC_PAGESIZE/_SC_OPEN_MAX) | `Program.inc:551`, `Threading.inc:398`, `Process.inc:77` | PARTIAL | implemented (`src/libc/src/stat.c:190-225`); _SC_ARG_MAX 256 is the *honest* kernel blob limit (ties to W2-10); → response-file churn | W2-10 |
| environ | posix_spawn path only (HAVE_POSIX_SPAWN 0 → unused); execvpe | FULL | `src/user/libc.c` environ_init | — |

## §4 Memory

| Call | Used by | Status | HobbyOS anchor | Work item |
|---|---|---|---|---|
| mmap anon PRIVATE R/W(/X) | `Memory.inc:115`, `Path.inc:830-895` (mapped_file_region) | FULL | `sys_mmap` anon-only `src/kernel/process.c:2911-2970`; MAP_ANON\|MAP_PRIVATE accepted; MAP_FIXED rejected (-ENOTSUP) but llvm never uses it | — |
| mmap file-backed (MAP_SHARED/MAP_PRIVATE fd) | `Path.inc:830-895`; `FileOutputBuffer.cpp:142-151`; `MemoryBuffer.cpp:498-511` | PARTIAL | v1: rejected at `process.c:2922` (`!MAP_ANONYMOUS`) and `process.c:3021-3022` (fd≥0); v2: `vm_mmap` accepts fds only for memfds, regular files `-ENOTSUP` — `src/kernel/vm.c:1224-1228` ("FAT16/NFS… waits for P6") → llvm falls back to read / in-memory buffers, see §1.3 | verify only |
| munmap | `Memory.inc:155`, `Path.inc:876` | FULL | SYS_MUNMAP (63) | — |
| mprotect | `Memory.inc:187-196` | PARTIAL | SYS_MPROTECT (81) `process.c:3033` (v2 only; v1 returns unsupported) — llvm uses for PROT changes of mmap'd blocks only (JIT/bigalloc) | — |
| madvise | `Path.inc:888`; lld `ELF/Driver.cpp:2664` | PARTIAL | SYS_MADVISE (82) advisory `process.c:3051` | — |
| posix_madvise | `Path.inc:886` | no-op | same syscall | — |
| getPageSize (getauxval/getpagesize/sysconf) | `Process.inc:71-87` | FULL | getpagesize=4096 | — |
| sbrk | `Process.inc:108-115` (HAVE_SBRK, MallocUsage only) | PARTIAL | brk/sbrk implemented if sysroot ported; MallocUsage optional → set HAVE_SBRK 0 | config |
| mallinfo/mallinfo2 | `Process.inc:89-122` | no-op | config 0 → returns 0 | config |
| __clear_cache | `Memory.inc:246` (JIT only) | no-op | not reachable (no JIT in clang/lld); provide in compiler-rt if ever | — |

## §5 Signals

| Call | Used by | Status | HobbyOS anchor | Work item |
|---|---|---|---|---|
| sigaction + SA_* | `Signals.inc:321,404`, `Program.inc:404` | PARTIAL | `src/libc/include/signal.h:95-115` (sigaction, SA_RESTART/SA_RESETHAND/SA_SIGINFO/SA_RESTORER); **SA_NODEFER/SA_ONSTACK absent** (compile gap, `Signals.inc:321`) | W2-7 |
| sigemptyset/sigfillset/sigaddset/sigprocmask | `Process.inc:256-281`; `Signals.inc:403` | FULL | declared (signal.h:116-120) | — |
| pthread_sigmask | `Process.inc:262,277` (LLVM_ENABLE_THREADS=1) | MISSING | undeclared — compile gap; = sigprocmask shim | W2-7 |
| raise | `Signals.inc` (crash resurrect) | PARTIAL | raise → kill(self) | — |
| sigaltstack | `Signals.inc:264-293` | no-op | set HAVE_SIGALTSTACK 0 (guarded `#if defined(HAVE_SIGALTSTACK)`) | config |
| kill(SIGKILL) | `Program.inc:429` (child timeout) | FULL | SYS_KILL (16) | — |
| signal() | `Process.inc:183-187` (mac only) | FULL | declared | — |
| setjmp/longjmp | `CrashRecoveryContext.cpp:77,421` (in-process cc1!) | MISSING | no `setjmp.h`, no userland impl — **compile+link gap** | W2-2 |
| crash handling / signals delivered to user handlers | `Signals.inc:295+` | PARTIAL | only SIGCHLD/SIGPIPE/SIGTERM handlers ever run (signal.h:67-75); faults kill+report; crash dump, temp cleanup on signal, RunSafely SEGV recovery all inert | W2 (QoL, M5) |

## §6 Threads (libc++ std::thread / llvm::parallel over pthreads)

| Call | Used by | Status | HobbyOS anchor | Work item |
|---|---|---|---|---|
| pthread_create/join/detach/self | `Threading.inc:100,117,109,124` | FULL | `src/libc/src/pthread.c` over SYS_THREAD_CREATE (72) | — |
| pthread_attr_init/destroy/setstacksize | `Threading.inc:81-94` | FULL | pthread.h attrs | — |
| pthread_mutex_*/cond_* (via libc++ std::mutex/condvar) | llvm::parallel, lld tasks | FULL | pthread.c mutex/cond (timedwait ✓) | — |
| pthread_setname/getname_np | `Threading.inc:198-265` | no-op | config 0 (`HAVE_PTHREAD_*_NP`) — guarded | config |
| sched_yield | libc++ `std::this_thread::yield` | FULL | SYS_YIELD (17) | — |
| std::thread::hardware_concurrency | `Threading.inc:331-338`; lld `Driver.cpp:1839` | FULL | libc++ → sysconf(_SC_NPROCESSORS_ONLN) = cpu count | — |
| sysconf(_SC_NPROCESSORS_ONLN) | `Threading.inc:398` | FULL | stat.c:220 | — |
| futex/thread_local/TLS | pthread internals | FULL | SYS_FUTEX (73), SYS_SET_TLS (75), ho_tls | — |

## §7 Env / time

| Call | Used by | Status | HobbyOS anchor | Work item |
|---|---|---|---|---|
| getenv (PATH, TMPDIR, HOME, TERM, CL, _CL_, COLUMNS, LLVM_CPUINFO, LLVM_DEFAULT_TARGET_TRIPLE…) | `Process.inc:195`; driver | FULL | `src/libc/src/stdlib.c:527` | — |
| setenv/putenv/unsetenv | driver? mostly env read | FULL | stdlib.c:558-657 | — |
| environ | — | FULL | src/user/libc.c environ_init | — |
| clock_gettime (via libc++ std::chrono::system_clock/steady_clock) | llvm Time/Chrono (`llvm/lib/Support/Chrono.cpp`), lld timers | FULL | SYS_GETTIME (87); CLOCK_REALTIME/MONOTONIC `src/libc/include/time.h:81` | — |
| gettimeofday | `Process/Time` legacy | FULL | `src/user/libc.c:835` + sys/time.h | — |
| time/gmtime/localtime/strftime/mktime | ar, objcopy, lld (build timestamps via chrono) | FULL | time.h / time_math.c | — |
| nanosleep/usleep | misc sleeps | FULL | `src/user/libc.c:484,495` | — |
| setlocale | command line parsing (locale) | PARTIAL | locale.c C-only | — |
| getrandom/getentropy | `RandomNumberGenerator` (moved; sys/random.h present) | FULL | SYS_GETRANDOM (71); getrandom/getentropy `src/user/libc.c:429,460` | — |
| /dev/urandom | `Process.inc:401-414` GetRandomNumberSeed | PARTIAL | open fails → falls back to time+pid hash (`Process.inc:418-419`) — acceptable uniqueness for TempFile/ar | optional (W2-15) |

## §8 tty / misc

| Call | Used by | Status | HobbyOS anchor | Work item |
|---|---|---|---|---|
| isatty | `Process.inc:303` (HAVE_ISATTY=1) | PARTIAL | `src/user/libc.c:640`; always false on non-tty pipes — colors off; llvm treats as non-tty ✓ | — |
| ioctl(TIOCGWINSZ) | `Process.inc:325` | no-op | config 0 (HAVE_SYS_IOCTL_H) — columns handled by COLUMNS env | config |
| tcgetattr/tcsetattr/ttyname | — | no-op | no reachable consumer in 21.1.8 Unix surface | — |
| dlopen/dlsym/dlclose/dladdr | `DynamicLibrary.inc:28-48`; `Path.inc`/`Signals.inc` dladdr for backtrace | MISSING (compile) | `Unix.h:37` includes `<dlfcn.h>` unconditionally; set HAVE_DLOPEN 0 → dladdr path still needs the header at compile time; stubs return "not supported" | W2-3 |
| uname | none at runtime (triple is compile-time baked) | no-op | `getProcessTriple` = LLVM_HOST_TRIPLE `TargetParser/Host.cpp:2317`; `getHostCPUName` only probes /proc/cpuinfo → "generic" | — |
| gethostname/sethostname | not in 21.1.8 Unix surface | no-op | — | — |
| getauxval | `Process.inc:73` (guarded), Host CPU detect | no-op | config 0 (HAVE_GETAUXVAL) | config |
| /dev/null | `Process.inc:241` (FixupStandardFileDescriptors at clang startup `driver.cpp:243`); `Program.inc:97,127` redirect-empty | PARTIAL | only needed when a std fd is *closed*; HobbyOS launcher wires 0-2 → fstat passes → never opened. If a launcher ever closes 0-2, open fails → returns error (treated as startup failure). Add a null device if needed | W2-15 (optional) |

## §9 The clang driver runtime flow (highest value)

1. **cc1 runs in-process by default.** `CC1Command::Execute` with `InProcess=true`
   (`clang/lib/Driver/Job.cpp:388-441`) calls `D.CC1Main(Argv)` directly **— no fork, no
   exec, no wait** — inside `CrashRecoveryContext::RunSafely` (`Job.cpp:428-440`, which needs
   `setjmp`/`longjmp`, W2-2). The driver sets `InProcess=false` only when
   `C.getJobs().size() > 1 || CCPrintProcessStats` (`Driver.cpp:5322-5326`). A single
   `clang -c`/`clang link` job stays in-process. **Consequence: the compile half of the
   toolchain never hits the exec-argv limit or `wait4`.** The link half does (see below).
   `-fintegrated-cc1`/`-fno-integrated-cc1` exist (`clang/include/clang/Driver/Options.td:6224-6230`).
2. **`HAVE_POSIX_SPAWN` decides the subprocess mechanism.** With 0 (no `spawn.h`), `Execute`
   takes the `fork()` → redirects (`open`/`dup2`) → `setsid` (only if detach) →
   `setrlimit` (only with MemoryLimit≠0) → `execv`/`execve` → `_exit` path
   (`Program.inc:186-321`); then `sys::Wait` → **`wait4`** (`Program.inc:418`) → W2-5.
3. **Program lookup is argv0/PATH based, not /proc based.** `GetExecutablePath`
   (`clang/tools/driver/driver.cpp:62-77`) without `-canonical-prefixes` (the default) uses
   argv[0] + `findProgramByName` (`Program.inc:63-89`, PATH scan + `can_execute` per dir —
   no directory iteration!). Even with `-canonical-prefixes`, `/proc/self/exe` failure falls
   back to `getprogpath` (in-tree, `Path.inc:148-186`), which is argv0/getcwd/PATH-based and
   composed of FULL calls. **So no `/proc` is required**; what is required is that the
   launcher passes a resolvable argv[0] (use `spawn_ex` with argv[0] = `/BIN/CLANG.BIN`-style
   absolute path, or set PATH). Ship ld.lld/llvm-ar/llvm-objcopy next to clang on PATH
   (`ToolChain::getProgramPaths` → `findProgramByName`, `Driver.cpp:6593-6613`).
4. **Resource dir is compile-time + argv0.** `GetResourcesPath` uses baked
   `CLANG_RESOURCE_DIR` when set (`Driver.cpp:184-210`). **Configure it to the on-disk
   absolute path** of the builtin headers (e.g. `/LIB/CLANG/21/res`) so resource headers
   (stddef/stdarg/float/stdint/…) resolve independently of argv0, and bake the default triple
   (`LLVM_DEFAULT_TARGET_TRIPLE`), so the build is fully deterministic in-OS. Cross-check with
   the current host build: `Makefile:155-156` uses `-print-resource-dir` + `-nostdinc`
   `-isystem` on x64, and `Makefile:189` uses the sysroot include path on ARM — the in-OS
   build will pass `-resource-dir` explicitly and skip `-nostdinc` games.
5. **Temp files & response files.** `Driver::GetTemporaryPath` (`Driver.cpp:6677`), the
   assembler/obj temp (`Driver.cpp:6188-6192`), and response files (`Driver.cpp:2198-2211`)
   all funnel through `createTemporaryFile` → `system_temp_directory` → **TMPDIR//tmp
   (W2-9)**. With `_SC_ARG_MAX=256` the 2 KB threshold makes response files common; they work
   because llvm::cl expands `@file` in cc1/lld, but they make W2-9 load-bearing.
6. **Env vars read**: PATH, TMPDIR/TMP/TEMP/TEMPDIR, TERM, COLUMNS, CL/_CL_, HOME
   (`user_cache_directory`), CLANG_TOOLCHAIN_PROGRAM_TIMEOUT (`ToolChain.cpp:118`),
   LLVM_* debug vars — all through `getenv` (FULL).
7. **Private stack**: cc1 wants 8 MiB (`cc1_main.cpp:84-107`, `DesiredStackSize`); the v2
   process image reserves exactly 8 MiB (`src/include/vm.h:58`) — **must run as v2**.

## §10 lld specifics

| Thing | Status | Anchor | Work item |
|---|---|---|---|
| `FileOutputBuffer` = stat → TempFile(o+x.tmp) → `resize_file`(ftruncate, `Path.inc:594-597`) → file mmap readwrite → commit=rename | PARTIAL | `FileOutputBuffer.cpp:127-155`; file mmap rejected (`process.c:2922`) → **InMemoryBuffer fallback** (`FileOutputBuffer.cpp:146-151`): `Memory::allocateMappedMemory` (anon ✓) + commit via `openFileForWrite`+stream (`FileOutputBuffer.cpp:91-107`). Correct, RAM-only output; keep kernel `-ENOTSUP` | verify (§1.3) |
| `F_mmap` flag | FULL | `ELF/Writer.cpp:2934` forces InMemoryBuffer (`FileOutputBuffer.cpp:189-192`) — same path | — |
| parallel tasks (default `--threads` = hardware_concurrency ≤16) | FULL | `ELF/Driver.cpp:1839-1843` → llvm::parallel → std::thread → pthreads (FULL) | — |
| unlink-as-rename trick | FULL | `lld/Common/Filesystem.cpp:44-109` (rename then threaded unlink; works) | — |
| `madvise(MADV_DONTNEED)` on output pages | PARTIAL | `ELF/Driver.cpp:2664` — advisory, works | — |
| fcntl file locks (archive writes) | FULL | `Path.inc:1227-1268` via SYS_FCNTL F_SETLK/F_SETLKW | — |
| exit codes / error handler | FULL | `_exit`/`exit` | — |

## §11 llvm-ar / llvm-objcopy

- **llvm-ar** (`llvm/tools/llvm-ar/llvm-ar.cpp`): `MemoryBuffer::getFile` (falls back to
  read loop, §1.3), `openFileForWrite` (597), `fs::status` + `getLastModificationTime`
  (890-893 → mtime via stat ✓), `getenv` (1346). No utimens, no locks beyond `fcntl`
  file-lock optional. **Status: FULL on current surface** (after M1 headers).
- **llvm-objcopy** (`llvm/tools/llvm-objcopy/*`): `MemoryBuffer::getFile` for input,
  `writeToOutput` → `llvm::writeToOutput` → `TempFile` **+ FileOutputBuffer semantics**;
  `--strip-all` etc all in-process on the buffer. Same InMemoryBuffer fallback as lld.
  **Status: FULL** (after M1 headers). `--preserve-dates` would use `setLastAccess...`
  (futimens no-op ✓).

---

## §12 Work items (W2-x)

Priority order within M1/M2. Sizes: S≤1 file/half-day, M=multi-file/1-2 days, L≥2 days.
"single-writer" = parent writes the shared header + both `trap.c` arms (per program rules).

### M1 — compile/link blockers for the LLVM build itself (do first)

- **W2-1 (S)** `float.h` sysroot header — `src/libc/include/float.h` (FLT_* / DBL_* limits).
  Every LLVM TU includes it; zero C lib has a real float.h today.
- **W2-2 (S-M)** `setjmp.h` + userland `setjmp`/`longjmp` — new `src/libc/include/setjmp.h`
  and impl (arm64 `stp/ldp` save or `__builtin_setjmp`; x64 classic). Needed by
  `CrashRecoveryContext.cpp:17,77,421`, which the **in-process cc1 path calls on every
  compile** (`Job.cpp:428-436`).
- **W2-3 (S)** `dlfcn.h` minimal — `RTLD_LAZY/RTLD_GLOBAL/RTLD_DEFAULT` + `dlopen/dlsym/
  dlclose/dladdr/dlerror` decls, impls return "not supported" (`DynamicLibrary.inc`'s
  `!HAVE_DLOPEN` branch, `DynamicLibrary.inc:51-60`). `Unix.h:37` needs the header regardless.
- **W2-4 (S-M)** `sys/resource.h` + stubs: `rlim_t`, `struct rlimit`, `RLIMIT_CORE/DATA/RSS/
  STACK`, `getrlimit` (report soft=hard=8 MiB stack, else 0/RLIM_INFINITY), `setrlimit`
  (return 0), `struct rusage` + `getrusage` (zeros). Consumed by `Process.inc:139-160`
  (runs at clang startup), `Program.inc:140-154` (only with MemoryLimit), and provides the
  `rusage` type for W2-5. Set `CLANG_HAVE_RLIMITS 0` to skip cc1's stack bump (v2 stack
  already 8 MiB).
- **W2-5 (S)** `wait4(pid,status,opts,rusage)` shim → `waitpid` — `src/libc/src/stat.c` or
  new `proc.c`; hot path for every subprocess (`Program.inc:341,418`).
- **W2-6 (S)** `sys/statvfs.h` + `statvfs/fstatvfs` stubs (f_type local, f_bsize 512,
  f_blocks/f_bfree 0) — `Path.inc:563-580` `is_local` (Linux `#define STATVFS statvfs`,
  `Path.inc:75`).
- **W2-7 (S)** Header micro-fixes, one commit:
  - `signal.h`: add `SA_NODEFER (0x40000000)`/`SA_ONSTACK (0x08000000)` (`Signals.inc:321`);
    declare `pthread_sigmask` (shim = `sigprocmask`, `Process.inc:262`).
  - `unistd.h`: `mkdir(const char*, mode_t)`, `link`, `symlink`, `alarm(unsigned)`,
    `fchown(int, uid_t, gid_t)`, `pipe(int[2])` (+ `pipe` impl in sysroot libc), `setsid`,
    `getrlimit/setrlimit` can live in resource.h instead.
  - `stdlib.h`: declare `_Exit` (impl exists at `src/libc/src/stdlib.c:767`).
  - stubs return `-1`/`errno` (`EPERM`/`ENOSYS`) as appropriate; `alarm` returns 0.

### M2 — pre-smoke runtime prerequisites

- **W2-9 (S, infra)** A writable temp dir: add `/tmp` (dir `TMP` at FAT root) to the disk
  image build **and/or** have the launcher `setenv("TMPDIR","/tmp",1)`. Blocks
  `Driver::GetTemporaryPath`, response files (§9.5), `TempFile` — first smoke failure if
  missed.
- **W2-10 (M, single-writer kernel)** Raise `HO_EXEC_ARGV_LEN` 256→4096 and
  `HO_EXEC_MAX_ARGS` 32→128 (`src/include/process.h:112-113`), update the spawn/exec
  marshalling and **both** `trap.c` copies if they hardcode sizes, and `_SC_ARG_MAX`
  (`src/libc/src/stat.c:195`) to match. Unblocks any out-of-process toolchain job; also
  stop `commandLineFitsWithinSystemLimits` (`Program.inc:549-588`) from forcing response
  files at 2 KB (harmless either way once response files work).
- **W2-11 (M, build+disk, wave-2)** Provision the toolchain bundle on disk: resource dir
  (`CLANG_RESOURCE_DIR` absolute, `Driver.cpp:184-210`), default triple baked
  (`LLVM_DEFAULT_TARGET_TRIPLE`/`LLVM_HOST_TRIPLE` — no runtime `uname`), pthreads on,
  threads default ≤16; ship `CLANG.BIN`, `LLD.BIN`, `LLVM-AR.BIN`, `LLVM-OBJCOPY.BIN` +
  runtime libs (libc.a, libc++.a, libc++abi.a, libunwind.a) on the image; PATH set to that
  dir at launch.
- **W2-12 (S, launcher)** Resolvable argv[0]/PATH for tool lookup (§9.3): launch clang via
  `spawn_ex` with argv[0] = absolute path, and export PATH; guarantees
  `findProgramByName("LD.LLD")`/`LD.lld` etc. resolve for the link step.

### M2 — when-hit (graceful already; verify or small)

- **W2-14 (S)** Implement `opendir/readdir/closedir` over `SYS_READ_DIR` (15) in
  `src/libc/src/dirent.c` (kernel already lists; remove the ENOSYS stubs). Needed only if a
  tool scans directories (`-print-search-dirs`, GCC-toolchain probing, `findLibrary`).
- **W2-13 (VERIFY)** After first in-OS compile+link: confirm lld output integrity via the
  InMemoryBuffer path (file-mmap is rejected by design, §1.3/§10). No code change expected;
  a regression in `sys_mmap` that starts mapping non-anon silently would silently corrupt
  outputs — keep `-ENOTSUP`.
- **W2-15 (S, optional)** `/dev/null`, `/dev/urandom` pseudo-devices (VFS `device_file`
  type or a tiny dev layer). Currently everything degrades gracefully; revisit only if a
  port complains.

### M5 — far future

- **W2-16** Real signal delivery to user handlers for fault-class signals + PrettyStackTrace
  crash dump (P5 extension; currently SIGCHLD/SIGPIPE/SIGTERM only, `signal.h:67-75`).
- **W2-17** `getrusage` with real u/s CPU times and `ru_maxrss` (needs kernel accounting) —
  only improves `-ftime-report` and `ProcessStatistics` (off by default).
- **W2-18** File-backed mmap for large inputs (MemoryBuffer/FileOutputBuffer) once the VM
  has file maps — removes the 32 MiB/anon-map constraints for big TUs.
- **W2-19** `ioctl(TIOCGWINSZ)` + tty plumbing if the in-OS shell ever allocates a real
  terminal; `_SC_OPEN_MAX`/fd table growth if parallel lld exhausts 64 fds.

## §12b No-action list (scary but unreachable in our build config)

| Item | Why it's fine |
|---|---|
| `uname` / `/proc/cpuinfo` / `getHostCPUName` | triple is compile-time; CPU name only affects `-mcpu=native` and version banner → "generic" |
| `-canonical-prefixes` / `/proc/self/exe` | not default; fallback is argv0/PATH (`getprogpath`, `Path.inc:148-186`) — all FULL calls |
| `posix_spawn` | configure `HAVE_POSIX_SPAWN 0` → fork+exec path, all FULL |
| `sigaltstack` | `HAVE_SIGALTSTACK 0` (guarded `Signals.inc:264`) |
| `pthread_getname_np`/`setname_np` | `HAVE_PTHREAD_*_NP 0` (guarded `Threading.inc:197-266`) |
| `getauxval`, `mallinfo*`, `getrusage` (#if paths) | config 0 (`Process.inc:51-122`); rusage struct still provided by W2-4 |
| `ioctl(TIOCGWINSZ)`/`tc*attr` | `HAVE_SYS_IOCTL_H 0`; no tty in the in-OS shell; columns via `COLUMNS` env |
| `getentropy`/`/dev/urandom` | falls back to time+pid hash for TempFile/ar names (`Process.inc:398-421`) |
| `alarm`/SIGALRM timeouts | driver passes `secondsToWait=0` (`Job.cpp:379-385`) → blocking wait; alarm stub fine |
| `__clear_cache`/JIT (MF_EXEC) | no JIT user; `Memory.inc:246` unreachable |
| `dladdr` crash backtraces | `HAVE_DLOPEN 0` + config `-g`-less; W2-3 stub returns failure |
| directory iteration (current build) | our driver/lld/link flows use explicit paths only (`ScanDirForExecutable` = `append+can_execute`, `Driver.cpp`); revisit with W2-14 if hit |
| `link(2)`/`--reproduce` archives | optional flag; stubbed (W2-7) |

## §13 wave-2 config.h / build directive snapshot (llvm/Config/config.h)

Set these to avoid pulling absent libc surface (each name → `llvm/include/llvm/Config/
config.h.cmake`; clang's at `clang/include/clang/Config/config.h.cmake`):

```
HAVE_POSIX_SPAWN 0    (no spawn.h — fork+exec path is FULL)
HAVE_SIGALTSTACK 0    HAVE_SYS_IOCTL_H 0    HAVE_GETAUXVAL 0
HAVE_MALLINFO 0       HAVE_MALLINFO2 0      HAVE_MALLCTL 0     HAVE_SBRK 0
HAVE_GETRUSAGE 0      (struct rusage still provided by W2-4)
HAVE_PTHREAD_GETNAME_NP 0  HAVE_PTHREAD_SETNAME_NP 0
HAVE_PTHREAD_GET_NAME_NP 0 HAVE_PTHREAD_SET_NAME_NP 0
HAVE_PREAD 0          (lseek+read fallback, Path.inc:1214-1221)
HAVE_FUTIMENS 1       (futimens no-op is fine)   HAVE_FUTIMES 0
HAVE_DLOPEN 0         (header W2-3 still required)
LLVM_ENABLE_THREADS ON  HAVE_PTHREAD 1  HAVE_LIBPTHREAD 1
CLANG_HAVE_RLIMITS 0   (v2 stack = DesiredStackSize already)
CLANG_RESOURCE_DIR "/LIB/CLANG/21/res"  (absolute, on-disk)
LLVM_HOST_TRIPLE / LLVM_DEFAULT_TARGET_TRIPLE = aarch64-none-elf (or the in-OS target)
```
The only `HAVE_*` that must be 1: `HAVE_ISATTY` (already hard-coded 1, config.h.cmake:87),
`HAVE_UNISTD_H`, `HAVE_SYSCONF`, `HAVE_GETPAGESIZE`, `HAVE_PTHREAD_H`,
`HAVE_PTHREAD_MUTEX_LOCK`, `HAVE_PTHREAD_RWLOCK_INIT`, `HAVE_SYS_MMAN_H`.

## §14 Verification checklist for the smoke (commands someone must run)

- `VERIFY` — compile llvm/Support against the sysroot after W2-1..W2-7: no missing-header
  errors (float.h, setjmp.h, dlfcn.h, sys/resource.h, sys/statvfs.h, SA_*).
- `VERIFY` — in-OS `CLANG.BIN -cc1 -triple aarch64-none-elf -I… -S src/user/hello.c` (pure
  in-process; needs resource dir + sysroot + /tmp only).
- `VERIFY` — in-OS `CLANG.BIN -c hello.c -o hello.o` then `LLD.BIN -o app.bin …` and check
  the object/link output bytes against the host-built equivalents (W2-13).
- `VERIFY` — `spawn_ex` a child with >256 B argv after W2-10 (regression test in
  `KERNEL_MODE_TEST` list).
- `VERIFY` — check `process.c:sys_mmap` still returns `-ENOTSUP` for fd≥0 (no silent anon).

---

*Audit source: LLVM/Clang/lld 21.1.8 (extracted from the selfhost vendor tarball), HobbyOS
tree at this repo's `src/`. Counts: 66 FULL / 20 PARTIAL / 16 MISSING / 12 NO-ACTION — see §1.*
