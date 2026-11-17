/*
 * src/kernel/linux_compat.c — the Linux x86_64 ABI compat dispatcher.
 *
 * The contract lives in src/include/linux_compat.h; docs/linux-abi.md is the
 * ABI reference.  This file owns exactly three things:
 *
 *   1. the dispatcher: Linux syscall number -> handler table (one table per
 *      subsystem file, weak, so lanes land independently);
 *   2. the [LCOMPAT] tracer: one console record per unhandled number, so the
 *      bring-up of a real Linux program is driven by evidence instead of
 *      guesswork ("it died somewhere in libc" is not a diagnosis);
 *   3. the user-memory copy helpers every handler shares.
 *
 * Plus l_tab_core: the handful of numbers that carry no policy at all
 * (identity, sleep, gettimeofday) and that musl's startup path calls before
 * anything else.  They are here rather than in a subsystem lane because every
 * lane needs them and duplicating them would be a silent divergence.
 *
 * Kernel constraints: freestanding, no libc, -mgeneral-regs-only, no FP/SIMD,
 * byte-wise copies only.
 */

#include "linux_compat.h"

#include <stdint.h>

#include "arch/trap.h"
#include "errno.h"
#include "kline.h"
#include "lock.h"
#include "linux_syscall.h"
#include "fat16.h"
#include "process.h"
#include "timer.h"
#include "vm.h"

/* Subsystem tables, defined by the lane that owns that file.  A missing file
   leaves the symbol undefined-and-weak: its address reads as 0 and the
   dispatcher skips it, so the tree builds and boots in any landing order. */
extern const struct l_sysent l_tab_fd[] __attribute__((weak));
extern const struct l_sysent l_tab_mem[] __attribute__((weak));
extern const struct l_sysent l_tab_proc[] __attribute__((weak));
extern const struct l_sysent l_tab_misc[] __attribute__((weak));
extern const struct l_sysent l_tab_thr[] __attribute__((weak));
extern const struct l_sysent l_tab_tmr[] __attribute__((weak));
extern const struct l_sysent l_tab_shm[] __attribute__((weak)); /* cp/x3 */
extern const struct l_sysent l_tab_cons[] __attribute__((weak)); /* C0-console probe stress */
/* cp/ext2 (src/kernel/ext2_compat.c): consulted FIRST, and its handlers may
   DECLINE (return 0) a number another table owns when the path/fd is not on
   the ext2 volume -- the search then continues below as if it were absent. */
extern const struct l_sysent l_tab_ext2[] __attribute__((weak));
/* cp/sys (src/kernel/tmpfs_compat.c): the compat /tmp.  Consulted before
   l_tab_ext2 with the same DECLINE contract: a row answers only for a path
   (or fd) on the tmpfs and returns 0 for everything else. */
extern const struct l_sysent l_tab_tmpfs[] __attribute__((weak));
/* cp/ext2b (src/kernel/vfs_compat.c): the generic setattr and file-lock rows
   (chmod/chown/utimensat families, flock, the fcntl lock commands).  Consulted
   right after l_tab_ext2 with the same decline protocol (return 0 = "not
   mine", e.g. a non-lock fcntl command). */
extern const struct l_sysent l_tab_vfsx[] __attribute__((weak));

/* ---- user-memory helpers ------------------------------------------------ */

int l_user_ok(struct process *p, uint64_t ptr, uint64_t len, int write) {
  if (!p || !p->as) return 0;
  if (len == 0) len = 1;
  return vm_range_ok(p, ptr, len, write) == 0;
}

int l_copy_from_user(struct process *p, void *kdst, uint64_t usrc,
                     uint64_t len) {
  if (len == 0) return 0;
  if (len > (uint64_t)(1 << 20)) return -EINVAL;
  if (!l_user_ok(p, usrc, len, 0)) return -EFAULT;
  return vm_user_read(p, usrc, kdst, (int)len) == 0 ? 0 : -EFAULT;
}

