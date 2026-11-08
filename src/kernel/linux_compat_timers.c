/*
 * src/kernel/linux_compat_timers.c — the Linux timer family as real kernel
 * state: setitimer/getitimer/alarm, POSIX timer_create..timer_delete, and
 * timerfd.
 *
 * WHY THIS FILE (docs/rimworld-launch-log.md §35-§36)
 * ---------------------------------------------------
 * The guest had NO implementation of any of these: getitimer(36) was in no
 * syscall table and setitimer(38), alarm(37), timer_create(222..226) and
 * timerfd(283/286/287) were not even named in linux_syscall.h, so every one of
 * them answered ENOSYS.  A managed runtime that builds a Task continuation or
 * a System.Threading.Timer on a timer would hang with its thread pool idle —
 * exactly the shape of the RimWorld loader stall — so "the timer is never
 * armed because the syscall does not exist" had to be excluded by measurement
 * (src/user/linux_probe_timers.c) instead of by reasoning.
 *
 * DESIGN
 * ------
 * One small fixed table, one 100 Hz tick (arch/x64/timer.c's timer_reload,
 * CPU0 only), and no per-timer thread: an expiry either
 *   - raises the configured real-time signal on the owning process group
 *     (ITIMER_REAL / POSIX SIGEV_SIGNAL), through the same process_signal()
 *     that kill(2) uses, or
 *   - bumps a timerfd's expiration counter, which makes the fd readable for
 *     read(2)/poll(2)/select(2).
 * A timerfd read returns the accumulated u64 count and resets it, as Linux
 * does; a disarmed timerfd blocks until it is armed (a timed park: the reader
 * is woken at the timer's next expiry, and re-checks).
 *
 * WHAT IS DELIBERATELY NOT HERE
 * -----------------------------
 * ITIMER_VIRTUAL/ITIMER_PROF (CPU-time timers: this kernel has no per-process
 * CPU-time accounting to charge them against), SIGEV_THREAD (needs a thread
 * created by the kernel on the caller's behalf), and timer overrun accounting
 * beyond "how many expiries have not been consumed".  Each answers -EINVAL /
 * reports 0 honestly rather than pretending.
 */

#include "linux_compat.h"

#include <stdint.h>

#include "arch/trap.h"
#include "errno.h"
#include "fs.h"
#include "kline.h"
#include "linux_syscall.h"
#include "process.h"

#define LT_MAX 48

/* Same contract as every other l_tab_* handler: write the result and say
   "answered" (linux_compat_fd.c/linux_compat_mem.c each carry this three-line
   helper privately). */
static int l_ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

/* SIGALRM is Linux 14; process.h names only the signals the native engine
   uses, so the number lives here (it is the Linux ABI's, not ours). */
#define L_SIGALRM 14

#define LT_ITIMER 0
#define LT_POSIX 1
#define LT_TFD 2

struct l_tmr {
  int used;
  int kind;
  int pid;      /* owning process group (leader pid) */
  int sig;      /* signal to raise, 0 = none */
  int slot;     /* timerfd: which fd-slot record this is (unused: kind identity) */
  uint64_t next;      /* absolute monotonic ms, 0 = disarmed */
  uint64_t interval;  /* ms, 0 = one-shot */
  uint64_t expir;     /* expirations not yet consumed (timerfd) */
  int32_t clockid;
};

static struct l_tmr lt[LT_MAX];
static spinlock_t lt_lock;

/* kb_timespec / kb_timeval in the guest's layout. */
struct lt_timespec {
  int64_t sec;
  int64_t nsec;
};
struct lt_itimerspec {
  struct lt_timespec interval;
  struct lt_timespec value;
};
struct lt_timeval {
  int64_t sec;
  int64_t usec;
};
struct lt_itimerval {
  struct lt_timeval interval;
  struct lt_timeval value;
};

static int64_t ts_to_ms(const struct lt_timespec *ts) {
  if (ts->sec < 0 || ts->nsec < 0 || ts->nsec >= 1000000000LL) return -1;
  return ts->sec * 1000 + ts->nsec / 1000000;
}

