#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "process.h"
#include "errno.h"
#include "fs.h"
#include "program_loader.h"
#include "timer.h"

extern void uart_puts(const char* s);

static void test_process_init_and_create(void) {
  tests_run++;
  uart_puts("  Running test_process_init_and_create...\n");

  int pid1 = process_create();
  EXPECT_EQ((pid1 >= 0 && pid1 < MAX_PROCESSES), 1);

  struct process *p1 = process_get_pcb(pid1);
  EXPECT_EQ((p1 != 0), 1);
  EXPECT_EQ(p1->pid, pid1);
  EXPECT_EQ(p1->state, PROC_STATE_ALLOCATED);
  EXPECT_EQ(p1->parent_pid, -1);

  process_free(pid1);
}

static void test_process_set_entry(void) {
  tests_run++;
  uart_puts("  Running test_process_set_entry...\n");

  int pid = process_create();
  EXPECT_EQ((pid >= 0 && pid < MAX_PROCESSES), 1);

  process_set_entry(pid, USER_VIRT_BASE, USER_VIRT_STACK);

  struct process *p = process_get_pcb(pid);
  EXPECT_EQ((p != 0), 1);
  EXPECT_EQ(p->state, PROC_STATE_READY);
  // x31 in trap_frame mapping context is elr, x33 is sp_el0
  EXPECT_EQ(p->context[31], USER_VIRT_BASE);
  EXPECT_EQ(p->context[33], USER_VIRT_STACK);

  process_free(pid);
}

static void test_process_num_cpus(void) {
  tests_run++;
  uart_puts("  Running test_process_num_cpus...\n");

  // Runtime CPU count must be sane: at least one CPU, never above the
  // static ceiling. Before any CPU has idled the getter falls back to
  // MAX_CPUS, so the range check holds in every boot state.
  int n = process_get_num_cpus();
  uart_puts("  [process] reported CPUs=");
  print_int(n);
  uart_puts("\n");
  EXPECT_EQ((n >= 1 && n <= MAX_CPUS), 1);

  // Aggregate idle time must never exceed (uptime * MAX_CPUS ms); a
  // violation would mean idle was accumulated more than once per tick.
  extern uint64_t timer_get_ms(void);
  uint64_t up = timer_get_ms();
  uint64_t idle = process_get_total_idle_ms();
  EXPECT_EQ((idle <= up * (uint64_t)MAX_CPUS), 1);
}

/* ---------------------------------------------------------------------
 * P1 (docs/browser/p1-threads-design.md): threads, groups, futex-lite.
 * ------------------------------------------------------------------- */

/* A leader's PCB is the group anchor: new members share its address space,
 * carry tgid/is_thread/live_threads exactly per section 1, and route back
 * to the anchor through process_group(). */
static void test_process_thread_group(void) {
  tests_run++;
  uart_puts("  Running test_process_thread_group...\n");

  int lid = process_create();
  EXPECT_EQ((lid >= 0 && lid < MAX_PROCESSES), 1);
  struct process *leader = process_get_pcb(lid);
  EXPECT_EQ((leader != 0), 1);
  EXPECT_EQ(leader->tgid, lid);
  EXPECT_EQ(leader->is_thread, 0);
  EXPECT_EQ(leader->live_threads, 1);

  int tid = process_thread_create(leader, USER_VIRT_BASE + 0x100, 0x50,
                                  USER_VIRT_STACK, 0);
  EXPECT_EQ((tid >= 0 && tid < MAX_PROCESSES), 1);
  struct process *t = process_get_pcb(tid);
  EXPECT_EQ((t != 0), 1);
  EXPECT_EQ(t->is_thread, 1);
  EXPECT_EQ(t->tgid, lid);
  EXPECT_EQ(t->live_threads, 0); /* "0 elsewhere" (section 1) */
  EXPECT_EQ(t->parent_pid, -1);  /* threads are never waitpid()ed */
  EXPECT_EQ(leader->live_threads, 2);
  EXPECT_EQ((t->user_phys_base == leader->user_phys_base), 1);
  EXPECT_EQ((t->user_l2_table == leader->user_l2_table), 1);
  EXPECT_EQ(t->phys_block_idx, -1); /* threads never own a block */
  EXPECT_EQ(t->state, PROC_STATE_READY);
  EXPECT_EQ((t->context[31] == USER_VIRT_BASE + 0x100), 1); /* entry/pc */
  EXPECT_EQ((t->context[33] == USER_VIRT_STACK), 1);        /* sp */
  EXPECT_EQ(t->context[32], 0); /* fresh user: SPSR/RFLAGS classify bit */
#ifdef __x86_64__
  EXPECT_EQ(t->context[5], 0x50); /* rdi = arg */
  EXPECT_EQ(t->context[34], 0);   /* cs: 0 = classify fresh */
  EXPECT_EQ(t->context[35], 0);
#else
  EXPECT_EQ(t->context[0], 0x50); /* x0 = arg */
#endif
  /* Fresh FP image at create (F1.5 per-thread contract): the clean
     architectural default, never the creator's — all-zero on aarch64; the
     x86_64 reset image (FCW=0x037F, FTW=0xFF, MXCSR=0x1F80, rest zero). */
#ifdef __x86_64__
  {
    uint8_t *img = (uint8_t *)t->fpu_state;
    EXPECT_EQ((img[0] == 0x7F && img[1] == 0x03 && img[4] == 0xFF &&
               img[24] == 0x80 && img[25] == 0x1F && img[26] == 0), 1);
  }
#else
  EXPECT_EQ((t->fpu_state[0] == 0 && t->fpu_state[65] == 0), 1);
#endif

  /* Routing: leaders map to themselves, members to the anchor, NULL stays. */
  EXPECT_EQ((process_group(leader) == leader), 1);
  EXPECT_EQ((process_group(t) == leader), 1);
  EXPECT_EQ((process_group(0) == 0), 1);

  /* Cleanup: kill the thread's tombstone bookkeeping and free both. */
  t->state = PROC_STATE_THREAD_DONE;
  leader->live_threads = 0;
  process_free(tid);
  process_free(lid);
}

/* Validation: flags == 0, entry/stack inside the caller's own region,
 * stack 16-aligned with a 16-byte slot available below it. */
