# cp-k5 — fd, file-table, poll and process-scale ceilings for Wine (lane C1-k5)

Wave 5, plan `docs/cyberpunk.md` §7.2e (C1-k5), §6 P8, §0.2 items 13 and 18.
Cyberpunk runs 173–190 threads at its menu. Each Wine thread costs its
process three pipes to wineserver (request, reply, wait), and wineserver holds
the other ends. Wine also raises `RLIMIT_NOFILE`'s soft limit to the hard one
at startup (`dlls/ntdll/unix/loader.c` `set_max_limit`, `server/main.c`
`init_limits`). On base5 the limits were 256 fds per process, 512 open files
system-wide (so 256 pipes) and 256 poll fds.

Evidence: `docs/cp-k5-evidence/`. All numbers are from strix (Zen 5,
`ssh strix`). Guests are intel `MODE=test`, KVM, 8 vCPU, 12 GiB, under
`tools/cp/qemu-slot.sh`.

## 1. Tables: what is static, what is allocated

The x86_64 image must end below `CP_KHOLE_HI` = 0x78000000
(`linker_x64.ld` ASSERT). On base5 `__stack_top` is 0x76727790, so about
24.8 MiB is left. Static arrays at the new sizes would have used most of
that:

| table | base5 | lane | as a static array | what the lane does |
|---|---|---|---|---|
| per-process fd table `open_fds[]` | 256 ints inline per slot (1 KiB × 512) | **4096** (`MAX_OPEN_FDS`, x86_64; ARM stays 256) | 16 KiB × 512 = **8 MiB** | `int *open_fds` points into **one whole 32 MiB frame-pool block** that `process_init()` takes right after `frame_init()` (`frame_block_alloc_first_free`; 8 MiB used, 24 MiB idle). It has to be a whole block: the block layer's invariant (free + whole-unit blocks = all blocks) is checked by `frame_test.c:118/:196`, and an 8 MiB frame run broke it (measured in the first unit run). Never freed. A second `process_init()` (trap_test re-runs `frame_init`) re-reserves the same block. ARM keeps a static 192 KiB array (`fdt_static`), the old inline size. |
| `fd_cloexec[]` | 4 words | 64 words inline | +240 KiB | stays inline |
| system-wide file table | 512 static (`global_file_table`, 627 KiB) | **16384** (`MAX_GLOBAL_FILES`, x86_64; ARM 512) | 1.2 KiB × 16384 = **19.6 MiB** | the first 512 (`GFT_STATIC`) stay static. The rest come in **64-slot chunks** (20 frames each) from the frame pool, taken when every materialised slot is busy and never freed. `gfile(i)` is the only accessor, and an index whose chunk does not exist yet names `gft_dead`, which is always EMPTY. `struct file` gained `gidx` (pointer arithmetic is gone). The static slots are scanned first and lowest first, as before; the chunks are scanned from a rotating hint. |
| pipe pool | `MAX_GLOBAL_FILES/2` = 256 static (237 KiB) | **8192** | 930 B × 8192 = **7.4 MiB** | 256 static, then 64-pipe chunks (15 frames each), the same scheme (`pipe.c` `pipe_slot`) |
| epoll | 16 instances × 1024 inline entries (257 KiB) | **64 instances × 4096 entries** | 4 MiB | instance headers are static (3.5 KiB). Each instance's 64 KiB entry array comes from the frame pool the first time its slot is used, and stays with the slot. |
| eventfd slots | 256 | **1024** | +110 KiB | static |
| timers (itimer + POSIX + timerfd, one table) | 48 | **256** | +11 KiB | static |

Measured on strix (`nm -S`, intel MODE=test): `proc_table` went from
0x5c8000 to 0x590000 (11,776 to 11,392 B/slot); `le` from 0x40300 to 0xe00;
`global_file_table` (627 KiB) became `gft_static` (632 KiB, +`gidx`);
`pipes` (0x3a000) became `pipes_static` (the same size); `l_efd` went from
0x9000 to 0x24000, `lt` from 0xa80 to 0x3800 and `fd_slot_gen` from 0x400 to
0x4000. `.bss` and `__stack_top` are unchanged (0x766a7784 / 0x76727790), and
so is the 24.8 MiB headroom: the shrinks and growths net to about −360 KiB,
which the section's 2 MiB alignment absorbs. The unit-test kernel ends at
`__stack_top` 0x76b3a790.

