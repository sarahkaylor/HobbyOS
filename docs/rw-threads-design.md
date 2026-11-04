# M2 — threads, futex, TLS and signals under the compat track

Design + implementation note, lane L7 (`rw-l7-threads`), 2026-11-02. Companions:
`docs/linux-abi.md`, `docs/rw-ldso-design.md`, `docs/rimworld-evidence.md` §3.

Why: the real game makes **1,095,446 `futex` calls in 150 s** (69 % of all
syscalls, 97 % of syscall time) and runs **106 threads at the menu** (145 peak).
A tick-poll futex is not a partial implementation, it is a wrong one.

Implementation: `src/kernel/linux_compat_thr.c` (`l_tab_thr[]`) plus four weak
hooks in `process.c` (fault delivery, signal delivery, thread-exit cleanup,
signal-wakes-a-park). It reuses the native `process_clone()` /
`process_thread_exit()` / futex park state: no second thread model, and the native
dispatch is untouched.
## 1. clone(56) — the exact set

Accepted flags; anything else is `-EINVAL`, never silently ignored:

    CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|SETTLS|PARENT_SETTID|CHILD_SETTID|
    CHILD_CLEARTID|DETACHED          (CLONE_VM|CLONE_THREAD is mandatory)

Refused: `CLONE_NEW*` (no namespaces), `CLONE_PARENT`, `CLONE_PTRACE`,
`CLONE_VFORK`, and any word without `CLONE_VM` — there is no copy-on-write
address space, so `fork(57)`/`vfork(58)` stay `-ENOSYS` and say so once.

Child semantics are Linux's: `rax = 0`, `rsp = child_stack`, the rest of the
register file is the caller's, resume after `syscall`; the parent gets the tid.
`exit(60)` from a non-leader is **thread exit**, not `SYS_EXIT`: glibc's
`start_thread` ends with `__exit(0)` and a group teardown there would kill every
sibling (`exit_group(231)` keeps its group meaning).
## 2. FS base, errno, arch_prctl

`CLONE_SETTLS` → `t->tls_base`; Linux's 5th clone argument is **`r8`**
(`tf->regs[7]`), not `r10`. The scheduler already writes `tls_base` into
`IA32_FS_BASE` on every resume, so a thread gets its own FS base for free.
`arch_prctl` is a table row: `SET_FS` → `process_set_tls()`, `GET_FS` → the live
MSR, `GET/SET_CPUID` → 0, else `-EINVAL`; the legacy case is deleted so the two
cannot disagree. errno needs no work: glibc keeps it in the TCB at `%fs:...`, and
kernel-side compat state keyed by pid (`l_slot()`) is per-thread for the same
reason — a thread *is* a pid here.
## 3. futex(202)

Key = **(address space, uaddr)**. The queue is a 64-entry slot table (one slot
per PCB); a waiter is the PCB itself, parked in `PROC_STATE_FUTEX` with
`p->futex_uaddr = uaddr` — the native futex state, so `clear_child_tid` wakes and
the tick timeout path work unchanged. A wake is a 64-comparison scan (~50 ns), not
a chain: the first cut used per-bucket chains and went stale across
enqueue/dequeue/requeue/timeout (measured: a wake found 0 waiters while a thread
was parked on the exact word). Every match is re-validated against the PCB under
`proc_lock`, so a stale entry cannot be woken for a wait it is not in.

Ops: `WAIT(0)`, `WAKE(1)`, `WAIT_BITSET(9)`, `WAKE_BITSET(10)`, `REQUEUE(3)`,
`CMP_REQUEUE(4)`. `FUTEX_PRIVATE_FLAG`/`CLOCK_REALTIME` are stripped; a shared
futex is treated as private (each key is already per address space).

`WAIT` compares the word **under `proc_lock`** after `vm_touch()`, so no wakeup
can slip between compare and park (mismatch → `-EAGAIN`, timeout 0 → `-ETIMEDOUT`).
`WAIT_BITSET`/`WAKE_BITSET` store and match the waiter's bitset — glibc's condvar
broadcast depends on it (two groups share one word with different bitsets) — and
`WAIT_BITSET`'s timeout is absolute. `REQUEUE`/`CMP_REQUEUE` follow the raw ABI
`futex(uaddr, op, val, val2, uaddr2, val3)` (`val` wake, `val2` requeue, `val3`
the expected word). `WAKE_OP` and the two PI **requeue** ops
(`WAIT_REQUEUE_PI`/`CMP_REQUEUE_PI`, the condvar-on-PI-mutex path) → `-ENOSYS`
with a counted serial row; no measured boot has issued one.

**`FUTEX_LOCK_PI`/`UNLOCK_PI`/`TRYLOCK_PI` are implemented** (L25). Not for
priority inheritance — this OS has no priority scheduling to inherit into — but
because glibc 2.31 *probes* the kernel on the first `PRIO_INHERIT`
`pthread_mutex_init()` (`futex(&zero, FUTEX_UNLOCK_PI|PRIVATE, 0)`); an
`-ENOSYS` there makes `tpi_supported = -1` and every PI init return
`EOPNOTSUPP` **without writing the mutex**, which is how the M6 park happened
(see `docs/rimworld-launch-log.md` §16). The lock is granted by **handoff**:
`UNLOCK_PI` writes the waiter's tid into the word before waking it, because
glibc reads `futex_lock_pi()` returning 0 as "the kernel gave me the mutex".
Consequently a plain `FUTEX_WAKE` and the signal-interrupt path must never wake
a PI waiter; the signal stays pending until the lock arrives.