static int64_t tv_to_ms(const struct lt_timeval *tv) {
  if (tv->sec < 0 || tv->usec < 0 || tv->usec >= 1000000LL) return -1;
  return tv->sec * 1000 + tv->usec / 1000;
}

static void ms_to_timespec(uint64_t ms, struct lt_timespec *ts) {
  ts->sec = (int64_t)(ms / 1000);
  ts->nsec = (int64_t)(ms % 1000) * 1000000;
}

static void ms_to_timeval(uint64_t ms, struct lt_timeval *tv) {
  tv->sec = (int64_t)(ms / 1000);
  tv->usec = (int64_t)(ms % 1000) * 1000;
}

static int lt_alloc(int kind, int pid, int sig) {
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  int i;
  for (i = 0; i < LT_MAX; i++)
    if (!lt[i].used) break;
  if (i == LT_MAX) {
    spinlock_release_irqrestore(&lt_lock, f);
    return -1;
  }
  lt[i].used = 1;
  lt[i].kind = kind;
  lt[i].pid = pid;
  lt[i].sig = sig;
  lt[i].next = 0;
  lt[i].interval = 0;
  lt[i].expir = 0;
  spinlock_release_irqrestore(&lt_lock, f);
  return i;
}

static void lt_free(int i) {
  if (i < 0 || i >= LT_MAX) return;
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  lt[i].used = 0;
  lt[i].next = 0;
  spinlock_release_irqrestore(&lt_lock, f);
}

static void lt_arm(int i, uint64_t next_ms, uint64_t interval_ms) {
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  lt[i].next = next_ms;
  lt[i].interval = interval_ms;
  spinlock_release_irqrestore(&lt_lock, f);
}

/* The tick.  Called from the PIT interrupt on CPU 0 (arch/x64/timer.c), i.e.
   once per 10 ms, with interrupts off; everything below is lock-only and
   allocation-free. */
void linux_compat_timer_tick(uint64_t now) {
  for (int i = 0; i < LT_MAX; i++) {
    if (!lt[i].used || !lt[i].next || now < lt[i].next) continue;
    int kind, pid, sig;
    uint64_t interval;
    {
      uint64_t f = spinlock_acquire_irqsave(&lt_lock);
      kind = lt[i].kind;
      pid = lt[i].pid;
      sig = lt[i].sig;
      interval = lt[i].interval;
      /* Advance BEFORE the signal so a handler that rearms cannot double-fire
         this tick, and so a slow consumer sees real overruns. */
      if (interval)
        lt[i].next += interval;
      else
        lt[i].next = 0;
      lt[i].expir++;
      spinlock_release_irqrestore(&lt_lock, f);
    }
    if (kind != LT_TFD && sig > 0) {
      extern int process_signal(int pid, int sig);
      (void)process_signal(pid, sig);
    }
  }
}

/* ---- fd side: a timerfd is a FILE_TYPE_TIMERFD global file ---------------- */

/* fs.c allocates the fd (the fd table and its helpers are private to it);
   this side owns the timer state.  Returns the slot index or -errno. */
int lt_timerfd_new(int clockid, int flags) {
  if (flags & ~(0x80000 /* TFD_CLOEXEC */ | 0x800 /* TFD_NONBLOCK */))
    return -EINVAL;
  struct process *grp = process_group(current_process());
  if (!grp) return -EINVAL;
  int slot = lt_alloc(LT_TFD, grp->pid, 0);
  if (slot < 0) return -ENOMEM;
  lt[slot].clockid = clockid;
  return slot;
}

/* Called by fs.c when the last reference to the file goes away. */
void lt_timerfd_release(int slot) { lt_free(slot); }

