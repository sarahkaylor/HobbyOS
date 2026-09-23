# HobbyOS POSIX Compatibility Plan

**Goal:** Grow HobbyOS's libc and syscall surface to a well-defined POSIX.1-2008 subset so that open-source Linux programs can be compiled and run with minimal patching.

**Non-goals (explicitly out of scope unless stated):** TCP, threads/pthreads, full signal semantics, math.h with FPU, locale/wide-char, ELF dynamic linking. Each is called out below with the conditions under which it becomes feasible.

---

## 1. Current state (audited, Sep 2026)

### 1.1 Syscall ABI

- 28 syscalls (1–28), identical numbers on ARM64 (`svc #0`, number in `x8`, args `x0`–`x4`, return `x0`) and x86_64 (`syscall`, number in `rax`).
- Numbers are `#define`d **in three places** that must stay in sync: `src/user/libc.c`, `src/kernel/arch/arm/trap.c`, `src/kernel/arch/x64/trap.c`.
- Dispatch is a **flat if/else chain** per architecture in `trap.c` — every new syscall is currently duplicated across both arch files by hand.

| # | Syscall | Notes |
|---|---------|-------|
| 1 | WRITE_CONSOLE | serial/direct console |
| 2 | EXIT | |
| 3 | FORK | full copy (see 5.2 risk) |
| 4 | OPEN | **no flags/mode arg** — the single biggest ABI gap |
| 5 | CLOSE | |
| 6 | READ | blocking; `-2` = would-block (syscall restart via `elr -= 4`) |
| 7 | WRITE | same restart scheme |
| 8 | SPAWN | parent blocks in `PROC_STATE_WAIT_SPAWN`, reaps synchronously |
| 9 | MAP_FB | framebuffer mapping precedent for future mmap |
| 10 | FLUSH_FB | |
| 11 | GET_CPUID | |
| 12 | PIPE | |
| 13 | GET_EVENTS | raw VirtIO input (full-screen apps) |
| 14 | AVAILABLE | bytes readable now (poll substitute) |
| 15 | READ_DIR | index-based directory enumeration |
| 16 | KILL | `sig==0` = existence check; otherwise `process_kill` (terminate) |
| 17 | YIELD | |
| 18 | CONNECT | creates UDP socket over IPv4 and connects it |
| 19 | SLEEP | ms |
| 20 | GET_ARGS | flat arg string, `char*` copy |
| 21 | SYSINFO | cmd 1 uptime, 2 mem, 3 procs, 4 net, 5 cpu, 6 RTC epoch, 7 fs stats, 8 mount table |
| 22 | UNLINK | |
| 23 | RENAME | |
| 24 | MKDIR | |
| 25 | GETCWD | |
| 26 | CHDIR | |
| 27 | MOUNT | NFSv3 |
| 28 | UMOUNT | |

### 1.2 Process / memory model

- Each process: fixed 32 MB user region at `USER_VIRT_BASE` (0x44000000), one L2 page table, physical pages from a global pool. No demand paging.
- Program image: **flat raw binary** (`llvm-objcopy -O binary`), linked at 0x44000000, copied directly to `USER_VIRT_BASE` by the loader. Not ELF.
- Loader cap `MAX_PROGRAM_SIZE` = `USER_INITIAL_CLEAR_SIZE` = **256 KB** — a hard constraint for real ports (see 5.1).
- Stack: fixed 256 KB at top of region. Heap: `malloc()` bumps from `_end` with a free list; **no syscall, no `brk`, no `realloc`**.
- Per-process: `MAX_OPEN_FDS` 32, fd table → global file table (128 entries). File types: FAT16, PIPE, SOCKET (UDP), NFS (read-only). Cursor exists per file but there is **no seek syscall**.
- No exit-status capture: `process_exit` does not record `status` anywhere reaped later; `spawn` is the only parent-child wait.

### 1.3 libc

