/* HobbyOS sysroot compat header: <sys/resource.h> (declarations only).
 * LLVM includes <sys/resource.h> unconditionally on Unix
 * (llvm/lib/Support/ProgramStack.cpp:14, Process.cpp) and uses
 * getrlimit/setrlimit(RLIMIT_CORE) in llvm/lib/Support/Unix/Process.inc.
 * The HobbyOS libc has no sys/resource.h and no getrlimit/setrlimit
 * implementation -- this header lets LLVMSupport COMPILE; the link-time
 * resolver then reports the missing implementations, which are L2 work
 * items (l1-dep-surface.txt section B).  Declarations model POSIX-2008.
 */
#ifndef HOBBYOS_SYS_RESOURCE_H
#define HOBBYOS_SYS_RESOURCE_H

#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long rlim_t;

struct rlimit {
  rlim_t rlim_cur; /* soft limit */
  rlim_t rlim_max; /* hard limit */
};

/* Full POSIX rusage: Unix/Program.inc:413 uses `rusage Info;` by value. */
struct rusage {
  struct timeval ru_utime; /* user CPU time used */
  struct timeval ru_stime; /* system CPU time used */
  long ru_maxrss;          /* maximum resident set size */
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

#define RLIMIT_CORE    4
#define RLIMIT_CPU     0
#define RLIMIT_DATA    2
#define RLIMIT_FSIZE   1
#define RLIMIT_NOFILE  7
#define RLIMIT_STACK   3
#define RLIM_INFINITY  (~0ULL)

/* Not implemented by HobbyOS libc today (L2 work items); declared so the
 * toolchain's LLVM/Support sources compile. */
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getrusage(int who, struct rusage *usage);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_RESOURCE_H */
