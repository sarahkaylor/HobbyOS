# cp/sys — the kernel prerequisites for Wine (lane C2-sys, P2 minus address space)

Plan: `docs/cyberpunk.md` §6 P2, §7.2b (C2-sys row).  Evidence this lane acts
on: `docs/cyberpunk-evidence.md` §3 (the measured Wine syscall set) and
`docs/cp-p0-host.md` §4 (what Proton adds).  Wine source citations are
`file:line` in the pinned tree (`linuxci:~/cp/wine-build/src/wine-11.0`).

This lane makes Wine's **non-address-space** prerequisites exist: a writable,
symlink-and-socket-capable, coherent-shared-page `/tmp`; the syscalls the
console Wine and Proton's Wine hit; and the fork and fd-passing shapes the
wineserver needs.  It does **not** do the low-VA / `MAP_FIXED` /
`ARCH_SET_GS` work (C1-mmu) — so `wine cmd /c echo` still cannot run; this doc
records exactly where it stops.

## 1. What this lane added

| Piece | File | Role |
|---|---|---|
| tmpfs core | new `src/kernel/tmpfs.c`, `src/include/tmpfs.h` | in-memory `/tmp` for compat processes: files/dirs/symlinks/socket inodes, perms+owner, 64-bit inodes, unlink-while-open, rename, O_TMPFILE, getdents64, statfs; file data is a `vm_object` so `read`/`write` and `MAP_SHARED` share frames |
| tmpfs Linux-ABI rows | new `src/kernel/tmpfs_compat.c` (`l_tab_tmpfs`), 13 lines in `linux_compat.c` | consulted **before** `l_tab_ext2`; **declines** (returns 0) anything not under `/tmp` |
| VFS glue | `src/kernel/fs.c`, `src/include/fs.h` | `FILE_TYPE_TMPFS`/`TMPFSDIR`/`EVENTFD`; open/read/write/seek/fstat/getdents/close/ftruncate/poll, record locks, `file_memfd_obj` for tmpfs files, `file_eventfd_create`, `file_fd_is_dir` |
| memfd file I/O | `fs.c`, `tmpfs.c` | memfds get `read`/`write`/`lseek`/`fstat` through the same object helpers tmpfs uses |
| SCM_RIGHTS on streams | `src/kernel/unix.c`, `src/include/unix.h`, `fs.c` | fds travel over `SOCK_STREAM`, anchored at a byte offset; `K_UNIX_MSGS` 8→32 |
| the syscalls | `linux_compat_fd.c`, `linux_compat_misc.c`, `linux_compat_proc.c` | fchdir, sendmmsg, the xattr family, eventfd/2, inotify, prctl(SUD), process_vm_readv/writev |
| fork-shaped clone | `linux_compat_thr.c`, `process.c` | glibc `fork()`'s clone shape routed to native `process_fork()`; fork keeps the Linux-ABI class and inherits dispositions/mask/altstack |
| object sizing | `src/include/vm.h`, `src/kernel/vm.c` (C1-mmu's files; minimal diff) | `VM_OBJ_MAX_BYTES` 64 MiB→1 GiB; geometric frame-array growth |
| probes | `src/user/linux_probe_tmpfs.c`, `linux_probe_wsys.c` | `bash tools/gate/t3_linux_probe.sh tmpfs` / `wsys` |

## 2. tmpfs model (`tmpfs.c`)

- **Who sees it.** `tmpfs_owns()` is true only when the path is `/tmp` or
  below **and** the calling group is `PROC_FLAG_LINUX_ABI`.  A native process
  keeps the FAT `/tmp`; the X server's `/tmp/.X11-unix/X0` bind stays in
  unix.c's namespace for everyone.  The node table is allocated from the frame
  pool on the first owned call, so a native-only boot and every ARM boot pay
  nothing.
- **Nodes.** `TMPFS_T_REG`/`DIR`/`LNK`/`SOCK`, each with `mode`/`uid`/`gid`/
  times.  A node is its own directory entry (parent index + name on the node,
  (parent,name) hashed); **hard links are not modelled** (`link(2)` → `EPERM`,
  Linux's answer on a filesystem without them).
- **Lifetime.** `nlink` (namespace) and `refs` (open descriptions) are
  separate; a node is freed only when both reach 0 — the unlinked `tmpmap-*`
  section files the wineserver keeps open survive to their last close.  Inode
  numbers are a 64-bit counter, **never reused within a boot**, so a lock
  keyed on `(K_DEV_TMPFS, ino)` cannot alias a dead file.
- **Data = `vm_object`.** A regular file's bytes live in a `VM_OBJ_MEMFD`
  object, created on first growth (an empty file costs no object slot).
  `file_memfd_obj()` returns that same object to `mmap`, so a page written by
  one process through `write()` or a `MAP_SHARED` store is the page another
  process reads — **coherence by construction**, which is what
  `KUSER_SHARED_DATA` requires (Wine `server/mapping.c:1495-1505`, `:362-392`;
  the server writes it, clients read it, `server/fd.c:356,380-425`).
- **memfd parity.** memfds use the same `tmpfs_obj_*` helpers, so a memfd and a
  tmpfs file have identical page semantics and a single object can exceed
  64 MiB (§6).
- **Symlink off `/tmp`.** A link that resolves outside `/tmp` (Wine's
  `dosdevices/z: -> /`) returns `TMPFS_EXTERNAL` with the redirected path, the
  same contract as `EXT2_EXTERNAL`; the caller finishes on FAT16/NFS/ext2.
- **Lock.** One ylock for the whole filesystem (metadata and data), held
  across user copies (they may fault; the fault path never takes it).
- **For C1-mmu.** Every physical-frame→pointer cast is the one macro `T_KVA`
  (today the identity); the direct-map conversion is a one-line change.
- **For C3-ext2b.** `tmpfs_setattr`/`tmpfs_fsetattr` are exported for the VFS
  setattr op `chmod`/`fchmod`/`chown`/`utimensat` will route to; this lane
  does not implement those syscalls.

## 3. SCM_RIGHTS over `SOCK_STREAM` (`unix.c`)

Not in the original brief, but mandatory: Wine passes **every** handle that
maps to a Unix fd over its `SOCK_STREAM` wineserver connection
(`server/request.c:377-505`, `dlls/ntdll/unix/server.c:950-980`), and
`file_sendmsg` refused ancillary data on a stream.  A stream now carries fds
as **records anchored at the stream offset of the first byte** sent with them
(reusing the SEQPACKET/DGRAM message slots, unused by STREAM otherwise):

- a read never crosses the next record's first byte;
- the read that starts **at** a record takes its fds;
- a plain `read()`/`recv()` (no control buffer) closes fds it consumes, as
  Linux discards them.

`K_UNIX_MSGS` is 8→**32** (≈940 B .bss per direction × 256 directions): the
slots double as stream records and the wineserver **kills a client whose fd
`sendmsg` answers `EAGAIN`** (`request.c:499-503`), so a shallow queue is a
correctness risk, not just a perf one.

**bind/connect** (`linux_compat_fd.c`): a relative AF_UNIX pathname is
resolved against the caller's cwd before it becomes a namespace key
(wineserver binds `"socket"` in its server dir, `request.c:814-818`).  A bind
inside `/tmp` creates the `S_IFSOCK` inode (`0777 & ~umask`), `EADDRINUSE`
when the name exists, removed again if the bind fails — the client `lstat()`s
it and refuses a non-socket (`dlls/ntdll/unix/server.c:1432-1443`).  connect
does not require the inode, so compat X clients still reach the native X
server's socket.

## 4. Syscalls, each with its Wine citation

| nr | call | decision | Wine 11.0 citation |
|---|---|---|---|
| 81 | `fchdir` | **implemented**: cwd := the path the dir fd was opened under; `EBADF`/`ENOTDIR`/`ENAMETOOLONG` | every `NtQueryDirectoryFile` does `fchdir(dir)…fchdir(cwd)` and fails on the first (`dlls/ntdll/unix/file.c:2914-2934`); server temp-file placement (`server/mapping.c:374-378`) |
| 88/266 | `symlink`/`symlinkat` | **implemented** for `/tmp`; `/EXT` stays ext2's; FAT/NFS → `EPERM` | prefix `dosdevices`, `com*` links |
| 89/267 | `readlink`/`readlinkat` | **implemented** for `/tmp` (ext2 keeps `/EXT`) | prefix link reads |
| 307 | `sendmmsg` | **implemented**: per-entry `sendmsg` (AF_UNIX) or connected-datagram write (INET, one iovec) | glibc 2.31 resolver's A+AAAA batch, from wineboot.exe's host lookup |
| 188-199 | xattr family | get/set/remove → `EOPNOTSUPP`, list → empty, after Linux's `ENOENT`/`EBADF`/`EFAULT` | Wine tolerates `ENOTSUP` on the get (`file.c:1704-1712`, `:1820-1830`) and set/remove (`file.c:1764`) paths |
| 284/290 | `eventfd`/`eventfd2` | **implemented properly**: counter, `EFD_SEMAPHORE`/`NONBLOCK`/`CLOEXEC`, poll, scheduler-slice blocking | not upstream Wine; Proton used it 4× (`docs/cp-p0-host.md §4`) |
| 253/254/255/294 | inotify | init/init1 → `ENOSYS`; add/rm → `EBADF`/`EINVAL` | `server/change.c:925-927` (compiled out: no `HAVE_SYS_INOTIFY_H`) and `winebus bus_udev.c:1492-1496` both carry on |
| 59 (prctl) | `PR_SET_SYSCALL_USER_DISPATCH` | `EINVAL` (pre-5.11 Linux) | upstream Wine never calls it (prctl use is `env.c:501`, `server.c:1656`, result ignored); the host's per-thread arming is Proton's |
| 310/311 | `process_vm_readv`/`writev` | **implemented** via a bounce buffer with `vm_user_read`/`vm_kwrite` on the target | wineserver reads client memory with it (`server/ptrace.c:345-404`); its ptrace + `/proc/<pid>/mem` fallback does not exist here |
| 98 | `getrusage(RUSAGE_THREAD)` | left as the existing cheap all-zero fill | Wine compares `ru_nvcsw` around `sched_yield` (`dlls/ntdll/unix/sync.c:2400-2409`); compat `sched_yield` does not actually yield (`linux_compat.c:142`), so "no yield performed" is the honest answer, and zeros make it stable |

`chmod`/`fchmod`/`chown`/`utimensat` are **not** this lane's (C3-ext2b).

## 5. Fork-shaped clone (`linux_compat_thr.c`, `process.c`)

glibc 2.31's `fork()` is
`clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD, 0, NULL, &tid)` — no
`CLONE_VM`, no `CLONE_THREAD`.  `l_thr_clone` used to refuse it with `ENOSYS`
("no fork/COW in this kernel"), which would have stopped the wine loader's
`start_server()` and the wineserver daemon fork dead.  Such a clone is now
routed to the native `process_fork()` (which already clones a v2 address
space); a non-zero child stack becomes the child's `%rsp`, `CLONE_PARENT_SETTID`
writes the pid in the parent.  `process_fork()` now:

- copies `abi_flags` — a compat child forked through `SYS_fork(57)` used to
  come back as a **native** process, so its own syscalls would miss the compat
  table;
- for a compat parent, calls `linux_compat_fork_child()` before the child is
  READY: `CLONE_CHILD_SETTID` writes the child's pid into its copy of `*ctid`
  (glibc's child tid), `CLONE_CHILD_CLEARTID` arms the exit word, and the
  dispositions, signal mask, `sa_flags` and `sigaltstack` are inherited as
  Linux `fork()` inherits them.

`CLONE_VM|CLONE_VFORK|SIGCHLD` (posix_spawn) is still refused.

This does **not** touch `linux_probe_join` row E: that case is a THREAD clone
(`CLONE_VM|...|CLONE_THREAD|CLONE_CHILD_SETTID`, `linux_probe_join.c:181-196`)
whose `EINVAL` comes from `process_clone()`.  It fails identically on base
(§9), i.e. a base defect, not this lane's.

## 6. `VM_OBJ_MAX_BYTES` 64 MiB → 1 GiB (C1-mmu's `vm.h`)

Wine's largest memfd is exactly 64.0 MiB and a GPU buffer mapping reaches
192 MiB (`docs/cyberpunk.md §0.2 item 12`); the old 64 MiB cap refused the
first and the second.  The cap also bounds FAT/NFS **file** mappings in
`vm_mmap`, so C1-mmu should be aware the ceiling moved.  `vm_object_truncate`
now reallocates the object's page array only when it outgrows the frames it
already owns, and then at least doubles it — growing a file by appends used to
copy the whole array on every page (quadratic once files exceed 64 MiB).

## 7. Gates and known limits

- Probes `tmpfs` and `wsys` print `[LP] done rc=0` on a Linux host (the
  reference) and are run in HobbyOS by `tools/gate/t3_linux_probe.sh`.  The
  tmpfs probe covers unlink-while-open, rename, symlink/readlink, a >64 MiB
  memfd and >64 MiB `/tmp` file mapped `MAP_SHARED`, a socket inode, record
  locks across fork, and a shared counter page passed over `SOCK_STREAM`
  `SCM_RIGHTS` to a forked reader that watches it change.  The wsys probe
  covers fchdir success/error, the xattr errnos, eventfd success/error paths,
  inotify, `process_vm_readv` (self), `prctl(SUD)`, `getrusage(RUSAGE_THREAD)`.

### Limits other lanes need

- **`process.cwd` is 128 bytes** — deep Wine prefix paths will hit
  `ENAMETOOLONG` from fchdir/chdir.
- **2048 `vm_object` slots** system-wide: each non-empty tmpfs file uses one,
  so a prefix left on tmpfs after `wineboot -i` can exhaust them (the prefix
  belongs on ext2, P3).
- **tmpfs `MAP_PRIVATE` is unsupported** (as for memfd: `ENOTSUP`).  Wine's
  exec-mapping probe on its temp dir falls back gracefully
  (`server/mapping.c:340-354`).
- **The staged `/CP/WINE` tree must be on NFS**, not FAT16: its names (`wine`,
  `wineserver`, two-dot `.so` names) do not fit FAT 8.3, and ext2 mmap/exec is
  P3.  NFS exec+mmap already work (RimWorld runs from NFS).

## 8. Where the Wine smoke stops (measured on strix, §9)

`tools/cp/run-wine-smoke.sh` stages the strix-built `/CP/WINE` over NFS
(export `127.0.0.1(ro,...,insecure,...)` only; `/etc/exports` backed up and
restored byte-for-byte, the export dir and the backup removed on exit) and
runs `src/user/wineprobe.c` (native) in the intel guest:

| step | result in HobbyOS |
|---|---|
| `wineserver --version` | prints `Wine 11.0`, exit 0 |
| `wine --version` | prints `wine-11.0`, exit 0 |
| `wineserver -f -p5` (WINEPREFIX `/CPPFX` on FAT) | starts; creates `/tmp/.wine-0/server-101-22c8/` on the tmpfs, binds its socket, takes the lock; alive after 4 s |
| `wineserver -k` (a second compat process) | finds the server through the lock file (`F_GETLK` -> owner pid), sends SIGINT (delivered to Wine's handler) then SIGCONT; exits 0 |
| server shutdown | **killed by SIGCONT** (`exited (signal 18)`): the compat SIG_DFL path terminates on SIGCONT, whose Linux default is "continue" — the same class as cp-cpu `fd52f2b0` (CHLD/URG/WINCH); SIGCONT (and SIGSTOP/SIGTSTP semantics) belongs on that list (C1-cpu) |

Two further findings, both recorded as the next lanes' input:

- **`MAP_PRIVATE` of a tmpfs file is refused** (`[LCOMPAT] mmap fail ... -> -95
  path=/tmp/.wine-0/server-101-22c8/tmpmap-…`).  Wine's
  `check_current_dir_for_exec()` (`server/mapping.c:340-354`) then moves its
  section temp files to the config dir (`:368-381`), i.e. onto FAT
  (`/CPPFX/tmpmap-…`).  For KUSER_SHARED_DATA and every anonymous section to
  stay on the coherent tmpfs object path, `vm_mmap` must accept MAP_PRIVATE
  (copy-on-write) over a memfd/tmpfs object — the vm.c mmap/fault path
  (C1-mmu).
- `chmod` (nr 90) is ENOSYS (`chmod("socket", 0600)`, result ignored,
  `server/request.c:831`) — C3-ext2b's setattr work.
- A compat `open(dir, O_RDONLY)` on FAT16 used to be ENOENT; Wine pins its
  config dir that way (`server/request.c:634`).  Fixed in this lane
  (Linux-ABI processes only).

`wine cmd /c echo` is not attempted: it needs `ARCH_SET_GS` and
KUSER at `0x7ffe0000` (low VA), C1-mmu.

## 9. Gate results on strix (2026-11-15)

linuxci's root filesystem failed (journal aborted on a disk I/O error) and
every result from it after 08:23 is void; all results below are from
**strix** (AMD RYZEN AI MAX+ 395, Zen 5, **AVX-512**, KVM, kernel
7.0.0-43-generic — linuxci was an AVX2-only EPYC), lane `cp-sys` vs base
`~/cp/base` = `cyberpunk` @ 6e73e34e.  Raw extracts:
`docs/cp-sys-evidence/strix-20261115/`.

| gate | lane | base |
|---|---|---|
| `linux_probe_tmpfs` | **PASS** rc=0 | (new) |
| `linux_probe_wsys` | **PASS** rc=0 | (new) |
| `linux_probe_join` | FAIL rc=2,2,1,2 (row E every run, row A intermittent) | FAIL rc=1,2,1,2 (same rows) — **parity, base defect** |
| `linux_probe_fd` | PASS | PASS |
| `linux_probe_mem` | PASS | PASS |
| `linux_probe_proc` | PASS | PASS |
| `linux_probe_timers` | PASS | PASS |
| `run_unit_tests_intel.sh` (TOOLS_SKIP=1) | 448 run, 2 failed (`virtio_gpu_test.c:213`, `:243`) | 448 run, the same 2 — **parity** |
| Wine build (`build-wine.sh all`, JOBS=16) | 202 MiB staged, 873 files, static-pie preloader, max `GLIBC_2.29` | — |
| Wine smoke | §8 | — |

`join` row A ("park-first") is swallowed in the console by a spliced kernel
`[FUTEX]` trace line in both trees and fails intermittently in both: a
pre-existing defect (with row E) for the owner of `process_clone()` / the
futex trace.
