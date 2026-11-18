/*
 * src/user/linux_probe_errnomap.c — the errno of a failed read/write family
 * call (H4-kglitch round 2, K8/K9; docs/cp-kglitch-evidence/park-audit.txt).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * On a CLOSED fd every call is EBADF (K9: readv/writev/pread64/pwrite64 passed
 * fs.c's generic -1 through, which is -EPERM):
 *   R1 read  R2 write  R3 readv  R4 writev  R5 pread64  R6 pwrite64
 * A write to a memfd sealed with F_SEAL_WRITE is EPERM (K8: the write route
 * mapped the -1 to EBADF):
 *   S1 write  S2 pwrite64
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static int fails;
static void row(const char *id, const char *what, long r, int e, int want) {
  int ok = r == -1 && e == want;
  printf("[LP] %s %-36s rc=%ld errno=%d want=%d -> %s\n", id, what, r, e, want, ok ? "PASS" : "FAIL");
  fails += !ok;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  char b[8] = "abcdefg";
  struct iovec iv = {b, 4};
  int fd = open("/tmp/lp_errnomap", O_RDWR | O_CREAT, 0600);
  close(fd); /* fd is now closed */
  long r;
  r = syscall(SYS_read, fd, b, 4); row("R1", "read on a closed fd", r, errno, EBADF);
  r = syscall(SYS_write, fd, b, 4); row("R2", "write on a closed fd", r, errno, EBADF);
  r = syscall(SYS_readv, fd, &iv, 1); row("R3", "readv on a closed fd", r, errno, EBADF);
  r = syscall(SYS_writev, fd, &iv, 1); row("R4", "writev on a closed fd", r, errno, EBADF);
  r = syscall(SYS_pread64, fd, b, 4, 0); row("R5", "pread64 on a closed fd", r, errno, EBADF);
  r = syscall(SYS_pwrite64, fd, b, 4, 0); row("R6", "pwrite64 on a closed fd", r, errno, EBADF);
  unlink("/tmp/lp_errnomap");

  int m = (int)syscall(SYS_memfd_create, "lp", 2 /* MFD_ALLOW_SEALING */);
  if (m < 0 || write(m, b, 4) != 4 || fcntl(m, 1033 /* F_ADD_SEALS */, 8 /* F_SEAL_WRITE */) != 0) {
    printf("[LP] S0 memfd + F_SEAL_WRITE setup failed errno=%d -> FAIL\n", errno);
    fails++;
  } else {
    r = syscall(SYS_write, m, b, 4); row("S1", "write to a write-sealed memfd", r, errno, EPERM);
    r = syscall(SYS_pwrite64, m, b, 4, 0); row("S2", "pwrite64 to a write-sealed memfd", r, errno, EPERM);
  }
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
