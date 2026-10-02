/* HobbyOS sysroot: <sched.h> — scheduling (P3.2).
 *
 * libc++'s pthread threading layer (#include <sched.h>) needs sched_yield,
 * which P1 already implements (src/libc/src/pthread.c, SYS_YIELD).  There
 * is no priority/affinity surface: sched_* priority functions are not
 * implemented (no names declared for them).
 */
#ifndef HOBBYOS_SCHED_H
#define HOBBYOS_SCHED_H 1

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
  int sched_get_priority_min(int policy);
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
