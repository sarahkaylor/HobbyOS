/* src/host/t4_compat_probe.c — T4 differential compat-syscall probe.
 *
 * One source, raced two ways by t4_parity.sh:
 *
 *   leg A (reference): build with `musl-gcc -static` — a real static-musl
 *                       Linux binary.  argv[0] program name, Linux errno.
 *   leg B (HobbyOS):    build with $(HOST_CC) $(HOST_CFLAGS) + src/host/
 *                       compat.c — glibc-backed ho_* mocks over the same
 *                       Linux errno numbering our in-tree errno.h uses.
 *
 * The probe prints *numbers*, not libc text, for every result (the error
 * strings differ between musl and glibc; the syscall + errno contract does
 * not).  The diff on stdout/exit is therefore meaningful byte-for-byte,
 * and it is exactly the surface libc-compat must hold stable.
 *
 * Every syscall is chosen so the outcome is identical on a static-musl
 * binary and on a HobbyOS flat-binary running the same wrapper logic:
 *   open(ENOENT), open(ENOTDIR), read/write(EBADF), close(EBADF),
 *   a successful getcwd round-trip, and a write to stdout we then diff.
 *
 * 30-second deadlock rule: the probe exits promptly; the harness enforces
 * a timeout on the QEMU leg so a hang is a defect, not a retry loop.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>

/* GB-017: /dev/urandom + getrandom return *random bytes* — the content is
 * non-deterministic and must never be diffed leg-to-leg.  Only the return
 * count (and errno) are part of the byte-meaningful contract. */

/* Normalized line: "<op> rc=<r> errno=<e>" — errno read only when rc<0. */
static void report(const char *op, long r) {
  if (r < 0) {
    printf("%s rc=%ld errno=%d\n", op, r, errno);
    /* restore for the next call so a stale value never leaks through */
    errno = 0;
  } else {
    printf("%s rc=%ld\n", op, r);
  }
}

int main(void) {
  int w = write(1, "t4-probe\n", 9);
  report("write_stdout", w);

  /* open() on a nonexistent path -> -1, errno=ENOENT (2) */
  errno = 0;
  int fd = open("/no/such/path/t4-nonexistent", O_RDONLY);
  report("open_nonexistent", fd);
  if (fd >= 0) close(fd);

  /* open() on a path whose parent is a regular file -> -1, errno=ENOTDIR (20) */
  {
    /* create a scratch file to use as a "non-directory intermediate" */
    int f = open("/tmp/t4_parent_probe", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { close(f); }
  }
  errno = 0;
  fd = open("/tmp/t4_parent_probe/child", O_RDONLY);
  report("open_notdir", fd);
  if (fd >= 0) close(fd);
  unlink("/tmp/t4_parent_probe");

  /* read()/write()/close() on a bad fd -> -1, errno=EBADF (9) */
  errno = 0;
  char c;
  report("read_badfd", read(-1, &c, 1));
  errno = 0;
  report("write_badfd", write(9999, "x", 1));
  errno = 0;
  report("close_badfd", close(424242));

  /* successful getcwd round-trip: buf filled, errno untouched.
   * chdir("/") first so the result is deterministic for BOTH libc flavors:
   * musl resolves symlinks in getcwd (physical path) while glibc returns
   * the logical path, so a run dir under a symlinked /tmp diverges. The
   * root is never a symlink, and HobbyOS's FAT16 has no symlinks at all. */
  chdir("/");
  errno = 0;
  char buf[256];
  char *cw = getcwd(buf, sizeof buf);
  if (cw == NULL || errno != 0) {
    printf("getcwd_success rc=-1 errno=%d\n", errno);
  } else {
    printf("getcwd_success rc=0 len=%lu first='%c'\n",
           (unsigned long)strlen(buf), buf[0]);
  }

  /* GB-017: getrandom — deterministic: rc = bytes requested (or -1/errno).
   * The payload is random and never diffed. */
  {
    unsigned char rnd[16];
    errno = 0;
    report("getrandom_16", getrandom(rnd, sizeof rnd, 0));
    errno = 0;
    /* GRND_NONBLOCK on a fresh pool may return EAGAIN on some kernels but
     * musl and glibc agree on it either way; keep flags 0 for determinism
     * across both legs. */
  }

  /* GB-017: /dev/urandom — open(RDONLY) + read rc is deterministic (16);
   * bytes are random, not diffed.  If the node is absent both legs report
   * the same open errno (ENOENT), which is itself a meaningful parity row. */
  {
    errno = 0;
    int u = open("/dev/urandom", O_RDONLY);
    report("urandom_open", u);
    if (u >= 0) {
      unsigned char b[16];
      errno = 0;
      report("urandom_read16", read(u, b, sizeof b));
      close(u);
    }
  }

  /* GB-017: /dev/null — write rc = bytes accepted (3), read rc = 0 (EOF).
   * Deterministic on both legs; a shim that doesn't consume writes or
   * never returns EOF at read diverges here. */
  {
    errno = 0;
    int n = open("/dev/null", O_WRONLY);
    report("null_open_w", n);
    if (n >= 0) {
      errno = 0;
      report("null_write3", write(n, "abc", 3));
      close(n);
    }
    errno = 0;
    int nr = open("/dev/null", O_RDONLY);
    report("null_open_r", nr);
    if (nr >= 0) {
      char c;
      errno = 0;
      report("null_read1", read(nr, &c, 1));
      close(nr);
    }
  }

  /* exit status is part of the diff: 0 here means every leg reached the
   * end; a differential failure above would have already diverged stdout. */
  printf("probe-exit\n");
  return 0;
}