Frame cost at run time: the fd tables take one 32 MiB block at boot (8 MiB of it used). Each file chunk is
80 KiB, each pipe chunk 60 KiB, each epoll instance 64 KiB the first time it
is used. A Linux-ABI pipe's 64 KiB ring (C1-k4) is unchanged, so 3000 Linux
pipes are 188 MiB of rings.

The chunks are never freed. A `struct file *` or `struct pipe *` may be held
by anything (a parked reader, an epoll entry, a fault path), so a chunk must
outlive every reference. Since the table only grows to the peak, this is
bounded. Caveat: trap_test re-runs `frame_init()`, which forgets every
allocation. Chunks created before that (`test_fs_table_growth` creates them)
stay installed over frames the allocator now thinks are free. This is safe in
the unit-test kernel for two reasons: chunks are scanned only after the 512
static slots are full, and no later suite holds more than 512 files or 256
pipes. It is never a concern in a real boot, which runs `frame_init` once.

The lock registry (`lockscale.c`, `LS_REG_SLOTS` 4096) still holds the 512
static file locks. The chunk slots' locks are deliberately not registered,
because 16384 would not fit.

## 2. RLIMIT_NOFILE

- It lives on the group anchor: `rlim_nofile_cur` / `rlim_nofile_max`. The
  defaults are soft 1024 and hard 4096, Linux's own defaults. fork copies
  them, exec keeps them, and a fresh process (native spawn included) gets the
  defaults.
- `getrlimit`/`setrlimit`/`prlimit64` (`linux_compat_proc.c`): cur > max is
  `EINVAL`. Every compat process is uid 0, so the hard limit may be raised,
  but only up to `MAX_OPEN_FDS`; beyond that is `EPERM` (Linux: `nr_open`).
  `prlimit64` works on any live pid (uid 0) and reads old before applying
  new. The other resources keep their old answers; cur > max is now `EINVAL`
  for them too.
- Enforcement (`fs.c`): every fd-allocating scan stops at the soft limit
  (`process_fd_limit`). That covers open, pipe, socket, socketpair,
  accept/msg-install, memfd, eventfd, timerfd, epoll, dup and F_DUPFD.
  Allocation past the limit is `EMFILE`, and `dup2`/`dup3` to newfd ≥ soft
  is `EBADF`.
- `fcntl(F_DUPFD / F_DUPFD_CLOEXEC)`. The compat layer used to answer
  `EINVAL` for both; they now go through `file_dup_min`, which returns the
  lowest free fd ≥ arg. An arg at or past the soft limit is `EINVAL`.
- `close_range` now skips closed slots. With 4096 slots it would otherwise
  do 4096 `file_close` calls, and its `CLOEXEC` form used to set the bit on
  closed slots, which the next fd allocated there would then inherit.

## 3. poll / ppoll / select / pselect6 / epoll

- `poll`: nfds up to `RLIMIT_NOFILE` (Linux `do_sys_poll`: `EINVAL` above
  it). The fs.c engine has no fixed array, so its own cap is now
  `MAX_OPEN_FDS`.
- `ppoll` (271) was ENOSYS and is now implemented. The timespec timeout is
  honoured. The signal mask is not applied, the same limitation pselect6 has.
- `select` / `pselect6`: Linux semantics for any nfds. nfds is clamped to the
  table, as Linux clamps to `max_fds`. The kernel copies `FDS_BYTES(nfds)` =
  ceil(nfds/64) longs, so a 1024-bit glibc/musl `fd_set` works as before and
  a larger array works too. Bits at or past nfds are not examined. The engine
  is `file_select_words` (word-count sets); the native `file_select` (256-bit
  `fd_set_k`, frozen A.1a ABI) is a wrapper over it with the native limits.
- epoll: one set holds up to 4096 fds, with 64 instances system-wide (§1).

## 4. getrusage and wait4

- Accounting is in `schedule()`. The slice a CPU's current thread has run
  since it was switched in is charged when `schedule()` is entered for it,
  using `timer_get_us` (rdtsc), under the `proc_lock` that `schedule()`
  already holds. Each slice is charged to the thread (`ru_run_us`) and to its
  group (`ru_grp_us`, which keeps exited threads). Reaping a child moves the
  child's group and children totals into the parent group's `ru_ch_*`. Both
  reap paths do this: the immediate one in `process_wait_common` and the
  delayed one in `reap_deliver_locked`.
