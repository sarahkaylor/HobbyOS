/* src/user/errno_test.c — Phase 0 POSIX errno convention, in-OS tier.
 *
 * Runs under KERNEL_MODE_TEST (a child of no parent, output via the serial
 * console). Verifies the kernel's negative-errno contract through the real
 * libc wrappers: failures return -1 and set errno to the matching constant,
 * successes leave errno untouched, and an out-of-range syscall number
 * returns -ENOSYS. */
#include <fcntl.h>
#include "libc.h"

/* Raw syscall for the "unknown syscall -> ENOSYS" case, which has no libc
 * wrapper. All numbers 1..28 are taken, so 999 is out of range. */
#ifdef __x86_64__
static long raw_syscall(long num) {
  long ret;
  __asm__ volatile("syscall\n" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
  return ret;
}
#else
static long raw_syscall(long num) {
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = 0;
  __asm__ volatile("svc #0\n" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}
#endif

static int failures = 0;

static void check(int cond, const char *name) {
  if (cond) {
    print_console("  PASS ");
    print_console(name);
  } else {
    print_console("  FAIL ");
    print_console(name);
    failures++;
  }
  print_console("\n");
}

__attribute__((section(".text._start")))
void _start(void) {
  errno = 0;
  check(errno == 0, "errno starts at 0");

  /* POSIX open() semantics: SYS_OPEN now honors O_CREAT/O_EXCL. A missing
   * file without O_CREAT fails ENOENT; O_CREAT|O_EXCL creates it and a
   * second exclusive attempt fails EEXIST; a plain open of an existing
   * file succeeds. */
  errno = 0;
  int fd = open("ERRNO9.TXT", 0);
  check(fd < 0 && errno == ENOENT, "open(missing, 0) -> ENOENT");
  errno = 0;
  fd = open("ERRNO9.TXT", O_CREAT | O_EXCL, 0600);
  check(fd >= 0 && errno == 0, "open(O_CREAT|O_EXCL) creates");
  errno = 0;
  int fd2 = open("ERRNO9.TXT", O_CREAT | O_EXCL, 0600);
  check(fd2 < 0 && errno == EEXIST, "second O_CREAT|O_EXCL -> EEXIST");
  errno = 0;
  int fd3 = open("ERRNO9.TXT", 0);
  check(fd3 >= 0 && errno == 0, "re-open of created file works");
  if (fd >= 0) close(fd);
  if (fd2 >= 0) close(fd2);
  if (fd3 >= 0) close(fd3);
  unlink("ERRNO9.TXT"); /* best-effort cleanup */

  /* close/read/write on a bad fd -> -1 + EBADF */
  errno = 0;
  check(close(-1) == -1 && errno == EBADF, "close(-1) -> EBADF");
  errno = 0;
  char c;
  check(read(-1, &c, 1) == -1 && errno == EBADF, "read(-1) -> EBADF");
  errno = 0;
  check(write(-1, "x", 1) == -1 && errno == EBADF, "write(-1) -> EBADF");
  errno = 0;
  check(close(31) == -1 && errno == EBADF, "close(31) [never opened] -> EBADF");

  /* kill(pid, 0) on a pid that cannot exist -> -1 + ESRCH */
  errno = 0;
  check(kill(999999, 0) == -1 && errno == ESRCH, "kill(999999,0) -> ESRCH");

  /* mkdir(): first create succeeds, second create of the same name
   * fails with EEXIST (Phase 0 maps all mkdir failures to EEXIST).
   * NOTE: there is no rmdir yet (fat16_unlink refuses directories), so a
   * leftover /ERRTEST from an earlier run is unavoidable — treat EEXIST
   * on the first call as success so the test is deterministic. */
  errno = 0;
  int r = mkdir("ERRTEST");
  check(r == 0 || (r == -1 && errno == EEXIST), "mkdir(ERRTEST) succeeds");
  errno = 0;
  r = mkdir("ERRTEST");
  check(r == -1 && errno == EEXIST, "mkdir(ERRTEST) again -> EEXIST");
  unlink("ERRTEST"); /* best-effort cleanup; ignore result */

  /* chdir() to a missing directory -> -1 + ENOENT */
  errno = 0;
  check(chdir("NO_SUCH_9") == -1 && errno == ENOENT, "chdir(missing) -> ENOENT");

  /* getcwd() with a buffer too small -> NULL + ERANGE */
  errno = 0;
  char tiny[1];
  check(getcwd(tiny, sizeof tiny) == 0 && errno == ERANGE, "getcwd(tiny) -> ERANGE");

  /* Unknown syscall number -> -ENOSYS (raw, no libc wrapper) */
  errno = 0;
  check(raw_syscall(999) == (long)-ENOSYS, "raw syscall 999 -> -ENOSYS");

  if (failures == 0) {
    print_console("[ERRNO] ALL TESTS PASSED\n");
    exit(0);
  }
  print_console("[ERRNO] TESTS FAILED: ");
  print_dec(failures);
  print_console("\n");
  exit(1);
}