int l_copy_to_user(struct process *p, uint64_t udst, const void *ksrc,
                   uint64_t len) {
  if (len == 0) return 0;
  if (len > (uint64_t)(1 << 20)) return -EINVAL;
  if (!l_user_ok(p, udst, len, 1)) return -EFAULT;
  return vm_kwrite(p, udst, ksrc, (int)len) == 0 ? 0 : -EFAULT;
}

int l_copy_cstr(struct process *p, char *kdst, uint64_t usrc, uint64_t cap) {
  if (cap == 0) return -EINVAL;
  for (uint64_t i = 0; i < cap; i++) {
    char c;
    if (l_copy_from_user(p, &c, usrc + i, 1) != 0) return -EFAULT;
    kdst[i] = c;
    if (c == '\0') return 0;
  }
  kdst[cap - 1] = '\0';
  return -ENAMETOOLONG;
}

int l_timespec_to_ms(struct process *p, uint64_t uts, int64_t *out_ms) {
  int64_t ts[2]; /* tv_sec, tv_nsec */
  if (l_copy_from_user(p, ts, uts, sizeof ts) != 0) return -EFAULT;
  if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL) return -EINVAL;
  /* Saturate rather than overflow: a 1e9-second sleep is a bug in the caller,
     not a reason to wrap into a negative delay (which the scheduler would
     treat as "no wait").  L52: the bound must NOT be 1e8 seconds.  An
     ABSOLUTE CLOCK_REALTIME deadline is ~1.79e9 seconds (2026), so a 1e8 s
     bound clamped every realtime futex deadline to 1e8 ms = 1970-01-02;
     subtracting the current realtime then went negative, became 0, and every
     glibc sem_timedwait with a 5..60 s timeout returned ETIMEDOUT at once
     (Mono's pool workers never parked; each request created a new thread and
     paid the 10-thread/s creation cap).  The cap for a *relative* wait lives
     at the callers that turn this into a wake_ms. */
  int64_t ms = ts[0] * 1000 + (ts[1] + 999999) / 1000000;
  if (ms < 0 || ts[0] > 10000000000LL) ms = 100000000LL;
  *out_ms = ms;
  return 0;
}

/* C1-k6: the same contract in microseconds, rounded UP (a deadline is never
   early).  Relative waits saturate at L_US_FOREVER (27.8 h, the old cap). */
#define L_US_FOREVER 100000000000LL
int l_timespec_to_us(struct process *p, uint64_t uts, int64_t *out_us) {
  int64_t ts[2]; /* tv_sec, tv_nsec */
  if (l_copy_from_user(p, ts, uts, sizeof ts) != 0) return -EFAULT;
  if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL) return -EINVAL;
  /* An absolute CLOCK_REALTIME value is ~1.8e15 us: well inside int64, so
     only an absurd tv_sec saturates (callers cap relative waits). */
  if (ts[0] > 9000000000000LL) {
    *out_us = 9000000000000000000LL;
    return 0;
  }
  *out_us = ts[0] * 1000000LL + (ts[1] + 999) / 1000;
  return 0;
}

/* ---- l_tab_core --------------------------------------------------------- */

/* Linux getpid() is the TGID, not the calling thread's tid.  In this kernel a
 * thread IS a pid slot, so returning p->pid made a worker's getpid() differ
 * from the group leader's.  glibc depends on the equality: its SIGCANCEL
 * handler refuses to cancel unless si_pid == getpid() (nptl/pthread_cancel.c),
 * and user code that tags logs/state with getpid() expects one value per
 * process.  gettid() keeps the per-thread number. */
static int l_core_getpid(struct process *p, struct trap_frame *tf) {
  struct process *grp = p ? process_group(p) : 0;
  tf->regs[0] = grp ? (uint64_t)(int64_t)grp->pid : (uint64_t)(int64_t)(-1);
  return 1;
}