- `src/user_include/libc.h` + `src/user/libc.c` (~300 lines) + `src/user/malloc.c`.
- I/O syscalls with no errno (just −1), no `errno.h`, printf family, string.h, ctype.h, stdlib functions, time.h, dirent API — **none**.
- Each user program gets duplicate hand-rolled `my_strlen`/`gui_strlen`/`fd_strcpy`/`my_strstr` etc. (confirmed: dialog.c, editor.c, filedialog.c, find.c, grep.c, gui.c each define their own).
- Entry point is `.text._start`, not `main(argc, argv)`.
- Host test layer: `src/host/compat.c` provides `ho_*` stubs; a syscall-level `ho_*` alias block in `libc.h` under `HOST_TEST`.

### 1.4 Non-negotiable constraints (from GEMINI.md / kernel skill)

- ARM64 primary; x86_64 port must stay in sync (**every** new syscall is implemented in both `arm/trap.c` and `x64/trap.c`).
- SMP ≥ 4 cores; IPC and memory protection/user mode always preserved.
- Kernelside: `-mgeneral-regs-only` (no SIMD in EL1), no unaligned access, byte-wise binary parsing, Normal-NC DMA memory.
- All user code currently also builds with `-mgeneral-regs-only` → **no FPU in userland today** (see 5.4).

---

## 2. Architecture decisions

### 2.1 One shared syscall number header + table dispatch (prerequisite, Phase 0)

Before adding syscalls, remove the triple-#define drift and the per-arch if/else chains:

- Create `src/include/syscall.h` with `#define SYS_*` numbers (single source of truth). Include it from `libc.c`, `arm/trap.c`, `x64/trap.c`.
- Replace the if/else chains in both arch `trap.c` with a table: `static int (*const syscall_table[])(struct trap_frame *tf)` (max syscall tracked). Each arch still has its own table only because the handler bodies differ by target; the **numbers** come from the shared header.
- Add a `syscall_table_size` guard so an out-of-range number returns `-ENOSYS` instead of falling through.
- Gate: `make host_tests`, `make test`, and `./run_unit_tests_intel.sh` all pass with zero behavior change. This is a pure refactor; commit it first.

### 2.2 Error returns and errno

Today every failure is −1. POSIX needs real error codes. Rule:

- Kernel returns **negative errno** (`-ENOENT`, `-EINVAL`, `-ENOSYS`, …) where it currently returns −1. Unknown syscalls → `-ENOSYS`.
- libc wrapper: `long r = syscall(...); if (r < 0) { errno = -r; return -1; } return r;`
- `errno.h` defines the standard ~30 constants; store `errno` as a single global per process (no threads yet — document that a future thread port must move it into per-thread storage; the kernel-side `-errno` convention already keeps the ABI thread-ready).
- This touches every existing wrapper and every `tf->regs[0] = -1` site in both trap files. Do it in Phase 0/1 with a mechanical sweep + host tests asserting specific errno values (compat.c must map its real syscalls to the same negative-errno convention).

### 2.3 libc organization (sysroot)

Restructure user headers into a real sysroot so ported code `#include <string.h>` and links against one archive:

- New tree: `src/libc/include/<header>.h` (string.h, stdio.h, stdlib.h, ctype.h, errno.h, time.h, unistd.h, fcntl.h, dirent.h, signal.h, limits.h, stdarg.h, stdbool.h, inttypes.h, …) and `src/libc/src/*.c`.
- Build `src/libc/libc.a`. Existing flat-binary apps (desktop, editor, games) keep compiling against the same sources — the archive is just a packaging change; `libc.h` stays as a compatibility umbrella that includes the new headers for the legacy apps until they migrate.
- Keep the `HOST_TEST` `ho_*` aliasing working: pure functions (string, ctype, stdlib, math-free) compile unchanged under host tests and run **natively on Linux** — this is the cheap, high-coverage test path.
- Add `src/libc/crt0.c` (see 2.4) and a `main()` convention.

### 2.4 Entry point: crt0 + `main(argc, argv)`

Every portable program expects `main(int argc, char **argv)`:

