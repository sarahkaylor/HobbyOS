/* HobbyOS sysroot: <sched.h> — scheduling (P3.2).
 *
 * libc++'s pthread threading layer (#include <sched.h>) needs sched_yield,
 * which P1 already implements (src/libc/src/pthread.c, SYS_YIELD).  There
 * is no priority/affinity surface: sched_* priority functions are not
 * implemented (no names declared for them).
 */
#ifndef HOBBYOS_SCHED_H
#define HOBBYOS_SCHED_H 1

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* POSIX puts sched_yield in <sched.h>; the P1 pthread header also keeps
   * a declaration for historical reasons (they must match). */
  int sched_yield(void);

  /* L8 ABI ext (topology only): the HobbyOS scheduler has a single fixed
   * priority level, so both bounds are 1 (SCHED_OTHER/FIFO/RR); invalid
   * policies return -1 with errno = EINVAL.  Implemented in the pthread
   * layer (src/libc/src/pthread.c), next to sched_yield(). */
  int sched_get_priority_min(int policy);  /* M4/L5: sched_getcpu().  Mesa's util/u_thread.c uses it for the thread
   * name; HobbyOS is single-CPU-visible per process today, so gl_stubs.c
   * answers 0 (a valid CPU id, and what the guest actually runs on). */
  int sched_getcpu(void);

  /* M4/L5: CPU sets.  Mesa's util/u_thread.c pins worker threads with
   * sched_setaffinity() (33 calls at startup on the host, MESA-NOTES §6.1)
   * and builds the mask with CPU_ZERO/CPU_SET, so the type and the macros
   * must exist even though HobbyOS's scheduler ignores affinity.  Layout is
   * the Linux one (1024-bit mask); gl_stubs.c accepts the calls and the
   * kernel has no notion of pinning yet. */
#ifndef CPU_SETSIZE
#define CPU_SETSIZE 1024
#define __NCPUBITS (8 * sizeof(unsigned long))
typedef struct {
  unsigned long __bits[CPU_SETSIZE / __NCPUBITS];
} cpu_set_t;
#define CPU_ZERO(set)   do { for (unsigned i = 0; i < CPU_SETSIZE / __NCPUBITS; i++) (set)->__bits[i] = 0; } while (0)
#define CPU_SET(cpu, set)   ((void)((cpu) < CPU_SETSIZE ? ((set)->__bits[(cpu) / __NCPUBITS] |=     (1UL << ((cpu) % __NCPUBITS))) : 0))
#define CPU_CLR(cpu, set)   ((void)((cpu) < CPU_SETSIZE ? ((set)->__bits[(cpu) / __NCPUBITS] &=     ~(1UL << ((cpu) % __NCPUBITS))) : 0))
#define CPU_ISSET(cpu, set)   ((cpu) < CPU_SETSIZE && ((set)->__bits[(cpu) / __NCPUBITS] &     (1UL << ((cpu) % __NCPUBITS))))
#define CPU_COUNT(set) 0
#endif

  int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
  int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);


  int sched_get_priority_max(int policy);

  /* sched_param + the two required policies: declared for source
   * compatibility only.  pthread_attr_setschedpolicy() accepts the policy
   * values as a no-op (P1, pthread.c); no scheduler policy is implemented. */
  struct sched_param {
    int sched_priority;
  };

#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SCHED_H */