static int l_core_gettid(struct process *p, struct trap_frame *tf) {
  tf->regs[0] = p ? (uint64_t)(int64_t)p->pid : (uint64_t)(int64_t)(-1);
  return 1;
}

static int l_core_getppid(struct process *p, struct trap_frame *tf) {
  tf->regs[0] = p ? (uint64_t)(int64_t)p->parent_pid : 0;
  return 1;
}

static int l_core_getuid(struct process *p, struct trap_frame *tf) {
  (void)p;
  tf->regs[0] = 0;
  return 1;
}

/* set_tid_address(tidptr): C1-k6 (docs/cp-k6.md §3) -- the address is now
   KEPT as the thread's clear_child_tid, as Linux does, so the thread's exit
   zeroes it and futex-wakes a waiter (process_thread_exit, GB-021).  It was
   only answered.  musl passes &__thread_list_lock for the main thread: a
   main thread that pthread_exit()s holds that lock and relies on the kernel
   to release it, so the remaining threads could never exit or create a
   thread again (linux_probe_killgrp K7: the group was never reaped). */
static int l_core_set_tid(struct process *p, struct trap_frame *tf) {
  if (p) p->clear_child_tid = l_arg(tf, 0);
  tf->regs[0] = p ? (uint64_t)(int64_t)p->pid : 0;
  return 1;
}

static int l_core_yield(struct process *p, struct trap_frame *tf) {
  (void)p;
  tf->regs[0] = 0;
  return 1;
}

/* gettimeofday(timeval *tv, void *tz).  struct timeval on x86_64 is 16 bytes:
   tv_sec is a 64-bit long, tv_usec a 64-bit long.  The legacy arm of the switch
   wrote tv_sec as a 32-bit word, which also lands in the low half of tv_sec and
   leaves tv_usec untouched -- a caller that reads usec gets whatever was on its
   stack.  A real Linux program reads this every frame (Doom's I_GetTime), so
   the 64-bit form is the only acceptable one. */
static int l_core_gettimeofday(struct process *p, struct trap_frame *tf) {
  uint64_t tv = l_arg(tf, 0);
  if (tv != 0) {
    /* M6/L43: gettimeofday is CLOCK_REALTIME, not uptime.  It used to answer
       timer_get_ms(), which broke every caller that builds an ABSOLUTE
       deadline from it: Mono's mono_os_sem_timedwait() does
       `gettimeofday() + timeout_ms` and passes it to sem_timedwait(), glibc
       turns that into futex(FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME), and the
       kernel compares it against hb_clock_realtime_ms() -- so a 5..60 s
       threadpool park became "56 years in the past" and returned -ETIMEDOUT
       at once.  Measured: sem_mono_deadline elapsed_ms=0 in the guest vs 50
       on the host (tools/gate/t3_linux_probe.sh clock).  time(2) below already
       used the RTC clock; this makes the two agree, as Linux does. */
    extern uint64_t hb_clock_realtime_us(void);
    uint64_t us = hb_clock_realtime_us(); /* C1-k6: microseconds */
    uint64_t out[2];
    out[0] = us / 1000000;
    out[1] = us % 1000000;
    int r = l_copy_to_user(p, tv, out, sizeof out);
    if (r != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return 1;
    }
  }
  tf->regs[0] = 0;
  return 1;
}

/* Block the caller until `deadline_us` (timer_get_us base) without spinning
   (the 30-second deadlock law: a compat syscall that blocks must deschedule,
   exactly like SYS_SLEEP does).  tf->regs[0] is written BEFORE the switch:
   schedule(tf,0) saves this frame and never returns here (the process resumes
   through enter_user_space with the saved frame), so a value stored after the
   call is never what the guest sees.  Measured: with the store after the call,
   nanosleep returned rax == 35 (the syscall number) and the probe read it as a
   positive error.  C1-k6: the deadline is in microseconds and a per-CPU
   one-shot meets it (process.c hrt_cover_locked), so nanosleep(1 ms) costs
   ~1 ms, not the next 10 ms tick. */
