/*
 * HobbyOS selfhost W2-4: variadic syscall() dispatcher.
 *
 * The Linux/glibc shape (declared in <unistd.h>): syscall(number, ...)
 * forwards the caller's raw syscall number and up to six pointer-width
 * arguments straight to the kernel trap, returning the raw result (value
 * or -errno) exactly like Linux.  LLVM's Unix/Signals.inc:426 uses it for
 * the async-signal-safe stderr write path (syscall(SYS_write, ...)), so it
 * must really dispatch — a stub would silently drop crash diagnostics.
 * Unknown numbers get -ENOSYS from the kernel's own dispatch table.
 *
 * Under HOST_TEST there is no HobbyOS kernel — and executing the raw trap
 * on the host would run host syscalls — so the hb_* unit form is an ENOSYS
 * stub.
 */
#include <stdarg.h>
#include <errno.h>

#ifdef HOST_TEST
#define HB_NAME(name) hb_##name
#else
#define HB_NAME(name) name
#endif

#ifdef HOST_TEST

long HB_NAME(syscall)(long number, ...) {
  (void)number;
  errno = ENOSYS;
  return -1;
}

#else /* !HOST_TEST */

long HB_NAME(syscall)(long number, ...) {
  va_list ap;
  long a0, a1, a2, a3, a4, a5;

  va_start(ap, number);
  a0 = va_arg(ap, long);
  a1 = va_arg(ap, long);
  a2 = va_arg(ap, long);
  a3 = va_arg(ap, long);
  a4 = va_arg(ap, long);
  a5 = va_arg(ap, long);
  va_end(ap);

#if defined(__aarch64__)
  register long x8 __asm__("x8") = number;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory");
  return x0;
#elif defined(__x86_64__)
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  register long r8  __asm__("r8")  = a4;
  register long r9  __asm__("r9")  = a5;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(number), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10),
                     "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
#else
  (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  errno = ENOSYS;
  return -1;
#endif
}

#endif /* !HOST_TEST */
