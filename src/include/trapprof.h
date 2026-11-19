/* trapprof.h -- C1-k7: where one syscall's cycles go (x86_64, test kernels).
 *
 * TP_MARK(i) closes segment i of the syscall path with an rdtsc: segment i is
 * the time from the previous mark to this one.  Only a syscall (vector 0x80)
 * of the thread group named by the probe is measured, and only on a CPU
 * whose sample was opened by TP_BEGIN and not disturbed by a nested trap.
 * Off unless linux_probe_sysc switches it on (test syscall 0x4B37, see
 * arch/x64/trapprof.c); when off a mark is one load of a read-mostly word.
 * Release kernels compile every mark out. */
#ifndef TRAPPROF_H
#define TRAPPROF_H

#include <stdint.h>

enum {
  TP_BEGIN = 0,      /* general_interrupt_handler entry */
  TP_TRAP_ENTER,     /* 1: ru_trap_enter, trap depth, lock audit */
  TP_INNER_DIAG,     /* 2: badvec stamp, torn-frame check, watchdog_tick */
  TP_INNER_ROUTE,    /* 3: rw_watch, wake-all drain, vector chain */
  TP_SYS_PRE,        /* 4: sync_lower bookkeeping before dispatch */
  TP_DISP_PRE,       /* 5: dispatch prologue (skew check, table lookup) */
  TP_HANDLER,        /* 6: the handler itself */
  TP_DISP_POST,      /* 7: FAT exit checks */
  TP_SIG_EXIT,       /* 8: process_signal_trap_exit */
  TP_UNWIND,         /* 9: back out, lock audit, sync-wake flush */
  TP_END,            /* 10: ru_trap_exit */
  TP_N
};

#if defined(__x86_64__) && defined(KERNEL_MODE_TEST)
struct trap_frame;
extern volatile uint32_t trap_prof_on;
void trap_prof_begin(struct trap_frame *tf);
void trap_prof_mark(int i);
#define TP_BEGIN_TF(tf) do { if (__builtin_expect(trap_prof_on != 0, 0)) trap_prof_begin(tf); } while (0)
#define TP_MARK(i) do { if (__builtin_expect(trap_prof_on != 0, 0)) trap_prof_mark(i); } while (0)
#else
#define TP_BEGIN_TF(tf) do { } while (0)
#define TP_MARK(i) do { } while (0)
#endif

#endif
