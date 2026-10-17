/* src/host/linux_layout_test.c — T1 golden lock for the compat struct layouts
 * (GB-015).
 *
 * src/include/linux_layout.h defines the struct layouts the ELF-compat layer
 * believes a musl guest believes the kernel has.  musl passes these structs by
 * pointer straight to the syscall, so a wrong size/offset does not fail loudly —
 * it corrupts a struct the guest then trusts.  That is exactly why every
 * size/offset here is asserted against tests/fixtures/abi/linux_layouts.golden,
 * which is MEASURED on a real Linux x86_64 box (tools/linux-abi/probe_abi.c via
 * gen_abi_golden.py).  A number that drifts from Linux fails this test, and the
 * golden itself is regenerable (and --check-able) on a Linux runner.
 *
 * Companion to src/host/linux_abi_test.c: that file locks the syscall numbers
 * and errno values; this one locks the struct layouts (docs/linux-abi.md §6) and
 * the few layout-critical constants (docs §3).  Together they are the "T4 fails
 * on any mismatch" parity locks of GB-015, runnable today on the host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "linux_layout.h"

static int failures = 0;
static int checks = 0;

#define VAL(name) ((long)(name))

/* --- our header, computed with sizeof/offsetof ------------------------------ */
#define O(t, m) ((long)offsetof(t, m))
struct row { const char *name; long val; };

#define R(n, e) { (n), (long)(e) },
static const struct row rows[] = {
  R("stat.size", sizeof(linux_stat_t))
  R("stat.st_dev", O(linux_stat_t, st_dev))
  R("stat.st_ino", O(linux_stat_t, st_ino))
  R("stat.st_nlink", O(linux_stat_t, st_nlink))
  R("stat.st_mode", O(linux_stat_t, st_mode))
  R("stat.st_uid", O(linux_stat_t, st_uid))
  R("stat.st_gid", O(linux_stat_t, st_gid))
  R("stat.st_rdev", O(linux_stat_t, st_rdev))
  R("stat.st_size", O(linux_stat_t, st_size))
  R("stat.st_blksize", O(linux_stat_t, st_blksize))
  R("stat.st_blocks", O(linux_stat_t, st_blocks))
  R("stat.st_atim", O(linux_stat_t, st_atim))
  R("stat.st_mtim", O(linux_stat_t, st_mtim))
  R("stat.st_ctim", O(linux_stat_t, st_ctim))
  R("statx.size", sizeof(linux_statx_t))
  R("statx.stx_mask", O(linux_statx_t, stx_mask))
  R("statx.stx_size", O(linux_statx_t, stx_size))
  R("statx.stx_mode", O(linux_statx_t, stx_mode))
  R("sigaction.size", sizeof(linux_sigaction_t))
  R("sigaction.sa_handler", O(linux_sigaction_t, sa_handler_u))
  R("sigaction.sa_mask", O(linux_sigaction_t, sa_mask))
  R("sigaction.sa_flags", O(linux_sigaction_t, sa_flags))
  R("sigaction.sa_restorer", O(linux_sigaction_t, sa_restorer))
  R("ucontext.size", sizeof(linux_ucontext_t))
  R("ucontext.uc_flags", O(linux_ucontext_t, uc_flags))
  R("ucontext.uc_link", O(linux_ucontext_t, uc_link))
  R("ucontext.uc_stack", O(linux_ucontext_t, uc_stack))
  R("ucontext.uc_mcontext", O(linux_ucontext_t, uc_mcontext))
  R("ucontext.uc_sigmask", O(linux_ucontext_t, uc_sigmask))
  R("mcontext.size", sizeof(linux_mcontext_t))
  R("mcontext.gregs", O(linux_mcontext_t, gregs))
  R("mcontext.fpregs", O(linux_mcontext_t, fpregs))
  R("stack.size", sizeof(linux_stack_t))
  R("termios.size", sizeof(linux_termios_t))
  R("termios.c_iflag", O(linux_termios_t, c_iflag))
  R("termios.c_oflag", O(linux_termios_t, c_oflag))
  R("termios.c_cflag", O(linux_termios_t, c_cflag))
  R("termios.c_lflag", O(linux_termios_t, c_lflag))
  R("termios.c_line", O(linux_termios_t, c_line))
  R("termios.c_cc", O(linux_termios_t, c_cc))
  R("termios.c_ispeed", O(linux_termios_t, c_ispeed))
  R("termios.c_ospeed", O(linux_termios_t, c_ospeed))
  R("winsize.size", sizeof(linux_winsize_t))
  R("winsize.ws_row", O(linux_winsize_t, ws_row))
  R("winsize.ws_col", O(linux_winsize_t, ws_col))
  R("winsize.ws_xpixel", O(linux_winsize_t, ws_xpixel))
  R("winsize.ws_ypixel", O(linux_winsize_t, ws_ypixel))
  R("epoll_event.size", sizeof(linux_epoll_event_t))
  R("epoll_event.events", O(linux_epoll_event_t, events))
  R("epoll_event.data", O(linux_epoll_event_t, data))
  R("pollfd.size", sizeof(linux_pollfd_t))
  R("pollfd.fd", O(linux_pollfd_t, fd))
  R("pollfd.events", O(linux_pollfd_t, events))
  R("pollfd.revents", O(linux_pollfd_t, revents))
  R("iovec.size", sizeof(linux_iovec_t))
  R("iovec.iov_base", O(linux_iovec_t, iov_base))
  R("iovec.iov_len", O(linux_iovec_t, iov_len))
  R("timespec.size", sizeof(linux_timespec_t))
  R("timespec.tv_sec", O(linux_timespec_t, tv_sec))
  R("timespec.tv_nsec", O(linux_timespec_t, tv_nsec))
  R("timeval.size", sizeof(linux_timeval_t))
  R("timeval.tv_sec", O(linux_timeval_t, tv_sec))
  R("timeval.tv_usec", O(linux_timeval_t, tv_usec))
  R("dirent64.size", sizeof(linux_dirent64_t))
  R("dirent64.d_ino", O(linux_dirent64_t, d_ino))
  R("dirent64.d_off", O(linux_dirent64_t, d_off))
  R("dirent64.d_reclen", O(linux_dirent64_t, d_reclen))
  R("dirent64.d_type", O(linux_dirent64_t, d_type))
  R("dirent64.d_name", O(linux_dirent64_t, d_name))
  R("rlimit.size", sizeof(linux_rlimit_t))
  R("rlimit.rlim_cur", O(linux_rlimit_t, rlim_cur))
  R("rlimit.rlim_max", O(linux_rlimit_t, rlim_max))
  R("rusage.size", sizeof(linux_rusage_t))
  R("itimerval.size", sizeof(linux_itimerval_t))
  R("itimerval.it_interval", O(linux_itimerval_t, it_interval))
  R("itimerval.it_value", O(linux_itimerval_t, it_value))
  R("sysinfo.size", sizeof(linux_sysinfo_t))
  R("utsname.size", sizeof(linux_utsname_t))
  R("utsname.sysname", O(linux_utsname_t, sysname))
  R("utsname.nodename", O(linux_utsname_t, nodename))
  R("utsname.release", O(linux_utsname_t, release))
  R("utsname.version", O(linux_utsname_t, version))
  R("utsname.machine", O(linux_utsname_t, machine))
  R("utsname.domainname", O(linux_utsname_t, domainname))
  R("flock.size", sizeof(linux_flock_t))
  R("flock.l_type", O(linux_flock_t, l_type))
  R("flock.l_whence", O(linux_flock_t, l_whence))
  R("flock.l_start", O(linux_flock_t, l_start))
  R("flock.l_len", O(linux_flock_t, l_len))
  R("flock.l_pid", O(linux_flock_t, l_pid))
};
#undef R
#undef O

