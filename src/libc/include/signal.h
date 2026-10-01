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

  /* POSIX sigaction surface.  P5 upgrade (OQ4): Linux-compatible layout —
   * the handler/sigaction union at offset 0, sa_mask, sa_flags, and
   * sa_restorer at offset 32 (struct size 40; the kernel marshals these
   * fields by offset).  Delivery: dispositions live in the kernel (row
   * 83); SIGCHLD/SIGPIPE/SIGTERM handlers actually run (P5 engine), while
   * fault-class (SEGV/BUS/ILL/ABRT) and SIGUSR1/SIGUSR2 handlers are
   * accepted-and-recorded but never invoked (documented divergence, the
   * fault's default action still kills + reports).  sigprocmask and
   * pthread_sigmask are no-ops returning 0 (no per-thread masks yet). */
  typedef struct {
    unsigned long __bits[2];
  } sigset_t;

  /* Minimal siginfo_t (glibc-sized: 128 bytes).  Only si_signo, si_code,
   * si_pid and si_status are meaningful today (a one-argument handler sees
   * none of it; SA_SIGINFO handlers are not invoked). */
  typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad0;
    int si_pid;   /* SIGCHLD: the child that died */
    unsigned int si_uid;
    int si_status; /* SIGCHLD: the child's exit status */
    int __pad1;
    long __pad2[12];
  } siginfo_t;

  struct sigaction {
    union {
      void (*sa_handler)(int);
      void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
  };

  /* Flag values (delivery honours the disposition, not the flags). */
#define SA_RESTART   0x1
#define SA_RESETHAND 0x2
#define SA_SIGINFO   0x4
#define SA_RESTORER  0x04000000 /* libc always fills sa_restorer anyway */

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