- `crt0.c` (linked at `.text._start`): call `SYS_GET_ARGS` once, tokenize into `argv[0..]` (reuse `parse_args`), call `main(argc, argv)`, then `exit(ret)`.
- Decree: **all new ports use `main()`**; legacy apps keep `_start` until touched.
- `argv[0]` should be the program name (PCB already carries `name`; export via `SYS_GET_ARGS` or a new `SYS_GETARGV` — decode: pass the name in front of args at spawn time, or add `SYS_GETPROGNAME`; simplest is prepending `name` to the args string at spawn).
- Environment: start with an in-memory table (`environ`) + `getenv/setenv/putenv/unsetenv` purely in libc. Optional kernel `SYS_GETENV`/`SYS_SETENV` later if env must survive across spawns.

### 2.5 The two-libc endgame (documented, not now)

Two viable end states:

1. **Expanded in-tree libc** — this plan, Phase-by-Phase. Fast, testable, zero loader changes. Suitable for ports that tolerate a subset (most classic small tools: cat, sort, grep, wc, ed, compress, tiny shells).
2. **Static musl** — gives ~100% of POSIX C + real printf/strtod/getopt, but requires: an **ELF loader** in the kernel (replaces objcopy-flat loading), `brk`/anon-`mmap` (Phase 4 provides), TLS setup (TPIDR_EL0 on context switch), and musl's syscall ABI expectations. This is the *stretch milestone* for Phase 9, only after the syscall ABI is stable. It is a self-contained project of its own; do not start it before Phases 1–5.

---

## 3. Phased plan

Each phase lists: new syscalls (with kernel-side work), new libc surface, and the acceptance gate (`make host_tests` + `make test` are mandatory after every phase).

### Phase 0 — Foundations (refactor only)

- `src/include/syscall.h` + table dispatch (2.1); negative-errno convention (2.2) across existing 28 syscalls.
- `src/libc/` sysroot skeleton, `libc.a` build, `errno.h`.
- Raise `MAX_PROGRAM_SIZE` from 256 KB to ≥ 1 MB (loader + `USER_INITIAL_CLEAR_SIZE`; the 32 MB region is not the constraint, the cap is arbitrary). **Required before any real port fits.**
- `crt0.c` + `main()` convention (2.4).
- **Gate:** all three test tiers pass; a demo `HELLO.C` built with `main()` runs on desktop and in `KERNEL_MODE_TEST`.

### Phase 1 — Pure-userland libc core (no new syscalls)

Highest leverage for porting, all host-testable natively:

| Header | Functions |
|---|---|
| `string.h` | `strlen, strnlen, strcmp, strncmp, strcpy, strncpy, strcat, strncat, strchr, strrchr, strstr, strpbrk, strspn, strcspn, strtok, strtok_r, strdup, strerror, memcmp, memmove, memchr, strcasecmp, strncasecmp` |
| `ctype.h` | full `is*`/`to*` set via char-class table |
| `stdlib.h` | `atoi, atol, atoll, strtol, strtoll, strtoul, strtoull, abs, labs, llabs, qsort, bsearch, rand, srand, getenv, setenv, putenv, unsetenv, **realloc**` (missing today — add to malloc.c), `abort` |
| `stdarg/limits/stdbool/inttypes` | header-only |

Rules: no stdio dependency; byte-exact behavior vs glibc for the covered cases; host tests must compare against glibc on randomized inputs (property tests) in addition to literal cases. **printf is not here — it is the single highest-value function and gets its own phase.**

### Phase 2 — File I/O: open(flags), lseek, stat, truncate, dup, access + stdio

*Rationale:* no port survives without positional I/O, metadata, and fd duplication; the known "stale tail" bug (no truncate) already bites user apps.

**Syscalls (29–36 + OPEN ABI change):**

