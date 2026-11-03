/*
 * src/kernel/linux_compat_thr.c — Linux-ABI threads, futex and signal frames.
 *
 * WHY A SEPARATE TABLE
 * --------------------
 * Everything in this file is about the ONE thing the compat track could not
 * do before M2: a Linux program with more than one thread of control.  The
 * measured target makes 1,095,446 futex calls in 150 s (69 % of all syscalls,
 * 97 % of syscall time) and runs 106 threads at the menu, so:
 *
 *   - clone(56) must produce a real thread: same address space, copied trap
 *     frame, rax=0, rsp=child_stack, its own FS base.  The native
 *     process_clone() already does exactly that; this file validates the
 *     Linux flag word and refuses what the kernel cannot express.
 *   - futex(202) must be a wait QUEUE keyed by (address space, uaddr), not a
 *     scan of the process table per wake and never a tick poll.
 *   - a fault (SIGSEGV/SIGBUS/SIGILL/SIGFPE) must reach the faulting
 *     THREAD's handler through a real x86-64 rt_sigframe, because glibc's
 *     restorer is `mov $15,%rax; syscall` and the handler reads a Linux
 *     ucontext_t -- the native HOS3 frame is a different ABI.
 *
 * The native dispatch is untouched: this is a l_sysent table like every other
 * compat subsystem, plus four weak hooks that only run for
 * PROC_FLAG_LINUX_ABI processes (process.c asks for them).
 *
 * Concurrency contract: every waiter is a PCB parked in PROC_STATE_FUTEX with
 * p->futex_uaddr set.  That is the same state the native futex uses, which is
 * deliberate -- clear_child_tid wakeups, the tick timeout path in
 * process_check_sleeping() and this queue all agree without a second state
 * machine.  The queue below is an INDEX (so a wake touches only the waiters of
 * one word, and only those whose bitset matches); the authoritative parked
 * state stays in the PCB.
 */
#include "linux_compat.h"

#include <stdint.h>

#include "arch/cpu.h"
#include "arch/trap.h"
#include "errno.h"
#include "kline.h"
#include "linux_syscall.h"
#include "lock.h"
#include "process.h"
#include "timer.h"
#include "vm.h"

/* ---- futex constants (Linux uapi) -------------------------------------- */

#define L_FUTEX_WAIT 0
#define L_FUTEX_WAKE 1
#define L_FUTEX_REQUEUE 3
#define L_FUTEX_CMP_REQUEUE 4
#define L_FUTEX_WAKE_OP 5
#define L_FUTEX_LOCK_PI 6
#define L_FUTEX_UNLOCK_PI 7
#define L_FUTEX_TRYLOCK_PI 8
#define L_FUTEX_WAIT_BITSET 9
#define L_FUTEX_WAKE_BITSET 10
#define L_FUTEX_WAIT_REQUEUE_PI 11
#define L_FUTEX_PRIVATE_FLAG 128
#define L_FUTEX_CLOCK_REALTIME 256
#define L_FUTEX_CMD_MASK (~(L_FUTEX_PRIVATE_FLAG | L_FUTEX_CLOCK_REALTIME))
#define L_FUTEX_BITSET_MATCH_ANY 0xffffffffu

/* Futex word bits (Linux: the low 30 bits are the owner TID). */
#define L_FUTEX_TID_MASK 0x3fffffffu
#define L_FUTEX_WAITERS 0x80000000u
#define L_FUTEX_OWNER_DIED 0x40000000u

#define L_ROBUST_LIST_LIMIT 2048

/* ---- per-thread compat state -------------------------------------------
 *
 * Keyed by pid (a Linux thread IS a pid here), so no process.h change is
 * needed.  Slots are allocated lazily by the first call that needs one and
 * released by linux_compat_thread_exit().
 *
 * THE WAIT QUEUE IS THIS TABLE, NOT A HASH CHAIN.  With MAX_PROCESSES == 64 a
 * wake costs at most 64 comparisons of (as, uaddr) -- about 50 ns -- while a
 * bucket index over the same 64 slots has to keep a chain consistent across
 * enqueue/dequeue/requeue/timeout/exit.  The first cut did that and the chain
 * went stale (measured: a wake found 0 waiters while a thread was parked on
 * the exact word, i.e. a lost wakeup).  The table cannot go stale: the only
 * queue state is (as, uaddr, bitset) on the slot and futex_uaddr on the PCB,
 * and every match re-validates both against the PCB state under proc_lock.
 */
struct l_thr_slot {
  int used;
  struct process *owner;    /* == &proc_table[pid]; identity check */
  struct addr_space *as;    /* futex key half 1 */
  uint64_t uaddr;           /* futex key half 2 (0 when not queued) */
  uint32_t bitset;          /* FUTEX_WAIT_BITSET mask */
  uint64_t robust_head;     /* set_robust_list(head, 24) */
  uint32_t waits;           /* diagnostics: parks / wakes */
  uint32_t wakes;
};

static struct l_thr_slot l_thr[MAX_PROCESSES];

