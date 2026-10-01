/*
 * HobbyOS libc math: integer-only IEEE-754 dispatch (P3.2 transcendentals).
 *
 * Every special-value decision in the transcendental set lives here, in
 * pure integer bit-space: classification, sign extraction, the NaN/Inf/
 * zero-result matrices, and the integer/odd-integer predicate pow uses for
 * its negative-base rule.  src/libc/src/math.c calls these from its FP
 * paths, and the EL1 kernel unit suite (src/kernel/math_test.c) compiles
 * this same header and checks the decisions directly -- the kernel runs
 * with -mgeneral-regs-only (F1.5), so EL1 is exactly where this header,
 * and only this header, can be exercised.
 *
 * The bit constants pin the exact doubles glibc answers with (probed on
 * x86-64 via ctypes -> libm, 2026-09-30); the host race
 * (src/host/math_transcend_test.c, MATH_HOST_SPECIALS) bit-compares the
 * live glibc answers and the in-wave MATH_T.BIN repeats them on-device.
 */
#ifndef HOBBYOS_MATH_DISPATCH_H
#define HOBBYOS_MATH_DISPATCH_H 1

#include <stdint.h>

/* classification codes (same values as the <math.h> fpclassify set) */
#define HO_MC_NAN 0
#define HO_MC_INF 1
#define HO_MC_ZERO 2
#define HO_MC_SUBNORMAL 3
#define HO_MC_NORMAL 4

/* double bit patterns -------------------------------------------------- */
#define HO_D_NAN 0x7ff8000000000000ULL      /* default quiet NaN */
#define HO_D_NAN_NEG 0xfff8000000000000ULL  /* glibc's negative qNaN for domain errors */
#define HO_D_INF 0x7ff0000000000000ULL
#define HO_D_NEG_INF 0xfff0000000000000ULL
#define HO_D_ZERO 0x0000000000000000ULL
#define HO_D_NEG_ZERO 0x8000000000000000ULL
#define HO_D_HALF 0x3fe0000000000000ULL     /* 0.5 */
#define HO_D_ONE 0x3ff0000000000000ULL      /* 1.0 */
#define HO_D_NEG_ONE 0xbff0000000000000ULL
#define HO_D_PI 0x400921fb54442d18ULL       /* pi rounded to double */
#define HO_D_NEG_PI 0xc00921fb54442d18ULL
#define HO_D_PI2 0x3ff921fb54442d18ULL      /* pi/2 */
#define HO_D_NEG_PI2 0xbff921fb54442d18ULL
#define HO_D_PI4 0x3fe921fb54442d18ULL      /* pi/4 */
#define HO_D_NEG_PI4 0xbfe921fb54442d18ULL
#define HO_D_3PI4 0x4002d97c7f3321d2ULL     /* 3*pi/4 */
#define HO_D_NEG_3PI4 0xc002d97c7f3321d2ULL
#define HO_D_QUIET 0x0008000000000000ULL    /* NaN quiet bit */

/* float bit patterns ---------------------------------------------------- */
#define HO_F_NAN 0x7fc00000u
#define HO_F_INF 0x7f800000u
#define HO_F_ZERO 0x00000000u
#define HO_F_ONE 0x3f800000u
#define HO_F_NEG_ONE 0xbf800000u
#define HO_F_QUIET 0x00400000u

static inline int ho_d_class(uint64_t b) {
  uint64_t e = (b >> 52) & 0x7ff;
  uint64_t m = b & 0x000fffffffffffffULL;
  if (e == 0x7ff)
    return m ? HO_MC_NAN : HO_MC_INF;
  if (e == 0)
    return m ? HO_MC_SUBNORMAL : HO_MC_ZERO;
  return HO_MC_NORMAL;
}

static inline int ho_d_signbit(uint64_t b) { return (int)(b >> 63); }
static inline uint64_t ho_d_quiet(uint64_t b) { return b | HO_D_QUIET; }
static inline uint64_t ho_d_sign_zero(uint64_t b) {
  return b & 0x8000000000000000ULL;
}

static inline int ho_f_class(uint32_t b) {
  uint32_t e = (b >> 23) & 0xff;
  uint32_t m = b & 0x007fffffu;
  if (e == 0xff)
    return m ? HO_MC_NAN : HO_MC_INF;
  if (e == 0)
    return m ? HO_MC_SUBNORMAL : HO_MC_ZERO;
  return HO_MC_NORMAL;
}

