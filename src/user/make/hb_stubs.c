/* hb_stubs.c - HobbyOS glue for the GNU make 4.4.1 port.
 *
 * First-party port code: a few libc symbols the sysroot does not provide
 * yet, wired as make-local definitions so the upstream sources stay
 * byte-identical (docs/gnu-ports.md).  Each stub documents the audit row
 * and the upstream call site it exists for.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/select.h>   /* fd_set + pselect() declaration (w11-makej) */
#include <time.h>         /* struct timespec */

#include "hb_stubs.h"

/* w11-makej: GNU make's parallel-build jobserver must acquire a token from
 * its pipe and return control to the caller (which then reaps finished
 * children and recovers their tokens) whenever a child may have exited.
 *
 * On Linux make's classic jobserver blocks in read() on the jobserver pipe
 * and relies on the SIGCHLD handler interrupting that read (EINTR).  This
 * OS cannot do that: the kernel defers SIGCHLD delivery for a process
 * parked in a pipe read until the syscall completes (process.c, D8.4),
 * and a token is only written back by reap_children() — after this call
 * returns.  A blocking wait would deadlock.
 *
 * The kernel DOES complete select() timeouts, so pselect() below is a
 * bounded-poll wrapper over the device select() syscall: each call waits
 * at most HOBBYOS_PSELECT_POLL_MS for the jobserver fd (or a caller
 * timeout), then returns 0 ("timeout") so make can reap children and
 * retry.  The polling interval only bounds how quickly a finished child's
 * token is noticed (a few tens of ms per acquisition); no fd duplexing is
 * needed because make's pselect path never relies on EINTR. */
#define HOBBYOS_PSELECT_POLL_MS 25

/* Device select() (src/user_include/libc.h): the sysroot's select is the
 * frozen SYS_SELECT wrapper and takes a MILLISECOND timeout — a deliberate
 * divergence from POSIX's struct timeval.  Declared here rather than in
 * sys/select.h so the POSIX-named header stays signal/timeval-clean. */
extern int select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, int timeout_ms);

int
pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
        const struct timespec *timeout, const sigset_t *sigmask) {
  int timeout_ms;

  /* sigprocmask is a no-op on this OS; SIGCHLD is delivered at syscall
   * exit/resume boundaries regardless of this argument. */
  (void)sigmask;

  if (timeout) {
    long long ms = (long long)timeout->tv_sec * 1000 +
                   ((long long)timeout->tv_nsec + 999999) / 1000000;
    if (ms < 0)
      ms = 0;
    if (ms > 0x7fffffff)
      ms = 0x7fffffff;
    timeout_ms = (int)ms;
  } else {
    /* NULL timeout = "wait for a token".  Must NOT block forever (see
     * the header comment). */
    timeout_ms = HOBBYOS_PSELECT_POLL_MS;
  }

  return select(nfds, readfds, writefds, exceptfds, timeout_ms);
}

/* Sysroot gap (audit S3.2 "getlogin" row): no getlogin(3) anywhere in the
 * sysroot, and read.c:3114 calls it unconditionally during `~` expansion
 * (the `#if !defined(_AMIGA) && !defined(WINDOWS32)` arm).  Return the
 * LOGNAME env var or the synthetic single-user account name "user"
 * (src/libc/pwd.c), which getpwnam() resolves to home "/".
 * A static buffer would be safe too, but getenv storage lives for the
 * whole process, so handing out that pointer is fine. */
char *
getlogin(void) {
  const char *name = getenv("LOGNAME");

  /* No env table in early boot: fall back to the synthetic single-user
   * account name so `~` and `~user` expansion keep working. */
  if (name == NULL || name[0] == '\0')
    name = "user";

  return (char *)name;
}

/* Sysroot gap (audit S3.2): no getloadavg(3).  Referenced by load_too_high
 * under -l (job.c:2138); -j1 v1 never runs it.  Returns -1 with errno 0,
 * make's "not supported" convention. */
int getloadavg(double loadavg[], int nelem) {
  (void)loadavg;
  (void)nelem;
  errno = 0; /* "not supported" vs perror path (job.c:2144-2149) */
  return -1;
}

/* Sysroot gap (not in the audit table): <unistd.h> declares dup() but no
 * object in libc.a implements it.  posixos.c os_anontmp() calls dup() only
 * on its tmpfile() fallback path (line 888); provide the real POSIX
 * semantics over dup2() (which the sysroot does implement by finding the
 * lowest free descriptor, exactly like glibc's dup). */
int dup(int oldfd) {
  int i;

  for (i = 0; i < 256; i++)
    if (fcntl(i, F_GETFD, 0) < 0)
      return dup2(oldfd, i);

  errno = EMFILE;
  return -1;
}