| # | Syscall | Signature (user) | Kernel work |
|---|---|---|---|
| 29 | `SYS_LSEEK` | `(fd, off, whence)` → new off | cursor already in `struct file`; add `SEEK_SET/CUR/END`; enforce `off >= 0` |
| 30 | `SYS_STAT` | `(path, struct stat*)` | implement on FAT16 dir entries + NFS `GETATTR`; returns `st_size, st_mode (S_IFREG/S_IFDIR), st_nlink, st_mtime` (FAT has no real times → 0 or BPB date) |
| 31 | `SYS_FSTAT` | `(fd, struct stat*)` | same data from open file |
| 32 | `SYS_TRUNCATE` | `(path, size)` | FAT16: shrink `file_size` + free tail clusters (fixes the stale-tail bug properly) |
| 33 | `SYS_FTRUNCATE` | `(fd, size)` | same on open file |
| 34 | `SYS_DUP` | `(fd)` → newfd | `fs_duplicate_fd` helper already exists; wire it + `ref_count` |
| 35 | `SYS_DUP2` | `(oldfd, newfd)` → newfd | close target if open; needed by shell redirection and `freopen` |
| 36 | `SYS_ACCESS` | `(path, mode)` | existence (R/W/X all return 0 — no permissions yet), `F_OK` |
| — | **`SYS_OPEN` extended** | `open(path, flags, mode)` | add args 1–2: `flags` (`O_RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND`); `flags==0` keeps today's behavior for legacy callers; wire `O_TRUNC`→`ftruncate(0)`, `O_APPEND`→always-write-at-end. This is an ABI extension of syscall 4, not a new number |

Directory API stays on `SYS_READ_DIR` (index-based); `dirent.h` wraps it (Phase 2 libc). `open()` on a directory path returns `-EISDIR`.

**Libc:**

- `stdio.h` — the centerpiece. Implement in this order:
  1. `vsnprintf`/`snprintf` (and `sprintf`) with full spec: `%d %i %u %x %X %o %c %s %p %%`, length modifiers `l/ll/z`, `-`, `0`, width, `.*` precision. **One shared implementation in libc** — every app currently hand-rolls this.
  2. `FILE` layer over fds: `fopen, fclose, fread, fwrite, fgetc, fputc, fputs, fgets, puts, putchar, getchar, printf, fprintf, vfprintf, fflush, feof, ferror, fileno, fdopen`. Single small buffer per `FILE`; start unbuffered, add line-buffering on tty later. `fgets` correctness is what most ported tools actually depend on.
  3. `getline`/`getdelim` — small, disproportionate porting value (every text tool).
  4. `fseek, ftell, rewind` — once `lseek` lands.
  5. `sscanf`/`vsscanf` — optional/deferred; many tools can use `strtol` instead.
- `unistd.h`: `lseek, dup, dup2, access, isatty` (windowed stdin/stdout → true; file fds → false), `usleep`, `getpid, getppid, getuid, getgid` (stubs, see Phase 3).
- `fcntl.h`: `O_*` constants + `open` prototype (needs Phase-2 kernel ABI).
- `dirent.h`: `opendir/readdir/closedir` over `SYS_READ_DIR`, `struct dirent {d_name[]}`, plus `scandir`/`alphasort` (easy once qsort exists).
- `sys/stat.h`: `struct stat` mirroring the syscall struct, `stat/fstat`, `S_IF*`/`S_IS*` macros.

**Gate:** `grep`, `sort`, `wc`, `head`, `tail`, `uniq`, `less`, `cat` (all already present in `src/user/`) recompiled to use the shared libc instead of their hand-rolled helpers; host tests for every stdio/string function; in-OS `POSIXTEST.BIN` exercises fopen/fseek/truncate/dup2 (fixes TODO.TXT stale-tail resurrection as a regression test).

### Phase 3 — Processes: pid, exec, waitpid, exit status

*Rationale:* ported code uses `fork()+exec()` and `waitpid()`, not HobbyOS `spawn` blocking. Their `fork` already exists and is tested.

**Syscalls (37–40):**

| # | Syscall | Notes |
|---|---|---|
| 37 | `SYS_GETPID` | trivially `current_process()->pid` |
| 38 | `SYS_GETPPID` | `parent_pid` already in PCB |
| 39 | `SYS_WAITPID` | `(pid, int *status, int options)` — kernel records `exit_status` in the PCB on `process_exit` (missing today), keeps the PCB in EXITED until reaped, returns child pid + `WEXITSTATUS`-compatible status. Non-blocking only for `WNOHANG` first pass |
| 40 | `SYS_EXEC` | `(path, args)` — load-and-run **into the current process**: reuse `load_and_run_program_in_scheduler`-style loading, reset region, entry, args; fails `-ENOENT` if binary missing. Makes `fork()+exec()` programs work |