static void l_block_until_us(struct trap_frame *tf, uint64_t deadline_us, int nr,
                             int rewind) {
  struct process *cur = current_process();
  if (!cur || deadline_us <= timer_get_us()) {
    tf->regs[0] = 0;
    return;
  }
  /* M6/L33: Linux's nanosleep checks signal_pending() before it sleeps and
     answers -EINTR when a signal the thread would take is already pending.
     The handler runs at this syscall's trap exit, before user code sees the
     return. */
  if (linux_compat_sig_deliverable(cur)) {
    tf->regs[0] = (uint64_t)(int64_t)-EINTR;
    return;
  }
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  if (cur->state == PROC_STATE_EXITED || cur->state == PROC_STATE_THREAD_DONE) {
    /* C1-thr: killed while inside the syscall; never re-park the dead. */
    spinlock_release_irqrestore(&proc_lock, flags);
    tf->regs[0] = (uint64_t)(int64_t)-EINTR;
    return;
  }
  proc_set_deadline_us(cur, deadline_us);
  if (rewind) {
    /* A slice of a longer wait (the CPU-time clock): the syscall re-runs on
       wake (rax is still its number) and re-checks its absolute target; a
       signal wake re-runs it too and the entry check above answers -EINTR
       (l_park's contract, kind 2). */
    cur->sig_park_kind = 2;
    cur->sig_park_nr = 0;
  } else {
    tf->regs[0] = 0;
    cur->sig_park_kind = 1; /* not rewound: a signal wake returns -EINTR */
    cur->sig_park_nr = nr;  /* identity for the SA_RESTART restart */
  }
  cur->state = PROC_STATE_BLOCKED;
  spinlock_release_irqrestore(&proc_lock, flags);
  if (rewind) {
#if defined(__x86_64__)
    if ((tf->cs & 3) == 3) tf->elr -= 2;
#else
    tf->elr -= 4;
#endif
    save_context(cur, tf);
  }
  schedule(tf, 0);
}

static int l_core_nanosleep(struct process *p, struct trap_frame *tf) {
  int64_t us = 0;
  int r = l_timespec_to_us(p, l_arg(tf, 0), &us);
  if (r != 0) {
    tf->regs[0] = (uint64_t)(int64_t)r;
    return 1;
  }
  if (us > L_US_FOREVER) us = L_US_FOREVER;
  if (us > 0) l_block_until_us(tf, timer_get_us() + (uint64_t)us, L_SYS_nanosleep, 0);
  else tf->regs[0] = 0;
  return 1;
}

/* C1-k6 (docs/cp-k6.md §2): clock_nanosleep(clockid, flags, req, rem) for
   every clock Linux sleeps on, relative and TIMER_ABSTIME:
     REALTIME(0), TAI(11), REALTIME_ALARM(8): the realtime clock;
     MONOTONIC(1), BOOTTIME(7), BOOTTIME_ALARM(9): timer_get_us (no suspend);
     PROCESS_CPUTIME_ID(2) and a process CPU clock of this group: the group's
       CPU time, met by parking in slices and re-checking.
   Linux has no nsleep for MONOTONIC_RAW(4), REALTIME_COARSE(5),
   MONOTONIC_COARSE(6) or THREAD_CPUTIME_ID(3): -EOPNOTSUPP (95).  Anything
   else is -EINVAL.  An absolute deadline already past returns 0 at once.  A
   relative REALTIME sleep does not follow clock_settime (there is none). */
