/* HobbyOS sysroot: <math.h> — C99 floating point (P3.2).
 *
 * HobbyOS userland may use the FPU/SSE since F1.5 (per-process save/restore)
 * but the library never had a <math.h>: this header + src/libc/src/math.c
 * are the first slice.  Classification macros compile to compiler builtins
 * (exact, no library call); the function implementations are a portable
 * subset (see math.c for the per-function notes and the host parity test).
 *
 * Both supported targets are IEEE-754 binary64 for double/binary32 for
 * float; long double is binary64-promoted on this port (see math.c), so
 * the *l functions alias their double counterparts.
 */
#ifndef HOBBYOS_MATH_H
#define HOBBYOS_MATH_H 1

#ifdef __cplusplus
extern "C" {
#endif

  /* ---- classification (glibc's C99 values) ---- */
#define FP_INFINITE 1
#define FP_NAN 0
#define FP_NORMAL 4
#define FP_SUBNORMAL 3
#define FP_ZERO 2

#define FP_ILOGB0 (-2147483647 - 1)
#define FP_ILOGBNAN 2147483647
#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))

#define fpclassify(x)                                                          \
  __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO,  \
                       x)
#define signbit(x) __builtin_signbit(x)
#define isfinite(x) __builtin_isfinite(x)
#define isinf(x) __builtin_isinf(x)
#define isnan(x) __builtin_isnan(x)
#define isnormal(x) __builtin_isnormal(x)
#define isgreater(x, y) __builtin_isgreater((x), (y))
#define isgreaterequal(x, y) __builtin_isgreaterequal((x), (y))
#define isless(x, y) __builtin_isless((x), (y))
#define islessequal(x, y) __builtin_islessequal((x), (y))
#define islessgreater(x, y) __builtin_islessgreater((x), (y))
#define isunordered(x, y) __builtin_isunordered((x), (y))

  typedef float float_t;
  typedef double double_t;

  /* ---- functions (implemented subset marked "impl."; the rest are
   * declared for source compatibility and fail at link time if used) ---- */

  /* impl. */
  double fabs(double x);
  float fabsf(float x);
  long double fabsl(long double x);
  double fmin(double x, double y);
  float fminf(float x, float y);
  double fmax(double x, double y);
  float fmaxf(float x, float y);
  double copysign(double x, double y);
  float copysignf(float x, float y);
  double trunc(double x);
  float truncf(float x);
  long double truncl(long double x);
  double floor(double x);
  float floorf(float x);
  long double floorl(long double x);
  double ceil(double x);
  float ceilf(float x);
  long double ceill(long double x);
  double round(double x);
  float roundf(float x);
  long double roundl(long double x);
  long lround(double x);
  long lroundf(float x);
  long long llround(double x);
  long long llroundf(float x);
  double fmod(double x, double y);
  float fmodf(float x, float y);
  double sqrt(double x);
  float sqrtf(float x);
  double rint(double x);
  float rintf(float x);
  double nearbyint(double x);
  float nearbyintf(float x);
  double fdim(double x, double y);

  /* declared-only (link error if referenced) */
  double acos(double x);
  float acosf(float x);
  long double acosl(long double x);
  double asin(double x);
  float asinf(float x);
  long double asinl(long double x);
  double atan(double x);
  float atanf(float x);
  long double atanl(long double x);
  double atan2(double y, double x);
  float atan2f(float y, float x);
  long double atan2l(long double y, long double x);
  double cos(double x);
  float cosf(float x);
  long double cosl(long double x);
  double sin(double x);
  float sinf(float x);
  long double sinl(long double x);
  double tan(double x);
  float tanf(float x);
  long double tanl(long double x);
  double cosh(double x);
  float coshf(float x);
  long double coshl(long double x);
  double sinh(double x);
  float sinhf(float x);
  long double sinhl(long double x);
  double tanh(double x);
  float tanhf(float x);
  long double tanhl(long double x);
  double acosh(double x);
  float acoshf(float x);
  long double acoshl(long double x);
  double asinh(double x);
  float asinhf(float x);
  long double asinhl(long double x);
  double atanh(double x);
  float atanhf(float x);
  long double atanhl(long double x);
  double exp(double x);
  float expf(float x);
  long double expl(long double x);
  double exp2(double x);
  float exp2f(float x);
  long double exp2l(long double x);
  double expm1(double x);
  float expm1f(float x);
  long double expm1l(long double x);
  double frexp(double value, int *exp);
  float frexpf(float value, int *exp);
  long double frexpl(long double value, int *exp);
  double ldexp(double value, int exp);
  float ldexpf(float value, int exp);
  long double ldexpl(long double value, int exp);
  double log(double x);
  float logf(float x);
  long double logl(long double x);
  double log10(double x);
  float log10f(float x);
  long double log10l(long double x);
  double log1p(double x);
  float log1pf(float x);
  long double log1pl(long double x);
  double log2(double x);
  float log2f(float x);
  long double log2l(long double x);
  double logb(double x);
  float logbf(float x);
  long double logbl(long double x);
  double modf(double value, double *iptr);
  float modff(float value, float *iptr);
  long double modfl(long double value, long double *iptr);
  double scalbn(double x, int n);
  float scalbnf(float x, int n);
  long double scalbnl(long double x, int n);
  double cbrt(double x);
  float cbrtf(float x);
  long double cbrtl(long double x);
  double hypot(double x, double y);
  float hypotf(float x, float y);
  long double hypotl(long double x, long double y);
  double pow(double x, double y);
  float powf(float x, float y);
  long double powl(long double x, long double y);
  double remainder(double x, double y);
  float remainderf(float x, float y);
  long double remainderl(long double x, long double y);
  double fma(double x, double y, double z);
  float fmaf(float x, float y, float z);
  long double fmal(long double x, long double y, long double z);
  double erf(double x);
  float erff(float x);
  double erfc(double x);
  float erfcf(float x);
  double tgamma(double x);
  float tgammaf(float x);
  double lgamma(double x);
  float lgammaf(float x);

  int ilogb(double x);
  int ilogbf(float x);
  long lrint(double x);
  long lrintf(float x);
  long long llrint(double x);
  long long llrintf(float x);
  double remquo(double x, double y, int *quo);
  float remquof(float x, float y, int *quo);

  /* C23 aliases the C99 singletons use */
  float fabsf(float);
  float sqrtf(float);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_MATH_H */