- `unistd.h`: `getpid, getppid, execv/execve` (user-facing: `SYS_EXEC` + args; env inherited or empty), `sleep(sec)` (wraps SYS_SLEEP×1000).
- `sys/wait.h`: `wait, waitpid, WEXITSTATUS, WIFEXITED, WNOHANG`.
- `stdlib.h`: `system()` (optional, spawns via shell — defer).
- **Gate:** a `fork_test`-style in-OS test does `fork(); child execs CAT.BIN; parent waitpid`s and checks `WEXITSTATUS`. Shell (`sh.c`) migrated from blocking spawn to `fork+exec+waitpid` semantics.

### Phase 4 — Memory: brk and anonymous mmap

*Rationale:* real libc malloc (and musl later) needs `brk`; ported programs use `malloc` extensively and `mmap(MAP_ANONYMOUS)` for big buffers.

**Syscalls (41–43):**

| # | Syscall | Kernel work |
|---|---|---|
| 41 | `SYS_BRK` | per-process heap break in PCB; validate within 32 MB region before user stack reserve; `brk(0)` returns current. Migration path: keep the `_end` bump allocator for legacy apps; new libc `malloc` uses `brk` |
| 42 | `SYS_MMAP` | `MAP_ANONYMOUS` only (prot R/W/X, addr hint honored-or-ignored). Implementation: carve from the region's unmapped pages — the L2 table already supports 4 KB pages, so this is region carving, **not** full VM. File-backed mmap **deferred** (needs FAT16 page-in; revisit with musl). Enforce `MAP_FAILED` = `(void*)-1` |
| 43 | `SYS_MUNMAP` | un-carve; must not split kernel-managed stacks |

- `sys/mman.h`: `mmap, munmap, PROT_*, MAP_ANONYMOUS|MAP_PRIVATE` (MAP_SHARED deferred).
- `stdlib.h`: `realloc` now real (Phase 1) on brk heap.
- **Gate:** `heap_test` extended: brk growth, mmap 1 MB buffer, munmap, then realloc stress. A ported tool that `mmap`s a file-sized anonymous buffer works.

### Phase 5 — Time

**Syscall (44):**

| # | Syscall | Notes |
|---|---|---|
| 44 | `SYS_CLOCK_GETTIME` | `(clk_id, struct timespec*)`; `CLOCK_MONOTONIC` → `timer_get_ms` (sysinfo 1), `CLOCK_REALTIME` → RTC epoch (sysinfo 6) converted to `timespec`; no RTC → return `-ENOSYS` and libc `time()` falls back to 0/uptime |

- `time.h`: `time, clock, clock_gettime, gettimeofday` (compose from REALTIME), `gmtime/localtime` (UTC only; note TZ unsupported), `mktime, difftime, strftime` (subset: `%Y %m %d %H %M %S %%`).
- Port `date`, `touch -t` timestamps, sysmon uptime display onto this.
- **Gate:** in-OS test prints epoch via `time()` and asserts it matches `sysinfo(6)`; a `strftime` formatting test.

### Phase 6 — Minimal signals

*Rationale:* most ports only need: `kill(0)`-style checks, `SIG_IGN`, default-death on `SIGTERM`/`SIGKILL`, and `SIGCHLD` for `waitpid` bookkeeping. **Async handler delivery is deferred** (needs a signal frame, IRQ→user trampoline, `sigreturn` — its own project; do not attempt before Phase 7 of this list).

**Syscalls (45+ reserve):**

| # | Syscall | Notes |
|---|---|---|
| 45 | `SYS_SIGNAL` | `(sig, handler, old)` — store per-process disposition; only `SIG_DFL` (terminate), `SIG_IGN`, `SIG_ERR` semantics now; `SIGKILL/SIGTERM` default; `SIGCHLD` default-ignore for waitpid. `SIG_DFL` on a pending-death signal terminates. `sig==0` check stays in `SYS_KILL` |
| reserved | `SYS_SIGRETURN` etc. | needed only when real delivery is implemented (documented, not scheduled) |

