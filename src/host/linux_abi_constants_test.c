/* src/host/linux_abi_constants_test.c — T1 golden lock for the compat L_* constants
 * (GB-014/GB-015, docs/linux-abi.md §3).
 *
 * linux_abi_test.c locks the syscall numbers + errno values; linux_layout_test.c
 * locks the struct sizes/offsets.  This closes the third promised surface:
 * docs/linux-abi.md §3 says the open-flag / PROT / MAP / POLL / SEEK / CLOCK /
 * W / SIG / TC / TIOC / FIO constants are "asserted by the M1 T4 suite".  They
 * are carried by src/include/linux_syscall.h as L_* macros (the compat layer's
 * source besides the syscall table), but nothing asserted them against Linux.
 *
 * Every L_<name> here is checked against tests/fixtures/abi/linux_constants.golden,
 * measured on a real Linux x86_64 box by tools/linux-abi/gen_abi_constants.py.
 * A transposed digit in the header (e.g. L_O_CREAT written 32 instead of 64) is
 * a test failure, not a runtime EEXIST/EMFILE surprise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_syscall.h"

static int failures = 0;
static int checks = 0;

#define R(n) {#n, (long)L_##n},
static const struct { const char *name; long val; } rows[] = {
  R(O_RDONLY) R(O_WRONLY) R(O_RDWR) R(O_CREAT) R(O_EXCL) R(O_TRUNC)
  R(O_APPEND) R(O_NONBLOCK) R(O_DIRECTORY) R(O_CLOEXEC)
  R(PROT_READ) R(PROT_WRITE) R(PROT_EXEC)
  R(MAP_SHARED) R(MAP_PRIVATE) R(MAP_FIXED) R(MAP_ANONYMOUS)
  R(SEEK_SET) R(SEEK_CUR) R(SEEK_END)
  R(CLOCK_REALTIME) R(CLOCK_MONOTONIC)
  R(POLLIN) R(POLLOUT) R(POLLERR) R(POLLHUP)
  R(WNOHANG) R(WUNTRACED)
  R(SIG_BLOCK) R(SIG_UNBLOCK) R(SIG_SETMASK)
  R(TCGETS) R(TCSETS) R(TIOCGWINSZ) R(FIONREAD) R(FIONBIO)
  R(S_IFMT) R(S_IFREG) R(S_IFDIR) R(S_IFCHR) R(S_IFBLK) R(S_IFLNK) R(S_IFIFO) R(S_ISUID)
  R(F_DUPFD) R(F_GETFL) R(F_SETFL) R(F_GETLK) R(F_SETLK) R(F_SETLKW)
  R(SIGCHLD) R(SIGSEGV) R(SIGINT) R(SIGWINCH) R(SIGKILL) R(SIGSTOP)
  R(SA_SIGINFO) R(SA_RESTART)
  R(EPOLLIN) R(EPOLLOUT) R(EPOLLET)
  R(DT_REG) R(DT_DIR) R(DT_CHR)
  R(RLIMIT_NOFILE)
};
#undef R

int main(void) {
  const char *dir = getenv("ABI_GOLDEN_DIR");
  char path[512];
  snprintf(path, sizeof path, "%s/linux_constants.golden",
           dir && *dir ? dir : "tests/fixtures/abi");

  FILE *f = fopen(path, "r");
  if (!f) {
    printf("  FAIL constants golden missing: %s (run tools/linux-abi/gen_abi_constants.py)\n", path);
    return 1;
  }

  int nrows = (int)(sizeof rows / sizeof rows[0]);
  int bad = 0, absent = 0, goldrows = 0;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char name[128];
    long v;
    if (sscanf(line, "%127s %ld", name, &v) != 2) continue;
    goldrows++;
    long ours = -1;
    int found = 0;
    for (int i = 0; i < nrows; i++) {
      if (strcmp(rows[i].name, name) == 0) { ours = rows[i].val; found = 1; break; }
    }
    if (!found) {
      printf("  FAIL constants: golden has `%s` but linux_syscall.h lacks L_%s\n", name, name);
      absent++; failures++;
      continue;
    }
    checks++;
    if (ours != v) {
      printf("  FAIL constant %s: ours=%ld linux=%ld\n", name, ours, v);
      bad++; failures++;
    }
  }
  fclose(f);
  checks++;
  if (bad == 0 && absent == 0)
    printf("  PASS all %d constant golden rows match (L_* macros in linux_syscall.h)\n", goldrows);
  else
    printf("  FAIL constant golden: %d wrong, %d absent\n", bad, absent);

  /* every row we carry must be in the golden (no invented constants) */
  {
    int extra = 0;
    for (int i = 0; i < nrows; i++) {
      char path2[512];
      snprintf(path2, sizeof path2, "%s/linux_constants.golden",
               dir && *dir ? dir : "tests/fixtures/abi");
      char cmd[600];
      snprintf(cmd, sizeof cmd, "grep -q '^%s ' %s", rows[i].name, path2);
      if (system(cmd) != 0) {
        printf("  FAIL our table has `%s` but the golden does not (re-run the generator)\n", rows[i].name);
        extra++; failures++;
      }
    }
    checks++;
    if (extra == 0) printf("  PASS no invented constant rows (%d checked)\n", nrows);
  }

  printf("=== %d checks, %d failed ===\n", checks, failures);
  if (failures) {
    printf("LINUX ABI CONSTANTS TESTS FAILED (%d)\n", failures);
    return 1;
  }
  printf("ALL LINUX ABI CONSTANTS TESTS PASSED\n");
  return 0;
}
