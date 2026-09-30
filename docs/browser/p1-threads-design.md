# P1 design note — kernel threads & futex-lite (P1.1/P1.2; pthread mapping for P1.3)

Status: **design for integrator review; no code in this commit.** Milestone: browser.md §6 P1 (lane
L2). Base: main@9660552. Consumers: the P1 implementer (kernel, L2), L3 (`src/libc/pthread.c` +
crt0 TLS), L9 (tests). Grounded in `process.c` (save/restore, F1.5 `fpu_state`, `schedule`), `fs.c`
(select park), `arch/{arm,x64}/{trap.c,mmu.c}`, `include/{process.h,syscall.h,errno.h}`.

## 0. Constraints carried into the design
- Kernel C stays `-mgeneral-regs-only` except the two FP TUs; no unaligned access; user words are
  range+alignment-checked before any kernel load. `mrs/msr tpidr_el0` and `rdmsr/wrmsr` are legal
  under the flag.
- SMP ≥ 4 never reduced; `MAX_PROCESSES` stays 64 (pipe masks `1ULL << pid`); pid == slot index. New
  state lives under the existing `proc_lock`; thread/futex bookkeeping is fixed per-PCB state,
  scanned O(64); no allocation.
- x64 rules stand: never preempt a user process in kernel mode; kernel tasks own their stacks.

## 1. Thread objects (P1.1)
**Decision: a thread IS a PCB slot.** No second table, no scheduler rewrite: secondary threads are
`struct process` entries sharing the leader's address space, so every existing path — the READY
run-queue scan, `cpu_current_pids` claims, `taken_elsewhere`, LOSTWAKE, watchdogs, save/restore —
treats them as schedulable entities unchanged.

New fields appended at the END of `struct process` (additive merge; process.h → L2):
```c
int tgid;              /* leader pid; == own pid for a leader (always valid)     */
int is_thread;         /* 1 = secondary thread                                   */
int live_threads;      /* leaders: members not yet dead incl. self; 0 elsewhere  */
uint64_t tls_base;     /* opaque TLS register value (TPIDR_EL0 / IA32_FS_BASE)   */
uint64_t futex_uaddr;  /* P1.2: non-zero while parked in FUTEX WAIT              */
uint64_t thread_ret;   /* SYS_THREAD_EXIT argument (diagnostics only)            */
```
plus `#define PROC_STATE_FUTEX 8`, `#define PROC_STATE_THREAD_DONE 9`.

Creation (SYS_THREAD_CREATE): allocate ONLY a slot — no physical block (`phys_block_idx = -1`),
`user_phys_base`/`user_l2_table` = the leader's (`mmu_switch_user_mapping` re-maps it), no zeroing.
Fresh context: `context[31]=entry`, `context[33]=stack` (16-aligned, validated), x0/rdi = arg,
SPSR/RFLAGS as a fresh user; `fpu_state` zeroed (ARM) / `arch_fpu_reset()` (x64) — never the
creator's; `tls_base` = creator's live value (placeholder; the trampoline installs its own);
`parent_pid = -1`; `name` copied; `state = READY`; `live_threads++` on the leader.

Shared state stays in the leader PCB; thread-side access routes through `process_group(p) =
&proc_table[p->tgid]`. MUST route: `open_fds`/`num_open_fds`, `cwd`, `heap_brk`,
`anon_maps`/`anon_map_count`, `eargc`/`eargv`, `user_phys_base`/`user_l2_table`/`phys_block_idx`,
`exit_status`; per-thread: `context[36]`, `fpu_state`, `state`, `wake_ms`, `tls_base`,
`futex_uaddr`, `name`. Call-site audit: `fs.c` ≈36 (L1-owned → diff request), `process.c` 10,
`program_loader.c` 15, `vfs.c`+`fat16.c` cwd 5, `arch/x64/trap.c` 1. `sysinfo(3)` filters
`is_thread` entries; the watchdog dump shows all.