- `signal.h`: `signal, sigaction` (minimal `struct sigaction`, `SA_*` accepted-but-ignored), `raise` (→ kill(self)), `abort` (→ `SIGABRT` default → death with distinguishable status).
- **Gate:** `kill_test`: raise SIGTERM on self dies with status; SIG_IGN lives; `kill(pid,0)` semantics unchanged.

### Phase 7 — Sockets: sendto/recvfrom/select (UDP)

*Rationale:* HobbyOS is UDP-only by design (GEMINI.md). The POSIX socket API surface that helps UDP tools: datagram send/recv with addresses, and multiplexing.

**Syscalls (46–49):**

| # | Syscall | Notes |
|---|---|---|
| 46 | `SYS_SOCKET` | `(domain, type, protocol)` → fd; today's `SYS_CONNECT` folds create+connect into one — add a real socket() and make `connect()` sit on top; legacy `SYS_CONNECT` keeps working |
| 47 | `SYS_SENDTO` | `(fd, buf, len, flags, sockaddr*, addrlen)` — UDP send with destination (needed by real dns/tftp/ntp-style clients) |
| 48 | `SYS_RECVFROM` | same shape, fills src addr |
| 49 | `SYS_SELECT` | `(nfds, fd_set*, …timeout)` — multiplex over file/pipe/socket fds; trivial first version = loop of `file_available` + `sleep`, still correct |

- Reserve 50–63 for `bind/listen/accept/getsockopt/setsockopt` — **only when the kernel gains TCP** (a networking epic, not a POSIX-surface task; see 5.3).
- `sys/socket.h`, `netinet/in.h`: `socket, connect, sendto, recvfrom, select`, `struct sockaddr_in`, `AF_INET`, `SOCK_DGRAM`, `htons/htonl/ntohs/ntohl` (byte-order helpers almost free).
- **Gate:** port a UDP client (e.g. a minimal `nc -u` or a DNS lookup tool) on top; both arches pass `net_test`.

### Phase 8 — Toolchain for ports (the "porting treadmill")

Make bringing in an external program a repeatable step, not a hand-rolled Makefile:

1. `src/libc/Makefile` builds `libc.a` (archives all `src/libc/src/*.o`, both arches).
2. A single wrapper script `tools/build-app.sh <src.c> <BINNAME>`: compiles with
   `clang --target=aarch64-none-elf -O2 -ffreestanding -nostdlib -Isrc/libc/include -Isrc/user_include -mcpu=cortex-a53 -mgeneral-regs-only src.c src/libc/crt0.o src/libc/libc.a -T src/user/linker.ld` → `objcopy -O binary` → `mcopy` onto disk.img. Document the same for x86_64.
3. Porting checklist (append to README): include only supported headers; no floats unless soft-float enabled; no `#ifdef __linux__` reliance; replace `open` flags beyond the supported set; assume 8.3-ish FAT names (or keep uppercase), no symlinks, no chmod, no mmap-shared; interactive apps must handle the desktop's ESC-protocol stdin (no Ctrl, no raw termios) — windowed ports write to the desktop pipe.
4. First real ports (small, high-value, reveal gaps fast):
   - **Phase 2:** `ed` (byte-level, stdio-only) or a BusyBox `cut/tr/seq`.
   - **Phase 4–5:** `cksum`/`md5sum`-style (needs lseek/read), a `sleep`/`timeout`-style tool (needs time + wait).
   - **Phase 7:** `nc -u` (UDP).
   - Each port must land **with** its in-OS test (pattern: `<name>_test.c` + `mcopy` + `load_and_run_program_in_scheduler` + auto-exit).

### Phase 9 — Stretch: static musl (evaluated, not promised)

Eligibility before starting:

- [ ] Syscall ABI stable with negative-errno (Phase 0–7 done)
- [ ] `brk` + anonymous `mmap` working (Phase 4)
- [ ] **ELF loader in the kernel** (replaces flat-binary `objcopy` model; loader parses ET_EXEC/ET_DYN static images, sets up sections, bss, and entry)
- [ ] TLS: `TPIDR_EL0`/(x86 `fs`) switched on context switch; musl needs it even single-threaded
- [ ] `waitpid`/exit-status and `SYS_EXEC` correct (Phase 3)