#define L_CLK_REALTIME 0
#define L_CLK_MONOTONIC 1
#define L_CLK_PROCESS_CPUTIME 2
#define L_CLK_THREAD_CPUTIME 3
#define L_CLK_MONOTONIC_RAW 4
#define L_CLK_REALTIME_COARSE 5
#define L_CLK_MONOTONIC_COARSE 6
#define L_CLK_BOOTTIME 7
#define L_CLK_REALTIME_ALARM 8
#define L_CLK_BOOTTIME_ALARM 9
#define L_CLK_TAI 11
#define L_CPU_SLICE_US 1000
static int l_core_clock_nanosleep(struct process *p, struct trap_frame *tf) {
  extern uint64_t hb_clock_realtime_us(void);
  int clk = (int)l_argi(tf, 0);
  int64_t flags = l_argi(tf, 1);
  int abstime = (flags & 1) != 0; /* TIMER_ABSTIME; other bits are ignored */
  int kind; /* 0 realtime, 1 monotonic, 2 group CPU time */
  if (clk == L_CLK_REALTIME || clk == L_CLK_TAI || clk == L_CLK_REALTIME_ALARM)
    kind = 0;
  else if (clk == L_CLK_MONOTONIC || clk == L_CLK_BOOTTIME ||
           clk == L_CLK_BOOTTIME_ALARM)
    kind = 1;
  else if (clk == L_CLK_PROCESS_CPUTIME)
    kind = 2;
  else if (clk == L_CLK_THREAD_CPUTIME || clk == L_CLK_MONOTONIC_RAW ||
           clk == L_CLK_REALTIME_COARSE || clk == L_CLK_MONOTONIC_COARSE)
    kind = -95;
  else if (clk < 0) {
    /* A dynamic CPU clock: ~pid << 3 | type, type 0 PROF 1 VIRT 2 SCHED,
       bit 2 per-thread.  Linux: a per-thread clock has no nsleep (EINVAL
       for the caller's own thread, EOPNOTSUPP otherwise -- EINVAL here);
       a process clock sleeps on that process's CPU time.  Only this group
       is supported; another pid is EINVAL. */
    int type = clk & 7;
    int pid = ~(clk >> 3);
    struct process *g = process_group(p);
    if ((type & 3) == 3 || (type & 4)) kind = -22;
    else if (pid != 0 && (!g || pid != g->pid)) kind = -22;
    else kind = 2;
  } else
    kind = -22;
  if (kind < 0) {
    tf->regs[0] = (uint64_t)(int64_t)kind;
    return 1;
  }
  int64_t us = 0;
  int r = l_timespec_to_us(p, l_arg(tf, 2), &us);
  if (r != 0) {
    tf->regs[0] = (uint64_t)(int64_t)r;
    return 1;
  }
  uint64_t mono = timer_get_us();
  if (kind == 2) {
    /* CPU time: the target is absolute in group-CPU microseconds.  A
       relative request is anchored once per call (p->cns_cpu_*), keyed by
       the call site and the req pointer so a restart re-uses it. */
    struct ho_rusage ru;
    process_rusage(p, 0, &ru);
    uint64_t cpu_now = (uint64_t)(ru.ru_utime_sec + ru.ru_stime_sec) * 1000000ULL +
                       (uint64_t)(ru.ru_utime_usec + ru.ru_stime_usec);
    uint64_t target;
    if (abstime) {
      target = (uint64_t)us;
    } else if (p->cns_cpu_pc == p->sys_entry_pc && p->cns_cpu_req == l_arg(tf, 2) &&
               p->cns_cpu_target) {
      target = p->cns_cpu_target;
    } else {
      if (us > L_US_FOREVER) us = L_US_FOREVER;
      target = cpu_now + (uint64_t)us;
      p->cns_cpu_pc = p->sys_entry_pc;
      p->cns_cpu_req = l_arg(tf, 2);
      p->cns_cpu_target = target;
    }
    if (cpu_now >= target) {
      p->cns_cpu_target = 0;
      tf->regs[0] = 0;
      return 1;
    }
    uint64_t left = target - cpu_now;
    if (left > L_CPU_SLICE_US) left = L_CPU_SLICE_US;
    if (linux_compat_sig_deliverable(p)) p->cns_cpu_target = 0;
    l_block_until_us(tf, mono + left, L_SYS_clock_nanosleep, 1);
    return 1;
  }
  uint64_t deadline;
  if (!abstime) {
    if (us > L_US_FOREVER) us = L_US_FOREVER;
    deadline = mono + (uint64_t)us;
  } else if (kind == 1) {
    deadline = (uint64_t)us; /* CLOCK_MONOTONIC is timer_get_us (b8cc46c8) */
  } else {
    uint64_t rt = hb_clock_realtime_us();
    deadline = ((uint64_t)us > rt) ? mono + ((uint64_t)us - rt) : 0;
  }
  if (deadline > mono) {
    if (deadline - mono > (uint64_t)L_US_FOREVER) deadline = mono + L_US_FOREVER;
    l_block_until_us(tf, deadline, L_SYS_clock_nanosleep, 0);
  } else {
    tf->regs[0] = 0;
  }
  return 1;
}