Group lifetime: the leader PCB is the group anchor. `live_threads` (under `proc_lock`) decrements at
each member death; phys block + fd table are released exactly once — at `live_threads == 0` with no
member claimed (`process_still_running`) — then the full `process_exit` tail runs (close fds, free
block, `EXITED`, waitpid delivery). A leader that exits early (`pthread_exit` on the initial thread)
becomes `PROC_STATE_THREAD_DONE` but STAYS the anchor (never reusable while `live_threads > 0`).

Slot reclamation: an exiting thread must NOT set `PROC_STATE_FREE` itself — a concurrent
`process_create` could re-init a PCB whose context `save_context` may still write (the hazard
`process_exit` avoids with `EXITED`). Exiting threads become `PROC_STATE_THREAD_DONE`, reusable only
when `!process_still_running(tid)`; extend `process_create_internal`'s no-free-slot fallback with
`THREAD_DONE && !still_running && (leader ? live_threads == 0 : 1) → FREE`. `schedule()` falls
through such contexts like `FREE`; `any_alive` counts them dead; `EXITED` scans (waitpid, orphan
sweep, zombie reclaim) never see threads (`parent_pid = -1` + a `!is_thread` guard).

Full-process exit: SYS_EXIT (2) keeps today's **exit_group** meaning from ANY thread: mark other
live members `THREAD_DONE`, clear their futex/select records, wait for member claims to drop
(tick-granular; one preemption per RUNNING member; parked members have no claim and die at once),
then the group teardown above. [Wedged-sibling edge: OQ3.]

Per-thread kernel stacks — **none added for user threads**: traps land on the per-CPU kernel stacks
(ARM SP_EL1 / the x64 per-CPU resume window), blocking syscalls abandon their kernel frames via the
existing save/park path, kernel-mode preemption stays forbidden (existing guards); kernel threads
keep their region-top stacks (P2 would revisit this).

## 2. Syscalls, numbers, semantics (§A.1b amendment)
```
SYS_THREAD_CREATE 72 (entry, arg, stack, flags)   -> tid | -EINVAL | -EAGAIN
SYS_FUTEX         73 (uaddr, op, val, timeout_ms) -> 0/count | -EAGAIN | -ETIMEDOUT | -EINVAL | -EFAULT
SYS_THREAD_EXIT   74 (retval)                     -> noreturn
SYS_SET_TLS       75 (tls)                        -> 0 | -EINVAL
```
72/73 are the planned P1 rows; 74/75 inserted → P4+ provisional rows shift +2 (`SYS_POLL` 74→76, et
seq.; nothing ≥72 is implemented). At the P1 gate the single writer applies this to `syscall.h`
(`SYS_MAX` 75) and records the rows in §A.1b [OQ1]. Dispatch: one `else-if` per syscall in BOTH
`arch/{arm,x64}/trap.c` (never a function table). Args: ARM x0..x3 = `regs[0..3]`; x64
rdi/rsi/rdx/r10 = `regs[5]/[4]/[3]/[9]` (as SYS_SELECT). Wrappers: `src/user/libc.c` on the existing
4-arg `syscall()`.

- **THREAD_CREATE**: `flags` must be 0; `entry` and `[stack-16, stack)` inside the caller's own
  region, stack 16-aligned → else `-EINVAL`; caller may be any user-process thread (kernel thread →
  `-EINVAL`); no free slot → `-EAGAIN`. The thread is runnable immediately; pthread publishes its
  TCB BEFORE the call. **THREAD_EXIT**: exits only the calling thread; `retval` recorded in the PCB
  for diagnostics — the authoritative value travels via the user TCB; last member → full group exit
  (§1). **SET_TLS**: validates `tls` is inside the caller's region (`-EINVAL`), stores it in the
  calling PCB AND the live register (a save before the next switch sees it); used once by crt0 and
  by every thread trampoline.
- **errno**: POSIX-shaped syscalls return `-errno` (wrapper normalizes via `errno_ret`); pthread
  functions return code values, never set errno. The `-2` restart convention is NOT used: a futex
  park returns directly (resume value fixed at wake time, §5). No SYS_THREAD_JOIN/DETACH —
  join/detach are libpthread (TCB + futex, musl model; D3).

