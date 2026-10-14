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

#include "hb_stubs.h"

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

/* Sysroot gap (audit S3.2 "alarm" row): no alarm(3).  Make only calls it
 * from posixos.c set_child_handler_action_flags (load-throttle wakeups for
 * the jobserver), which compiles out entirely under the v1 -j1 port policy
 * (MAKE_JOBSERVER 0, config.h).  Declared + shipped anyway so enabling
 * the jobserver later does not start with a link error (the audit says a
 * stub returning 0 is acceptable; the load throttle then never fires). */
unsigned int alarm(unsigned int seconds) {
  (void)seconds;
  return 0;
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
