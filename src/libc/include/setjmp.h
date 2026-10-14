/*
 * setjmp.h — non-local jumps for the HobbyOS sysroot (selfhost W2-2).
 *
 * Real implementations (src/libc/src/setjmp.c) save and restore the
 * callee-saved register file, the stack pointer and the return address for
 * the AArch64 and x86-64 ABIs.  The normal-return path — the one LLVM's
 * CrashRecoveryContext relies on for every in-process cc1 compile — is
 * fully correct; signal-frame recovery / crash unwinding is out of scope.
 *
 * Buffer layout (word array sized for the larger ABI; both arches fit):
 *   aarch64: x19-x28 (10), x29, x30, SP (13) + d8-d15 (8)  = 21 words
 *   x86_64 : rbx, rbp, r12-r15 (6), SP, return address     =  8 words
 *
 * Under HOST_TEST the real <setjmp.h> is pulled in via include_next so
 * host-parity builds behave exactly like the system libc (the same pattern
 * as signal.h); the device implementation in setjmp.c renames to hb_*.
 */
#ifndef HOBBYOS_SETJMP_H
#define HOBBYOS_SETJMP_H

#ifdef HOST_TEST
#include_next <setjmp.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__aarch64__) || defined(__x86_64__)
#define _JBLEN 24
#else
#error "setjmp: unsupported architecture (aarch64 / x86_64 only)"
#endif

  typedef unsigned long jmp_buf[_JBLEN];

  int  setjmp(jmp_buf env) __attribute__((returns_twice));
  void longjmp(jmp_buf env, int val) __attribute__((noreturn));

  /* sigsetjmp/siglongjmp: HobbyOS has no per-thread signal masking
   * (sigprocmask is a no-op), so both map onto the plain jump machinery. */
  typedef unsigned long sigjmp_buf[_JBLEN];
  int  sigsetjmp(sigjmp_buf env, int savesigs);
  void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SETJMP_H */
