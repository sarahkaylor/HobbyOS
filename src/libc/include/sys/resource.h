/*
 * sys/resource.h — resource limits and usage for the HobbyOS sysroot
 * (selfhost W2-4).
 *
 * Implementations in src/libc/src/resource.c are honest for what this OS
 * actually has:
 *   - getrlimit() reports the v2 main-stack cap (8 MiB) for RLIMIT_STACK,
 *     0 for RLIMIT_CORE (no core dumps), unlimited for the rest;
 *   - setrlimit() succeeds as a no-op (per-process limits are not
 *     enforced — single-user, no quotas);
 *   - getrusage() returns zeros (kernel CPU-time accounting is W2-17).
 * LLVM's llvm/lib/Support/Unix/Process.inc runs getrlimit(RLIMIT_STACK)
 * at startup (Process.inc:139-160), so the stack answer must be accurate.
 */
#ifndef HOBBYOS_SYS_RESOURCE_H
#define HOBBYOS_SYS_RESOURCE_H

#include <sys/types.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef unsigned long long rlim_t;

  struct rlimit {
    rlim_t rlim_cur; /* soft limit */
    rlim_t rlim_max; /* hard limit */
  };

  /* Full POSIX rusage (llvm Unix/Program.inc uses `rusage Info;` by value). */
  struct rusage {
    struct timeval ru_utime; /* user CPU time used */
    struct timeval ru_stime; /* system CPU time used */
    long ru_maxrss;          /* max resident set size (KiB) */
    long ru_ixrss;           /* integral shared memory size */
    long ru_idrss;           /* integral unshared data size */
    long ru_isrss;           /* integral unshared stack size */
    long ru_minflt;          /* page reclaims (soft page faults) */
    long ru_majflt;          /* page faults (hard page faults) */
    long ru_nswap;           /* swaps */
    long ru_inblock;         /* block input operations */
    long ru_oublock;         /* block output operations */
    long ru_msgsnd;          /* IPC messages sent */
    long ru_msgrcv;          /* IPC messages received */
    long ru_nsignals;        /* signals received */
    long ru_nvcsw;           /* voluntary context switches */
    long ru_nivcsw;          /* involuntary context switches */
  };

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

  /* Linux numbering (the values LLVM and ports compare against). */
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NOFILE  7
#define RLIM_INFINITY  (~0ULL)

  int getrlimit(int resource, struct rlimit *rlim);
  int setrlimit(int resource, const struct rlimit *rlim);
  int getrusage(int who, struct rusage *usage);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_RESOURCE_H */