static void test_process_thread_validation(void) {
  tests_run++;
  uart_puts("  Running test_process_thread_validation...\n");

  int lid = process_create();
  struct process *leader = process_get_pcb(lid);
  EXPECT_EQ((leader != 0), 1);

  EXPECT_EQ(process_thread_create(leader, USER_VIRT_BASE + 0x100, 0,
                                  USER_VIRT_STACK, 1), -EINVAL);
  EXPECT_EQ(process_thread_create(leader, USER_VIRT_BASE - 4, 0,
                                  USER_VIRT_STACK, 0), -EINVAL);
  EXPECT_EQ(process_thread_create(leader, USER_VIRT_BASE + USER_REGION_SIZE, 0,
                                  USER_VIRT_STACK, 0), -EINVAL);
  EXPECT_EQ(process_thread_create(leader, USER_VIRT_BASE + 0x100, 0,
                                  USER_VIRT_STACK + 8, 0), -EINVAL);
  EXPECT_EQ(process_thread_create(leader, USER_VIRT_BASE + 0x100, 0,
                                  USER_VIRT_BASE + 8, 0), -EINVAL);
  EXPECT_EQ(process_thread_create(0, USER_VIRT_BASE + 0x100, 0,
                                  USER_VIRT_STACK, 0), -EINVAL);

  process_free(lid);
}

/* THREAD_DONE reclamation: with the table full, only a dead thread's slot
 * (or a leader tombstone with live_threads == 0) is reusable.  The unit
 * context has no CPU claims, so the still_running gate passes trivially. */
static void test_process_thread_slot_reclaim(void) {
  tests_run++;
  uart_puts("  Running test_process_thread_slot_reclaim...\n");

  int lid = process_create();
  struct process *leader = process_get_pcb(lid);
  int tid = process_thread_create(leader, USER_VIRT_BASE + 0x100, 0,
                                  USER_VIRT_STACK, 0);
  EXPECT_EQ((tid >= 0), 1);

  /* Snapshot + poison every other slot: no FREE slot and no reclaimable
     tombstone survives except the ones this test creates. */
  int saved[MAX_PROCESSES];
  for (int i = 0; i < MAX_PROCESSES; i++) {
    saved[i] = -1;
    struct process *q = process_get_pcb(i);
    if (!q || i == lid || i == tid)
      continue;
    if (q->state == PROC_STATE_FREE || q->state == PROC_STATE_EXITED ||
        q->state == PROC_STATE_THREAD_DONE) {
      saved[i] = q->state;
      q->state = PROC_STATE_ALLOCATED; /* poison: not reclaimable */
      q->is_thread = 0;
      q->live_threads = 1;
      q->phys_block_idx = -1;
      q->futex_uaddr = 0;
      q->wake_ms = 0;
    }
  }

  /* 1. Dead thread, unclaimed: reclaimable even while the anchor is gated
     by a live member. */
  struct process *t = process_get_pcb(tid);
  t->state = PROC_STATE_THREAD_DONE;
  leader->state = PROC_STATE_THREAD_DONE;
  leader->live_threads = 1;
  int r = process_create();
  EXPECT_EQ(r, tid);
  process_free(r);
  /* Re-tombstone the scratch slot for step 2. */
  t->state = PROC_STATE_THREAD_DONE;
  t->is_thread = 1;
  t->live_threads = 0;

  /* 2. Anchor gated by a live member and the thread slot poisoned: nothing
     is reclaimable -> create fails. */
  t->state = PROC_STATE_ALLOCATED;
  t->is_thread = 0;
  t->live_threads = 1;
  r = process_create();
  EXPECT_EQ(r, -1);

  /* 3. Release the gate: the anchor tombstone is reclaimed. */
  leader->live_threads = 0;
  r = process_create();
  EXPECT_EQ(r, lid);
  process_free(r);

  /* Restore the poisoned slots so later suites see a clean table. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *q = process_get_pcb(i);
    if (!q || saved[i] == -1)
      continue;
    q->state = saved[i];
    q->is_thread = 0;
    q->live_threads = 0;
    q->phys_block_idx = -1;
  }
}

/* futex-lite machine (section 5): immediate-return paths plus a fake parked
 * waiter exercising the wake scan (no scheduler needed). */
static void test_process_futex_machine(void) {
  tests_run++;
  uart_puts("  Running test_process_futex_machine...\n");

  int lid = process_create();
  struct process *leader = process_get_pcb(lid);
  EXPECT_EQ((leader != 0), 1);
  struct trap_frame tf;
  for (int i = 0; i < (int)(sizeof(tf) / sizeof(uint64_t)); i++)
    ((uint64_t *)&tf)[i] = 0;

  /* A real word inside the leader's physical region; the kernel reads it
     through the caller's physical translation. */
  uint64_t uaddr = USER_VIRT_BASE + 0x40;
  *(volatile uint32_t *)(leader->user_phys_base + 0x40) = 7;

  /* WAIT: value mismatch -> -EAGAIN; timeout 0 with a match -> -ETIMEDOUT;
     misaligned -> -EINVAL; out of region -> -EFAULT. */
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 0, 8, -1), -EAGAIN);
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 0, 7, 0), -ETIMEDOUT);
  EXPECT_EQ(process_futex(leader, &tf, uaddr + 2, 0, 7, -1), -EINVAL);
  EXPECT_EQ(process_futex(leader, &tf, 0x1000, 0, 7, -1), -EFAULT);

  /* WAKE: val < 0 -> -EINVAL, val == 0 -> 0, unknown op -> -ENOSYS. */
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 1, -5, 0), -EINVAL);
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 1, 0, 0), 0);
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 2, 0, 0), -ENOSYS);

  /* Fake a parked waiter in the SAME group and wake it. */
  int wid = process_create();
  struct process *w = process_get_pcb(wid);
  w->tgid = lid;
  w->state = PROC_STATE_FUTEX;
  w->futex_uaddr = uaddr;
  w->wake_ms = 12345;
  w->context[0] = 0xdead;
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 1, 1, 0), 1);
  EXPECT_EQ(w->state, PROC_STATE_READY);
  EXPECT_EQ(w->futex_uaddr, 0);
  EXPECT_EQ(w->wake_ms, 0);
  EXPECT_EQ((w->context[0] == 0), 1); /* resume value: 0 */
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 1, 1, 0), 0); /* nobody left */

  /* Another group's waiter on the same word is skipped (process-private). */
  w->state = PROC_STATE_FUTEX;
  w->futex_uaddr = uaddr;
  w->tgid = MAX_PROCESSES - 1;
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 1, 1, 0), 0);
  w->state = PROC_STATE_READY;
  w->tgid = lid;

  /* A waiter parked on a different word is skipped too. */
  w->state = PROC_STATE_FUTEX;
  w->futex_uaddr = uaddr + 8;
  EXPECT_EQ(process_futex(leader, &tf, uaddr, 1, 1, 0), 0);
  w->state = PROC_STATE_READY;

  process_free(wid);
  process_free(lid);
}

