/*
 * hello.c — the GB-007 differential probe: a tiny `hello` binary whose
 * observable contract is {stdout, exit code, errno}.  Built with the native
 * host cc (skeleton; the x86_64-musl cross build for the HobbyOS-intel leg is
 * the same source via build_hello.sh).  The differential harness compares a
 * reference (Linux/musl) run to a target (HobbyOS-intel) run on exactly these
 * three outputs.
 *
 * stdout:  "hello from hobbyos\n"
 * exit:    0
 * errno:   ENOENT (=2) — the output line is printed by the probe helper after
 *          a deliberately-failing open(), so the harness can compare errno
 *          values across the two runtimes without the binary itself varying.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
  printf("hello from hobbyos\n");

  /* An operation that MUST fail with a known errno on both runtimes: opening a
   * file that does not exist sets ENOENT.  The harness reads this line to
   * compare errno values. */
  errno = 0;
  int fd = open("/no/such/file/anywhere", O_RDONLY);
  if (fd >= 0) { close(fd); return 3; }  /* should never happen */
  printf("errno=%d\n", errno);

  return 0;
}
