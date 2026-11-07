#ifndef RW_WATCH_H
#define RW_WATCH_H

/*
 * rw_watch — one x86 hardware write breakpoint on one user word.
 *
 * WHY THIS EXISTS
 * ---------------
 * M6's park is a pthread_mutex_lock on 0x10C00964A0 (libpthread
 * __lll_lock_wait, FUTEX_WAIT val=2, infinite) in a 4 MiB anonymous arena.
 * Every observation available from the syscall side is a *read*: the futex
 * trace names the waiter, the userspace recorder names callers that went
 * through the PLT, and neither can see who *wrote* the lock word.  A normal
 * glibc mutex keeps no owner tid (__kind=0), so the holder is invisible.
 *
 * A read of the word cannot answer it; a write trap can.  This module arms
 * DR0 on RW_WATCH_ADDR with DR7's 4-byte data-write encoding and reports
 * every write as (pid, cpu, rip, new value, registers).  The rip is the
 * instruction after the store (data breakpoints are traps), so the writer is
 * named by module+offset, exactly like a fault PC.
 *
 * Compile-time only: with RW_WATCH_ADDR undefined (0) not a single byte of
 * this is linked into the shipping kernel.  A diagnostic boot passes
 *     make ... RW_WATCH_ADDR=0x10C00964A0
 * (see the Makefile's RW_WATCH_ADDR knob).
 *
 * Per-CPU because DR0/DR7 are per-CPU registers and this kernel does not
 * context-switch them: each CPU arms itself the first time it takes any
 * interrupt, so the whole machine is covered within one tick.
 */

#include <stdint.h>

struct trap_frame;

#ifndef RW_WATCH_ADDR
#define RW_WATCH_ADDR 0UL
#endif

/* How many write rows reach the console before the reporter goes quiet. */
#define RW_WATCH_ROWS 256

/* Program DR0/DR7 on this CPU if the watch is compiled in and not yet armed.
 * Cheap when off (empty inline); safe to call from any interrupt path. */
void rw_watch_arm_cpu(void);

/* Handle a #DB.  Returns 1 when it was this watchpoint (frame consumed, the
 * caller must return to the interrupted context); 0 when it was not ours, so
 * the normal exception path still sees it. */
int rw_watch_db(struct trap_frame *tf);

#endif /* RW_WATCH_H */