/* TLS register plumbing (section 4): validation + the PCB word. */
static void test_process_set_tls(void) {
  tests_run++;
  uart_puts("  Running test_process_set_tls...\n");

  int lid = process_create();
  struct process *leader = process_get_pcb(lid);
  EXPECT_EQ((leader != 0), 1);

  EXPECT_EQ(process_set_tls(leader, USER_VIRT_BASE + 0x200), 0);
  EXPECT_EQ((leader->tls_base == USER_VIRT_BASE + 0x200), 1);
  EXPECT_EQ(process_set_tls(leader, 0x1000), -EINVAL);
  EXPECT_EQ(process_set_tls(leader, USER_VIRT_BASE + USER_REGION_SIZE), -EINVAL);
  /* leave the live register benign for the rest of the suite */
  process_set_tls(leader, USER_VIRT_BASE);

  process_free(lid);
}

/* ---------------------------------------------------------------------
 * P5 (docs/browser/p5-exec-signals-design.md): signals, kill, SIGPIPE.
 * ------------------------------------------------------------------- */

/* Row-83 storage side: validation, storage, query-back. */
static void test_p5_sigaction_storage(void) {
  tests_run++;
  uart_puts("  Running test_p5_sigaction_storage...\n");

  int pid = process_create();
  EXPECT_EQ((pid >= 0 && pid < MAX_PROCESSES), 1);
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);
  struct process *p = process_group(process_get_pcb(pid));

  struct ho_sigaction act = {0}, old = {0};

  /* range + reserved-signal rejections (D6/D7) */
  EXPECT_EQ(process_sigaction(0, 0, 0), -EINVAL);
  EXPECT_EQ(process_sigaction(HO_SIG_MAX, 0, 0), -EINVAL);
  EXPECT_EQ(process_sigaction(HO_SIGKILL, 0, 0), -EINVAL);
  EXPECT_EQ(process_sigaction(HO_SIGSTOP, 0, 0), -EINVAL);

  /* a real handler needs a valid in-region restorer */
  act.sa_handler = USER_VIRT_BASE + 0x1000;
  act.sa_restorer = 0;
  EXPECT_EQ(process_sigaction(HO_SIGPIPE, &act, 0), -EINVAL);
  act.sa_restorer = USER_VIRT_BASE + 0x2000;
  EXPECT_EQ(process_sigaction(HO_SIGPIPE, &act, 0), 0);
  EXPECT_EQ((p->sig_handler[HO_SIGPIPE] == USER_VIRT_BASE + 0x1000), 1);
  EXPECT_EQ((p->sig_restorer == USER_VIRT_BASE + 0x2000), 1);

  /* query form returns what was stored */
  EXPECT_EQ(process_sigaction(HO_SIGPIPE, 0, &old), 0);
  EXPECT_EQ((old.sa_handler == USER_VIRT_BASE + 0x1000), 1);
  EXPECT_EQ((old.sa_restorer == USER_VIRT_BASE + 0x2000), 1);
  EXPECT_EQ(old.sa_flags, 0);

  /* SIG_IGN needs no restorer; SIG_DFL clears */
  act.sa_handler = HO_SIG_IGN;
  act.sa_restorer = 0;
  EXPECT_EQ(process_sigaction(HO_SIGPIPE, &act, 0), 0);
  EXPECT_EQ((p->sig_handler[HO_SIGPIPE] == HO_SIG_IGN), 1);
  act.sa_handler = HO_SIG_DFL;
  EXPECT_EQ(process_sigaction(HO_SIGPIPE, &act, 0), 0);
  EXPECT_EQ(p->sig_handler[HO_SIGPIPE], 0);

  set_current_process_pid(0, old_pid);
  process_free(pid);
}

/* Row-16 semantics (D9): validation, ESRCH, disposition outcomes. */
static void test_p5_kill_semantics(void) {
  tests_run++;
  uart_puts("  Running test_p5_kill_semantics...\n");

  int pid = process_create();
  struct process *p = process_get_pcb(pid);
  EXPECT_EQ((p != 0), 1);

  /* unknown numbers -> -EINVAL (D6); missing target -> -ESRCH */
  EXPECT_EQ(process_signal(pid, 3), -EINVAL); /* SIGQUIT is not in the set */
  EXPECT_EQ(process_signal(pid, 40), -EINVAL);
  EXPECT_EQ(process_signal(999999, HO_SIGTERM), -ESRCH);
  EXPECT_EQ(process_signal(pid, 0), -EINVAL); /* sig 0 is the trap-layer check */

  /* handler disposition: accepted-and-recorded, group survives */
  p->sig_handler[HO_SIGTERM] = USER_VIRT_BASE + 0x4000;
  EXPECT_EQ(process_signal(pid, HO_SIGTERM), 0);
  EXPECT_EQ((p->sig_pending & (1u << (HO_SIGTERM - 1))) != 0, 1);
  EXPECT_EQ((p->state != PROC_STATE_EXITED), 1);

  /* SIG_IGN: no-op, no pending */
  p->sig_pending = 0;
  p->sig_handler[HO_SIGTERM] = HO_SIG_IGN;
  EXPECT_EQ(process_signal(pid, HO_SIGTERM), 0);
  EXPECT_EQ(p->sig_pending, 0);

  /* default: immediate group kill with a signal-shaped status byte */
  p->sig_handler[HO_SIGTERM] = HO_SIG_DFL;
  EXPECT_EQ(process_signal(pid, HO_SIGTERM), 0);
  EXPECT_EQ(p->state, PROC_STATE_EXITED);
  EXPECT_EQ(p->exit_status, HO_SIGTERM);

  process_free(pid);
}