## 3. Scheduler integration
- Threads ride the existing round-robin scan unchanged: the timer tick (ARM 10 ms generic timer; x64
  CPU0 PIT + 100 Hz 0x81 IPI) preempts and rotates across all READY PCBs; one tick = one quantum.
  Scheduler edits: save-set gains `PROC_STATE_FUTEX`; `PROC_STATE_THREAD_DONE` is skipped (§1).
  Claims stay per-THREAD; `taken_elsewhere`, LOSTWAKE and `process_still_running` carry over
  verbatim.
- Migration: any thread may run on any CPU; all group threads share `user_phys_base`, so
  `mmu_switch_user_mapping` maps the same block; `context`/`fpu_state`/`tls_base` travel with the
  PCB — nothing new.
- FPU: `fpu_state` is per PCB, so F1.5 save/restore is automatically per-thread and survives
  migration. Only P1 work: init fresh-thread FP state at create (as `process_create_internal`) and
  never copy the creator's.
- Blocking: a parked thread never stalls siblings (per-PCB states; the scan keeps picking READY
  ones). select's 10 ms slice + `-2` restart is untouched; no priority/affinity/timeslice
  accounting in P1 (non-goals).

## 4. TLS register management on switch
**Decision:** the TLS register joins the switch as one per-PCB word. `save_context`: read live →
`p->tls_base` (`mrs tpidr_el0` / `rdmsr 0xC0000100`); `restore_context`: write back (`msr`/`wrmsr`).
Order vs `fpu_save`/`fpu_restore` is irrelevant. Kernel threads keep `tls_base = 0`.

Layout pinned by probe (2026-09-30, llvm 21 + ld.lld; `__thread` variables disassembled — re-verify
before trusting):
- **aarch64 (variant I)**: `mrs TPIDR_EL0` + `ADD_TPREL_HI12/LO12`; lld bakes a 16-byte TCB head —
  TLS offsets 0/8/16 resolve to TPREL +0x10/+0x18/+0x20. So **TPIDR_EL0 = TLS image start − 16**.
- **x86_64 (variant II)**: lld relaxes to `movq %fs:0, %rax; <op> −K(%rax)`, `R_X86_64_TPOFF32` =
  off − align_up(size, align) (probe: −0x18/−0x10/−0x8 for a 20-byte image). So **FS base = TLS
  image end (aligned), self-pointer at FS:[0] = the FS base value** (kept even if a future link
  relaxes to direct `%fs:−K` — harmless).
- Initial thread: crt0 calls SYS_SET_TLS once with the linker-placed image (`&__tls_start − 16` ARM
  / aligned `&__tls_end` x64); `linker.ld` (L3) places `.tdata`/`.tbss` contiguously and exports
  `__tls_start/__tls_end/__tls_size/__tls_align`. The loader zeroes `[base, base+1 MiB)`, so `.tbss`
  arrives zeroed and the initial thread's TLS survives `fork()`. L3 deps: `errno` → `__thread`
  (errno.h flags it), a malloc lock, crt0 TLS; host builds unaffected.
- New threads: libpthread allocates `[16-byte head][image]` (ARM) / `[image][8-byte self-pointer]`
  (x64), copies `.tdata`, zeroes `.tbss`; the trampoline calls SYS_SET_TLS before any TLS-using C
  code (until then it runs on the creator's copied value — never a fault). x64 alternative rejected
  for P1: CR4.FSGSBASE + `wrfsbase` (CPUID/QEMU dependency, new CR4 surface).

## 5. Futex-lite (P1.2)
Surface as §A.1b, Linux op numbering: `op` 0 = WAIT, 1 = WAKE; other ops → `-ENOSYS` (CMP_REQUEUE
deferred).

**WAIT(uaddr, val, timeout_ms):** (1) validate — 4-byte aligned, `[uaddr, uaddr+4)` inside the
caller's own region (misaligned → `-EINVAL`, outside → `-EFAULT`). (2) Under `proc_lock`: 4-byte
load through the caller's live per-CPU user mapping (same class as select's masks); `word != val` →
`-EAGAIN`. (3) Match: `timeout_ms == 0` → immediate `-ETIMEDOUT` (compare-only); else record `{tgid,
uaddr}`, `state = PROC_STATE_FUTEX`, `wake_ms = now + timeout_ms` (`< 0` → 0 = infinite), set
`tf->regs[0] = 0`, release, `schedule(tf, 0)`. (4) The resume value rides the parked thread's SAVED
context (waitpid `context[0]` pattern): wake → 0; timeout → `-ETIMEDOUT`.

