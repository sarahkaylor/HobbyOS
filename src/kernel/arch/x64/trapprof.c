/* trapprof.c -- C1-k7: the syscall path's cycles, segment by segment
 * (docs/cp-k7.md §2).  Test kernels only; see trapprof.h.
 *
 * Test syscall L_SYS_hb_trapprof (0x4B37, linux_compat.c's l_tab_core):
 *   op 0  reset and measure the caller's thread group
 *   op 1  stop
 *   op 2  copy the totals to arg1 (arg2 bytes): u64 words
 *         [0] TSC ticks per ms   [1] samples
 *         [2 .. 2+TP_N)          cycles per segment (sum over CPUs)
 *         [2+TP_N]               entry leg cycles: user rdtsc before `syscall`
 *                                to TP_BEGIN (op 3 only)
 *         [3+TP_N]               entry leg samples
 *         [4+TP_N]               exit leg cycles: TP_END to the user rdtsc
 *                                after the return (op 3 only)
 *         [5+TP_N]               exit leg samples
 *   op 3  a null call: arg1 = the user's rdtsc just before this `syscall`,
 *         arg2 = the user's rdtsc just after the previous op-3 call returned.
 */
#if defined(KERNEL_MODE_TEST)
#include "trapprof.h"
#include "process.h"
#include "arch/trap.h"
#include "linux_compat.h"
#include <stdint.h>

volatile uint32_t trap_prof_on;
static volatile int trap_prof_tgid;

struct tp_pc {
  uint64_t last;
  uint64_t acc[TP_N];
  uint64_t n;
  uint64_t ent_sum, ent_n, ext_sum, ext_n;
  uint64_t end_tsc;   /* TP_END of the last sample on this CPU */
  int end_pid;        /* ... and its thread */
  uint32_t active;
} __attribute__((aligned(64)));
static struct tp_pc tp[MAX_CPUS];

static inline uint64_t tp_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

void trap_prof_begin(struct trap_frame *tf) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS) return;
  struct tp_pc *s = &tp[c];
  uint64_t now = tp_rdtsc();
  struct process *p = current_process();
  /* a nested trap (an interrupt inside the syscall) spoils the open sample */
  s->active = 0;
  if (!tf || tf->vector != 0x80 || (tf->cs & 3) != 3 || !p || p->tgid != trap_prof_tgid)
    return;
  s->active = 1;
  s->last = now;
  if (tf->regs[0] == 0x4B37 && tf->regs[5] == 3) {
    uint64_t u0 = tf->regs[4], u1 = tf->regs[3]; /* rsi, rdx */
    if (u0 && now > u0 && now - u0 < 1000000) { s->ent_sum += now - u0; s->ent_n++; }
    if (u1 && s->end_pid == p->pid && u1 > s->end_tsc && u1 - s->end_tsc < 1000000) {
      s->ext_sum += u1 - s->end_tsc;
      s->ext_n++;
    }
  }
}

void trap_prof_mark(int i) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS || i <= 0 || i >= TP_N) return;
  struct tp_pc *s = &tp[c];
  if (!s->active) return;
  uint64_t now = tp_rdtsc();
  s->acc[i] += now - s->last;
  s->last = now;
  if (i == TP_END) {
    s->n++;
    s->active = 0;
    s->end_tsc = now;
    struct process *p = current_process();
    s->end_pid = p ? p->pid : -1;
  }
}

/* the test syscall's body (linux_compat.c) */
int64_t trap_prof_op(struct process *p, int op, uint64_t a1, uint64_t a2) {
  extern uint64_t timer_tsc_per_ms(void);
  switch (op) {
  case 0:
    trap_prof_on = 0;
    for (unsigned c = 0; c < MAX_CPUS; c++) {
      volatile uint8_t *b = (volatile uint8_t *)&tp[c];
      for (unsigned k = 0; k < sizeof tp[c]; k++) b[k] = 0;
    }
    trap_prof_tgid = p ? p->tgid : -1;
    __atomic_store_n(&trap_prof_on, 1, __ATOMIC_SEQ_CST);
    return 0;
  case 1:
    __atomic_store_n(&trap_prof_on, 0, __ATOMIC_SEQ_CST);
    return 0;
  case 2: {
    uint64_t w[6 + TP_N];
    for (unsigned k = 0; k < sizeof w / sizeof w[0]; k++) w[k] = 0;
    w[0] = timer_tsc_per_ms();
    for (unsigned c = 0; c < MAX_CPUS; c++) {
      w[1] += tp[c].n;
      for (int i = 0; i < TP_N; i++) w[2 + i] += tp[c].acc[i];
      w[2 + TP_N] += tp[c].ent_sum;
      w[3 + TP_N] += tp[c].ent_n;
      w[4 + TP_N] += tp[c].ext_sum;
      w[5 + TP_N] += tp[c].ext_n;
    }
    uint64_t n = a2 < sizeof w ? a2 : sizeof w;
    if (l_copy_to_user(p, a1, w, n) != 0) return -14;
    return (int64_t)n;
  }
  case 3:
    return 0;
  }
  return -22;
}
#endif
