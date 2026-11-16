# cp/k4 — kernel stability debt from Gate P2 (lane C1-k4, wave 4)

Plan: `docs/cyberpunk.md` §0.2 item 18 ("Still open"), §7.2d (C1-k4 row).
Evidence: `docs/cp-k4-evidence/` (strix, Zen 5, QEMU/KVM, 8 vCPUs, 12 GiB).
Probes (ordinary musl static-PIE programs; the same ELF runs on strix's Linux
as the reference): `linux_probe_pipestorm`, `linux_probe_tlb`,
`linux_probe_manythr`, `linux_probe_execthr`, `linux_probe_siginfo`.

## 1. The wineboot-tree hang: blocking IPC

The P2 hang shape (every Wine client BLOCKED in `read()` — the dumps' RIPs
are libpthread-2.31 `__read`'s two `syscall` sites, +0x13390 / +0x133ca — no
futex, no timer, wineserver alive in its poll loop) is a request that the
server never reads or a reply it never writes.  The suspected lost wakeup in
`pipe.c`'s park is **not** there: the reader's registration and BLOCKED are
committed under proc_lock + the pipe lock and the writer collects the mask
under the pipe lock, and `pipestorm` R1/R2 (thread and process ping-pong
over pipes, 8 CPUs) pass on base.  What `pipestorm` found instead, each
seen failing on base (`cp-k4-evidence/pipestorm-base.txt`):

| row | base | cause | fix |
|---|---|---|---|
| R5 epoll with 48 fds | HANG | the compat epoll held **32 fds** (and 8 instances system-wide); the 33rd `EPOLL_CTL_ADD` failed `ENOSPC`.  wineserver keeps one fd per client thread plus one per process in ONE epoll set; on that error it `perror()`s and carries on, and that thread's requests are never read again — exactly the P2 shape | 1024 fds per instance, 16 instances (x86_64); `[EPOLLHW]`/`[EPOLLFULL]` rows |
| R6 wine_proto | FAIL: reply `writev` = 512 of 557 | pipes held **512 bytes**; Wine's `send_reply`/`send_request` treat a short count as a protocol error | Linux-ABI pipes get Linux's 64 KiB ring (16 frames, allocated at `pipe()`), PIPE_BUF atomicity, blocking writes complete in full (a park part-way records its progress in the PCB and the restarted syscall resumes), `writev` on a pipe is one gather write |
| R7 bigwrite | FAIL: `write(100000)` = 512 | same | same |
| R8 nonblock | HANG | pipes **ignored O_NONBLOCK**: an empty read parked the caller (wineserver's request fds are non-blocking) | `-EAGAIN`; `file_alloc` no longer leaks a recycled slot's `O_NONBLOCK` |
| R10 eventfd ping-pong | HANG (> 30 s) | eventfd reads/writes re-polled in 10 ms slices with nobody waking them | wait masks + wakes |
| R11/R13 SIGKILL of a busy group | base guest **rebooted** | a thread killed while inside a pipe/AF_UNIX/poll/epoll/eventfd/timerfd park re-parked itself over THREAD_DONE (C1-thr2 fixed only futex/nanosleep/sigsuspend/vfork); the resurrected threads ran on with a freed anchor (`[GB145-GHOST]`) | `process_park_ok_locked()` guard on every such park |

Also fixed on the same paths: poll/select/epoll_wait re-scanned in 10 ms
slices — every request wineserver served from an idle wait cost up to 10 ms.
Now `poll_notify()` (every pipe / AF_UNIX / eventfd state change) wakes
parked pollers; a Dekker pair (`poll_event_seq` read before the scan,
`poll_park_locked()` under proc_lock) makes the wake unlosable; a set made
only of notifying fds keeps a 250 ms safety slice, others the old 10 ms.  An
AF_UNIX stream reader now wakes parked writers on any freed byte (it waited
for an empty ring).  `l_park` and `epoll_wait` answered `EINTR` with the
thread already marked BLOCKED; vectored reads/writes that moved bytes before
a later segment parked returned with it BLOCKED (`process_cancel_park`).

## 2. Cross-CPU TLB shootdown

`munmap`, `mprotect`, `mremap` and `madvise(DONTNEED)` flushed only the local
TLB and freed the frames at once: another thread of the group kept reading
(and writing) the old frame — possibly already someone else's page — until
its CPU happened to reload CR3.  Now (vm.c `vm_tlb_batch`): leaves are
cleared under vm_lock with the released frames *parked* in a batch (inline
array + frame-backed overflow list) and the span's objects referenced; after
the lock is dropped `vm_arch_shootdown()` IPIs every CPU whose CR3
bookkeeping is the root or whose claimed process is in the group and waits
for each to flush; then frames and object references are released.  The x64
shootdown (`mmu.c`) has per-CPU pending words, a sender lock whose spinners
service their own pending flush, ICR-idle waits, a re-send every ~1 ms of
TSC and a `[TLBTIMEOUT]` row after ~2 s.  ARM's TLBI is already broadcast.

## 3. MAX_PROCESSES 512 (x86_64)

See the comment in `process.h` for the per-slot cost; ARM stays at 192 (its
ASIDs are 8 bits, keyed by tgid).  `PID_MASK_BITS` follows.

## 4. execve from a secondary thread

`process_exec_become_leader()` (process.c), called from `linux_execve` right
after the siblings are terminated: the leader's slot takes the exec'ing
thread's state (signal mask, thread-directed pending, registers, FPU image),
this CPU's claim moves to it, children the thread forked are re-parented to
the group, and the thread's slot becomes a THREAD_DONE tombstone.  The new
image runs with `gettid() == getpid()` == the pid its parent knows.  (Before,
the leader slot was a THREAD_DONE tombstone and the scheduler's ghost
detector skipped the exec'ed thread at its first preemption.)

## 5. siginfo detail

`trap.c` records Linux's `si_code`/`si_addr` in the PCB (`fault_si_*`) where
the fault is understood; `linux_compat_fault` uses it.  `#PF`: no region →
`SEGV_MAPERR`, a refusing region → `SEGV_ACCERR`, a page wholly past the end
of its file/object (memfd, tmpfs, MAP_SHARED objects, and file objects in a
compat address space) → SIGBUS `BUS_ADRERR`; `#UD` → `ILL_ILLOPN`; `#MF`/`#XM`
→ the `FPE_FLT*` code from FSW&~FCW / MXCSR (Linux `fpu__exception_code`);
`#GP`/`#BP` and the rest → `SI_KERNEL`.