## 4. Robust lists — honoured, not stubbed

`set_robust_list(head, 24)` records the head per thread; `get_robust_list` returns
it (same group only, else `-EPERM`). On thread exit the walk (≤ 2048 nodes) marks
every owned mutex word `FUTEX_OWNER_DIED` (keeping the TID bits and `FUTEX_WAITERS`)
and wakes it — otherwise a thread that dies holding a robust mutex parks its
joiners forever, which reads as a `pthread_join` bug.

**The empty list is self-referential** (glibc: `head.list == &head.list`,
`futex_offset == -32`). Treating it as one entry reads 32 bytes *below* the head —
`pd->list`, the stack-cache link — and ORs `0x40000000` into it. Measured:
`0x10802122D0` became `0x10C02122D0` and the next `pthread_join` died in
`__free_tcb` at `0x10C02122D8`. The walk stops at `list == head`.
## 5. Signals — per-thread, Linux-shaped

The native engine plants an HOS3 frame and expects `process_sigreturn()`; glibc's
restorer does `mov $15,%rax; syscall` and its handler reads a Linux `ucontext_t`.
So a compat process gets a real x86-64 **`rt_sigframe`**: `pretcode` (the guest's
`sa_restorer`) at `%rsp`, `ucontext` (`uc_mcontext.gregs[23]`, fpstate pointer,
`uc_sigmask`), `siginfo`, and a 512-byte FXSAVE image. `rt_sigreturn` parses it
back (regs, `rip`, `rsp`, `rflags`, mask, FP).

The frame goes on the **faulting thread's** stack (or its altstack when `SA_ONSTACK`
is installed). **Fault signals are deliverable**: `process_fault_exit()` asks the
compat layer first and, with a handler, the thread resumes in it — `#PF →
SIGSEGV(11)` with `si_addr = CR2`, `#UD → SIGILL(4)`, `#DE/#OF → SIGFPE(8)`, `#GP
→ SIGSEGV`; without a handler the process still dies with the signal-shaped status.
SIGFPE joined the known-signal set but stays non-deliverable natively, so native
behaviour is unchanged. `kill`/`tgkill` record pending on the **addressed thread**
(not the anchor) and a compat thread parked in futex wakes with `-EINTR` — Mono's
stop-the-world needs both; delivery consults the per-thread mask.
## 6. Dynamic TLS / `__tls_get_addr`

`__tls_get_addr` is **glibc's**, not the kernel's: under AD-2 the guest's own
`ld.so` owns the DTV, the static TLS blocks and the surplus. The kernel owes it a
per-thread FS base, `clone(CLONE_SETTLS)` and `mmap`/`mprotect` — all present, so
the smallest correct step is **none in the kernel**; the gate proves it by
`dlopen`ing a DSO with `__thread` state and reading it from two threads. The native
`/LDHOBBY.SO` loader stays initial-exec only (`docs/shared-libs.md` §1.3); a DTV
there is a separate job.
## 7. What the gate found in the M1 startup block

The compat startup block left an **8-byte hole between `envp`'s NULL and the auxv**
when `(argc + envc)` made `argv_addr` 16-aligned: `argv_addr` moved down 8 for the
SysV rule, the auxv did not. glibc scans past `argv`'s NULL, past `envp`'s NULL and
reads the next word as the first `Elf64_auxv_t`; with the hole it read `{0,0}` =
`AT_NULL`, saw **no auxv**, and took its "invoked directly as `ld.so program`"
path, so `argv[1]` became the program. Measured: `mono --version` died with
`--version: error while loading shared libraries: --version: invalid ELF header`,
and a 20-line probe with two arguments failed identically. Fix
(`program_loader.c`): move `auxv_addr` down with `argv_addr`, keeping `envp_end ==
auxv`.
## 8. Mono 6.12 — how far it gets

`mono --version` / `mono hello.exe` reach the runtime: all nine `DT_NEEDED` DSOs
load (libz, librt, libdl, libpthread, libstdc++, libm, libgcc_s, libc, ld.so),
glibc installs its handlers, and mono exits 1 with its own diagnostic:

    Mono requires /proc to be mounted.

That is an `access()`/`open()` of `/proc/self/maps`, which this kernel does not
synthesize (`fs.c` has `/proc/self/fd` and the `readlink` links only). It is not a
threading/TLS/signal failure: with no assembly argument the same runtime prints its
full usage banner, i.e. it initialized. The maps file is the next dependency and
lives in `fs.c` (L6's file), so this lane did not touch it.
## 9. Known ceiling

`MAX_PROCESSES` is **64** and every thread costs a PCB slot; the game measured **106
threads at the menu / 145 peak**. Raising the table is kernel-wide (pid masks,
per-CPU arrays, 63-slot assumptions) and is not smuggled into this lane.
