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
#define sigaction  hb_sigaction
#define sigemptyset hb_sigemptyset
#define sigfillset hb_sigfillset
#define sigaddset hb_sigaddset
#define sigdelset hb_sigdelset
#define sigprocmask hb_sigprocmask
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

#ifdef HOST_TEST
/* Host tests (tail -p probes kill()) need this stub under the hb_kill name;
 * on HobbyOS proper, user/libc.c implements kill() over SYS_KILL, so the
 * sysroot must NOT define a second one (duplicate symbol at link time). */
int kill(pid_t pid, int sig) {
  (void)pid;
  (void)sig;
  errno = ESRCH;
  return -1;
}
#endif

int raise(int sig) {
  (void)sig;
  errno = ENOSYS;
  return -1;
}

/* ---- sigaction family: remembered, never delivered (see signal.h) ---- */

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
  if (signum <= 0 || signum >= MAX_SIG) {
    errno = EINVAL;
    return -1;
  }
  if (oldact) {
    oldact->sa_handler = handlers[signum];
    oldact->sa_flags = 0;
  }
  if (act) handlers[signum] = act->sa_handler;
  return 0;
}

int sigemptyset(sigset_t *set) {
  set->__bits[0] = 0;
  set->__bits[1] = 0;
  return 0;
}

int sigfillset(sigset_t *set) {
  set->__bits[0] = ~0ul;
  set->__bits[1] = ~0ul;
  return 0;
}

int sigaddset(sigset_t *set, int signum) {
  if (signum <= 0 || signum >= MAX_SIG) {
    errno = EINVAL;
    return -1;
  }
  set->__bits[signum / 64] |= 1ul << (signum % 64);
  return 0;
}

int sigdelset(sigset_t *set, int signum) {
  if (signum <= 0 || signum >= MAX_SIG) {
    errno = EINVAL;
    return -1;
  }
  set->__bits[signum / 64] &= ~(1ul << (signum % 64));
  return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how;
  (void)set;
  if (oldset) sigemptyset(oldset);
  return 0;
}