/* D10: the write-side SIGPIPE disposition matrix. */
static void test_p5_sigpipe_semantics(void) {
  tests_run++;
  uart_puts("  Running test_p5_sigpipe_semantics...\n");

  int pid = process_create();
  struct process *p = process_group(process_get_pcb(pid));

  /* SIG_IGN (the WebKit child): -EPIPE keeps flowing, group survives */
  p->sig_handler[HO_SIGPIPE] = HO_SIG_IGN;
  EXPECT_EQ(signal_epipe(p), 0);
  EXPECT_EQ(p->sig_pending, 0);
  EXPECT_EQ((p->state != PROC_STATE_EXITED), 1);

  /* real handler: pending bit set, survives */
  p->sig_handler[HO_SIGPIPE] = USER_VIRT_BASE + 0x3000;
  EXPECT_EQ(signal_epipe(p), 0);
  EXPECT_EQ((p->sig_pending & (1u << (HO_SIGPIPE - 1))) != 0, 1);

  /* default: the writer's group dies immediately, WTERMSIG = SIGPIPE */
  p->sig_handler[HO_SIGPIPE] = HO_SIG_DFL;
  p->sig_pending = 0;
  EXPECT_EQ(signal_epipe(p), 1);
  EXPECT_EQ(p->state, PROC_STATE_EXITED);
  EXPECT_EQ(p->exit_status, HO_SIGPIPE);

  process_free(pid);
}

/* Row-85 storage side: env blob readback + truncation contract. */
static void test_p5_env_readback(void) {
  tests_run++;
  uart_puts("  Running test_p5_env_readback...\n");

  int pid = process_create();
  struct process *p = process_group(process_get_pcb(pid));

  EXPECT_EQ(sys_readenv(p, -1, 0, 0), 0); /* empty set */
  EXPECT_EQ(sys_readenv(p, 0, 0, 0), -EINVAL);

  const char blob[] = "A=1\0B=22";
  for (unsigned i = 0; i < sizeof(blob); i++)
    p->env[i] = blob[i];
  p->envc = 2;

  EXPECT_EQ(sys_readenv(p, -1, 0, 0), 2);

  char buf[16];
  int len = sys_readenv(p, 0, buf, sizeof(buf));
  EXPECT_EQ(len, 3);
  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[2], '1');
  EXPECT_EQ(buf[3], '\0');
  len = sys_readenv(p, 1, buf, sizeof(buf));
  EXPECT_EQ(len, 4);
  EXPECT_EQ(buf[1], '=');
  EXPECT_EQ(buf[3], '2');
  EXPECT_EQ(sys_readenv(p, 2, buf, sizeof(buf)), -EINVAL);

  /* truncation: size clamps and still NUL-terminates */
  len = sys_readenv(p, 1, buf, 3);
  EXPECT_EQ(len, 2);
  EXPECT_EQ(buf[2], '\0');

  process_free(pid);
}

/* D5.3/D5.4: unified reap delivery on the KILL path -- a parent parked in
 * WAIT_CHILD wakes with the status when a third party kills the child
 * (the confirmed wakeup gap), and SIGCHLD goes pending. */
static void test_p5_reap_delivery_on_kill(void) {
  tests_run++;
  uart_puts("  Running test_p5_reap_delivery_on_kill...\n");

  int ppid = process_create();
  int ppid2 = process_create();
  int cpid = process_create();
  EXPECT_EQ((ppid >= 0 && ppid2 >= 0 && cpid >= 0), 1);
  struct process *parent = process_get_pcb(ppid);
  struct process *parent2 = process_get_pcb(ppid2);
  struct process *child = process_get_pcb(cpid);

  child->parent_pid = ppid;
  /* Park both parents; #1 wants exactly this child, #2 wants another pid. */
  parent->state = PROC_STATE_WAIT_CHILD;
  parent->context[0] = cpid;
  parent->context[1] = USER_VIRT_BASE + 0x120;
  parent2->state = PROC_STATE_WAIT_CHILD;
  parent2->context[0] = 9999;

  /* Third-party default kill (SIGTERM without a handler). */
  EXPECT_EQ(process_signal(cpid, HO_SIGTERM), 0);

  /* Parent #1 woke with the reap result; the status word landed in its
     region; the zombie slot was delivered (FREE). */
  EXPECT_EQ(parent->state, PROC_STATE_READY);
  EXPECT_EQ((int)parent->context[0], cpid);
  EXPECT_EQ(child->state, PROC_STATE_FREE);
  EXPECT_EQ(*(int *)(parent->user_phys_base + 0x120), HO_SIGTERM);
  EXPECT_EQ((parent->sig_pending & (1u << (HO_SIGCHLD - 1))) != 0, 1);

  /* Parent #2 was waiting for a different pid: still parked. */
  EXPECT_EQ(parent2->state, PROC_STATE_WAIT_CHILD);

  process_free(ppid2);
  process_free(ppid);
  process_free(cpid);
}

/* D5.1/D5.7: waitpid option validation, EFAULT, and the WNOHANG reap. */
static void test_p5_waitpid_validation(void) {
  tests_run++;
  uart_puts("  Running test_p5_waitpid_validation...\n");

  int ppid = process_create();
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, ppid);

  struct trap_frame tf;
  for (unsigned i = 0; i < sizeof(tf) / sizeof(uint64_t); i++)
    ((uint64_t *)&tf)[i] = 0;

  /* unknown option bits -> -EINVAL (before any child scan) */
  tf.regs[0] = -1;
  tf.regs[1] = USER_VIRT_BASE + 0x200;
  tf.regs[2] = 4;
  EXPECT_EQ(process_waitpid(&tf), -EINVAL);

  /* out-of-region status pointer -> -EFAULT */
  tf.regs[2] = 0;
  tf.regs[1] = 0x1000;
  EXPECT_EQ(process_waitpid(&tf), -EFAULT);

  /* no children at all -> -ECHILD; WUNTRACED is accepted (ignored) */
  tf.regs[1] = USER_VIRT_BASE + 0x200;
  tf.regs[2] = 1;
  struct process *wcaller = process_get_pcb(ppid);
  EXPECT_EQ(wcaller->echild_logged, 0); /* diagnostic not yet spoken */
  EXPECT_EQ(process_waitpid(&tf), -ECHILD);
  /* CI 2026-10-26: the ECHILD diagnostic must speak ONCE per PCB.  A
     long-lived parent polls waitpid() in its main loop (desktop_reap_
     children() every iteration), so ECHILD-with-no-children is its steady
     state: one line per CALL produced 811k lines in a 12-minute ARM T3 run
     (99.97% of the serial), saturated the UART, and starved the shell test
     into a false "SHELLTEST WATCHDOG: protocol stalled 25 s" FAIL.  The
     latch is the observable here -- a unit test cannot capture the console --
     and it is what makes the second and every later call silent. */
  EXPECT_EQ(wcaller->echild_logged, 1);
  tf.regs[2] = 1 | 2;
  EXPECT_EQ(process_waitpid(&tf), -ECHILD);
  EXPECT_EQ(wcaller->echild_logged, 1); /* still one, not a running count */

  /* an EXITED child reaps via WNOHANG (status ptr NULL: the write itself
     is covered by the kill-path test above and by PROC_T on device) */
  int cpid = process_create();
  struct process *ch = process_get_pcb(cpid);
  ch->parent_pid = ppid;
  ch->state = PROC_STATE_EXITED;
  ch->exit_status = 42 << 8;
  tf.regs[0] = -1;
  tf.regs[1] = 0;
  tf.regs[2] = 1;
  EXPECT_EQ(process_waitpid(&tf), cpid);
  EXPECT_EQ(ch->state, PROC_STATE_FREE);

  set_current_process_pid(0, old_pid);
  process_free(ppid);
}

