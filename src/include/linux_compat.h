#ifndef LINUX_COMPAT_H
#define LINUX_COMPAT_H

/*
 * Linux x86_64 ABI compat layer — dispatch contract (docs/linux-abi.md).
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The compat layer began as a number-translation switch inside
 * src/kernel/arch/x64/trap.c (GB-014): Linux numbers are rewritten to the
 * HobbyOS numbers and the native handlers do the work.  That shape cannot scale
 * to a real Linux program (an X11 client, an SDL game) because most of the
 * surface has no native equivalent with the same semantics, and because every
 * addition would edit the same hot function.  It also cannot be developed by
 * parallel lanes: one shared switch is a guaranteed merge conflict.
 *
 * So: a syscall-number -> handler TABLE, split across one file per subsystem.
 * Each subsystem file defines its own array and nothing else, which means a
 * lane that adds "sockets" or "mmap" never edits a file another lane owns:
 *
 *   src/kernel/linux_compat.c      dispatcher + tracer + user-copy helpers
 *   src/kernel/linux_compat_fd.c   l_tab_fd[]    descriptors, sockets, poll
 *   src/kernel/linux_compat_mem.c  l_tab_mem[]   mmap/brk/mprotect
 *   src/kernel/linux_compat_proc.c l_tab_proc[]  signals, identity, stat, time
 *   src/kernel/linux_compat_misc.c l_tab_misc[]  (spare; first-come)
 *
 * The tables are WEAK symbols, so a missing subsystem file is simply an absent
 * table (NULL), not a link error: the tree stays buildable while lanes land in
 * any order, and the dispatcher skips what is not there yet.
 *
 * CONTRACT
 * --------
 *  - A handler returns 1 after writing tf->regs[0] (the Linux return value:
 *    >= 0 success, -errno in [-4095,-1] on failure).  It returns 0 to say
 *    "I do not handle this number", and the dispatcher falls through to the
 *    next table and finally to the legacy switch in trap.c.
 *  - A handler that BLOCKS must block through the scheduler (the native
 *    syscalls do); it must never spin.  The 30-second deadlock law applies.
 *  - Numbers come from src/include/linux_syscall.h, never from memory.  An
 *    unhandled number is -ENOSYS (38) and prints one [LCOMPAT] record so the
 *    next run names the wall instead of hiding it.
 *  - Arguments are Linux's: (rdi, rsi, rdx, r10, r8, r9) == regs[5,4,3,9,7,8].
 *    Use l_arg(), never a hand-written index.
 */

#include <stdint.h>

#include "arch/trap.h"

struct process;

/* x86_64 Linux argument register order as the x64 trap frame stores it:
   rax=[0], and (rdi,rsi,rdx,r10,r8,r9) = regs[5],regs[4],regs[3],regs[9],
   regs[7],regs[8] -- the same scatter sys_mmap6 and compat_clone already use. */
static inline uint64_t l_arg(struct trap_frame *tf, int i) {
  static const unsigned char idx[6] = {5, 4, 3, 9, 7, 8};
  return (i >= 0 && i < 6) ? tf->regs[idx[i]] : 0;
}

/* int64 view of an argument (a pointer or a signed value). */
static inline int64_t l_argi(struct trap_frame *tf, int i) {
  return (int64_t)l_arg(tf, i);
}

/* 1 = handled (tf->regs[0] written), 0 = not handled by this entry. */
typedef int (*l_sys_fn)(struct process *p, struct trap_frame *tf);

struct l_sysent {
  int nr;            /* Linux x86_64 syscall number (linux_syscall.h) */
  l_sys_fn fn;
  const char *name;  /* short name for the [LCOMPAT] tracer */
};

/* Every table ends with this; fn == 0 is the only sentinel (nr 0 is `read`). */
#define L_SYSENT_END { -1, 0, 0 }

int linux_compat_dispatch(struct process *p, struct trap_frame *tf, uint64_t nr);

/* One console record per (process, number) when nothing handles a number, plus
   a global cap so a spinning guest cannot flood the serial log.  Called from
   trap.c's compat default arm; safe on any CPU (single uart_puts record). */
void linux_compat_unhandled(struct process *p, uint64_t nr, uint64_t rip);

#ifdef RW_TRACE_MEM
/* Diagnostic (make RW_TRACE_MEM=1): one compact row per memory syscall with the
   six raw arguments and the return value.  Called from the dispatcher, so no
   handler needs a second exit path.  Cap is inside the implementation. */
void l_mem_trace(const char *name, struct process *p, uint64_t a0, uint64_t a1,
                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                 int64_t ret);
#endif

/* --- user-memory helpers -------------------------------------------------
 * All return 0 on success, -EFAULT on a bad or unmapped user range.  They are
 * the ONLY sanctioned way for a compat handler to touch guest memory: the
 * addresses arrive from an untrusted process and the v2 address space can have
 * holes (vm_range_ok walks it). */
int l_user_ok(struct process *p, uint64_t ptr, uint64_t len, int write);
int l_copy_from_user(struct process *p, void *kdst, uint64_t usrc, uint64_t len);
int l_copy_to_user(struct process *p, uint64_t udst, const void *ksrc,
                   uint64_t len);
/* NUL-terminated string, at most cap-1 bytes copied; -ENAMETOOLONG if the
   guest string does not terminate inside cap. */
int l_copy_cstr(struct process *p, char *kdst, uint64_t usrc, uint64_t cap);

/* struct timespec (16 B: tv_sec, tv_nsec) -> milliseconds, rounded up.
   Returns -EINVAL for a negative or absurd timespec. */
int l_timespec_to_ms(struct process *p, uint64_t uts, int64_t *out_ms);

#endif /* LINUX_COMPAT_H */