int lt_timerfd_settime(int slot, int flags, const struct lt_itimerspec *its) {
  extern uint64_t timer_get_ms(void);
  int64_t val = ts_to_ms(&its->value);
  int64_t itv = ts_to_ms(&its->interval);
  if (val < 0 || itv < 0) return -EINVAL;
  if (slot < 0 || slot >= LT_MAX || !lt[slot].used) return -EBADF;
  uint64_t next = 0;
  if (val > 0) {
    next = (flags & 1 /* TIMER_ABSTIME */) ? (uint64_t)val
                                           : timer_get_ms() + (uint64_t)val;
  }
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  lt[slot].next = next;
  lt[slot].interval = (uint64_t)itv;
  if (next) lt[slot].expir = 0; /* arming discards unread expiries, as Linux */
  spinlock_release_irqrestore(&lt_lock, f);
  return 0;
}

int lt_timerfd_gettime(int slot, struct lt_itimerspec *its) {
  extern uint64_t timer_get_ms(void);
  if (slot < 0 || slot >= LT_MAX || !lt[slot].used) return -EBADF;
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  uint64_t next = lt[slot].next, itv = lt[slot].interval;
  spinlock_release_irqrestore(&lt_lock, f);
  uint64_t now = timer_get_ms();
  ms_to_timespec(itv, &its->interval);
  ms_to_timespec(next && next > now ? next - now : 0, &its->value);
  return 0;
}

/* -EAGAIN when nothing has expired and the fd is non-blocking; the caller
   (file_read) decides how to park. */
int lt_timerfd_take(int slot, uint64_t *out) {
  if (slot < 0 || slot >= LT_MAX || !lt[slot].used) return -EBADF;
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  uint64_t n = lt[slot].expir;
  lt[slot].expir = 0;
  spinlock_release_irqrestore(&lt_lock, f);
  *out = n;
  return n ? 0 : -EAGAIN;
}

int lt_timerfd_pending(int slot) {
  if (slot < 0 || slot >= LT_MAX || !lt[slot].used) return 0;
  return lt[slot].expir != 0;
}

uint64_t lt_timerfd_next(int slot) {
  if (slot < 0 || slot >= LT_MAX || !lt[slot].used) return 0;
  return lt[slot].next;
}

/* ---- setitimer / getitimer / alarm -------------------------------------- */

#define L_ITIMER_REAL 0

static int lt_itimer_index(struct process *p) {
  /* one ITIMER_REAL per process group, synthesised as a table row whose kind
     is LT_ITIMER and whose pid is the group leader */
  for (int i = 0; i < LT_MAX; i++)
    if (lt[i].used && lt[i].kind == LT_ITIMER && lt[i].pid == p->pid) return i;
  return -1;
}

static int l_tmr_setitimer(struct process *p, struct trap_frame *tf) {
  int which = (int)l_argi(tf, 0);
  uint64_t unew = l_arg(tf, 1);
  uint64_t uold = l_arg(tf, 2);
  if (which != L_ITIMER_REAL) return l_ret(tf, -EINVAL); /* VIRTUAL/PROF: no CPU-time clock */
  struct process *grp = process_group(p);
  if (!grp) return l_ret(tf, -EINVAL);
  struct lt_itimerval nv;
  if (unew && l_copy_from_user(p, &nv, unew, sizeof nv) != 0)
    return l_ret(tf, -EFAULT);
  extern uint64_t timer_get_ms(void);
  int i = lt_itimer_index(grp);
  if (uold) {
    struct lt_itimerval ov;
    ov.interval.sec = ov.interval.usec = 0;
    ov.value.sec = ov.value.usec = 0;
    if (i >= 0) {
      uint64_t f = spinlock_acquire_irqsave(&lt_lock);
      uint64_t next = lt[i].next, itv = lt[i].interval;
      spinlock_release_irqrestore(&lt_lock, f);
      uint64_t now = timer_get_ms();
      ms_to_timeval(itv, &ov.interval);
      ms_to_timeval(next && next > now ? next - now : 0, &ov.value);
    }
    if (l_copy_to_user(p, uold, &ov, sizeof ov) != 0)
      return l_ret(tf, -EFAULT);
  }
  if (!unew) return l_ret(tf, 0);
  int64_t val = tv_to_ms(&nv.value);
  int64_t itv = tv_to_ms(&nv.interval);
  if (val < 0 || itv < 0) return l_ret(tf, -EINVAL);
  if (i < 0 && val > 0) {
    i = lt_alloc(LT_ITIMER, grp->pid, L_SIGALRM);
    if (i < 0) return l_ret(tf, -ENOMEM);
  }
  if (i >= 0) {
    uint64_t next = val > 0 ? timer_get_ms() + (uint64_t)val : 0;
    lt_arm(i, next, itv > 0 ? (uint64_t)itv : 0);
  }
  return l_ret(tf, 0);
}

