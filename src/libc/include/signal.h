/*
 * signal.h — standard <signal.h> for the HobbyOS sysroot.
 *
 * Host (HOST_TEST): the real <signal.h> is pulled in via include_next so
 * host parity builds behave exactly like glibc.  NOTE: the guard below is
 * deliberately HOBBYOS_SIGNAL_H, not glibc's _SIGNAL_H — pre-defining
 * _SIGNAL_H would make the include_next'ed glibc header self-disable and
 * leave kill()/raise() undeclared for host builds (this bit tail_host).
 *
 * Device: signals do not exist yet (Phase 6).  We provide the standard
 * names and prototypes; the implementations (src/libc/src/signal.c) are
 * low-fidelity placeholders so GNU ported code compiles and links.  Any
 * program that actually depends on signal delivery must wait for Phase 6.
 */
#ifndef HOBBYOS_SIGNAL_H
#define HOBBYOS_SIGNAL_H

#ifdef HOST_TEST
#include_next <signal.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

  typedef void (*sighandler_t)(int);
  typedef int sig_atomic_t;

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

  /* POSIX signal numbers (subset used by GNU utilities). */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

  sighandler_t signal(int signum, sighandler_t handler);
  /* KEEP THIS SIGNATURE IDENTICAL to <libc.h>'s kill() (implemented in
   * src/user/libc.c as kill(int, int)).  A C++ TU that includes both headers
   * sees a single extern "C" function there, so any signature drift is a
   * hard "conflicting types" error; pid_t (long) arguments convert
   * implicitly at the call sites. */
  int kill(int pid, int sig);
  int raise(int sig);

  /* POSIX sigaction surface.  Like signal() above, handlers are only
   * remembered, never delivered (Phase 6); enough for ported programs that
   * install handlers defensively (nano masks SIGINT/SIGWINCH to keep the
   * terminal sane).  The flags below exist so code compiles; they have no
   * effect. */
  typedef struct {
    unsigned long __bits[2];
  } sigset_t;

  struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
  };

  /* No-op flag values (delivery does not exist yet). */
#define SA_RESTART   0x1
#define SA_RESETHAND 0x2
#define SA_SIGINFO   0x4

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

  int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
  int sigemptyset(sigset_t *set);
  int sigfillset(sigset_t *set);
  int sigaddset(sigset_t *set, int signum);
  int sigdelset(sigset_t *set, int signum);
  int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SIGNAL_H */
