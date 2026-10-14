/*
 * HobbyOS selfhost W2-4: sys/resource.h backing — honest limits for what
 * the OS actually has.
 *
 *   getrlimit(RLIMIT_STACK): soft = hard = 8 MiB.  The v2 process image
 *     claims the top 8 MiB of the 32 MiB user region for the main stack
 *     (src/include/vm.h, USER_MAIN_STK_*_V2); LLVM's Process.inc reads
 *     this at startup to size its own recursion/thread expectations.
 *   getrlimit(RLIMIT_CORE): 0/0 — no core dumps exist here.
 *   other RLIMIT_*: unlimited — no per-process quotas are enforced.
 *   setrlimit(): advisory no-op success (single-user, no quotas); the
 *     request is validated for a sane resource number only.
 *   getrusage(): zeros.  The kernel keeps no per-process CPU-time
 *     accounting yet (that is W2-17); LLVM's -ftime-report consumers
 *     treat all-zero as "no data".
 *
 * Under HOST_TEST the symbols are renamed hb_*.
 */
#include <sys/resource.h>
#include <errno.h>
#include <string.h>

#ifdef HOST_TEST
#define HB_NAME(name) hb_##name
#else
#define HB_NAME(name) name
#endif

/* The v2 main-thread stack cap (src/include/vm.h). */
#define HB_STACK_LIMIT_BYTES (8u * 1024u * 1024u)

int HB_NAME(getrlimit)(int resource, struct rlimit *rlim) {
  if (!rlim) {
    errno = EFAULT;
    return -1;
  }
  switch (resource) {
  case RLIMIT_CORE:
    rlim->rlim_cur = 0; /* no core dumps */
    rlim->rlim_max = 0;
    return 0;
  case RLIMIT_STACK:
    rlim->rlim_cur = HB_STACK_LIMIT_BYTES;
    rlim->rlim_max = HB_STACK_LIMIT_BYTES;
    return 0;
  case RLIMIT_CPU:
  case RLIMIT_DATA:
  case RLIMIT_FSIZE:
  case RLIMIT_NOFILE:
  case RLIMIT_RSS:
    rlim->rlim_cur = RLIM_INFINITY; /* no per-process quotas */
    rlim->rlim_max = RLIM_INFINITY;
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

int HB_NAME(setrlimit)(int resource, const struct rlimit *rlim) {
  if (!rlim) {
    errno = EFAULT;
    return -1;
  }
  switch (resource) {
  case RLIMIT_CPU:
  case RLIMIT_FSIZE:
  case RLIMIT_DATA:
  case RLIMIT_STACK:
  case RLIMIT_CORE:
  case RLIMIT_RSS:
  case RLIMIT_NOFILE:
    /* Advisory only: nothing enforces per-process limits on this
     * single-user OS, so a well-formed request always "succeeds". */
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

int HB_NAME(getrusage)(int who, struct rusage *usage) {
  if (!usage) {
    errno = EFAULT;
    return -1;
  }
  if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) {
    errno = EINVAL;
    return -1;
  }
  /* No kernel CPU/swap accounting yet (W2-17); zeros are the honest
   * "no data" answer and match what unmeasured fields report on Linux. */
  memset(usage, 0, sizeof(*usage));
  return 0;
}