static int l_thr_slot_of(struct process *p, int create) {
  if (!p || p->pid <= 0 || p->pid >= MAX_PROCESSES)
    return -1;
  int i = p->pid;
  struct l_thr_slot *s = &l_thr[i];
  if (!s->used) {
    if (!create)
      return -1;
    s->used = 1;
    s->owner = p;
    s->as = 0;
    s->uaddr = 0;
    s->bitset = 0;
    s->robust_head = 0;
    s->waits = 0;
    s->wakes = 0;
  }
  return i;
}

/* Is slot `i` parked on (as,uaddr) right now?  Caller holds proc_lock. */
static int l_futex_queued_locked(int i, struct addr_space *as,
                                 uint64_t uaddr) {
  struct l_thr_slot *s = &l_thr[i];
  struct process *p = s->owner;
  return s->used && p && s->uaddr == uaddr && s->as == as &&
         p->state == PROC_STATE_FUTEX && p->futex_uaddr == uaddr;
}

/* Wake up to `val` waiters on (as,uaddr) whose bitset intersects `bitset`.
   Caller holds proc_lock.  Returns the number woken. */
static int l_futex_wake_locked(struct addr_space *as, uint64_t uaddr,
                               uint32_t val, uint32_t bitset) {
  if (val == 0)
    return 0;
  int woke = 0;
  for (int i = 1; i < MAX_PROCESSES && (uint32_t)woke < val; i++) {
    struct l_thr_slot *s = &l_thr[i];
    if (!l_futex_queued_locked(i, as, uaddr))
      continue;
    if ((s->bitset & bitset) == 0)
      continue;
    struct process *p = s->owner;
    s->uaddr = 0;
    s->wakes++;
    p->futex_uaddr = 0;
    p->wake_ms = 0;
    p->context[0] = 0; /* WAIT returns 0 on wake */
    p->state = PROC_STATE_READY;
    woke++;
  }
  return woke;
}

/* Move up to `nr` waiters from key1 to key2.  Caller holds proc_lock. */
static int l_futex_requeue_locked(struct addr_space *as, uint64_t uaddr1,
                                  uint64_t uaddr2, uint32_t nr) {
  int moved = 0;
  for (int i = 1; i < MAX_PROCESSES && (uint32_t)moved < nr; i++) {
    if (!l_futex_queued_locked(i, as, uaddr1))
      continue;
    l_thr[i].uaddr = uaddr2;
    l_thr[i].owner->futex_uaddr = uaddr2;
    moved++;
  }
  return moved;
}

/* ---- fork-free thread creation ----------------------------------------- */

static int l_thr_clone(struct process *p, struct trap_frame *tf) {
  uint64_t flags = l_arg(tf, 0);
  uint64_t stack = l_arg(tf, 1);
  uint64_t ptid = l_arg(tf, 2);
  uint64_t ctid = l_arg(tf, 3);
  uint64_t tls = l_arg(tf, 4); /* r8 -- Linux's 5th clone argument */

  /* process_clone() owns the accepted mask (CLONE_VM|FS|FILES|SIGHAND|
     THREAD|SYSVSEM|SETTLS|PARENT_SETTID|CHILD_SETTID|CHILD_CLEARTID|
     DETACHED) and refuses namespaces, a new AS and a new fd table.  Do the
     cheap shape check here so the refusal is attributable in the log. */
  if ((flags & (CLONE_VM | CLONE_THREAD)) != (CLONE_VM | CLONE_THREAD)) {
    static int fork_reports;
    if (fork_reports < 4) {
      fork_reports++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] clone refused flags=0x");
      kl_hex(&kl, (unsigned long)flags);
      kl_s(&kl, " (no CLONE_VM|CLONE_THREAD: no fork/COW in this kernel)\n");
      kl_emit(&kl);
    }
    tf->regs[0] = (uint64_t)(int64_t)-ENOSYS;
    return 1;
  }
  long r = (long)process_clone(p, tf, flags, stack, ptid, ctid, tls);
  {
    static int clone_reports;
    if (clone_reports < 5) {
      clone_reports++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] clone flags=");
      kl_hex(&kl, (unsigned long)flags);
      kl_s(&kl, " stack=");
      kl_hex(&kl, stack);
      kl_s(&kl, " tls=");
      kl_hex(&kl, tls);
      kl_s(&kl, " -> ");
      kl_i(&kl, r);
      kl_s(&kl, "\n");
      kl_emit(&kl);
    }
  }
  tf->regs[0] = (uint64_t)(int64_t)r;
  return 1;
}

/* exit(60) is THREAD exit, not process exit: glibc's start_thread ends with
   __exit(0) after the routine returns, and a plain SYS_EXIT here would tear
   down the whole group on the first thread that finished.  (exit_group(231)
   keeps the group meaning and stays on the legacy path.) */
static int l_thr_exit(struct process *p, struct trap_frame *tf) {
  process_thread_exit(tf, l_arg(tf, 0));
  return 1; /* not reached */
}

/* ---- futex ------------------------------------------------------------- */

/* Read the futex word through the caller's address space.  The page is
   materialized first (vm_touch), then read directly -- the same model
   process_futex uses, valid because the trap's CR3 is still the caller's. */
static int l_futex_word(struct process *p, uint64_t uaddr, uint32_t *out) {
  if (uaddr & 3)
    return -EINVAL;
  if (vm_touch(p, uaddr, 4, 0) != 0)
    return -EFAULT;
  *out = *(volatile uint32_t *)uaddr;
  return 0;
}

