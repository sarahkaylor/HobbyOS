/*
 * signal.c — signal support for the HobbyOS sysroot.
 *
 * P5 (docs/browser/p5-exec-signals-design.md): the device path now talks
 * to the kernel.  sigaction() marshals the OQ4 struct through row 83
 * (filling sa_restorer with the libc sigreturn trampoline when the caller
 * left it 0), signal() is sigaction + old-handler return, raise() is
 * kill(self, sig).  Dispositions live kernel-side; SIGCHLD/SIGPIPE/SIGTERM
 * handlers run (P5 delivery engine), while fault-class (SEGV/BUS/ILL/ABRT)
 * and SIGUSR1/SIGUSR2 handlers are accepted-and-recorded but never invoked
 * (documented divergence, D6/D13) — the fault's default action still kills
 * and reports.
 *
 * Under HOST_TEST everything stays the old placeholder (with the hb_*
 * renames) so glibc-host builds keep their own real signal machinery.
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

#ifdef HOST_TEST

/* ---- host placeholder: remembered, never delivered ---- */

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

/* Host tests (tail -p probes kill()) need this stub under the hb_kill name;
 * on HobbyOS proper, user/libc.c implements kill() over SYS_KILL, so the
 * sysroot must NOT define a second one (duplicate symbol at link time). */
int kill(pid_t pid, int sig) {
  (void)pid;
  (void)sig;
  errno = ESRCH;
  return -1;
}

int raise(int sig) {
  (void)sig;
  errno = ENOSYS;
  return -1;
}

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

#else /* device */

#include "syscall.h"

/* Dual-arch 4-arg syscall helper, matching user/libc.c's ABI:
 * aarch64: x8=num, x0..x3 args; x86_64: rax=num, rdi,rsi,rdx,r10. */
static long hb_syscall4(long num, long a0, long a1, long a2, long a3) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "=r"(x0)
                   : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
#endif
}

/* The sigreturn trampoline (D8.2): sigaction() plants its address as
 * sa_restorer; the delivery engine uses sa_restorer as the handler's return
 * address, so a handler that returns executes row 84 here.  The kernel
 * restores the interrupted context from the frame and never returns to the
 * instruction after svc — no epilogue needed (or possible: the register
 * state at this point is the handler's, not ours). */
__asm__(
    ".globl __ho_sigreturn_trampoline\n"
#ifdef __x86_64__
    ".type __ho_sigreturn_trampoline, @function\n"
    "__ho_sigreturn_trampoline:\n"
    "mov $84, %eax\n"
    "syscall\n"
    ".size __ho_sigreturn_trampoline, .-__ho_sigreturn_trampoline\n"
#else
    ".type __ho_sigreturn_trampoline, %function\n"
    "__ho_sigreturn_trampoline:\n"
    "mov x8, #84\n"
    "svc #0\n"
    ".size __ho_sigreturn_trampoline, .-__ho_sigreturn_trampoline\n"
#endif
);
extern void __ho_sigreturn_trampoline(void);

extern int getpid(void);
extern int kill(int pid, int sig);

sighandler_t signal(int signum, sighandler_t handler) {
  struct sigaction sa;
  struct sigaction old;
  sa.sa_handler = handler;
  sa.sa_mask.__bits[0] = 0;
  sa.sa_mask.__bits[1] = 0;
  sa.sa_flags = SA_RESTART;
  sa.sa_restorer = 0;
  if (sigaction(signum, &sa, &old) != 0)
    return SIG_ERR;
  return old.sa_handler;
}

int raise(int sig) {
  return kill(getpid(), sig);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
  if (signum <= 0 || signum >= MAX_SIG) {
    errno = EINVAL;
    return -1;
  }
  struct sigaction local;
  const struct sigaction *actp = act;
  if (act && act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
    local = *act;
    if (local.sa_restorer == 0)
      local.sa_restorer = __ho_sigreturn_trampoline;
    actp = &local;
  }
  long r = hb_syscall4(SYS_SIGACTION, (long)signum, (long)actp, (long)oldact, 0);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (int)r;
}

#endif /* HOST_TEST */

/* ---- pure user-space sigset helpers (both builds) ---- */

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