/* time(2): `time_t time(time_t *tloc)` — seconds since the epoch, and the
   same value stored through tloc when it is not NULL.  Source of truth is the
   kernel's boot-anchored RTC clock (hb_clock_realtime_ms), so the answer
   matches gettimeofday's wall clock rather than uptime.  A write fault on
   tloc must still deliver the value (Linux's copy_to_user failure is ignored
   for time(2)?  no: it returns -EFAULT), so report -EFAULT without a value. */
static int l_core_time(struct process *p, struct trap_frame *tf) {
  extern uint64_t hb_clock_realtime_ms(void);
  uint64_t tloc = l_arg(tf, 0);
  uint64_t secs = hb_clock_realtime_ms() / 1000ULL;
  if (tloc != 0) {
    if (l_copy_to_user(p, tloc, &secs, sizeof secs) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return 1;
    }
  }
  tf->regs[0] = secs;
  return 1;
}

static const struct l_sysent l_tab_core[] = { {L_SYS_time, l_core_time, "time"}, {L_SYS_gettimeofday, l_core_gettimeofday, "gettimeofday"}, {L_SYS_nanosleep, l_core_nanosleep, "nanosleep"}, {L_SYS_clock_nanosleep, l_core_clock_nanosleep, "clock_nanosleep"}, {L_SYS_getpid, l_core_getpid, "getpid"}, {L_SYS_getppid, l_core_getppid, "getppid"}, {L_SYS_getuid, l_core_getuid, "getuid"}, {L_SYS_getgid, l_core_getuid, "getgid"}, {L_SYS_geteuid, l_core_getuid, "geteuid"}, {L_SYS_getegid, l_core_getuid, "getegid"}, {L_SYS_gettid, l_core_gettid, "gettid"}, {L_SYS_set_tid_address, l_core_set_tid, "set_tid_address"}, {L_SYS_sched_yield, l_core_yield, "sched_yield"},
  L_SYSENT_END
};

/* ---- dispatcher --------------------------------------------------------- */

/* M6/L36: the generic stack-parity skew detector.
 *
 * A syscall site (the instruction after `syscall`) is a fixed point in code:
 * a compiled function's frame puts RSP at a fixed distance from its entry, and
 * every caller maintains the psABI invariant, so the PARITY of %rsp at that
 * site is constant for the life of the process.  If the same (thread, site)
 * pair is ever seen with the other parity, something moved %rsp by 8 bytes --
 * exactly what Mono's trampoline check (`and $15; cmp $0`) dies on.  This
 * catches the moment of the skew, with no knowledge of any layout. */
struct l_skw_site {
  uint64_t rip;
  uint32_t pid;
  uint8_t par;
  uint8_t used;
};
static struct l_skw_site l_skw[512];
static int l_skw_rows;