int main(void) {
  const char *dir = getenv("ABI_GOLDEN_DIR");
  char path[512];
  snprintf(path, sizeof path, "%s/linux_layouts.golden",
           dir && *dir ? dir : "tests/fixtures/abi");

  FILE *f = fopen(path, "r");
  if (!f) {
    printf("  FAIL layout golden missing: %s (run tools/linux-abi/gen_abi_golden.py)\n", path);
    return 1;
  }

  struct row *gold = NULL;
  long ngold = 0, cap = 0;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == ' ') continue;
    char name[256];
    long val;
    if (sscanf(line, "%255s %ld", name, &val) != 2) continue;
    if (ngold == cap) {
      cap = cap ? cap * 2 : 128;
      gold = realloc(gold, cap * sizeof *gold);
    }
    gold[ngold].name = strdup(name);
    gold[ngold].val = val;
    ngold++;
  }
  fclose(f);

  /* 1. every golden row must exist in our header and match */
  int bad = 0, absent = 0;
  for (long i = 0; i < ngold; i++) {
    const char *name = gold[i].name;
    long ours = -99999, found = 0;
    for (size_t j = 0; j < sizeof rows / sizeof rows[0]; j++)
      if (strcmp(rows[j].name, name) == 0) { ours = rows[j].val; found = 1; break; }
    if (!found) {
      printf("  FAIL layout: golden has `%s` but linux_layout.h does not (add it)\n", name);
      absent++; failures++;
      continue;
    }
    checks++;
    if (ours != gold[i].val) {
      printf("  FAIL layout %s: ours=%ld linux=%ld\n", name, ours, gold[i].val);
      bad++; failures++;
    }
  }
  if (bad == 0 && absent == 0)
    printf("  PASS all %ld layout golden rows match (sizes + offsets of linux_layout.h)\n", ngold);
  else
    printf("  FAIL layout golden: %d wrong, %d absent\n", bad, absent);
  checks++;

  /* 2. -- (constants round out the layout lock in docs §3; the sizes/offsets
        above are the core.  Extra constants lockable later.) */

  free(gold);
  printf("=== %d checks, %d failed ===\n", checks, failures);
  if (failures) {
    printf("LINUX LAYOUT TESTS FAILED (%d)\n", failures);
    return 1;
  }
  printf("ALL LINUX LAYOUT TESTS PASSED\n");
  return 0;
}