- `nvcsw` / `nivcsw` count only real switches away (Linux: `prev != next`).
  nvcsw is counted when the thread had blocked, nivcsw when it was still
  runnable (preempted or `sched_yield`). Going idle counts too. So a
  `sched_yield` with nothing else runnable changes neither counter, which is
  exactly what Wine's `NtYieldExecution` tests (`dlls/ntdll/unix/sync.c`: it
  compares both counters around `sched_yield()` and returns
  `STATUS_NO_YIELD_PERFORMED` when they are equal).
- `getrusage(RUSAGE_THREAD / SELF / CHILDREN)` is O(1), with no slot walk and
  no lock (`process_rusage`), and it includes the caller's running slice.
  Any other `who` is `EINVAL`. `wait4`'s rusage is the child's self + children
  total (Linux `RUSAGE_BOTH`).
- All CPU time is reported as `ru_utime`, and `ru_stime` stays 0. Splitting
  user from system time needs a hook on every trap entry, and the kernel
  preempts only user mode, so it cannot be sampled at the tick either.
  `ru_maxrss` keeps its old meaning: v1 processes only.

## 5. MAX_PROCESSES

512 on x86_64 since cp-k4, and this lane leaves it alone. Every thread takes
a slot. Measured on the host: the game menu has 15–16 processes and 173–190
threads in total (§0.2 item 13). The P2 tree peaked at 18 processes.

thrstorm in the guest (§7) ran with 257 threads in its own process (its
Toolhelp count) and 9 live processes (the launcher's `sysinfo(3)` census,
which counts thread groups). Adding a few threads for each Wine service
process still keeps the run under about 300 slots, so 512 leaves more than
200 spare.

Per-slot cost is now 11.4 KiB of BSS plus a 16 KiB frame-pool fd table.
Going to 1024 slots would add about 5.7 MiB of BSS (proc_table and the other
`MAX_PROCESSES`-sized tables, ~19 KiB per slot, docs/cp-k4.md §3) and 8 MiB
of frames. P8 does not need it.

## 6. Fixed-size tables on the fd path (inventory)

| table | size | where | owner | status |
|---|---|---|---|---|
| per-process fds | 4096 | process.h `MAX_OPEN_FDS` | C1-k5 | raised (§1, §2) |
| open files | 16384 | fs.h `MAX_GLOBAL_FILES` | C1-k5 | raised, chunked |
| pipes | 8192 | pipe.c `PIPES_MAX` | C1-k5 | raised, chunked |
| poll nfds | RLIMIT_NOFILE | linux_compat_fd.c | C1-k5 | raised |
| select bits | MAX_OPEN_FDS (compat) / 256 (native `fd_set_k`, frozen A.1a) | fs.h `K_FD_SETSIZE` | C1-k5 | compat raised; native unchanged |
| epoll instances × fds | 64 × 4096 | linux_compat_timers.c `LE_MAX`/`LE_FDS` | C1-k5 | raised, entries allocated |
| eventfd | 1024 | linux_compat_fd.c `L_EVENTFD_SLOTS` | C1-k5 | raised; ENFILE when full |
| timers (itimer/POSIX/timerfd) | 256 | linux_compat_timers.c `LT_MAX` | C1-k5 | raised |
| signalfd | — | — | — | **not implemented** (ENOSYS; Wine 11 does not use it) |
| AF_UNIX pairs (socketpair, every connect) | **128** | unix.h `MAX_UNIX_PAIRS` | (unix.c) | **open**. Each pair is 3.5 KiB of BSS plus 1 MiB of ring frames. Wine needs one per process to wineserver plus one per X client, ~40 for the game, so it is not a P8 blocker; raise it to 256 (+448 KiB BSS) if C8-game hits it |
| SCM_RIGHTS fds per message | 16 | ipc_proto.h `K_IPC_MAX_FDS` | (unix.c) | open; wineserver passes 1 per message |
| AF_INET sockets | **16** | net.h `MAX_SOCKETS` | net.c (NFS lane) | **open**. 4 MiB of RX buffer each, so 64 MiB of BSS for 16; growing it needs net.c to allocate RX rings. The game's telemetry/launcher sockets count here. |
| netlink sockets | 32 | linux_compat_netlink.c `L_NL_MAX` | — | open, ample |
| ext2 open inodes | **512** | ext2.h `EXT2_OPEN_MAX` | C3-ext2w | **open**. ENFILE past 512 distinct open /EXT inodes. The comment ties it to the old `MAX_GLOBAL_FILES`. The prefix on ext2 opens few files at once, but 512 can be reached; suggest 4096 or dynamic |
| record locks | 512 / 64 waiters | vfs_lock.c `VFS_LK_MAX` / `VFS_LKW_MAX` | — | open, ample for Wine |
| page-cache objects (file mmaps, memfds) | 2048 | vm.h `VM_OBJ_MAX` | vm.c | open; ~200 DLLs for the game |
| tmpfs nodes | 8192 | tmpfs.c `TMPFS_MAX_NODES` | — | open, ample |
| fd flight recorder | 1024 events | fs.c `FD_EV_MAX` | — | a ring, not a limit |