**WAKE(uaddr, val):** (1) validate as above; `val < 0` → `-EINVAL`; `val == 0` → 0. (2) Under
`proc_lock`, scan for `PROC_STATE_FUTEX && tgid == caller && futex_uaddr == uaddr`; up to `val` of
them (INT_MAX = all): clear record, saved `context[0] = 0`, READY. Return count. (3) Never
dereferences `uaddr`.

Bounds (keep this wording in the kernel header). **Process-private**: match includes `tgid`; two
processes futexing the same VA never interact (shared futexes arrive with P2, memfd/MAP_SHARED).
**No lost wakeups** for "release-store the word, then WAKE": compare+enqueue and scan+deliver both
sit under `proc_lock`; a WAKE scan that precedes the enqueue means the WAIT compare runs after the
lock chain, sees the store, and returns `-EAGAIN` (never parks); WAKE-before-store, timeout-vs-WAKE
races and a dying thread's wake are no-wakeup outcomes, not losses. **No spurious success**: only
`futex_wake` (and group teardown, which kills) changes FUTEX waiters; `process_wake_all()` skips the
state. **Timeout granularity**: 10 ms tick, overshoot < 1 tick, never early
(`process_check_sleeping` gains: FUTEX + expired → `-ETIMEDOUT`, clear, READY; a wake/timeout race
goes to whoever holds `proc_lock` first). Fixed per-PCB state, O(64) scans; no allocation, no new
lock.

## 6. pthread layer mapping outline (P1.3; L3 — `src/libc/pthread.c`)
- `pthread_t` = TCB* (musl model): `{ int tid; void *tls; start, arg, retval; int detach; uint64_t
  exit_word; struct tcb *dead_next; stack_base, stack_size }`. `pthread_self` reads the TLS
  register; `pthread_equal` compares pointers.
- **create**: drain dead list; alloc TCB + TLS + stack (default 256 KiB, attr overrides; guard pages
  deferred to P2 — overflow faults the process); init; publish; `SYS_THREAD_CREATE(trampoline, tcb,
  stack_top16, 0)`; store tid (failure → EAGAIN/ENOMEM). **trampoline** (asm shim):
  `SYS_SET_TLS(tcb->tls)`; `start(arg)`; `pthread_exit(retval)`.
- **exit**: store retval; release-store `tid = 0`; `futex_wake`; `SYS_THREAD_EXIT`; if detached →
  push to dead list (the deferred reaper avoids freeing one's own stack in place). **join**: self →
  EDEADLK; detached → EINVAL; while `tid != 0` futex_wait on it; read retval; free TCB+stack. Second
  join undefined (documented). **detach**: set flag; if already exited free now, else at exit.
- **mutex**: 3-state futex word (0/1/2); CAS lock; contended sets 2 + waits; unlock stores 0 and
  wakes 1 if prev==2; recursive = owner+count over the same word; static init = zeros.
- **cond**: seq counter + futex (no requeue): wait = snapshot, futex_wait while unchanged; signal =
  seq++, wake(1); broadcast = seq++, wake(INT_MAX); lost-signal-safe by snapshot-before-park.
  **once**: 0/1/2 CAS + wake-all. **rwlock**: reader CAS unless writer bit; read-mostly correctness
  over fairness (documented window). **barrier**: count + generation flip by the last arriver.
- **keys**: libc-global table (128), values in TCB, destructors at exit (bounded). **attr**:
  detachstate + stacksize, rest ENOTSUP. `sched_yield` → SYS_YIELD. v1 window: detach+exit before
  the creator's create returns is freed only via the dead list — UB-classed (POSIX: pthread_t
  invalid until create returns).

## 7. Test plan
`THRD_T.BIN` (`src/user/thrd_test.c`; loaded LATE — threads consume PCB slots; ≤ 8 concurrent; both
arches):
1. 8 threads bump a shared counter under a mutex → exact total; a second shard with `__atomic_*` (no
   lock) → exact total (torn-update detector).