/* W2-5 (selfhost): process_wait4 -- SYS_WAIT4 core.  The rusage out-param
 * rides tf->regs[3]; a bad buffer -> -EFAULT; a reap fills the caller's
 * Linux-shaped rusage blob (144 bytes: two timevals, then 14 longs) with
 * documented values (zeros except ru_maxrss = the v1 resident block size
 * in KiB at byte offset 32). */
static void test_p5_wait4_rusage(void) {
  tests_run++;
  uart_puts("  Running test_p5_wait4_rusage...\n");

  int ppid = process_create();
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, ppid);
  struct process *par = process_get_pcb(ppid);

  struct trap_frame tf;
  for (unsigned i = 0; i < sizeof(tf) / sizeof(uint64_t); i++)
    ((uint64_t *)&tf)[i] = 0;

  /* Out-of-region rusage pointer -> -EFAULT (in-window status OK, no
     children yet; the rusage check precedes any child scan). */
  tf.regs[0] = -1;
  tf.regs[1] = USER_VIRT_BASE + 0x200;
  tf.regs[2] = 1;            /* WNOHANG */
  tf.regs[3] = 0x1000;       /* bogus rusage VA */
  EXPECT_EQ(process_wait4(&tf), -EFAULT);

  /* A reapable v1 child (process_create also sets user_phys_base). */
  int cpid = process_create();
  struct process *ch = process_get_pcb(cpid);
  ch->parent_pid = ppid;
  ch->state = PROC_STATE_EXITED;
  ch->exit_status = 7 << 8;

  uint64_t st_va = USER_VIRT_BASE + 0x400; /* status out */
  uint64_t ru_va = USER_VIRT_BASE + 0x500; /* rusage out, 144 B */
  tf.regs[0] = cpid;
  tf.regs[1] = st_va;
  tf.regs[2] = 1;
  tf.regs[3] = ru_va;
  EXPECT_EQ(process_wait4(&tf), cpid);
  EXPECT_EQ(ch->state, PROC_STATE_FREE);

  /* Status word: the immediate-reap path writes it through the direct
     user VA (the P2 contract -- the waiting process's own context where
     USER_VIRT_BASE is live).  At unit test time USER_VIRT_BASE is the
     boot identity map, so read it back at the SAME VA.  (The rusage
     store below deliberately goes through user_phys_base -- the v1
     physical translation -- which is why THAT one is inspected via the
     parent's physical block.) */
  EXPECT_EQ(*(int *)(uint64_t)st_va, 7 << 8);

  /* The rusage blob landed at ru_va: everything zero except the 8-byte
     ru_maxrss at byte offset 32 (USER_REGION_SIZE/1024 = 0x8000 -> one
     non-zero byte). */
  uint8_t *ru = (uint8_t *)(par->user_phys_base + (ru_va - USER_VIRT_BASE));
  int nonzero = 0;
  for (int k = 0; k < (int)HO_RUSAGE_SIZE; k++)
    if (ru[k]) nonzero++;
  EXPECT_EQ(nonzero, 1);
  int64_t maxrss = 0;
  for (int k = 0; k < 8; k++)
    maxrss |= ((int64_t)ru[32 + k]) << (8 * k);
  EXPECT_EQ(maxrss, (int64_t)(USER_REGION_SIZE / 1024));

  set_current_process_pid(0, old_pid);
  process_free(ppid);
}

/* W2-10 (selfhost): the exec argv envelope must comfortably exceed the
 * old 256 B / 32-arg blob -- a 60-arg, ~480 B blob (dead at the old
 * limits) round-trips through proc_set_argv_array + sys_readargv. */