/* sin/cos/tan: NaN propagates (quieted); +-Inf -> glibc's -NaN. */
static inline int ho_trig_special(uint64_t b, uint64_t *out) {
  int c = ho_d_class(b);
  if (c == HO_MC_NAN) {
    *out = ho_d_quiet(b);
    return 1;
  }
  if (c == HO_MC_INF) {
    *out = HO_D_NAN_NEG;
    return 1;
  }
  return 0;
}

/* atan: NaN quieted; +-Inf -> +-pi/2 (exact rounded doubles). */
static inline int ho_atan_special(uint64_t b, uint64_t *out) {
  int c = ho_d_class(b);
  if (c == HO_MC_NAN) {
    *out = ho_d_quiet(b);
    return 1;
  }
  if (c == HO_MC_INF) {
    *out = (b >> 63) ? HO_D_NEG_PI2 : HO_D_PI2;
    return 1;
  }
  return 0;
}

/* log: NaN quieted; +Inf -> +Inf; +-0 -> -Inf; negative -> glibc -NaN.
 * (Subnormals are positive finite inputs: not special -- the FP path
 * scales them; see d_log_split in math.c.) */
static inline int ho_log_special(uint64_t b, uint64_t *out) {
  int c = ho_d_class(b);
  if (c == HO_MC_NAN) {
    *out = ho_d_quiet(b);
    return 1;
  }
  if (c == HO_MC_INF) {
    *out = (b >> 63) ? HO_D_NAN_NEG : HO_D_INF;
    return 1;
  }
  if (c == HO_MC_ZERO) {
    *out = HO_D_NEG_INF;
    return 1;
  }
  if (b >> 63) {
    *out = HO_D_NAN_NEG;
    return 1;
  }
  return 0;
}

/* asin: NaN quieted; |x| > 1 (incl. +-Inf) -> +NaN (glibc answers a
 * positive qNaN for asin's domain errors, unlike log/trig's -NaN);
 * |x| == 1 stays on the FP path (it lands exactly on +-pi/2). */
static inline int ho_asin_special(uint64_t b, uint64_t *out) {
  int c = ho_d_class(b);
  if (c == HO_MC_NAN) {
    *out = ho_d_quiet(b);
    return 1;
  }
  if ((b & 0x7fffffffffffffffULL) > HO_D_ONE) {
    *out = HO_D_NAN;
    return 1;
  }
  return 0;
}

/* expf: NaN quieted; +Inf -> +Inf; -Inf -> +0. */
static inline int ho_expf_special(uint32_t b, uint32_t *out) {
  int c = ho_f_class(b);
  if (c == HO_MC_NAN) {
    *out = b | HO_F_QUIET;
    return 1;
  }
  if (c == HO_MC_INF) {
    *out = (b >> 31) ? HO_F_ZERO : HO_F_INF;
    return 1;
  }
  return 0;
}

/* tanhf: NaN quieted; +-Inf -> +-1. */
static inline int ho_tanhf_special(uint32_t b, uint32_t *out) {
  int c = ho_f_class(b);
  if (c == HO_MC_NAN) {
    *out = b | HO_F_QUIET;
    return 1;
  }
  if (c == HO_MC_INF) {
    *out = (b >> 31) ? HO_F_NEG_ONE : HO_F_ONE;
    return 1;
  }
  return 0;
}

/* modf: NaN -> (quieted NaN, quieted NaN); +-Inf -> (same, signed zero). */
static inline int ho_d_modf_special(uint64_t b, uint64_t *ipart,
                                    uint64_t *fpart) {
  int c = ho_d_class(b);
  if (c == HO_MC_NAN) {
    *ipart = ho_d_quiet(b);
    *fpart = ho_d_quiet(b);
    return 1;
  }
  if (c == HO_MC_INF) {
    *ipart = b;
    *fpart = ho_d_sign_zero(b);
    return 1;
  }
  return 0;
}

/* Is the double an integer, and if so is it odd?  (pow's negative-base
 * rule and its small-integer fast path.)  NaN/Inf -> not integers. */
static inline int ho_d_isint_odd(uint64_t b, int *isint, int *isodd) {
  uint64_t m = b & 0x000fffffffffffffULL;
  int c = ho_d_class(b);
  int e;

  *isint = 0;
  *isodd = 0;
  if (c == HO_MC_NAN || c == HO_MC_INF)
    return 0;
  if (c == HO_MC_ZERO) {
    *isint = 1;
    return 1;
  }
  if (c == HO_MC_SUBNORMAL)
    return 1; /* nonzero, |x| < 1: not an integer */
  e = (int)((b >> 52) & 0x7ff) - 1023;
  if (e < 0)
    return 1; /* in (0, 1): not an integer */
  if (e >= 53) {
    *isint = 1; /* every value >= 2^53 is an even integer */
    return 1;
  }
  {
    uint64_t sig = 0x0010000000000000ULL | m; /* 53-bit significand */
    int shift = 52 - e;
    *isint = (m & ((1ULL << shift) - 1)) == 0;
    *isodd = *isint && (((sig >> shift) & 1) != 0);
  }
  return 1;
}

