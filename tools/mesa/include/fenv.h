/* fenv.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * HobbyOS has no <fenv.h> and no floating-point exception model.  Mesa uses
 * exactly one thing from it: src/compiler/nir/nir_constant_expressions.c
 * wraps constant folding in
 *
 *     fegetexceptflag / feholdexcept ... fesetenv
 *
 * to stop FP exceptions raised while folding from escaping to the caller.
 * HobbyOS's userland runs with the default MXCSR/x87 control word and never
 * enables exception traps (the kernel does not unmask them), so "exceptions
 * are already held" is the truthful implementation - every entry point here
 * is a no-op that reports the default environment.
 *
 * These are static inline on purpose: no symbol has to be supplied by libc
 * or by gl_stubs.c, and a no-op call costs nothing at -O2. */
#ifndef HOBBYOS_FENV_H
#define HOBBYOS_FENV_H 1

#ifdef __cplusplus
extern "C" {
#endif

typedef int fenv_t;
typedef int fexcept_t;

/* Exception flags: the C99 names, glibc's values (x86 MXCSR bits). */
#define FE_INVALID   0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW  0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT   0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | \
                       FE_UNDERFLOW | FE_INEXACT)

/* Rounding modes (glibc values; x86-64 defaults to round-to-nearest). */
#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0c00

#define FE_DFL_ENV ((const fenv_t *)-1)

#define FE_DENORMAL 0x02

static inline int feclearexcept(int excepts) { (void)excepts; return 0; }
static inline int fegetexceptflag(fexcept_t *flagp, int excepts)
{ (void)excepts; if (flagp) *flagp = 0; return 0; }
static inline int fesetexceptflag(const fexcept_t *flagp, int excepts)
{ (void)flagp; (void)excepts; return 0; }
static inline int feraiseexcept(int excepts) { (void)excepts; return 0; }
static inline int fetestexcept(int excepts) { (void)excepts; return 0; }
static inline int fegetround(void) { return FE_TONEAREST; }
static inline int fesetround(int rounding_mode)
{ return rounding_mode == FE_TONEAREST ? 0 : -1; }
static inline int fegetenv(fenv_t *envp) { if (envp) *envp = 0; return 0; }
static inline int feholdexcept(fenv_t *envp) { if (envp) *envp = 0; return 0; }
static inline int fesetenv(const fenv_t *envp) { (void)envp; return 0; }
static inline int feupdateenv(const fenv_t *envp) { (void)envp; return 0; }
static inline int feenableexcept(int excepts) { (void)excepts; return -1; }
static inline int fedisableexcept(int excepts) { (void)excepts; return 0; }
static inline int fegetexcept(void) { return 0; }

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_FENV_H */