With those, a static-linked musl binary (no pthread) becomes the fastest path to "port anything." Until the ELF loader exists, **do not** attempt musl.

---

## 4. Master syscall table (current + planned)

| # | Syscall | Phase | # | Syscall | Phase |
|---|---|---|---|---|---|
| 1–28 | current (see §1.1) | — | 37 | GETPID | 3 |
| 4 | OPEN (ext. flags/mode) | 2 | 38 | GETPPID | 3 |
| 29 | LSEEK | 2 | 39 | WAITPID | 3 |
| 30 | STAT | 2 | 40 | EXEC | 3 |
| 31 | FSTAT | 2 | 41 | BRK | 4 |
| 32 | TRUNCATE | 2 | 42 | MMAP (anon) | 4 |
| 33 | FTRUNCATE | 2 | 43 | MUNMAP | 4 |
| 34 | DUP | 2 | 44 | CLOCK_GETTIME | 5 |
| 35 | DUP2 | 2 | 45 | SIGNAL | 6 |
| 36 | ACCESS | 2 | 46–49 | SOCKET/SENDTO/RECVFROM/SELECT | 7 |
| — | (50–63 reserved for TCP-era socket calls) | 7 | | | |

Total: 49 syscalls (28 existing, 21 new + 1 ABI extension). This is a natural size for a flat dispatch table; revisit the table design only if it grows past ~64.

---

## 5. Risks, tradeoffs, open questions

1. **Loader cap vs. library size (blocking).** A static libc + ported tool will exceed 256 KB almost immediately. Phase 0 must raise `MAX_PROGRAM_SIZE` (+ `USER_INITIAL_CLEAR_SIZE`) to ≥ 1 MB and re-verify the silent-truncation guard still trips loudly. Watch `llvm-size` on every port.
2. **`fork()` cost / `exec` reset.** `process_fork` copies the whole region today (fine for now); `exec` must rebuild the region and the page table cleanly. No COW — acceptable at this scale; revisit only if fork-heavy tools (shells) thrash.
3. **TCP (explicitly out of scope).** The POSIX socket API for TCP (`bind/listen/accept/connect` to a listening port) requires a TCP stack in the kernel — a networking epic independent of the POSIX surface. UDP tools port fine; curl/http clients do not.
4. **No FPU in userland.** User code compiles with `-mgeneral-regs-only`, so `double` math (strtod, math.h) is unavailable unless: (a) EL0 FP is enabled (kernel `CPACR_EL1` trap config + x86 SSE intercept — touches the security model), or (b) soft-float (`-msoft-float`, slow but safe) for specific ports. Decide per-port; default is *no float*, document for porters. `strtod` is deferred because of this.
5. **Error-code sweep regression risk.** Switching from −1 to −errno can silently change behavior of existing callers that compare `== -1` (fine) vs `== 0` (fine) — the risk is code treating any negative as "errno number." Host tests asserting exact errno values per syscall close this.
6. **stdio + the windowed input model.** Ported interactive programs will read the desktop's stdin pipe, which speaks ESC-protocol (arrows, menus) and has **no Ctrl**. POSIX conformance won't fix UX; interactive ports either adapt (editor already does) or target the serial console. Note, don't fight.
7. **Both-arch discipline.** Every new syscall is two implementations. The Phase-0 table refactor fixes number drift but not body drift — keep the ARM/x64 parity rule in review and in `run_unit_tests_intel.sh` CI.
8. **Open question: `time_t`/`size_t` widths.** Recommend 64-bit `time_t`; FAT16 stores 32-bit sizes but process-local sizes are fine as `size_t` 64-bit. Confirm when `struct stat` is specified (Phase 2) so the ABI doesn't churn.
9. **Open question: environment across `exec`.** In-memory `environ` is enough for single-process tools; decide in Phase 8 whether kernel-side env (per-PCB strings) is worth it for `exec` chains.

---

## 6. Suggested execution order (if this plan is approved)

Phase 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8, with **one commit per sub-task**, host tests per function, in-OS integration tests per phase, and a `make test` + `./run_unit_tests_intel.sh` green check at every phase boundary. Phase 9 is evaluated against its eligibility list, never started blind.
