/* src/host/linux_abi_test.c — T1 golden lock for the compat ABI tables (GB-014/GB-015).
 *
 * Two independent surfaces are checked here:
 *
 *  1. src/include/linux_syscall.h — every L_SYS_* number must equal the Linux
 *     x86_64 number, per tests/fixtures/abi/linux_syscalls.golden (generated on a
 *     real Linux box by tools/linux-abi/gen_abi_golden.py).  A transposed digit in
 *     the translation table is otherwise a silent "wrong syscall ran" at runtime.
 *  2. src/include/errno.h — every E* value must equal Linux's, per
 *     tests/fixtures/abi/linux_errno.golden.  The compat layer returns these
 *     numbers straight to musl, and musl hands them to the guest as errno.
 *
 * Plus THE invariant of the whole layer: the compat and native syscall namespaces
 * must not share a number for a different operation (docs/linux-abi.md §2).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_syscall.h"
#include "errno.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, name) do {                                       \
    checks++;                                                        \
    if (cond) { printf("  PASS %s\n", name); }                       \
    else { printf("  FAIL %s\n", name); failures++; }                \
  } while (0)

/* --- our tables, as name -> value ---------------------------------------- */
struct row { const char *name; long val; };

#define X(n) {#n, (long)L_SYS_##n},
static const struct row syscall_rows[] = {
  X(read) X(write) X(open) X(close) X(stat) X(fstat)
  X(lstat) X(poll) X(lseek) X(mmap) X(mprotect) X(munmap)
  X(brk) X(ioctl) X(pread64) X(pwrite64) X(readv) X(writev)
  X(access) X(pipe) X(select) X(sched_yield) X(mremap) X(msync)
  X(mincore) X(madvise) X(dup) X(dup2) X(nanosleep) X(getitimer)
  X(getpid) X(clone) X(fork) X(vfork) X(execve) X(exit)
  X(wait4) X(kill) X(uname) X(fcntl) X(fsync) X(getcwd)
  X(chdir) X(rename) X(mkdir) X(rmdir) X(creat) X(link)
  X(unlink) X(readlink) X(chmod) X(fchmod) X(umask) X(gettimeofday)
  X(getrlimit) X(getrusage) X(sysinfo) X(times) X(getuid) X(getgid)
  X(geteuid) X(getegid) X(setpgid) X(getppid) X(arch_prctl) X(gettid)
  X(rt_sigaction) X(rt_sigprocmask) X(rt_sigreturn) X(futex) X(sched_setaffinity) X(sched_getaffinity)
  X(epoll_create) X(getdents64) X(fadvise64) X(clock_gettime) X(clock_getres) X(clock_nanosleep) X(exit_group)
  X(epoll_wait) X(epoll_ctl) X(tgkill) X(openat) X(mkdirat) X(newfstatat)
  X(unlinkat) X(renameat) X(readlinkat) X(fchmodat) X(faccessat) X(pselect6)
  X(ppoll) X(set_robust_list) X(splice) X(tee) X(utimensat) X(epoll_pwait)
  X(signalfd) X(eventfd) X(eventfd2) X(epoll_create1) X(dup3) X(pipe2)
  X(inotify_init1) X(preadv) X(pwritev) X(prlimit64) X(getrandom) X(memfd_create)
  X(statx) X(rseq) X(clone3) X(close_range) X(faccessat2) X(process_madvise)
};
#undef X

#define X(n) {#n, (long)n},
static const struct row errno_rows[] = {
  X(EPERM) X(ENOENT) X(ESRCH) X(EINTR) X(EIO) X(ENXIO)
  X(E2BIG) X(ENOEXEC) X(EBADF) X(ECHILD) X(EAGAIN) X(ENOMEM)
  X(EACCES) X(EFAULT) X(ENOTBLK) X(EBUSY) X(EEXIST) X(EXDEV)
  X(ENODEV) X(ENOTDIR) X(EISDIR) X(EINVAL) X(ENFILE) X(EMFILE)
  X(ENOTTY) X(ETXTBSY) X(EFBIG) X(ENOSPC) X(ESPIPE) X(EROFS)
  X(EMLINK) X(EPIPE) X(EDOM) X(ERANGE) X(ENAMETOOLONG) X(ENOSYS)
  X(ENOTEMPTY) X(ELOOP) X(EOVERFLOW) X(EMSGSIZE) X(EPROTOTYPE) X(ENOPROTOOPT)
  X(EPROTONOSUPPORT) X(EAFNOSUPPORT) X(EADDRINUSE) X(EADDRNOTAVAIL) X(ENETDOWN) X(ENETUNREACH)
  X(ENOTSOCK) X(EMSGSIZE) X(ENOPROTOOPT) X(ECONNRESET) X(ENOTCONN) X(EOPNOTSUPP)
  X(ECONNABORTED) X(ECONNRESET) X(ENOBUFS) X(EISCONN) X(ENOTCONN) X(ETIMEDOUT)
  X(ECONNREFUSED) X(EHOSTUNREACH) X(EALREADY) X(EINPROGRESS) X(EILSEQ)
  X(EDESTADDRREQ) X(ENOTSOCK) X(ENOTSUP) X(EWOULDBLOCK) X(EDEADLK) X(ENOLCK)
};
#undef X