static void test_w2_argv_envelope(void) {
  tests_run++;
  uart_puts("  Running test_w2_argv_envelope...\n");

  /* Compile-time contract, made explicit at runtime. */
  EXPECT_EQ(HO_EXEC_ARGV_LEN >= 2048, 1);
  EXPECT_EQ(HO_EXEC_MAX_ARGS >= 64, 1);

  int pid = process_create();
  struct process *p = process_group(process_get_pcb(pid));
  EXPECT_EQ((p != 0), 1);

  /* 60 args of ~8 bytes each = ~480 B: over the old 256 B blob and the
     old 32-arg cap, under the new 4096 B / 128-arg envelope. */
  char blob[HO_EXEC_ARGV_LEN];
  int nargs = 60;
  int pos = 0;
  for (int i = 0; i < nargs; i++) {
    blob[pos++] = 'A';
    blob[pos++] = 'R';
    blob[pos++] = 'G';
    blob[pos++] = '-';
    blob[pos++] = (char)('0' + (i / 100) % 10);
    blob[pos++] = (char)('0' + (i / 10) % 10);
    blob[pos++] = (char)('0' + i % 10);
    blob[pos++] = '\0';
  }
  blob[pos] = '\0';
  EXPECT_EQ(pos > 256, 1);        /* provably past the old 256 B blob */

  EXPECT_EQ(proc_set_argv_array(p, blob, nargs), 0);
  EXPECT_EQ(p->eargc, nargs);
  EXPECT_EQ(sys_readargv(p, -1, 0, 0), nargs);

  char buf[64];
  int got = sys_readargv(p, 0, buf, sizeof buf);     /* "ARG-000" */
  EXPECT_EQ(got, 7);
  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[6], '0');
  got = sys_readargv(p, nargs - 1, buf, sizeof buf); /* "ARG-059" */
  EXPECT_EQ(got, 7);
  EXPECT_EQ(buf[0], 'A');
  EXPECT_EQ(buf[6], '9');
  EXPECT_EQ(sys_readargv(p, nargs, buf, sizeof buf), -1); /* out of range */

  process_free(pid);
}
static void test_p5_exec_env_and_apply(void) {
  tests_run++;
  uart_puts("  Running test_p5_exec_env_and_apply...\n");

  int pid = process_create();
  EXPECT_EQ((pid >= 0 && pid < MAX_PROCESSES), 1);
  struct process *p = process_group(process_get_pcb(pid));

  /* envp == NULL -> empty blob, count 0; bad array pointer -> -1 */
  char blob[HO_ENV_LEN];
  int cnt = -1;
  EXPECT_EQ(process_read_envp(p, 0, blob, HO_ENV_LEN, &cnt), 0);
  EXPECT_EQ(cnt, 0);
  EXPECT_EQ(blob[0], '\0');
  EXPECT_EQ(process_read_envp(p, (const char *const *)0x1000, blob,
                               HO_ENV_LEN, &cnt), -1);

  /* Two open fds, first CLOEXEC via fcntl(F_SETFD). */
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);
  int fd1 = file_open(p, "TEST.TXT", 0);
  int fd2 = file_open(p, "TEST.TXT", 0);
  EXPECT_EQ((fd1 >= 0 && fd2 >= 0), 1);
  EXPECT_EQ(file_fcntl(p, fd1, K_F_GETFD, 0), 0);
  EXPECT_EQ(file_fcntl(p, fd1, K_F_SETFD, K_FD_CLOEXEC), 0);
  EXPECT_EQ(file_fcntl(p, fd1, K_F_GETFD, 0), 1);
  EXPECT_EQ(file_fcntl(p, fd2, K_F_GETFD, 0), 0);

  /* Dispositions: caught -> DFL, ignored stays ignored, pending cleared. */
  p->sig_handler[HO_SIGTERM] = USER_VIRT_BASE + 0x5000;
  p->sig_handler[HO_SIGINT] = HO_SIG_IGN;
  p->sig_pending = 0xFFFFFFFFu;

  const char envblob[] = "A=1\0B=22\0";
  process_exec_apply(p, envblob, 2);

  EXPECT_EQ((p->fd_cloexec & (1ull << fd1)) != 0, 0); /* bit cleared */
  EXPECT_EQ(p->open_fds[fd1], -1);                  /* sweep closed it */
  EXPECT_EQ((p->open_fds[fd2] != -1), 1);           /* others survive */
  EXPECT_EQ(p->envc, 2);
  EXPECT_EQ(p->env[0], 'A');
  EXPECT_EQ(p->env[2], '1');
  EXPECT_EQ(p->env[4], 'B');
  EXPECT_EQ(p->sig_handler[HO_SIGTERM], 0);           /* caught -> DFL */
  EXPECT_EQ(p->sig_handler[HO_SIGINT], HO_SIG_IGN);   /* ignored stays */
  EXPECT_EQ(p->sig_pending, 0);
  EXPECT_EQ(sys_readenv(p, -1, 0, 0), 2);

  file_close(p, fd2);
  set_current_process_pid(0, old_pid);
  process_free(pid);
}

/* Famine fail-fast regression (P8.2 soak): with the process table full,
 * a CHILD load (real caller pid) must give up with -EAGAIN (folded to
 * -1 by the v2 wrapper) after the short child retry budget instead of
 * retrying ~30 minutes like the pre-fix loop — that outlasted the
 * transient, flooded the console, and wedged the soak at 1 round.  The
 * 2 s budget is real wall time (timer_get_ms reads the ARM generic
 * counter), so the test self-bounds; the elapsed assertion would catch a
 * regression back to the unbounded retry. */
