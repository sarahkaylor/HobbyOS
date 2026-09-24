/*
 * signal.c — placeholder signal support for the HobbyOS sysroot.
 *
 * Phase 6 promises real signal delivery; until then the POSIX names
 * exist (so GNU ported code compiles and links) but nothing is ever
 * delivered.  signal() stores the handler (silently ignoring it) and
 * returns the previous handler; raise()/kill() return -1.  These are
 * functionally dead ends by design: GNU utilities that fetch or install
 * handlers (tail -f, head) run fine because they never rely on a
 * signal arriving on this OS.
 */
#include <signal.h>
#include <errno.h>

#ifdef HOST_TEST
#define signal hb_signal
#define kill   hb_kill
#define raise  hb_raise
#endif

#define MAX_SIG 64

static sighandler_t handlers[MAX_SIG];

sighandler_t signal(int signum, sighandler_t handler) {
  sighandler_t old;

  if (signum <= 0 || signum >= MAX_SIG) {
    errno = EINVAL;
    return SIG_ERR;
  }
  old = handlers[signum];
  handlers[signum] = handler;
  return old;
}

int kill(pid_t pid, int sig) {
  (void)pid;
  (void)sig;
  /* A real kill would deliver sig to pid; Phase 6.  Failing with
   * ESRCH here is what GNU code probes for (tail -p checks it). */
  errno = ESRCH;
  return -1;
}

int raise(int sig) {
  (void)sig;
  errno = ENOSYS;
  return -1;
}