static int l_thr_futex(struct process *p, struct trap_frame *tf) {
  uint64_t uaddr = l_arg(tf, 0);
  int op = (int)l_arg(tf, 1);
  uint32_t val = (uint32_t)l_arg(tf, 2);
  uint64_t utime = l_arg(tf, 3);
  int cmd = op & L_FUTEX_CMD_MASK;
  int realtime = (op & L_FUTEX_CLOCK_REALTIME) != 0;
  struct addr_space *as = p->as;
  int64_t timeout_ms = -1;

  switch (cmd) {
  case L_FUTEX_WAIT:
  case L_FUTEX_WAIT_BITSET: {
    uint32_t bitset = L_FUTEX_BITSET_MATCH_ANY;
    int absolute = (cmd == L_FUTEX_WAIT_BITSET);
    if (cmd == L_FUTEX_WAIT_BITSET) {
      /* The 6th argument is the bitset; 0 means "match anything". */
      bitset = (uint32_t)l_arg(tf, 5);
      if (bitset == 0) {
        tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
        return 1;
      }
    }
    if (utime) {
      int64_t ms = 0;
      int r = l_timespec_to_ms(p, utime, &ms);
      if (r != 0) {
        tf->regs[0] = (uint64_t)(int64_t)r;
        return 1;
      }
      if (absolute) {
        /* FUTEX_WAIT_BITSET takes an ABSOLUTE time (monotonic unless
           FUTEX_CLOCK_REALTIME).  Convert to a relative deadline. */
        int64_t now = (int64_t)timer_get_ms();
        ms -= now;
        if (ms < 0)
          ms = 0;
      }
      timeout_ms = ms;
    }
    /* Realtime waits are accepted as monotonic: the difference between the
       two clocks here is the boot RTC offset, and a timeout that fires a
       few seconds early is not a hang.  Recorded rather than refused. */
    (void)realtime;

    uint32_t word = 0;
    int r = l_futex_word(p, uaddr, &word);
    if (r != 0) {
      tf->regs[0] = (uint64_t)(int64_t)r;
      return 1;
    }
    int slot = l_thr_slot_of(p, 1);
    if (slot < 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EAGAIN;
      return 1;
    }

    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    /* Compare under the lock: the waker cannot slip between the compare and
       the park, so no wakeup is lost. */
    if (*(volatile uint32_t *)uaddr != val) {
      spinlock_release_irqrestore(&proc_lock, flags);
      tf->regs[0] = (uint64_t)(int64_t)-EAGAIN;
      return 1;
    }
    if (timeout_ms == 0) {
      spinlock_release_irqrestore(&proc_lock, flags);
      tf->regs[0] = (uint64_t)(int64_t)-ETIMEDOUT;
      return 1;
    }
    p->futex_uaddr = uaddr;
    p->wake_ms = (timeout_ms < 0) ? 0 : timer_get_ms() + (uint64_t)timeout_ms;
    p->context[0] = 0; /* resume value: 0 on wake */
    tf->regs[0] = 0;
    p->state = PROC_STATE_FUTEX; /* save-set member: context is saved */
    l_thr[slot].as = as;
    l_thr[slot].uaddr = uaddr;
    l_thr[slot].bitset = bitset;
    l_thr[slot].waits++;
    spinlock_release_irqrestore(&proc_lock, flags);
    schedule(tf, 0);
    return 1; /* not reached: resume re-enters user space via context[0] */
  }

  case L_FUTEX_WAKE:
  case L_FUTEX_WAKE_BITSET: {
    uint32_t bitset = L_FUTEX_BITSET_MATCH_ANY;
    if (cmd == L_FUTEX_WAKE_BITSET) {
      bitset = (uint32_t)l_arg(tf, 5);
      if (bitset == 0) {
        tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
        return 1;
      }
    }
    if ((int32_t)val < 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
      return 1;
    }
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    int woke = l_futex_wake_locked(as, uaddr, val, bitset);
    spinlock_release_irqrestore(&proc_lock, flags);
    tf->regs[0] = (uint64_t)(int64_t)woke;
    return 1;
  }

  case L_FUTEX_REQUEUE:
  case L_FUTEX_CMP_REQUEUE: {
    /* Raw ABI (man FUTEX_CMP_REQUEUE.2const):
         futex(uaddr, op, val, val2, uaddr2, val3)
       val  = max waiters to WAKE on uaddr
       val2 = max waiters to REQUEUE onto uaddr2
       val3 = expected value at uaddr (CMP_REQUEUE only) */
    uint64_t uaddr2 = l_arg(tf, 4);
    uint32_t val2 = (uint32_t)l_arg(tf, 3);
    uint32_t val3 = (uint32_t)l_arg(tf, 5);
    if ((uaddr2 & 3) || vm_touch(p, uaddr2, 4, 1) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return 1;
    }
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    if (cmd == L_FUTEX_CMP_REQUEUE && *(volatile uint32_t *)uaddr != val3) {
      spinlock_release_irqrestore(&proc_lock, flags);
      tf->regs[0] = (uint64_t)(int64_t)-EAGAIN;
      return 1;
    }
    int woke = l_futex_wake_locked(as, uaddr, val, L_FUTEX_BITSET_MATCH_ANY);
    int moved = l_futex_requeue_locked(as, uaddr, uaddr2, val2);
    spinlock_release_irqrestore(&proc_lock, flags);
    tf->regs[0] = (uint64_t)(int64_t)(woke + moved);
    return 1;
  }

  case L_FUTEX_WAKE_OP:
  case L_FUTEX_LOCK_PI:
  case L_FUTEX_UNLOCK_PI:
  case L_FUTEX_TRYLOCK_PI:
  case L_FUTEX_WAIT_REQUEUE_PI:
    /* Linux's own answer when PI futexes are not configured.  Nothing in the
       measured profile uses them; a PRIO_INHERIT mutex fails loudly at
       pthread_mutexattr_setprotocol time instead of deadlocking.  WAKE_OP is
       not used by glibc 2.31's sem_post (atomic + plain wake). */
    tf->regs[0] = (uint64_t)(int64_t)-ENOSYS;
    return 1;

  default:
    tf->regs[0] = (uint64_t)(int64_t)-ENOSYS;
    return 1;
  }
}