static void l_skw_check(struct process *p, struct trap_frame *tf) {
  uint64_t rip = tf->elr;
  uint8_t par = (uint8_t)(tf->lr & 15);
  if (!rip)
    return;
  struct l_skw_site *slot = 0;
  for (int i = 0; i < 512; i++) {
    if (l_skw[i].used && l_skw[i].pid == (uint32_t)p->pid &&
        l_skw[i].rip == rip) {
      if (l_skw[i].par != par && l_skw_rows < 12) {
        l_skw_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[SKW] pid=");
        kl_i(&kl, p->pid);
        kl_s(&kl, " rip=0x");
        kl_hex(&kl, (unsigned long)rip);
        kl_s(&kl, " was=");
        kl_i(&kl, (int)l_skw[i].par);
        kl_s(&kl, " now=");
        kl_i(&kl, (int)par);
        kl_s(&kl, " rsp=0x");
        kl_hex(&kl, (unsigned long)tf->lr);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
      l_skw[i].par = par;
      return;
    }
    if (!l_skw[i].used && !slot)
      slot = &l_skw[i];
  }
  if (slot) {
    slot->used = 1;
    slot->pid = (uint32_t)p->pid;
    slot->rip = rip;
    slot->par = par;
  }
}

int linux_compat_dispatch(struct process *p, struct trap_frame *tf, uint64_t nr) {
  if (p && nr != L_SYS_rt_sigreturn)
    l_skw_check(p, tf);
  if (p) {
    /* C1-thr: a park's signal bookkeeping belongs to ONE syscall.  It used
       to outlive it: a nanosleep that ended by timeout left sig_park_kind=1
       and sig_park_nr=35, a later read(2) parked through the native -2
       restart, and the next SA_RESTART signal "restarted" that read as a
       nanosleep -- rewinding the already-rewound PC two more bytes into the
       middle of an instruction (measured: linux_probe_thr L-read, SIGSEGV at
       a PC on the thread's stack).  Each park sets its own kind. */
    p->sig_park_kind = 0;
    p->sig_park_nr = 0;
    p->sys_entry_pc = tf->elr;
    p->poll_waiting = 0; /* C1-k4: a poll registration belongs to one syscall */
  }
#ifdef RW_SC_RING
  /* L85: open a history slot BEFORE the handler runs, so a fault inside the
     handler (or in its copy_to_user/copy_from_user) still names the call and
     the arguments it was given; the return value is filled on the way out. */
  int sh_ix = -1;
  if (p) {
    sh_ix = p->sc_hist_at & (SC_HIST_N - 1);
    p->sc_hist[sh_ix].nr = (uint16_t)nr;
    p->sc_hist[sh_ix].done = 0;
    p->sc_hist[sh_ix].arg[0] = l_arg(tf, 0);
    p->sc_hist[sh_ix].arg[1] = l_arg(tf, 1);
    p->sc_hist[sh_ix].arg[2] = l_arg(tf, 2);
    p->sc_hist[sh_ix].arg[3] = l_arg(tf, 3);
    p->sc_hist[sh_ix].ret = 0;
    p->sc_hist[sh_ix].rip = tf->elr;
    p->sc_hist_at = (uint8_t)((sh_ix + 1) & (SC_HIST_N - 1));
  }
#define L_DISPATCH_RET(v)                                                     \
  do {                                                                        \
    if (sh_ix >= 0) {                                                         \
      p->sc_hist[sh_ix].ret = (int64_t)tf->regs[0];                           \
      p->sc_hist[sh_ix].done = 1;                                             \
    }                                                                         \
    /* FAT flushes land here, on the way OUT of a syscall: no filesystem lock */\
    /* is held at this point, so a device write can never run under fat_lock. */\
    fat16_flush_pending();                                                    \
    return (v);                                                               \
  } while (0)
#else
#define L_DISPATCH_RET(v) do { fat16_flush_pending(); return (v); } while (0)
#endif
  if (l_tab_tmpfs) {
    for (unsigned i = 0; l_tab_tmpfs[i].fn; i++) {
      if ((uint64_t)(int64_t)l_tab_tmpfs[i].nr == nr) {
        int _t = l_tab_tmpfs[i].fn(p, tf);
        if (_t) L_DISPATCH_RET(_t);
        break;
      }
    }
  }
  if (l_tab_ext2) {
    for (unsigned i = 0; l_tab_ext2[i].fn; i++) {
      if ((uint64_t)(int64_t)l_tab_ext2[i].nr == nr) {
        int _e = l_tab_ext2[i].fn(p, tf);
        if (_e) L_DISPATCH_RET(_e);
        break;
      }
    }
  }
  if (l_tab_vfsx) {
    for (unsigned i = 0; l_tab_vfsx[i].fn; i++) {
      if ((uint64_t)(int64_t)l_tab_vfsx[i].nr == nr) {
        int _e = l_tab_vfsx[i].fn(p, tf);
        if (_e) L_DISPATCH_RET(_e);
        break;
      }
    }
  }
  const struct l_sysent *const tables[] = {
    l_tab_core,
    (const struct l_sysent *)l_tab_fd,
    (const struct l_sysent *)l_tab_mem,
    (const struct l_sysent *)l_tab_proc,
    (const struct l_sysent *)l_tab_misc,
    (const struct l_sysent *)l_tab_thr,
    (const struct l_sysent *)l_tab_tmr,
    (const struct l_sysent *)l_tab_shm,
    (const struct l_sysent *)l_tab_cons,
  };
  for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++) {
    const struct l_sysent *tab = tables[t];
    if (!tab) continue;
    for (unsigned i = 0; tab[i].fn; i++) {
      if ((uint64_t)(int64_t)tab[i].nr == nr) {
#ifdef RW_TRACE_MEM
        /* The memory table is traced here, not in each handler: every handler
           has several exits and a second exit path would drift.  Arguments are
           read BEFORE the call (the handler writes regs[0]) and the return
           value is what the handler left in regs[0]. */
        if (tab == (const struct l_sysent *)l_tab_mem) {
          uint64_t a0 = l_arg(tf, 0), a1 = l_arg(tf, 1), a2 = l_arg(tf, 2);
          uint64_t a3 = l_arg(tf, 3), a4 = l_arg(tf, 4), a5 = l_arg(tf, 5);
          int64_t r = tab[i].fn(p, tf);
          l_mem_trace(tab[i].name, p, a0, a1, a2, a3, a4, a5,
                      (int64_t)tf->regs[0]);
          L_DISPATCH_RET((int)r);
        }
#endif
        int _r = tab[i].fn(p, tf);
        L_DISPATCH_RET(_r);
      }
    }
  }
  L_DISPATCH_RET(0);
#undef L_DISPATCH_RET
}

