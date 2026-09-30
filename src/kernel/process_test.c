#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "process.h"
#include "errno.h"

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

void process_test_suite(void) {
  uart_puts("process_test_suite:\n");
  test_process_init_and_create();
  test_process_set_entry();
  test_process_num_cpus();
  test_process_thread_group();
  test_process_thread_validation();
  test_process_thread_slot_reclaim();
  test_process_futex_machine();
  test_process_set_tls();
}

#endif // KERNEL_MODE_UNIT_TEST