/* pow: complete special matrix; returns 0 when the FP general path owns
 * the call (finite nonzero x, finite nonzero y).  Order matters: IEEE says
 * pow(x, +-0) = 1 for every x and pow(1, y) = 1 for every y, NaN included
 * (glibc-probed), before any NaN/inf handling. */
static inline int ho_pow_special(uint64_t xb, uint64_t yb, uint64_t *out) {
  int xc = ho_d_class(xb);
  int yc = ho_d_class(yb);
  int xs = ho_d_signbit(xb);
  int ys = ho_d_signbit(yb);
  uint64_t ux = xb & 0x7fffffffffffffffULL;
  int isint, isodd;

  if (yc == HO_MC_ZERO) {
    *out = HO_D_ONE;
    return 1;
  }
  if (xb == HO_D_ONE) {
    *out = HO_D_ONE;
    return 1;
  }
  if (yc == HO_MC_NAN) {
    *out = ho_d_quiet(yb);
    return 1;
  }
  if (xc == HO_MC_NAN) {
    *out = ho_d_quiet(xb);
    return 1;
  }
  if (yc == HO_MC_INF) {
    if (ux == HO_D_ONE) { /* (-1)^+-inf = 1 */
      *out = HO_D_ONE;
      return 1;
    }
    if (ux > HO_D_ONE) { /* |x| > 1: +inf for y>0, +0 for y<0 */
      *out = ys ? HO_D_ZERO : HO_D_INF;
      return 1;
    }
    *out = ys ? HO_D_INF : HO_D_ZERO; /* |x| < 1, x = +-0 included */
    return 1;
  }
  /* y finite and nonzero from here on. */
  ho_d_isint_odd(yb, &isint, &isodd);
  if (xc == HO_MC_ZERO) {
    if (!ys)
      *out = (isint && isodd && xs) ? HO_D_NEG_ZERO : HO_D_ZERO;
    else
      *out = (isint && isodd && xs) ? HO_D_NEG_INF : HO_D_INF;
    return 1;
  }
  if (xc == HO_MC_INF) {
    if (!ys)
      *out = (isint && isodd && xs) ? HO_D_NEG_INF : HO_D_INF;
    else
      *out = (isint && isodd && xs) ? HO_D_NEG_ZERO : HO_D_ZERO;
    return 1;
  }
  if (xs && !isint) {
    *out = HO_D_NAN_NEG; /* negative base, non-integer exponent */
    return 1;
  }
  return 0;
}

/* atan2: complete special matrix (C99 Annex F / glibc-probed).  The FP
 * general path owns finite nonzero y with finite nonzero x. */
static inline int ho_atan2_special(uint64_t yb, uint64_t xb, uint64_t *out) {
  int yc = ho_d_class(yb);
  int xc = ho_d_class(xb);
  int ys = ho_d_signbit(yb);
  int xs = ho_d_signbit(xb);

  if (yc == HO_MC_NAN) {
    *out = ho_d_quiet(yb);
    return 1;
  }
  if (xc == HO_MC_NAN) {
    *out = ho_d_quiet(xb);
    return 1;
  }
  if (yc == HO_MC_INF) {
    if (xc == HO_MC_INF) {
      *out = ys ? (xs ? HO_D_NEG_3PI4 : HO_D_NEG_PI4)
                : (xs ? HO_D_3PI4 : HO_D_PI4);
      return 1;
    }
    *out = ys ? HO_D_NEG_PI2 : HO_D_PI2; /* +-inf over finite x */
    return 1;
  }
  if (xc == HO_MC_INF) { /* finite y over +-inf: +-0 / +-pi by x's sign */
    *out = xs ? (ys ? HO_D_NEG_PI : HO_D_PI)
              : (ys ? HO_D_NEG_ZERO : HO_D_ZERO);
    return 1;
  }
  if (yc == HO_MC_ZERO) {
    if (!xs)
      *out = ys ? HO_D_NEG_ZERO : HO_D_ZERO;
    else
      *out = ys ? HO_D_NEG_PI : HO_D_PI;
    return 1;
  }
  if (xc == HO_MC_ZERO) { /* y nonzero over +-0 */
    *out = ys ? HO_D_NEG_PI2 : HO_D_PI2;
    return 1;
  }
  return 0;
}

#endif /* HOBBYOS_MATH_DISPATCH_H */
