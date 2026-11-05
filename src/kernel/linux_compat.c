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
     treat as "no wait"). */
  int64_t ms = ts[0] * 1000 + (ts[1] + 999999) / 1000000;
  if (ms < 0 || ts[0] > 100000000LL) ms = 100000000LL;
  *out_ms = ms;
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

static int l_core_set_tid(struct process *p, struct trap_frame *tf) {
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
    uint64_t ms = timer_get_ms();
    uint64_t out[2];
    out[0] = ms / 1000;
    out[1] = (ms % 1000) * 1000;
    int r = l_copy_to_user(p, tv, out, sizeof out);
    if (r != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return 1;
    }
  }
  tf->regs[0] = 0;
  return 1;
}

/* Block the caller for ms without spinning (the 30-second deadlock law: a
   compat syscall that blocks must deschedule, exactly like SYS_SLEEP does).
   tf->regs[0] is written BEFORE the switch: schedule(tf,0) saves this frame and
   never returns here (the process resumes through enter_user_space with the
   saved frame), so a value stored after the call is never what the guest sees.
   Measured: with the store after the call, nanosleep returned rax == 35 (the
   syscall number) and the probe read it as a positive error. */
static void l_block_ms(struct trap_frame *tf, int ms, int nr) {
  struct process *cur = current_process();
  tf->regs[0] = 0;
  if (cur && ms > 0) {
    /* M6/L33: Linux's nanosleep checks signal_pending() before it sleeps and
       answers -EINTR when a signal the thread would take is already pending.
       The handler runs at this syscall's trap exit, before user code sees the
       return. */
    if (linux_compat_sig_deliverable(cur)) {
      tf->regs[0] = (uint64_t)(int64_t)-EINTR;
      return;
    }
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    cur->wake_ms = timer_get_ms() + (uint64_t)ms;
    cur->sig_park_kind = 1; /* not rewound: a signal wake returns -EINTR */
    cur->sig_park_nr = nr;  /* identity for the SA_RESTART restart */
    cur->state = PROC_STATE_BLOCKED;
    spinlock_release_irqrestore(&proc_lock, flags);
    schedule(tf, 0);
  }
}

static int l_core_nanosleep(struct process *p, struct trap_frame *tf) {
  int64_t ms = 0;
  int r = l_timespec_to_ms(p, l_arg(tf, 0), &ms);
  if (r != 0) {
    tf->regs[0] = (uint64_t)(int64_t)r;
    return 1;
  }
  if (ms > 0) l_block_ms(tf, (int)ms, L_SYS_nanosleep);
  tf->regs[0] = 0;
  return 1;
}

/* clock_nanosleep(clockid, flags, const struct timespec *req, struct timespec *rem)
   TIMER_ABSTIME (1) is not honoured: nothing in the target programs uses it and
   silently sleeping the wrong duration is worse than saying so. */
static int l_core_clock_nanosleep(struct process *p, struct trap_frame *tf) {
  int64_t flags = l_argi(tf, 1);
  if (flags & 1) {
    tf->regs[0] = (uint64_t)(int64_t)-ENOTSUP;
    return 1;
  }
  int64_t ms = 0;
  int r = l_timespec_to_ms(p, l_arg(tf, 2), &ms);
  if (r != 0) {
    tf->regs[0] = (uint64_t)(int64_t)r;
    return 1;
  }
  if (ms > 0) l_block_ms(tf, (int)ms, L_SYS_clock_nanosleep);
  tf->regs[0] = 0;
  return 1;
}

static const struct l_sysent l_tab_core[] = { {L_SYS_gettimeofday, l_core_gettimeofday, "gettimeofday"}, {L_SYS_nanosleep, l_core_nanosleep, "nanosleep"}, {L_SYS_clock_nanosleep, l_core_clock_nanosleep, "clock_nanosleep"}, {L_SYS_getpid, l_core_getpid, "getpid"}, {L_SYS_getppid, l_core_getppid, "getppid"}, {L_SYS_getuid, l_core_getuid, "getuid"}, {L_SYS_getgid, l_core_getuid, "getgid"}, {L_SYS_geteuid, l_core_getuid, "geteuid"}, {L_SYS_getegid, l_core_getuid, "getegid"}, {L_SYS_gettid, l_core_gettid, "gettid"}, {L_SYS_set_tid_address, l_core_set_tid, "set_tid_address"}, {L_SYS_sched_yield, l_core_yield, "sched_yield"},
  L_SYSENT_END
};

/* ---- dispatcher --------------------------------------------------------- */

int linux_compat_dispatch(struct process *p, struct trap_frame *tf, uint64_t nr) {
  const struct l_sysent *const tables[] = {
    l_tab_core,
    (const struct l_sysent *)l_tab_fd,
    (const struct l_sysent *)l_tab_mem,
    (const struct l_sysent *)l_tab_proc,
    (const struct l_sysent *)l_tab_misc,
    (const struct l_sysent *)l_tab_thr,
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
          return (int)r;
        }
#endif
        return tab[i].fn(p, tf);
      }
    }
  }
  return 0;
}

/* ---- [LCOMPAT] tracer --------------------------------------------------- */

#define LCOMPAT_SEEN 64
#define LCOMPAT_REPORTS 96
static uint64_t lcompat_seen[LCOMPAT_SEEN];
static int lcompat_reports;

void linux_compat_unhandled(struct process *p, uint64_t nr, uint64_t rip) {
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
  kl_s(&kl, "\n");
  kl_emit(&kl);
}