static long lookup(const struct row *rows, size_t n, const char *name) {
  for (size_t i = 0; i < n; i++) if (strcmp(rows[i].name, name) == 0) return rows[i].val;
  return -1;
}

static int check_golden(const char *path, const struct row *rows, size_t nrows,
                        const char *what, int *missing) {
  FILE *f = fopen(path, "r");
  if (!f) {
    printf("  FAIL %s golden missing: %s (run tools/linux-abi/gen_abi_golden.py)\n", what, path);
    failures++;
    return 0;
  }
  char line[256];
  int rows_seen = 0, bad = 0, absent = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char name[128];
    long val;
    if (sscanf(line, "%127s %ld", name, &val) != 2) continue;
    rows_seen++;
    long ours = lookup(rows, nrows, name);
    if (ours < 0) {
      printf("  FAIL %s: golden has `%s` but our table does not (add it, then regenerate)\n", what, name);
      absent++; failures++;
      continue;
    }
    if (ours != val) {
      printf("  FAIL %s: %s ours=%ld linux=%ld\n", what, name, ours, val);
      bad++;
      failures++;
    }
  }
  fclose(f);
  checks++;
  if (bad == 0 && absent == 0)
    printf("  PASS %s: all %d golden rows match\n", what, rows_seen);
  else
    printf("  FAIL %s: %d wrong, %d unknown\n", what, bad, absent);
  if (missing) *missing = rows_seen;
  return rows_seen;
}

int main(void) {
  const char *dir = getenv("ABI_GOLDEN_DIR");
  char sc[512], er[512];
  snprintf(sc, sizeof sc, "%s/linux_syscalls.golden", dir && *dir ? dir : "tests/fixtures/abi");
  snprintf(er, sizeof er, "%s/linux_errno.golden", dir && *dir ? dir : "tests/fixtures/abi");

  printf("=== Linux ABI table lock (GB-014/GB-015) ===\n");
  int nsc = 0;
  check_golden(sc, syscall_rows, sizeof syscall_rows / sizeof syscall_rows[0], "SYS ", &nsc);
  check_golden(er, errno_rows, sizeof errno_rows / sizeof errno_rows[0], "ERR ", NULL);

  /* every row we carry must be in the golden too (no invented syscalls) */
  {
    int extra = 0;
    for (size_t i = 0; i < sizeof syscall_rows / sizeof syscall_rows[0]; i++) {
      char cmd[512];
      snprintf(cmd, sizeof cmd, "grep -q '^%s ' %s", syscall_rows[i].name, sc);
      if (system(cmd) != 0) {
        printf("  FAIL our table has `%s` but the golden does not (regenerate the golden)\n",
               syscall_rows[i].name);
        extra++; failures++;
      }
    }
    checks++;
    if (extra == 0) printf("  PASS no invented syscall rows (%d checked)\n", nsc);
  }

  /* duplicates in our own table: two names sharing a number is the classic
   * copy/paste bug this file exists to catch */
  {
    int dup = 0;
    for (size_t i = 0; i < sizeof syscall_rows / sizeof syscall_rows[0]; i++)
      for (size_t j = i + 1; j < sizeof syscall_rows / sizeof syscall_rows[0]; j++)
        if (syscall_rows[i].val == syscall_rows[j].val) {
          printf("  FAIL duplicate number %ld: %s and %s\n", syscall_rows[i].val,
                 syscall_rows[i].name, syscall_rows[j].name);
          dup++; failures++;
        }
    checks++;
    if (dup == 0) printf("  PASS no duplicate syscall numbers in the compat table\n");
  }

  /* THE namespace invariant: same operation name, different number on each side —
   * that is the whole reason the translation table exists, and a shim that forgot
   * to translate would run the wrong native op. */
  CHECK(L_SYS_write != 0 && L_SYS_read == 0, "compat read is 0 (native SYS_READ is 6)");
  CHECK(L_SYS_exit_group == 231, "exit_group is 231");
  CHECK(L_SYS_arch_prctl == 158, "arch_prctl is 158 (TLS path)");
  CHECK(L_ARCH_SET_FS == 0x1002, "ARCH_SET_FS is 0x1002");
  CHECK(L_SYS_MAX_KNOWN == 440, "L_SYS_MAX_KNOWN is the table bound");
  CHECK(EWOULDBLOCK == EAGAIN, "EWOULDBLOCK == EAGAIN (11) — a shim must not use both");
  CHECK(ENOTSUP == 95, "ENOTSUP is 95 (== EOPNOTSUPP)");
  CHECK(ENOSYS == 38, "ENOSYS is 38 — the required answer for an unimplemented compat syscall");

  printf("=== %d checks, %d failed ===\n", checks, failures);
  if (failures == 0) {
    printf("ALL LINUX ABI TABLE TESTS PASSED\n");
    return 0;
  }
  printf("LINUX ABI TABLE TESTS FAILED (%d)\n", failures);
  return 1;
}