2. Join correctness: before- and after-exit joins return each thread's own retval; detached side
   effect observed without join.
3. Mutex/cond ping-pong: 2 threads × 1000 rounds under a 2 s watchdog (timeout → FAIL, so lost
   wakeups fail deterministically).
4. `__atomic_*` contention: fetch_add/CAS/exchange across 4 threads == expected; a C11 spinlock
   plus a direct FUTEX WAIT/WAKE pair, incl. the 50 ms timeout case (ETIMEDOUT, elapsed ≥ 40 ms)
   and the word-compare EAGAIN case.
5. TLS (P1.4, `TLS_T.BIN` from `src/user/tls_test.c`): 2 threads × distinct `__thread` values
   under forced preemption, stable over 1000 iterations; TLS register values differ;
   pthread_self distinct. THRD_T re-checks a 2-thread TLS case inline.
6. Extras: FP registers inside a fresh thread start clean and survive preemption (mini FPU_T in a
   thread, guarding init-at-create); the same file builds under `-DHOST_TEST` against glibc
   pthreads to validate the test logic itself.

Kernel unit additions (`process_test.c`, registered in `unit_test.c`): thread-create slot
accounting; the futex record machine (compare/queue/wake-match/timeout, cleared on teardown);
THREAD_DONE reclamation gating. Regression guards (green both arches before merge): `make
host_tests` (F1 baseline 482/0), `./run_unit_tests.sh` (48) + `./run_unit_tests_intel.sh` (50), full
in-OS waves (FPU_T, PROCTEST, POLLTST/SOCK2TST, STRESS, DNSTST), QMP E2E (`run_xcalc_test.py`).
Evidence per §8.4: raw logs, zero FAIL tokens, boot-time delta recorded like Gate F1.
Makefile/disk/wave diffs: lane proposes, integrator applies (§7.3 F6).

## 8. Decisions & open questions for integrator review
**Decisions**
- **D1** Threads are PCB slots sharing the leader's AS (no thread table, no scheduler rewrite); same
  64-slot table.
- **D2** §A.1b amendment: 72 CREATE, 73 FUTEX, 74 THREAD_EXIT, 75 SET_TLS; P4+ rows shift +2;
  `SYS_MAX 75` (the P1 gate's freeze rows).
- **D3** join/detach user-space (TCB + futex). Rejected SYS_THREAD_JOIN (+zombie slots + detach
  flag): join state in the kernel for no isolation gain, +2 rows, and Linux auto-reaps exited
  threads in its kernel — the userspace model matches.
- **D4** SYS_EXIT keeps exit_group semantics from any thread; pthread_exit uses 74.
- **D5** `PROC_STATE_FUTEX`/`PROC_STATE_THREAD_DONE` are separate states so existing wake/sleep/reap
  paths stay precise.
- **D6** TLS register rides the switch as `tls_base`; layouts per probe; no FSGSBASE.
- **D7** Shared state routes via `process_group()`; fs.c/vfs.c/fat16.c edits as diff requests to
  their owners.
- **D8** No per-thread kernel stacks for user threads; kernel-preemption rules unchanged.
- **D9** Futex process-private, Linux op numbering; the WAIT compare is the kernel's only user-word
  read.

**Open questions**
- **OQ1** §A.1b renumber consent (P4 rows +2) — fallback: append 74/75 at the end of the provisional
  block.
- **OQ2** Ownership routing for fs.c (~36) and vfs.c/fat16.c (5) thread-routing edits: L1 diff
  request vs integrator-applied.
- **OQ3** exit_group waits on a wedged sibling claim with no progress guarantee beyond existing
  watchdogs — accept for P1, or extend LOSTWAKE-style reclamation to `THREAD_DONE`.
- **OQ4** THRD_T budget (≤8 threads, late position); revisit the 64-slot ceiling for WebKit at
  P4/P5.
- **OQ5** fork+threads: child is a single-threaded copy of the CALLER (heap past the copied window
  and heap TLS blocks not carried) — confirm documenting the divergence; atfork later. **OQ6** Guard
  pages/overflow deferred to P2 (P1 stacks are heap blocks; overflow faults the process).