static void test_loader_fail_fast_full_table(void) {
  tests_run++;
  uart_puts("  Running test_loader_fail_fast_full_table...\n");

  /* Snapshot + poison every PCB slot so process_create_v2() must fail.
     Mirror the slot-reclaim test's poison fields (no stale block/AS
     pointers for the reclaim scan to touch). */
  int saved[MAX_PROCESSES];
  for (int i = 0; i < MAX_PROCESSES; i++) {
    saved[i] = -1;
    struct process *q = process_get_pcb(i);
    if (!q || q->state != PROC_STATE_FREE)
      continue;
    saved[i] = q->state;
    q->state = PROC_STATE_ALLOCATED; /* poison: not reclaimable */
    q->is_thread = 0;
    q->live_threads = 1;
    q->phys_block_idx = -1;
    q->as = 0;
    q->futex_uaddr = 0;
    q->wake_ms = 0;
  }

  uint64_t t0 = timer_get_ms();
  /* Child spawn (caller_pid >= 0): must give up (bounded), not loop for
     ~30 minutes. */
  int r = load_and_run_program_v2("NOPE.BIN", -1, -1, -1, 1, 0);
  uint64_t elapsed = timer_get_ms() - t0;

  EXPECT_EQ(r, -1);                  /* -EAGAIN folded by the wrapper */
  EXPECT_EQ((int)(elapsed < 25000), 1); /* child budget is 20 s, not 30 min */

  /* Restore the table so later suites see a clean state. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *q = process_get_pcb(i);
    if (!q || saved[i] == -1)
      continue;
    q->state = saved[i];
    q->is_thread = 0;
    q->live_threads = 0;
    q->phys_block_idx = -1;
    q->as = 0;
  }
}

/* ---------------------------------------------------------------------
 * P8.2 class-B idle-fix regression: the lostwake stale-claim reclaim
 * decision.  Covers the frozen-soak signature — a RUNNING slot whose
 * every claimer has a stale heartbeat (a CPU that consumed the wake but
 * never ran the body and never returned to the scheduler) with the rest
 * of the table drained — and its false sides (live claimer / busy table)
 * so the historical x64 double-run (reclaiming a genuinely-running
 * process whose owner was merely print-stalled, tearing its frames) can
 * never come back without this test failing.  Runs in EL1 before the
 * scheduler, so no 30-minute soak is needed to cover the class.
 * ------------------------------------------------------------------- */
static void test_lostwake_stale_claim_reclaim(void) {
  tests_run++;
  uart_puts("  Running test_lostwake_stale_claim_reclaim...\n");

  extern int cpu_current_pids[MAX_CPUS];
  extern volatile uint64_t cpu_heartbeat_ms[MAX_CPUS];
  extern spinlock_t proc_lock;

  /* Snapshot everything the test touches. */
  int saved_state[MAX_PROCESSES];
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *q = process_get_pcb(i);
    saved_state[i] = q ? q->state : PROC_STATE_FREE;
  }
  int saved_claims[MAX_CPUS];
  uint64_t saved_hb[MAX_CPUS];
  for (int c = 0; c < MAX_CPUS; c++) {
    saved_claims[c] = cpu_current_pids[c];
    saved_hb[c] = cpu_heartbeat_ms[c];
  }

  const int k = 5;  /* the stuck slot (pid == slot index) */
  const int c3 = 3; /* the vanished claimer */
  /* Injected clock for the decision: a fabricated mid-run instant, so the
     test is deterministic on both arches (x64's early-boot clock can be a
     small value; ARM's is uptime-based).  Stale == heartbeat older than
     LOSTWAKE_DEAD_OWNER_MS at this instant. */
  const uint64_t now_ms = 1000000;
  const uint64_t stale_hb = now_ms - LOSTWAKE_DEAD_OWNER_MS - 1;

  /* Clean the table: drain every other slot so the drain check sees
     exactly what each case sets up. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (i == k)
      continue;
    struct process *q = process_get_pcb(i);
    if (q)
      q->state = PROC_STATE_THREAD_DONE;
  }

  uint64_t f = spinlock_acquire_irqsave(&proc_lock);

  /* Case 1: the frozen-soak signature — RUNNING, stale claim, drained:
     must reclaim. */
  process_get_pcb(k)->state = PROC_STATE_RUNNING;
  cpu_current_pids[c3] = k;
  cpu_heartbeat_ms[c3] = stale_hb;
  EXPECT_EQ(lostwake_stale_claim_reclaimable_at(k, now_ms), 1);

  /* Case 2: same, but the claimer's heartbeat is fresh (the process is
     genuinely running on a live core): must NOT reclaim — this is the
     false-positive that double-ran pids in the old stale-heartbeat
     reaper. */
  cpu_heartbeat_ms[c3] = now_ms;
  EXPECT_EQ(lostwake_stale_claim_reclaimable_at(k, now_ms), 0);
  cpu_heartbeat_ms[c3] = stale_hb;

  /* Case 3: a busy table — another slot is READY: must NOT reclaim. */
  process_get_pcb(6)->state = PROC_STATE_READY;
  EXPECT_EQ(lostwake_stale_claim_reclaimable_at(k, now_ms), 0);
  process_get_pcb(6)->state = PROC_STATE_THREAD_DONE;

  /* Case 4: another slot is RUNNING with a LIVE claimer (a live core is
     executing it): must NOT reclaim — a requeue of k could double-run. */
  process_get_pcb(6)->state = PROC_STATE_RUNNING;
  cpu_current_pids[4] = 6;
  cpu_heartbeat_ms[4] = now_ms;
  EXPECT_EQ(lostwake_stale_claim_reclaimable_at(k, now_ms), 0);
  cpu_current_pids[4] = -1;
  process_get_pcb(6)->state = PROC_STATE_THREAD_DONE;

  /* Case 5: RUNNING with NO claimer: the existing no-claimer reaper path
     owns this; the stale path must not claim it. */
  cpu_current_pids[c3] = -1;
  EXPECT_EQ(lostwake_stale_claim_reclaimable_at(k, now_ms), 0);
  cpu_current_pids[c3] = k;

  /* Case 6: not RUNNING: never reclaimable via the stale path. */
  process_get_pcb(k)->state = PROC_STATE_READY;
  EXPECT_EQ(lostwake_stale_claim_reclaimable_at(k, now_ms), 0);
  process_get_pcb(k)->state = PROC_STATE_RUNNING;
  EXPECT_EQ(lostwake_stale_claim_reclaimable_at(k, now_ms), 1); /* drained again */

  spinlock_release_irqrestore(&proc_lock, f);

  /* Restore the snapshot. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *q = process_get_pcb(i);
    if (q)
      q->state = saved_state[i];
  }
  for (int c = 0; c < MAX_CPUS; c++) {
    cpu_current_pids[c] = saved_claims[c];
    cpu_heartbeat_ms[c] = saved_hb[c];
  }
}

/* GB-057 (Coordinator, 2026-10-26): the wave-end predicate must not count a
 * RUNNING slot that only a HALTED CPU claims as live work.  Measured on the
 * intel T3 tier: two CPUs printed `resume call RETURNED (must never happen)`,
 * called cpu_halt_record() and stopped, still claiming pids 15 and 20; those two
 * slots then kept `any_alive` true for the rest of the run while six cores idled
 * in safe_wfi, so the tier never reached `System halt`.
 *
 * The rule is deliberately narrow and keys on the POSITIVE halt record, not on a
 * timeout: a slot with any live claimer, or with no claimer at all, is live work
 * exactly as before (the no-claimer case belongs to the LOSTWAKE reaper). */
static void test_halted_claimer_is_not_live_work(void) {
  tests_run++;
  uart_puts("  Running test_halted_claimer_is_not_live_work...\n");
  extern volatile uint8_t cpu_halted[];
  const int k = 7;
  int saved_state[MAX_PROCESSES];
  int saved_claims[MAX_CPUS];
  uint8_t saved_halted[MAX_CPUS];
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *q = process_get_pcb(i);
    saved_state[i] = q ? q->state : PROC_STATE_FREE;
  }
  for (int c = 0; c < MAX_CPUS; c++) {
    saved_claims[c] = cpu_current_pids[c];
    saved_halted[c] = cpu_halted[c];
    cpu_current_pids[c] = -1;
    cpu_halted[c] = 0;
  }
  process_get_pcb(k)->state = PROC_STATE_RUNNING;

  /* Case 1: no claimer -- not this function's case (the reaper owns it). */
  EXPECT_EQ(slot_claimed_only_by_halted_cpus(k), 0);

  /* Case 2: one claimer, alive: genuine live work. */
  cpu_current_pids[3] = k;
  EXPECT_EQ(slot_claimed_only_by_halted_cpus(k), 0);

  /* Case 3: that claimer recorded its halt: dead by construction. */
  cpu_halted[3] = 1;
  EXPECT_EQ(slot_claimed_only_by_halted_cpus(k), 1);

  /* Case 4: a second, LIVE claimer appears: live work again (the halt of one
     CPU must not let the wave end under a CPU that is still running the slot). */
  cpu_current_pids[5] = k;
  EXPECT_EQ(slot_claimed_only_by_halted_cpus(k), 0);

  /* Case 5: both claimers halted: dead. */
  cpu_halted[5] = 1;
  EXPECT_EQ(slot_claimed_only_by_halted_cpus(k), 1);

  /* Case 6: bounds. */
  EXPECT_EQ(slot_claimed_only_by_halted_cpus(-1), 0);
  EXPECT_EQ(slot_claimed_only_by_halted_cpus(MAX_PROCESSES), 0);

  /* Restore. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *q = process_get_pcb(i);
    if (q)
      q->state = saved_state[i];
  }
  for (int c = 0; c < MAX_CPUS; c++) {
    cpu_current_pids[c] = saved_claims[c];
    cpu_halted[c] = saved_halted[c];
  }
}

/* w5 (ii): a fault-killed child must NEVER report status 0.  The trap
 * fault paths (EC=0x00/0x3C/generic) used to call process_exit(tf), which
 * reaped the stale x0 left in the frame -- a crash could false-pass as
 * rc=0 (w4-clang t23).  They now route through process_fault_set_status,
 * which stamps PROCESS_STATUS_SIGNAL|signo, and group_teardown maps that
 * marker to the waitpid SIGNAL byte (always non-zero). */
static void test_fault_kill_status_nonzero(void) {
  tests_run++;
  uart_puts("  Running test_fault_kill_status_nonzero...\n");

  /* 1) The encoder: even with a stale x0 == 0 in the frame (the exact
     t23 shape: the tool faulted at entry, x0 was 0), the stamped code
     carries the marker and turns into a non-zero signal-shaped status
     through group_teardown's mapping (process.c group_teardown). */
  struct trap_frame tf;
  tf.regs[0] = 0; /* stale "success" */
  process_fault_set_status(&tf, 11);
  uint64_t code = tf.regs[0];
  EXPECT_EQ((int)(code & PROCESS_STATUS_SIGNAL) != 0, 1);
  /* group_teardown (:552): marker set -> waitpid status = (code & 0x7f) */
  uint32_t st = ((int)(code & PROCESS_STATUS_SIGNAL))
                  ? (uint32_t)((int)code & 0x7f)
                  : ((uint32_t)((int)code & 0xff) << 8);
  EXPECT_EQ((int)st != 0, 1);
  EXPECT_EQ((int)st, 11); /* SIGSEGV */

  /* every signal number the fault paths use must encode non-zero */
  {
    struct trap_frame tf2;
    tf2.regs[0] = 0;
    process_fault_set_status(&tf2, 4);
    EXPECT_EQ((int)((tf2.regs[0] & PROCESS_STATUS_SIGNAL) != 0), 1); /* SIGILL */
  }

  /* 2) End-to-end: the default-disposition kill path (what the trap
     handler's process_fault_exit invokes) lands a non-zero signal-shaped
     status on the dead group, exactly what waitpid() reports. */
  int pid = process_create();
  struct process *p = process_group(process_get_pcb(pid));
  p->sig_handler[HO_SIGSEGV] = HO_SIG_DFL;
  EXPECT_EQ(process_signal(pid, HO_SIGSEGV), 0);
  EXPECT_EQ(p->state, PROC_STATE_EXITED);
  EXPECT_EQ(p->exit_status != 0, 1);
  EXPECT_EQ(p->exit_status, HO_SIGSEGV);
  process_free(pid);
}

/* SEC-OBS-A (security-plan.md observation A): pid doubles as an index into
 * fixed kernel pools (spawn_args_pool, spawn_ex_pool, select_wait, ...).
 * The invariant that makes this safe: pid is a dense slot index — every
 * proc_table slot has a pcb whose pid equals the slot number, and no
 * lookup succeeds outside [0, MAX_PROCESSES). */
static void test_pid_slot_invariant(void) {
  tests_run++;
  uart_puts("  Running test_pid_slot_invariant...\n");

  for (int pid = 0; pid < MAX_PROCESSES; pid++) {
    struct process *pcb = process_get_pcb(pid);
    EXPECT_EQ((pcb != 0), 1);
    if (pcb) EXPECT_EQ(pcb->pid, pid);
  }
  EXPECT_EQ((process_get_pcb(-1) == 0), 1);
  EXPECT_EQ((process_get_pcb(MAX_PROCESSES) == 0), 1);
}

void process_test_suite(void) {
  uart_puts("process_test_suite:\n");
  test_process_init_and_create();
  test_pid_slot_invariant(); /* SEC-OBS-A: pid == bounded slot index */
  test_process_set_entry();
  test_process_num_cpus();
  test_process_thread_group();
  test_process_thread_validation();
  test_process_thread_slot_reclaim();
  test_loader_fail_fast_full_table();
  test_lostwake_stale_claim_reclaim();
  test_halted_claimer_is_not_live_work(); /* GB-057 */
  test_process_futex_machine();
  test_process_set_tls();
  test_p5_sigaction_storage();
  test_p5_kill_semantics();
  test_p5_sigpipe_semantics();
  test_fault_kill_status_nonzero(); /* w5 (ii) */
  test_p5_env_readback();
  test_p5_reap_delivery_on_kill();
  test_p5_waitpid_validation();
  test_p5_wait4_rusage();      /* W2-5 (selfhost) */
  test_w2_argv_envelope();     /* W2-10 (selfhost) */
  test_p5_exec_env_and_apply();
}

#endif // KERNEL_MODE_UNIT_TEST