## 7. Probes

### Linux: `src/user/linux_probe_fdscale.c`

Rows F1–F10 are described in the source header. The probe passes on the
strix Linux host (`ulimit -Hn` 524288) and fails 10/10 on base5
(`docs/cp-k5-evidence/fdscale-base.txt`). With this lane's kernel it passes
10/10 (`fdscale-lane.txt`). The Makefile needs no change: probes are
wildcard-driven (`LINUX_PROBE_SRCS`).

### Windows: `src/user/cp/thrstorm.c`

`thrstorm.exe` starts 256 threads, each on its own auto-reset event plus
either two anonymous pipes or one duplex named pipe. For 60 s it runs rounds
of 256 round trips (SetEvent, then the worker's WaitForMultipleObjects, a
token over its pipe, and the reply back). Then it stops every worker and
checks that each worker's exit code equals the round count. Build:
`tools/cp/k5/build-thrstorm.sh` (C2-wine2's winprobe recipe: clang
`-target x86_64-windows`, Wine's headers and import libraries, no MinGW).
Guest harness: `tools/cp/k5/run-thrstorm.sh`, a cut-down
`run-wine-p2.sh`. Its launcher is `tools/cp/k5/k5wine.c`: wineboot -i,
thrstorm, then wineserver -k. Grades: S1 done rc=0 and exit 0, S2 storm
≥ 60 s, S3 nothing left after wineserver -k, S4 no fault line.

Results on strix:

| run | rounds | round trips | per second | worst round |
|---|---|---|---|---|
| host: strix Wine in a private glibc-2.31 chroot, 20 s | 2899 | 742,144 | ~37,000 | 18 ms |
| base5 guest | — | — | — | — |
| lane guest, 60 s | 261 | 66,816 | ~1,100 | 1,813 ms |

On base5, thrstorm printed its header and then never printed the T1 row. It
was killed at the 900 s step timeout, and the guest stayed wedged afterwards
(`thrstorm-base.txt`).

On the lane, T1 started 256 threads (Toolhelp 257) in 2.9 s, and T2 and T3
passed. wineserver's epoll set grew past 256 fds (`[EPOLLHW] n=256`), and
`wineserver -k` left nothing behind (`thrstorm-lane.txt`).

The guest runs about 33 times slower than the host per round trip. Each trip
is a SetEvent, a wait, a pipe write and read, and a reply, and every one of
those is a wineserver request over pipes. This is the wineserver IPC cost P8
will pay. It is a performance item for a later lane, not a ceiling.

## 8. P8 scope added mid-wave (C8-game's first game boots, docs/cp-p8.md)

C8-game's boots died within 42 s. The wineserver's 256-fd table filled up,
`recvmsg` with SCM_RIGHTS failed with EMFILE and left the message queued, and
Wine killed the client; the wineserver then crashed in `grab_object(NULL)`.
The integrator moved the following into this lane. C8's probes
`src/user/cp/p8{env,scm,kill}probe.c` and `p8fdcount.c` are copied unchanged
from `cp-game`. `tools/cp/k5/run-k5probes.sh` with its launcher
`tools/cp/k5/k5probes.c` runs them in one boot without NFS.
Evidence: `p8probes-base.txt` (base5) and `p8probes-lane.txt`.

| item | change | probe | base5 | lane |
|---|---|---|---|---|
| fd/file tables | §1, §2 | p8fdcount | opened 253, EMFILE | opened 600 of 600 |
| SCM_RIGHTS into a full table (`unix.c`) | A Linux-ABI receiver gets `scm_detach_fds` semantics in both the stream and the dgram/seqpacket paths: the data is delivered, the fds that do not fit are closed, `MSG_CTRUNC` is set and the record is consumed. Native callers keep the P4 rule (EMFILE, message not consumed; `docs/browser/p4-ipc-design.md` §4.2). | p8scmprobe S1–S3 | FAIL ×3 (`ret=-1 errno=24`; the message stays queued) | PASS ×3 |
| ENFILE vs EMFILE (`fs.c`) | When the file table is full (open, socket, socketpair, pipe), the error is `ENFILE`. `file_pipe` now finds its two fds before it allocates anything; on a full table it used to leak two file slots and a pipe. The compat `pipe2` passes the code through. | — | — | — |
| env (`HO_ENV_LEN`, `COMPAT_MAX_ENV`) | `HO_ENV_LEN` is 2048 on x86_64. ARM stays at 512 to keep its .bss inside the limine window, and keeps the old truncating behaviour. Too large an env for `spawn_ex`/`execve` is `E2BIG` (`process_read_envp` returns -2), not truncated. `COMPAT_MAX_ENV` is 128; more entries is `E2BIG` from the loader. The env strings go to the compat stack straight from the PCB blob, with no 512-byte bounce. | p8envprobe E3/E4, k5probes E5 | E3 FAIL (13/40 intact, 535 bytes); E5 FAIL (an 80-entry, 3120-byte env was accepted and cut) | E3 PASS (40/40, 1583 bytes); E5 PASS (`errno=7`) |
| the native execve's env buffer (`trap.c`, x64 and ARM) | **moved off the stack**. The frame already held three 4 KiB argv buffers, so the env blob now lives in one `frame_alloc_zeroed()` frame, freed after `process_exec_current` copies it into the PCB. Running out of frames is `ENOMEM`. `compat_stack_block` still uses the stack: `s[512]` + `argp[129]` + `envp[129]` + `off[128]` ≈ 3.1 KiB, checked against the 64 KiB per-CPU stack (the loader worker calls it near the top of its stack). | — | — | — |
| `fadvise64` (221) | 0 after validation: `EBADF` for a closed fd, `ESPIPE` for a pipe, `EINVAL` for advice > 5 or len < 0 | — | ENOSYS | 0 |
| SIGKILL to a group whose leader exited (stop 7) | **not done; it goes to wave 6.** Two gates stop it: `process_signal_directed` treats a THREAD_DONE anchor as gone (`-ESRCH`, which reaches the caller as `kill rc=-1`), and `process_kill_group_sig` returns early for a THREAD_DONE anchor. The fix has to let both act on an anchor whose `live_threads` > 0, and then make sure group teardown and the reap cope with an anchor that is already THREAD_DONE. That is the process/signal teardown, not this lane's tables. | p8killprobe K1/K2 | FAIL | FAIL (unchanged) |

BSS: `HO_ENV_LEN` 2048 grows `struct process` and `spawn_ex_pool` by
1.5 KiB per slot, 1.5 MiB in total on x86_64. On the merged tree
(cp-k5 + cyberpunk 15ad4a35), `__stack_top` is 0x76927640, which leaves
22.8 MiB below `CP_KHOLE_HI`.

### Merge with cyberpunk 15ad4a35 (C0-sync)

- Main added `f1_pos_locks[MAX_GLOBAL_FILES]`, a static array of `ylock_t`,
  for positioned I/O. At 16384 slots that would be about 1.2 MiB. It is now
  `struct file`'s `pos_lock`, initialised with each slot (`gft_slot_init`),
  so it grows with the chunked table.
- Main's new `global_file_table[]` uses now go through `gfile()`.
- Main's AF_INET loopback `file_accept` fd-install loop honours
  RLIMIT_NOFILE. Its full-table answers were already EMFILE and ENFILE.
- `COMPAT_MAX_ARGV` is 128 (main) and `COMPAT_MAX_ENV` is 128 (this lane).