static int l_tmr_getitimer(struct process *p, struct trap_frame *tf) {
  int which = (int)l_argi(tf, 0);
  uint64_t uold = l_arg(tf, 1);
  if (which != L_ITIMER_REAL) return l_ret(tf, -EINVAL);
  if (!uold) return l_ret(tf, -EFAULT);
  struct process *grp = process_group(p);
  if (!grp) return l_ret(tf, -EINVAL);
  struct lt_itimerval ov;
  ov.interval.sec = ov.interval.usec = 0;
  ov.value.sec = ov.value.usec = 0;
  int i = lt_itimer_index(grp);
  if (i >= 0) {
    extern uint64_t timer_get_ms(void);
    uint64_t f = spinlock_acquire_irqsave(&lt_lock);
    uint64_t next = lt[i].next, itv = lt[i].interval;
    spinlock_release_irqrestore(&lt_lock, f);
    uint64_t now = timer_get_ms();
    ms_to_timeval(itv, &ov.interval);
    ms_to_timeval(next && next > now ? next - now : 0, &ov.value);
  }
  if (l_copy_to_user(p, uold, &ov, sizeof ov) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* alarm(2) IS setitimer(ITIMER_REAL) on Linux, and glibc builds it that way.
   It matters here beyond tidiness: rule 35's note records that a SIGALRM-based
   diagnostic never ran in this guest because alarm(2) was ENOSYS. */
static int l_tmr_alarm(struct process *p, struct trap_frame *tf) {
  unsigned secs = (unsigned)l_arg(tf, 0);
  struct process *grp = process_group(p);
  if (!grp) return l_ret(tf, -EINVAL);
  extern uint64_t timer_get_ms(void);
  unsigned prev = 0;
  int i = lt_itimer_index(grp);
  if (i >= 0) {
    uint64_t f = spinlock_acquire_irqsave(&lt_lock);
    uint64_t next = lt[i].next;
    spinlock_release_irqrestore(&lt_lock, f);
    uint64_t now = timer_get_ms();
    if (next && next > now) prev = (unsigned)((next - now + 999) / 1000);
  }
  if (i < 0 && secs > 0) {
    i = lt_alloc(LT_ITIMER, grp->pid, L_SIGALRM);
    if (i < 0) return l_ret(tf, -ENOMEM);
  }
  if (i >= 0)
    lt_arm(i, secs ? timer_get_ms() + (uint64_t)secs * 1000 : 0, 0);
  return l_ret(tf, (int64_t)prev);
}

/* pause(2): park until a signal is delivered.  Implemented as an infinite
   timed block that the signal path interrupts (the same -EINTR contract every
   other park honours, rule 57). */
static int l_tmr_pause(struct process *p, struct trap_frame *tf) {
  (void)tf;
  if (!p) return 1;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  p->wake_ms = 0;
  p->state = PROC_STATE_BLOCKED;
  spinlock_release_irqrestore(&proc_lock, flags);
  return -2;
}

/* ---- POSIX timers ------------------------------------------------------- */

#define L_SIGEV_SIGNAL 0
#define L_SIGEV_NONE 1

static int l_tmr_create(struct process *p, struct trap_frame *tf) {
  int clockid = (int)l_argi(tf, 0);
  uint64_t usev = l_arg(tf, 1);
  uint64_t utid = l_arg(tf, 2);
  if (!utid) return l_ret(tf, -EFAULT);
  (void)clockid; /* every supported clock shares the monotonic ms base */
  int sig = 0;
  if (usev) {
    struct {
      uint64_t value;
      int32_t signo;
      int32_t notify;
      uint8_t pad[32];
    } sev;
    if (l_copy_from_user(p, &sev, usev, sizeof sev) != 0)
      return l_ret(tf, -EFAULT);
    if (sev.notify == L_SIGEV_SIGNAL)
      sig = sev.signo;
    else if (sev.notify == L_SIGEV_NONE)
      sig = 0;
    else
      return l_ret(tf, -EINVAL); /* SIGEV_THREAD / THREAD_ID: not supported */
    if (sig < 0 || sig >= HO_SIG_MAX) return l_ret(tf, -EINVAL);
  }
  struct process *grp = process_group(p);
  if (!grp) return l_ret(tf, -EINVAL);
  int i = lt_alloc(LT_POSIX, grp->pid, sig);
  if (i < 0) return l_ret(tf, -ENOMEM);
  int32_t handle = i + 1; /* timer_t is an opaque int here; 0 is never valid */
  if (l_copy_to_user(p, utid, &handle, sizeof handle) != 0) {
    lt_free(i);
    return l_ret(tf, -EFAULT);
  }
  return l_ret(tf, 0);
}

static int lt_posix_of(struct process *p, int64_t handle) {
  if (handle <= 0 || handle > LT_MAX) return -1;
  int i = (int)handle - 1;
  if (!lt[i].used || lt[i].kind != LT_POSIX) return -1;
  struct process *grp = process_group(p);
  if (!grp || lt[i].pid != grp->pid) return -1;
  return i;
}

static int l_tmr_settime(struct process *p, struct trap_frame *tf) {
  int64_t handle = l_argi(tf, 0);
  int flags = (int)l_argi(tf, 1);
  uint64_t unew = l_arg(tf, 2);
  uint64_t uold = l_arg(tf, 3);
  int i = lt_posix_of(p, handle);
  if (i < 0) return l_ret(tf, -EINVAL);
  struct lt_itimerspec nv;
  if (unew && l_copy_from_user(p, &nv, unew, sizeof nv) != 0)
    return l_ret(tf, -EFAULT);
  extern uint64_t timer_get_ms(void);
  if (uold) {
    struct lt_itimerspec ov;
    uint64_t f = spinlock_acquire_irqsave(&lt_lock);
    uint64_t next = lt[i].next, itv = lt[i].interval;
    spinlock_release_irqrestore(&lt_lock, f);
    uint64_t now = timer_get_ms();
    ms_to_timespec(itv, &ov.interval);
    ms_to_timespec(next && next > now ? next - now : 0, &ov.value);
    if (l_copy_to_user(p, uold, &ov, sizeof ov) != 0) return l_ret(tf, -EFAULT);
  }
  if (!unew) return l_ret(tf, 0);
  int64_t val = ts_to_ms(&nv.value);
  int64_t itv = ts_to_ms(&nv.interval);
  if (val < 0 || itv < 0) return l_ret(tf, -EINVAL);
  uint64_t next = 0;
  if (val > 0)
    next = (flags & 1) ? (uint64_t)val : timer_get_ms() + (uint64_t)val;
  lt_arm(i, next, itv > 0 ? (uint64_t)itv : 0);
  return l_ret(tf, 0);
}

static int l_tmr_gettime(struct process *p, struct trap_frame *tf) {
  int64_t handle = l_argi(tf, 0);
  uint64_t ucur = l_arg(tf, 1);
  int i = lt_posix_of(p, handle);
  if (i < 0) return l_ret(tf, -EINVAL);
  if (!ucur) return l_ret(tf, -EFAULT);
  extern uint64_t timer_get_ms(void);
  struct lt_itimerspec cv;
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  uint64_t next = lt[i].next, itv = lt[i].interval;
  spinlock_release_irqrestore(&lt_lock, f);
  uint64_t now = timer_get_ms();
  ms_to_timespec(itv, &cv.interval);
  ms_to_timespec(next && next > now ? next - now : 0, &cv.value);
  if (l_copy_to_user(p, ucur, &cv, sizeof cv) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

static int l_tmr_getoverrun(struct process *p, struct trap_frame *tf) {
  int64_t handle = l_argi(tf, 0);
  int i = lt_posix_of(p, handle);
  if (i < 0) return l_ret(tf, -EINVAL);
  uint64_t f = spinlock_acquire_irqsave(&lt_lock);
  uint64_t e = lt[i].expir;
  lt[i].expir = 0;
  spinlock_release_irqrestore(&lt_lock, f);
  return l_ret(tf, e ? (int64_t)(e - 1) : 0);
}

static int l_tmr_delete(struct process *p, struct trap_frame *tf) {
  int64_t handle = l_argi(tf, 0);
  int i = lt_posix_of(p, handle);
  if (i < 0) return l_ret(tf, -EINVAL);
  lt_free(i);
  return l_ret(tf, 0);
}

/* ---- epoll ---------------------------------------------------------------
 *
 * Not a timer, but the same question ("does a wait with a timeout behave")
 * and the same missing-syscall list: epoll_create/create1/ctl/wait/pwait all
 * answered ENOSYS.  This is a LEVEL-TRIGGERED implementation over the one
 * readiness engine the kernel already has (file_poll): every wake re-polls the
 * registered set, and a wait with no ready fd parks with a 10 ms slice until
 * its deadline.  That is enough for a timerfd/socket/pipe event loop and it
 * does not invent a second poller; EPOLLET/EPOLLONESHOT are accepted and
 * ignored (level-triggered is the documented fallback of last resort, and no
 * guest in this tree asks for edge semantics yet).
 */
#define LE_MAX 8
#define LE_FDS 32
struct l_epoll {
  int used;
  int pid;
  int n;
  uint64_t deadline; /* 0 = none; set on the first pass of a timed wait */
  struct {
    int fd;
    uint32_t events;
    uint64_t data;
  } e[LE_FDS];
};
static struct l_epoll le[LE_MAX];

int lt_epoll_new(void) {
  struct process *grp = process_group(current_process());
  if (!grp) return -EINVAL;
  for (int i = 0; i < LE_MAX; i++)
    if (!le[i].used) {
      le[i].used = 1;
      le[i].pid = grp->pid;
      le[i].n = 0;
      le[i].deadline = 0;
      return i;
    }
  return -ENOMEM;
}

void lt_epoll_release(int slot) {
  if (slot >= 0 && slot < LE_MAX) le[slot].used = 0;
}

static int lt_epoll_of(struct process *p, int fd) {
  extern int file_epoll_slot(struct process *p, int fd);
  return file_epoll_slot(p, fd);
}

#define L_EPOLL_ADD 1
#define L_EPOLL_DEL 2
#define L_EPOLL_MOD 3

static int l_tmr_epoll_create(struct process *p, struct trap_frame *tf) {
  extern int file_epoll_create(struct process *cur, int flags);
  (void)l_argi(tf, 0);
  return l_ret(tf, file_epoll_create(p, 0));
}

static int l_tmr_epoll_create1(struct process *p, struct trap_frame *tf) {
  extern int file_epoll_create(struct process *cur, int flags);
  return l_ret(tf, file_epoll_create(p, (int)l_argi(tf, 0)));
}

static int l_tmr_epoll_ctl(struct process *p, struct trap_frame *tf) {
  int epfd = (int)l_argi(tf, 0);
  int op = (int)l_argi(tf, 1);
  int fd = (int)l_argi(tf, 2);
  uint64_t uev = l_arg(tf, 3);
  int slot = lt_epoll_of(p, epfd);
  if (slot < 0) return l_ret(tf, -EINVAL);
  struct l_epoll *e = &le[slot];
  if (op == L_EPOLL_DEL) {
    for (int i = 0; i < e->n; i++)
      if (e->e[i].fd == fd) {
        e->e[i] = e->e[--e->n];
        return l_ret(tf, 0);
      }
    return l_ret(tf, -ENOENT);
  }
  if (op != L_EPOLL_ADD && op != L_EPOLL_MOD) return l_ret(tf, -EINVAL);
  struct {
    uint32_t events;
    uint64_t data;
  } __attribute__((packed)) ev;
  if (!uev) return l_ret(tf, -EFAULT);
  if (l_copy_from_user(p, &ev, uev, sizeof ev) != 0) return l_ret(tf, -EFAULT);
  int i;
  for (i = 0; i < e->n; i++)
    if (e->e[i].fd == fd) break;
  if (i == e->n) {
    if (op == L_EPOLL_MOD) return l_ret(tf, -ENOENT);
    if (e->n >= LE_FDS) return l_ret(tf, -ENOSPC);
    e->n++;
  }
  e->e[i].fd = fd;
  e->e[i].events = ev.events;
  e->e[i].data = ev.data;
  return l_ret(tf, 0);
}

static int lt_epoll_park(struct process *p, struct trap_frame *tf) {
  if (linux_compat_sig_deliverable(p)) return l_ret(tf, -EINTR);
#if defined(__x86_64__)
  if ((tf->cs & 3) == 3) tf->elr -= 2; /* rewind the x64 2-byte syscall */
#endif
  save_context(p, tf);
  schedule(tf, 0);
  return 1;
}

static int l_tmr_epoll_wait(struct process *p, struct trap_frame *tf) {
  int epfd = (int)l_argi(tf, 0);
  uint64_t uev = l_arg(tf, 1);
  int maxev = (int)l_argi(tf, 2);
  int timeout = (int)l_argi(tf, 3);
  extern uint64_t timer_get_ms(void);
  int slot = lt_epoll_of(p, epfd);
  if (slot < 0) return l_ret(tf, -EINVAL);
  if (maxev <= 0) return l_ret(tf, -EINVAL);
  struct l_epoll *e = &le[slot];
  struct k_pollfd pf[LE_FDS];
  int n = e->n;
  for (int i = 0; i < n; i++) {
    pf[i].fd = e->e[i].fd;
    pf[i].revents = 0;
    pf[i].events = 0;
    if (e->e[i].events & 0x1) pf[i].events |= 0x1; /* EPOLLIN  -> POLLIN  */
    if (e->e[i].events & 0x4) pf[i].events |= 0x4; /* EPOLLOUT -> POLLOUT */
  }
  int ready = n ? file_poll(p, pf, n, 0) : 0;
  if (ready > 0) {
    int out = 0;
    for (int i = 0; i < n && out < maxev; i++) {
      if (!pf[i].revents) continue;
      struct {
        uint32_t events;
        uint64_t data;
      } __attribute__((packed)) ev;
      ev.events = 0;
      if (pf[i].revents & 0x1) ev.events |= 0x1;
      if (pf[i].revents & 0x4) ev.events |= 0x4;
      if (pf[i].revents & 0x8) ev.events |= 0x8;  /* POLLERR  -> EPOLLERR  */
      if (pf[i].revents & 0x10) ev.events |= 0x10; /* POLLHUP -> EPOLLHUP  */
      ev.data = e->e[i].data;
      if (l_copy_to_user(p, uev + (uint64_t)out * sizeof ev, &ev, sizeof ev) != 0)
        return l_ret(tf, -EFAULT);
      out++;
    }
    e->deadline = 0;
    return l_ret(tf, out);
  }
  uint64_t now = timer_get_ms();
  if (timeout == 0) { e->deadline = 0; return l_ret(tf, 0); }
  if (e->deadline == 0 && timeout > 0) e->deadline = now + (uint64_t)timeout;
  if (e->deadline && now >= e->deadline) { e->deadline = 0; return l_ret(tf, 0); }
  uint64_t wake = now + 10;
  if (e->deadline && e->deadline < wake) wake = e->deadline;
  {
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    p->wake_ms = wake;
    p->state = PROC_STATE_BLOCKED;
    spinlock_release_irqrestore(&proc_lock, flags);
  }
  return lt_epoll_park(p, tf);
}

/* ---- timerfd syscalls --------------------------------------------------- */

static int l_tmr_tfd_create(struct process *p, struct trap_frame *tf) {
  extern int file_timerfd_create(struct process *cur, int clockid, int flags);
  return l_ret(tf, file_timerfd_create(p, (int)l_argi(tf, 0), (int)l_argi(tf, 1)));
}

static int lt_tfd_of(struct process *p, int fd) {
  extern int file_timerfd_slot(struct process *p, int fd);
  return file_timerfd_slot(p, fd);
}

static int l_tmr_tfd_settime(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int flags = (int)l_argi(tf, 1);
  uint64_t unew = l_arg(tf, 2);
  uint64_t uold = l_arg(tf, 3);
  int slot = lt_tfd_of(p, fd);
  if (slot < 0) return l_ret(tf, -EINVAL);
  struct lt_itimerspec nv;
  if (unew && l_copy_from_user(p, &nv, unew, sizeof nv) != 0)
    return l_ret(tf, -EFAULT);
  if (uold) {
    struct lt_itimerspec ov;
    if (lt_timerfd_gettime(slot, &ov) == 0 &&
        l_copy_to_user(p, uold, &ov, sizeof ov) != 0)
      return l_ret(tf, -EFAULT);
  }
  if (!unew) return l_ret(tf, 0);
  return l_ret(tf, lt_timerfd_settime(slot, flags, &nv));
}

static int l_tmr_tfd_gettime(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t ucur = l_arg(tf, 1);
  int slot = lt_tfd_of(p, fd);
  if (slot < 0) return l_ret(tf, -EINVAL);
  if (!ucur) return l_ret(tf, -EFAULT);
  struct lt_itimerspec cv;
  int r = lt_timerfd_gettime(slot, &cv);
  if (r != 0) return l_ret(tf, r);
  if (l_copy_to_user(p, ucur, &cv, sizeof cv) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

const struct l_sysent l_tab_tmr[] = {
  {L_SYS_pause, l_tmr_pause, "pause"},
  {L_SYS_alarm, l_tmr_alarm, "alarm"},
  {L_SYS_setitimer, l_tmr_setitimer, "setitimer"},
  {L_SYS_getitimer, l_tmr_getitimer, "getitimer"},
  {L_SYS_timer_create, l_tmr_create, "timer_create"},
  {L_SYS_timer_settime, l_tmr_settime, "timer_settime"},
  {L_SYS_timer_gettime, l_tmr_gettime, "timer_gettime"},
  {L_SYS_timer_getoverrun, l_tmr_getoverrun, "timer_getoverrun"},
  {L_SYS_timer_delete, l_tmr_delete, "timer_delete"},
  {L_SYS_epoll_create, l_tmr_epoll_create, "epoll_create"},
  {L_SYS_epoll_create1, l_tmr_epoll_create1, "epoll_create1"},
  {L_SYS_epoll_ctl, l_tmr_epoll_ctl, "epoll_ctl"},
  {L_SYS_epoll_wait, l_tmr_epoll_wait, "epoll_wait"},
  {L_SYS_epoll_pwait, l_tmr_epoll_wait, "epoll_pwait"},
  {L_SYS_timerfd_create, l_tmr_tfd_create, "timerfd_create"},
  {L_SYS_timerfd_settime, l_tmr_tfd_settime, "timerfd_settime"},
  {L_SYS_timerfd_gettime, l_tmr_tfd_gettime, "timerfd_gettime"},
  L_SYSENT_END
};