/* ---- robust list ------------------------------------------------------- */

static int l_thr_set_robust_list(struct process *p, struct trap_frame *tf) {
  uint64_t head = l_arg(tf, 0);
  uint64_t len = l_arg(tf, 1);
  int slot = l_thr_slot_of(p, 1);
  if (slot < 0)
    return 0;
  if (len != 24) { /* sizeof(struct robust_list_head) */
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  l_thr[slot].robust_head = head;
  tf->regs[0] = 0;
  return 1;
}

static int l_thr_get_robust_list(struct process *p, struct trap_frame *tf) {
  int32_t pid = (int32_t)l_arg(tf, 0);
  uint64_t uhead = l_arg(tf, 1);
  uint64_t ulen = l_arg(tf, 2);
  struct process *t = 0;
  if (pid == 0)
    t = current_process();
  else if (pid > 0 && pid < MAX_PROCESSES)
    t = process_get_pcb(pid);
  if (!t) {
    tf->regs[0] = (uint64_t)(int64_t)-ESRCH;
    return 1;
  }
  /* Same-process only: no ptrace, so a foreign tid is not inspectable. */
  if (process_group(t)->pid != process_group(p)->pid) {
    tf->regs[0] = (uint64_t)(int64_t)-EPERM;
    return 1;
  }
  int slot = l_thr_slot_of(t, 0);
  uint64_t head = slot >= 0 ? l_thr[slot].robust_head : 0;
  uint64_t len = 24;
  if (uhead && l_copy_to_user(p, uhead, &head, 8) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  if (ulen && l_copy_to_user(p, ulen, &len, 8) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  tf->regs[0] = 0;
  return 1;
}

/* Walk the exiting thread's robust list and mark every owned mutex
   FUTEX_OWNER_DIED, waking its waiters.  Without this a thread that dies
   holding a robust mutex parks its joiners forever (the hang looks like a
   pthread_join bug, not a kernel bug).  Called from process_thread_exit()
   BEFORE the address space is torn down. */
void linux_compat_thread_exit(struct process *p) {
  int slot = l_thr_slot_of(p, 0);
  if (slot < 0)
    return;
  /* Drop any futex queue entry: the PCB is leaving. */
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  l_thr[slot].uaddr = 0;
  p->futex_uaddr = 0;
  spinlock_release_irqrestore(&proc_lock, flags);

  uint64_t head = l_thr[slot].robust_head;
  l_thr[slot].robust_head = 0;
  if (!head)
    return;

  uint64_t list = 0, pending = 0;
  int64_t off = 0;
  if (l_copy_from_user(p, &list, head + 0, 8) != 0)
    return;
  if (l_copy_from_user(p, &off, head + 8, 8) != 0)
    return;
  if (l_copy_from_user(p, &pending, head + 16, 8) != 0)
    return;

  /* The list is CIRCULAR and an empty one points at the head itself.  glibc
     installs exactly that (head.list == &head.list, futex_offset == -32), so
     treating it as an entry reads 32 bytes BELOW the head -- which is
     pd->list (the stack-cache link) -- and ORs FUTEX_OWNER_DIED into it.
     Measured: pd->list.next 0x10802122D0 became 0x10C02122D0 and the next
     pthread_join died in __free_tcb at 0x10C02122D8. */
  for (int n = 0; n < L_ROBUST_LIST_LIMIT && list && list != head; n++) {
    uint64_t word_va = list + (uint64_t)off;
    uint32_t w = 0;
    if (l_copy_from_user(p, &w, word_va, 4) != 0)
      break;
    if (w != 0 && (w & L_FUTEX_OWNER_DIED) == 0) {
      uint32_t nw = (w & L_FUTEX_TID_MASK) | L_FUTEX_OWNER_DIED;
      if (w & L_FUTEX_WAITERS)
        nw |= L_FUTEX_WAITERS;
      if (l_copy_to_user(p, word_va, &nw, 4) == 0 && (nw & L_FUTEX_WAITERS)) {
        uint64_t f = spinlock_acquire_irqsave(&proc_lock);
        l_futex_wake_locked(p->as, word_va, 0x7fffffff,
                            L_FUTEX_BITSET_MATCH_ANY);
        spinlock_release_irqrestore(&proc_lock, f);
      }
    }
    uint64_t next = 0;
    if (l_copy_from_user(p, &next, list, 8) != 0)
      break;
    if (next == list)
      break; /* self node: an empty list (defensive) */
    list = next;
  }
  (void)pending; /* list_op_pending: the node above was already visited */
}

/* ---- arch_prctl / sched_getcpu ----------------------------------------- */

static uint64_t l_read_fs_base(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
  return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int l_thr_arch_prctl(struct process *p, struct trap_frame *tf) {
  uint64_t code = l_arg(tf, 0);
  uint64_t addr = l_arg(tf, 1);
  if (code == L_ARCH_SET_FS) {
    tf->regs[0] = (uint64_t)(int64_t)process_set_tls(p, addr);
    return 1;
  }
  if (code == L_ARCH_GET_FS) {
    uint64_t cur = l_read_fs_base();
    tf->regs[0] = (uint64_t)(int64_t)(l_copy_to_user(p, addr, &cur, 8) == 0
                                          ? 0
                                          : -EFAULT);
    return 1;
  }
  if (code == L_ARCH_GET_CPUID || code == 0x1012 /* ARCH_SET_CPUID */) {
    /* glibc's CPUID feature probe.  SET_CPUID(1) means "enable the CPUID
       fault emulation", which this kernel does not do: Linux's answer for a
       kernel without it is the number of the last CPUID leaf, and glibc
       treats any non-negative value as "nothing to do".  Report 0. */
    uint32_t zero = 0;
    if (code == L_ARCH_GET_CPUID &&
        l_copy_to_user(p, addr, &zero, 4) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return 1;
    }
    tf->regs[0] = 0;
    return 1;
  }
  tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
  return 1;
}

/* getcpu(309): glibc's sched_getcpu() is built on this, not on a syscall of
   its own name.  (cpu, node, tcache) -- the first two are optional output
   pointers, the third is glibc's rseq cache and must be NULL (Linux returns
   EINVAL for a non-NULL tcache when rseq is not in use). */
static int l_thr_getcpu(struct process *p, struct trap_frame *tf) {
  uint64_t ucpu = l_arg(tf, 0);
  uint64_t unode = l_arg(tf, 1);
  uint64_t tcache = l_arg(tf, 2);
  if (tcache) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  uint32_t cpu = get_cpuid();
  uint32_t node = 0;
  if (ucpu && l_copy_to_user(p, ucpu, &cpu, 4) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  if (unode && l_copy_to_user(p, unode, &node, 4) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  tf->regs[0] = 0;
  return 1;
}

/* ---- Linux x86-64 rt_sigframe ------------------------------------------
 *
 * Layout (Linux arch/x86/include/asm/sigframe.h + uapi sigcontext):
 *
 *   frame+0    pretcode      the guest's sa_restorer (glibc: __restore_rt)
 *   frame+8    ucontext     uc_flags, uc_link, uc_stack[24], mcontext[256],
 *                           uc_sigmask[128]  (sizeof = 424)
 *   frame+432  siginfo_t    128 bytes
 *   frame+560  fpstate      512-byte FXSAVE image
 *   frame+1072 end
 *
 * At handler entry %rsp = frame, %rdi = signo, %rsi = &siginfo, %rdx = &uc.
 * The handler returns to pretcode, which does `mov $15,%rax; syscall`; that
 * reaches rt_sigreturn, which parses this frame back.
 */
#define L_SF_PRETCODE 0
#define L_SF_UC 8
#define L_SF_INFO 432
#define L_SF_FP 560
#define L_SF_SIZE 1072

#define L_UC_FLAGS 0
#define L_UC_LINK 8
#define L_UC_STACK 16
#define L_UC_MCONTEXT 40
#define L_UC_SIGMASK 296
#define L_UC_SIZE 424

#define L_MC_GREGS 0
#define L_MC_FPREG 184
#define L_MC_RESERVED 192
#define L_MC_SIZE 256

#define L_G_R8 0
#define L_G_R9 1
#define L_G_R10 2
#define L_G_R11 3
#define L_G_R12 4
#define L_G_R13 5
#define L_G_R14 6
#define L_G_R15 7
#define L_G_RDI 8
#define L_G_RSI 9
#define L_G_RBP 10
#define L_G_RBX 11
#define L_G_RDX 12
#define L_G_RAX 13
#define L_G_RCX 14
#define L_G_RSP 15
#define L_G_RIP 16
#define L_G_EFL 17
#define L_G_CSGSFS 18
#define L_G_ERR 19
#define L_G_TRAPNO 20
#define L_G_OLDMASK 21
#define L_G_CR2 22

#define L_SA_ONSTACK 0x08000000

/* SEGV codes */
#define L_SEGV_MAPERR 1
#define L_SEGV_ACCERR 2
#define L_ILL_ILLOPC 1
#define L_FPE_INTDIV 1
#define L_BUS_ADRALN 1

static uintptr_t l_sf_addr(uint64_t base, uint32_t off) {
  return (uintptr_t)(base + off);
}

/* Per-thread signal masks live in linux_compat_proc.c (one place owns the
   rt_sigprocmask storage); these are its accessors. */
extern uint64_t linux_compat_sigmask_get(struct process *p);
extern void linux_compat_sigmask_set(struct process *p, uint64_t mask);
extern int linux_compat_altstack_get(struct process *p, uint64_t *sp,
                                     uint64_t *size, int *flags);
extern int linux_compat_sigflags_get(struct process *p, int sig);

/* Write the register file into uc_mcontext.gregs. */
static void l_sf_fill_gregs(uint64_t base, struct trap_frame *tf,
                            uint64_t cr2) {
  volatile uint64_t *g = (volatile uint64_t *)l_sf_addr(
      base, L_SF_UC + L_UC_MCONTEXT + L_MC_GREGS);
  g[L_G_R8] = tf->regs[7];
  g[L_G_R9] = tf->regs[8];
  g[L_G_R10] = tf->regs[9];
  g[L_G_R11] = tf->regs[10];
  g[L_G_R12] = tf->regs[11];
  g[L_G_R13] = tf->regs[12];
  g[L_G_R14] = tf->regs[13];
  g[L_G_R15] = tf->regs[14];
  g[L_G_RDI] = tf->regs[5];
  g[L_G_RSI] = tf->regs[4];
  g[L_G_RBP] = tf->regs[6];
  g[L_G_RBX] = tf->regs[1];
  g[L_G_RDX] = tf->regs[3];
  g[L_G_RAX] = tf->regs[0];
  g[L_G_RCX] = tf->regs[2];
  g[L_G_RSP] = tf->lr;
  g[L_G_RIP] = tf->elr;
  g[L_G_EFL] = tf->spsr;
  g[L_G_CSGSFS] = (tf->cs & 0xffff) | ((tf->ss & 0xffff) << 48);
  g[L_G_ERR] = tf->error_code;
  g[L_G_TRAPNO] = tf->vector;
  g[L_G_OLDMASK] = 0;
  g[L_G_CR2] = cr2;
}

static void l_sf_fill_siginfo(uint64_t base, int sig, int code,
                              uint64_t fault_addr) {
  volatile int32_t *si = (volatile int32_t *)l_sf_addr(base, L_SF_INFO);
  /* si_signo, si_errno, si_code, __pad0 */
  si[0] = sig;
  si[1] = 0;
  si[2] = code;
  si[3] = 0;
  /* union _sifields: _sigfault.si_addr at offset 16 */
  *(volatile uint64_t *)l_sf_addr(base, L_SF_INFO + 16) = fault_addr;
}

/* Plant the frame for `sig` on the current thread.  Returns 1 on success. */
static int l_sf_plant(struct process *p, struct process *cur,
                      struct process *grp, struct trap_frame *tf, int sig,
                      int code, uint64_t fault_addr) {
  uint64_t handler = grp->sig_handler[sig];
  uint64_t restorer = grp->sig_restorer;
  uint64_t sp = tf->lr;
  int flags = linux_compat_sigflags_get(p, sig);

  if (restorer == 0 || !l_user_ok(p, restorer, 4, 0))
    return 0;

  if ((flags & L_SA_ONSTACK) != 0) {
    uint64_t asp = 0, asize = 0;
    int astate = 0;
    if (linux_compat_altstack_get(p, &asp, &asize, &astate) == 0 &&
        astate == 0 && asp && asize > L_SF_SIZE + 64)
      sp = asp + asize;
  }
  if (sp < (uint64_t)L_SF_SIZE + 64)
    return 0;
  uint64_t base = (sp - (uint64_t)L_SF_SIZE) & ~(uint64_t)15;
  if (vm_touch(p, base, L_SF_SIZE, 1) != 0)
    return 0;

  /* pretcode + ucontext header */
  *(volatile uint64_t *)l_sf_addr(base, L_SF_PRETCODE) = restorer;
  volatile uint8_t *uc = (volatile uint8_t *)l_sf_addr(base, L_SF_UC);
  for (int i = 0; i < L_UC_SIZE; i++)
    uc[i] = 0;
  *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_FLAGS) = 0;
  *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_LINK) = 0;
  /* uc_stack: the altstack that is in use, or SS_DISABLE */
  {
    uint64_t asp = 0, asize = 0;
    int astate = 2; /* SS_DISABLE */
    if ((flags & L_SA_ONSTACK) != 0)
      (void)linux_compat_altstack_get(p, &asp, &asize, &astate);
    volatile uint64_t *st =
        (volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_STACK);
    st[0] = asp;                    /* ss_sp */
    *(volatile uint32_t *)(st + 1) = (uint32_t)astate; /* ss_flags */
    st[2] = asize;                  /* ss_size */
  }
  l_sf_fill_gregs(base, tf, fault_addr);
  /* fpstate pointer + a real FXSAVE image, so rt_sigreturn can put the FP
     registers back exactly as the interrupted code had them. */
  *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_MCONTEXT + L_MC_FPREG) =
      base + L_SF_FP;
  arch_fpu_save_to((void *)l_sf_addr(base, L_SF_FP));
  /* uc_sigmask = the caller's current kernel mask (8 bytes) */
  {
    uint64_t m = linux_compat_sigmask_get(p);
    *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_SIGMASK) = m;
  }
  l_sf_fill_siginfo(base, sig, code, fault_addr);

  /* Enter the handler: Linux hands it rsp=frame (16-aligned), with the
     arguments in rdi/rsi/rdx and rax=0.  Clear DF, as Linux does. */
  tf->regs[5] = (uint64_t)sig;                    /* rdi = signo */
  tf->regs[4] = base + L_SF_INFO;                 /* rsi = siginfo* */
  tf->regs[3] = base + L_SF_UC;                   /* rdx = ucontext* */
  tf->regs[0] = 0;                                /* rax */
  tf->elr = handler;
  tf->lr = base;
  tf->spsr &= ~(uint64_t)0x400;                   /* clear DF */
  return 1;
}

/* Drop a pending bit wherever it was recorded (thread or anchor). */
static void l_sig_drop(struct process *cur, struct process *grp, int sig) {
  uint64_t bit = 1ull << (sig - 1);
  cur->sig_pending &= ~bit;
  if (grp && grp != cur)
    grp->sig_pending &= ~bit;
}

/* The compat delivery engine: process.c calls this instead of the HOS3
   engine when the group is a Linux-ABI process.  `grp` is the group anchor;
   the frame belongs to the CURRENT thread (that is what makes a fault signal
   run on the thread that faulted). */
int linux_compat_signal_deliver(struct process *grp, struct trap_frame *tf) {
  struct process *cur = current_process();
  if (!cur || !grp || cur->is_kernel_process || !tf)
    return 0;
  if ((tf->cs & 3) != 3)
    return 0;
  if (cur->sig_in_handler)
    return 0; /* no nesting; a further signal stays pending */
  int slot = l_thr_slot_of(cur, 1);
  (void)slot;

  uint64_t pend = cur->sig_pending;
  if (cur != grp)
    pend |= grp->sig_pending;
  if (!pend)
    return 0;

  int sig = 0;
  for (int s = 1; s < HO_SIG_MAX; s++) {
    if (!(pend & (1ull << (s - 1))))
      continue;
    if (s == HO_SIGKILL || s == HO_SIGSTOP)
      continue; /* not catchable */
    if (linux_compat_sigmask_get(cur) & (1ull << (s - 1)))
      continue; /* blocked: stays pending */
    sig = s;
    break;
  }
  if (!sig)
    return 0;

  uint64_t handler = grp->sig_handler[sig];
  if (handler == HO_SIG_IGN) {
    l_sig_drop(cur, grp, sig);
    return 0;
  }
  if (handler == HO_SIG_DFL) {
    /* Default action is the native one (kill with the signal); the compat
       layer only changes how a HANDLER is entered. */
    l_sig_drop(cur, grp, sig);
    process_kill_group_sig(grp, sig);
    return 0;
  }

  uint64_t fault_addr = 0;
  int code = 0;
  if (tf->vector == 14) {
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
    code = L_SEGV_MAPERR;
  } else if (sig == HO_SIGILL) {
    code = L_ILL_ILLOPC;
  } else if (sig == HO_SIGFPE) {
    code = L_FPE_INTDIV;
  } else if (sig == HO_SIGBUS) {
    code = L_BUS_ADRALN;
  }

  /* Commit: mark this thread in-handler before the frame becomes visible, so
     a nested trap cannot plant a second one. */
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  if (!(cur->sig_pending & (1ull << (sig - 1))) &&
      !(grp->sig_pending & (1ull << (sig - 1)))) {
    spinlock_release_irqrestore(&proc_lock, flags);
    return 0;
  }
  cur->sig_in_handler = 1;
  cur->sig_frame = 0;
  l_sig_drop(cur, grp, sig);
  spinlock_release_irqrestore(&proc_lock, flags);

  if (!l_sf_plant(cur, cur, grp, tf, sig, code, fault_addr)) {
    cur->sig_in_handler = 0;
    uart_puts("[LCOMPAT] signal: no usable frame/restorer -> default action\n");
    process_kill_group_sig(grp, sig);
    return 0;
  }
  return 1;
}

/* rt_sigreturn: parse the Linux frame the handler was entered with and
   restore the interrupted thread.  Replaces the native HOS3 restore for
   compat processes (the restorer is the guest's, so the frame is Linux's). */
int linux_compat_sigreturn(struct process *p, struct trap_frame *tf) {
  struct process *cur = current_process();
  uint64_t sp = tf->lr;
  if (!cur || sp < 16) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  uint64_t base = sp - 8; /* pretcode was consumed by the handler's ret */
  if (vm_touch(p, base, L_SF_SIZE, 0) != 0) {
    static int bad_reports;
    if (bad_reports < 3) {
      bad_reports++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] rt_sigreturn: frame not readable at 0x");
      kl_hex(&kl, (unsigned long)base);
      kl_s(&kl, " -> EINVAL\n");
      kl_emit(&kl);
    }
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  volatile uint64_t *g = (volatile uint64_t *)l_sf_addr(
      base, L_SF_UC + L_UC_MCONTEXT + L_MC_GREGS);
  uint64_t rip = g[L_G_RIP];
  uint64_t rsp = g[L_G_RSP];
  if (!l_user_ok(p, rip, 1, 0) || !l_user_ok(p, rsp, 8, 1)) {
    /* A corrupt frame must not resume the kernel: force SIGSEGV. */
    uart_puts("[LCOMPAT] rt_sigreturn: bad rip/rsp -> SIGSEGV\n");
    cur->sig_in_handler = 0;
    process_fault_exit(tf, HO_SIGSEGV);
    return 1;
  }
  tf->regs[7] = g[L_G_R8];
  tf->regs[8] = g[L_G_R9];
  tf->regs[9] = g[L_G_R10];
  tf->regs[10] = g[L_G_R11];
  tf->regs[11] = g[L_G_R12];
  tf->regs[12] = g[L_G_R13];
  tf->regs[13] = g[L_G_R14];
  tf->regs[14] = g[L_G_R15];
  tf->regs[5] = g[L_G_RDI];
  tf->regs[4] = g[L_G_RSI];
  tf->regs[6] = g[L_G_RBP];
  tf->regs[1] = g[L_G_RBX];
  tf->regs[3] = g[L_G_RDX];
  tf->regs[0] = g[L_G_RAX];
  tf->regs[2] = g[L_G_RCX];
  tf->elr = rip;
  tf->lr = rsp;
  tf->spsr = g[L_G_EFL];
  linux_compat_sigmask_set(p, *(volatile uint64_t *)l_sf_addr(
                                  base, L_SF_UC + L_UC_SIGMASK));
  /* FP: the frame's fpstate is a real FXSAVE image; put it back now (the
     iretq path does not reload FP for a same-process resume). */
  {
    uint64_t fp = *(volatile uint64_t *)l_sf_addr(
        base, L_SF_UC + L_UC_MCONTEXT + L_MC_FPREG);
    if (fp && l_user_ok(p, fp, 512, 0))
      arch_fpu_restore_from((const void *)fp);
    else
      arch_fpu_restore_from((const void *)l_sf_addr(base, L_SF_FP));
  }
  cur->sig_in_handler = 0;
  cur->sig_frame = 0;
  return 1;
}

/* Fault hook: process.c's process_fault_exit() asks before killing.  A
   compat process with a handler for this signal resumes in the handler. */
int linux_compat_fault(struct process *p, struct trap_frame *tf, int signo) {
  if (!p || !tf)
    return 0;
  {
    /* Four lines is evidence (which fault, on which thread, with which FS
       base); a fault storm is a flood. */
    static int ffr;
    if (ffr < 4) {
      ffr++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] fault sig=");
      kl_i(&kl, signo);
      kl_s(&kl, " pid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " tgid=");
      kl_i(&kl, p->tgid);
      kl_s(&kl, " tls=");
      kl_hex(&kl, p->tls_base);
      kl_s(&kl, " live_fs=0x");
      kl_hex(&kl, l_read_fs_base());
      kl_s(&kl, "\n");
      kl_emit(&kl);
    }
  }
  struct process *grp = process_group(p);
  if (grp->sig_handler[signo] <= HO_SIG_IGN)
    return 0;
  if ((tf->cs & 3) != 3)
    return 0;
  if (p->sig_in_handler)
    return 0; /* a fault inside the handler: the default action is right */
  int code = L_SEGV_MAPERR;
  uint64_t fault_addr = 0;
  if (tf->vector == 14)
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
  else if (signo == HO_SIGILL)
    code = L_ILL_ILLOPC;
  else if (signo == HO_SIGFPE)
    code = L_FPE_INTDIV;
  else if (signo == HO_SIGBUS)
    code = L_BUS_ADRALN;
  p->sig_in_handler = 1;
  if (!l_sf_plant(p, p, grp, tf, signo, code, fault_addr)) {
    p->sig_in_handler = 0;
    return 0;
  }
  return 1;
}

/* process_signal() hook: a signal aimed at a thread parked in futex must
   interrupt the wait (Linux returns EINTR and glibc re-checks the word).
   Without this Mono's stop-the-world tgkill never reaches a parked worker. */
void linux_compat_signal_interrupt(struct process *t) {
  if (!t || !(t->abi_flags & PROC_FLAG_LINUX_ABI))
    return;
  if (t->state != PROC_STATE_FUTEX)
    return;
  t->context[0] = (uint64_t)(int64_t)-EINTR;
  t->futex_uaddr = 0;
  t->wake_ms = 0;
  t->state = PROC_STATE_READY;
}

/* ---- the table --------------------------------------------------------- */

const struct l_sysent l_tab_thr[] = {
    {L_SYS_clone, l_thr_clone, "clone"},
    {L_SYS_futex, l_thr_futex, "futex"},
    {L_SYS_exit, l_thr_exit, "exit"},
    {L_SYS_arch_prctl, l_thr_arch_prctl, "arch_prctl"},
    {L_SYS_set_robust_list, l_thr_set_robust_list, "set_robust_list"},
    {L_SYS_get_robust_list, l_thr_get_robust_list, "get_robust_list"},
    {L_SYS_getcpu, l_thr_getcpu, "getcpu"},
    L_SYSENT_END};