/* ---- [LCOMPAT] tracer --------------------------------------------------- */

#define LCOMPAT_SEEN 64
#define LCOMPAT_REPORTS 96
static uint64_t lcompat_seen[LCOMPAT_SEEN];
static int lcompat_reports;

void linux_compat_unhandled(struct process *p, uint64_t nr, uint64_t rip,
                            uint64_t rdi) {
  int slot = (int)(nr % LCOMPAT_SEEN);
  if (lcompat_seen[slot] == nr + 1) return; /* already reported on this boot */
  lcompat_seen[slot] = nr + 1;
  if (lcompat_reports >= LCOMPAT_REPORTS) return;
  lcompat_reports++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[LCOMPAT] ENOSYS nr=");
  kl_i(&kl, (long)nr);
  kl_s(&kl, " name=");
  kl_s(&kl, p && p->name[0] ? p->name : "?");
  kl_s(&kl, " pid=");
  kl_i(&kl, p ? (long)p->pid : -1);
  kl_s(&kl, " rip=");
  kl_hex(&kl, (unsigned long)rip);
  /* GB-199: the first unhandled syscall of an entered ld.so is the named stop;
     rdi is the argument that says which path/resource it asked for. */
  kl_s(&kl, " rdi=");
  kl_hex(&kl, (unsigned long)rdi);
  kl_s(&kl, "\n");
  kl_emit(&kl);
}
