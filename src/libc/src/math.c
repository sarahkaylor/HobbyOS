/*
 * HobbyOS libc: math.c — portable C99 floating-point subset (P3.2).
 *
 * Scope: the functions libc++ and ported programs need so far, all
 * implemented with IEEE-754-exact integer/bit techniques so the host parity
 * test can race them byte-for-byte against glibc:
 *
 *   fabs family, fmin/fmax, copysign, trunc, floor, ceil, round,
 *   lround/llround, fmod, rint/nearbyint, fdim.
 *
 * Transcendentals: the ICU link-hole set — sin, cos, tan, asin, atan,
 * atan2, log, pow, sqrt, modf, plus expf/tanhf — lives at the bottom of
 * this file behind a banner comment with the accuracy contract.  The rest
 * of the declared-only <math.h> set still fails at link time rather than
 * returning wrong values (browser.md §6 P3.2).
 *
 * Long double === double on this port (both targets would otherwise need
 * soft-float quad helpers that userland does not link); the *l entry
 * points below forward to the double implementations.
 *
 * HOST_TEST renames every symbol to hb_* so host tests can race glibc.
 */
#include <math.h>
#include "math_dispatch.h"

#ifdef HOST_TEST
#define fabs hb_fabs
#define fabsf hb_fabsf
#define fabsl hb_fabsl
#define fmin hb_fmin
#define fminf hb_fminf
#define fmax hb_fmax
#define fmaxf hb_fmaxf
#define copysign hb_copysign
#define copysignf hb_copysignf
#define trunc hb_trunc
#define truncf hb_truncf
#define truncl hb_truncl
#define floor hb_floor
#define floorf hb_floorf
#define floorl hb_floorl
#define ceil hb_ceil
#define ceilf hb_ceilf
#define ceill hb_ceill
#define round hb_round
#define roundf hb_roundf
#define roundl hb_roundl
#define lround hb_lround
#define lroundf hb_lroundf
#define llround hb_llround
#define llroundf hb_llroundf
#define fmod hb_fmod
#define fmodf hb_fmodf
#define rint hb_rint
#define rintf hb_rintf
#define nearbyint hb_nearbyint
#define nearbyintf hb_nearbyintf
#define fdim hb_fdim
#define sin hb_sin
#define cos hb_cos
#define tan hb_tan
#define asin hb_asin
#define atan hb_atan
#define atan2 hb_atan2
#define log hb_log
#define pow hb_pow
#define sqrt hb_sqrt
#define sqrtf hb_sqrtf
#define modf hb_modf
#define modff hb_modff
#define expf hb_expf
#define tanhf hb_tanhf

/* The renames rewrite intra-file call sites too: everything called before
 * its definition needs an hb_* prototype (plain names -- macros rename). */
double fabs(double x);
float fabsf(float x);
double fmin(double x, double y);
float fminf(float x, float y);
double fmax(double x, double y);
float fmaxf(float x, float y);
double copysign(double x, double y);
float copysignf(float x, float y);
double trunc(double x);
float truncf(float x);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double round(double x);
float roundf(float x);
double fmod(double x, double y);
float fmodf(float x, float y);
double rint(double x);
float rintf(float x);
double nearbyint(double x);
float nearbyintf(float x);
double fdim(double x, double y);
#endif

typedef union {
  double d;
  unsigned long long u;
} dshape_t;

typedef union {
  float f;
  unsigned int u;
} fshape_t;

#define DBITS 52
#define DEXP_ONE 0x3FFULL

static int d_isnan(double x) { return x != x; }
static int f_isnan(float x) { return x != x; }

/* Exact truncation toward zero: the cast is exact for |x| < 2^52, and
 * every value >= 2^52 is already an integer. */
static double d_trunc(double x) {
  if (d_isnan(x) || __builtin_isinf(x))
    return x;
  if (x >= 4503599627370496.0 || x <= -4503599627370496.0)
    return x;
  {
    double r = (double)(long long)x;
    /* (-1, 0) and -0.0 truncate to -0.0 (glibc); the cast alone gives +0.0 */
    if (r == 0.0)
      return copysign(0.0, x);
    return r;
  }
}

static float f_trunc(float x) {
  if (f_isnan(x) || __builtin_isinf(x))
    return x;
  if (x >= 8388608.0f || x <= -8388608.0f)
    return x;
  {
    float r = (float)(long long)x;
    if (r == 0.0f)
      return copysignf(0.0f, x);
    return r;
  }
}

static double d_round_half_away(double x) {
  double t, frac;
  if (x >= 4503599627370496.0 || x <= -4503599627370496.0)
    return x;
  t = d_trunc(x);
  frac = x - t; /* exact: t and x are within a factor of 2 */
  if (frac >= 0.5)
    return t + 1.0;
  if (frac <= -0.5)
    return t - 1.0;
  return t;
}

static float f_round_half_away(float x) {
  float t, frac;
  if (x >= 8388608.0f || x <= -8388608.0f)
    return x;
  t = f_trunc(x);
  frac = x - t;
  if (frac >= 0.5f)
    return t + 1.0f;
  if (frac <= -0.5f)
    return t - 1.0f;
  return t;
}

/* Exact fmod via scaling + Sterbenz subtractions: every subtraction below
 * is exact (the operands are within a factor of two), so the result is the
 * true remainder. */
static double d_fmod(double x, double y) {
  double ay, ax;
  unsigned int k = 0;

  if (d_isnan(x) || d_isnan(y) || __builtin_isinf(x) || y == 0.0)
    return (x * y) / (x * y); /* NaN: glibc semantics */
  if (x == 0.0)
    return x; /* keep the sign of zero */
  if (__builtin_isinf(y))
    return x;

  ax = fabs(x);
  ay = fabs(y);
  if (ax < ay)
    return x;
  while (ay * 2.0 <= ax) { /* scale y up to <= x */
    ay *= 2.0;
    k++;
  }
  for (;;) {
    if (ax >= ay)
      ax -= ay;
    if (k == 0)
      break;
    ay *= 0.5;
    k--;
  }
  return copysign(ax, x);
}

static float f_fmod(float x, float y) {
  float ay, ax;
  unsigned int k = 0;

  if (f_isnan(x) || f_isnan(y) || __builtin_isinf(x) || y == 0.0f)
    return (x * y) / (x * y);
  if (x == 0.0f)
    return x;
  if (__builtin_isinf(y))
    return x;

  ax = fabsf(x);
  ay = fabsf(y);
  if (ax < ay)
    return x;
  while (ay * 2.0f <= ax) {
    ay *= 2.0f;
    k++;
  }
  for (;;) {
    if (ax >= ay)
      ax -= ay;
    if (k == 0)
      break;
    ay *= 0.5f;
    k--;
  }
  return copysignf(ax, x);
}

/* ---------------- exported set ---------------- */

double fabs(double x) { return __builtin_fabs(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
long double fabsl(long double x) { return (long double)__builtin_fabs((double)x); }

/* glibc's rule for the otherwise unspecified equal-operand case (probed
 * with the -0.0/+0.0 pairs, both orders): the FIRST argument wins -- fmin
 * is "x <= y ? x : y", fmax is "x >= y ? x : y".  So fmin(+0,-0)=+0 and
 * fmax(-0,+0)=-0 on glibc, and the parity test pins exactly that. */
double fmin(double x, double y) {
  if (d_isnan(x))
    return y;
  if (d_isnan(y))
    return x;
  return x <= y ? x : y;
}

float fminf(float x, float y) {
  if (f_isnan(x))
    return y;
  if (f_isnan(y))
    return x;
  return x <= y ? x : y;
}

double fmax(double x, double y) {
  if (d_isnan(x))
    return y;
  if (d_isnan(y))
    return x;
  return x >= y ? x : y;
}

float fmaxf(float x, float y) {
  if (f_isnan(x))
    return y;
  if (f_isnan(y))
    return x;
  return x >= y ? x : y;
}

double copysign(double x, double y) {
  dshape_t dx, dy;
  dx.d = x;
  dy.d = y;
  dx.u = (dx.u & ~(1ULL << 63)) | (dy.u & (1ULL << 63));
  return dx.d;
}

float copysignf(float x, float y) {
  fshape_t dx, dy;
  dx.f = x;
  dy.f = y;
  dx.u = (dx.u & ~0x80000000u) | (dy.u & 0x80000000u);
  return dx.f;
}

double trunc(double x) { return d_trunc(x); }
float truncf(float x) { return f_trunc(x); }
long double truncl(long double x) { return (long double)d_trunc((double)x); }

double floor(double x) {
  double t = d_trunc(x);
  if (t > x)
    t -= 1.0;
  return t;
}

float floorf(float x) {
  float t = f_trunc(x);
  if (t > x)
    t -= 1.0f;
  return t;
}

long double floorl(long double x) { return (long double)floor((double)x); }

double ceil(double x) {
  double t = d_trunc(x);
  if (t < x)
    t += 1.0;
  return t;
}

float ceilf(float x) {
  float t = f_trunc(x);
  if (t < x)
    t += 1.0f;
  return t;
}

long double ceill(long double x) { return (long double)ceil((double)x); }

double round(double x) { return d_round_half_away(x); }
float roundf(float x) { return f_round_half_away(x); }
long double roundl(long double x) {
  return (long double)d_round_half_away((double)x);
}

long lround(double x) { return (long)d_round_half_away(x); }
long lroundf(float x) { return (long)f_round_half_away(x); }
long long llround(double x) { return (long long)d_round_half_away(x); }
long long llroundf(float x) { return (long long)f_round_half_away(x); }

double fmod(double x, double y) { return d_fmod(x, y); }
float fmodf(float x, float y) { return f_fmod(x, y); }

/* rint/nearbyint: round-to-nearest-even, exact via the add-magic trick
 * (2^52 + x rounds off the fraction; the constant is exactly representable
 * for |x| < 2^51, which covers the trick's domain). */
/* Round-to-nearest, ties-to-even: truncate, then inspect the exact fraction
 * (|frac| < 1, so it is error-free).  The classic (x + 2^52) - 2^52 trick is
 * NOT used: for negative x the add is exact (e.g. -1.5 + 2^52) and performs
 * no rounding at all -- caught by the host parity test. */
static double d_rint(double x) {
  double t, frac;
  if (d_isnan(x) || __builtin_isinf(x) || fabs(x) >= 4503599627370496.0)
    return x;
  t = d_trunc(x);
  frac = x - t;
  if (frac > 0.5)
    return t + 1.0;
  if (frac < -0.5)
    return t - 1.0;
  if (frac == 0.5 || frac == -0.5) {
    if (((long long)t & 1) != 0)
      t += (frac > 0.0) ? 1.0 : -1.0;
  }
  if (t == 0.0)
    return copysign(0.0, x);
  return t;
}

static float f_rint(float x) {
  float t, frac;
  if (f_isnan(x) || __builtin_isinf(x) || fabsf(x) >= 8388608.0f)
    return x;
  t = f_trunc(x);
  frac = x - t;
  if (frac > 0.5f)
    return t + 1.0f;
  if (frac < -0.5f)
    return t - 1.0f;
  if (frac == 0.5f || frac == -0.5f) {
    if (((long long)t & 1) != 0)
      t += (frac > 0.0f) ? 1.0f : -1.0f;
  }
  if (t == 0.0f)
    return copysignf(0.0f, x);
  return t;
}

double rint(double x) { return d_rint(x); }
float rintf(float x) { return f_rint(x); }
double nearbyint(double x) { return d_rint(x); }
float nearbyintf(float x) { return f_rint(x); }

double fdim(double x, double y) {
  if (d_isnan(x) || d_isnan(y))
    return x * y / (x * y);
  return x > y ? x - y : 0.0;
}

/* ======================= P3.2: transcendentals ==========================
 *
 * Closes the ICU 78.3 link-hole set: sin cos tan asin atan atan2 log pow
 * sqrt modf (double) + expf tanhf (float), plus the sqrtf/modff companions
 * <math.h> already advertised.  Structure, top to bottom: five-chunk
 * Cody-Waite pi/2 reduction -> sin/cos kernels -> tan -> atan (table +
 * argument-reduction formula) -> asin/atan2 -> log -> exp core -> expf /
 * tanhf -> pow -> sqrt -> modf.
 *
 * Accuracy contract (clean-room: no libm code copied; polynomials are
 * Taylor truncations / double-double-computed table entries, all verified
 * in the lane's numeric harness, and the numbers below are enforced by
 * src/host/libc_num_parity_test.c racing glibc over dense grids):
 *   sqrt, modf       correctly rounded / exact
 *   sin cos expf     <= 1.5 ulp (kernels measure 1 ulp; tan needs the
 *                    reduction's ~2^-150 relative precision below)
 *   tan              <= 2.5 ulp including near-pole arguments
 *   atan asin        <= 2.5 ulp
 *   atan2            <= 2.5 ulp
 *   log              <= 1.5 ulp (subnormals scaled exactly)
 *   pow              <= 4 ulp general; <= ~3 ulp on the small-integer
 *                    fast path; special matrix exact (math_dispatch.h)
 * All special values are decided in math_dispatch.h (integer-only, so the
 * EL1 kernel unit suite exercises the same decisions).
 *
 * FP contraction MUST stay off for this TU (-ffp-contract=off, Makefile):
 * the stage residual recoveries and the Dekker two_product depend on
 * strictly-rounded individual operations.
 */

#define D_MANT_MASK 0x000fffffffffffffULL
#define D_ONE_EXP 0x3ff0000000000000ULL

static double d_sqrt(double x); /* hardware sqrt, defined with the epilogue */

/* pi/2 as five chunks; every chunk has few enough trailing bits that
 * n*chunk is an exact product for |n| <= 5.8e6 (c4 is the tight one).
 * The dropped tail is ~2^-174, so the reduced argument keeps ~2^-110
 * relative accuracy even for near-pole arguments (validated against exact
 * rational arithmetic in the lane harness, 20k cases). */
static const double PIO2_1 = 0x1.921fb54400000p+0;
static const double PIO2_2 = 0x1.0b4611a600000p-34;
static const double PIO2_3 = 0x1.3198a2e000000p-69;
static const double PIO2_4 = 0x1.b839a25200000p-104;
static const double PIO2_5 = 0x1.2704453300000p-142;
static const double PIO2_HI = 0x1.921fb54442d18p+0;   /* pi/2, rounded  */
static const double PIO2_LO = 0x1.1a62633145c07p-54;  /* pi/2 - PIO2_HI */
static const double PI_HI = 0x1.921fb54442d18p+1;     /* pi, rounded    */
static const double PI_LO = 0x1.1a62633145c07p-53;    /* pi - PI_HI     */
static const double D_INV_PIO2 = 0x1.45f306dc9c883p-1; /* 2/pi */
/* ln2 split: D_LN2_HI has 22 trailing zeros so k*D_LN2_HI is exact for
 * every k this file produces; the pair carries ~2^-86. */
static const double D_LN2_HI = 0x1.62e42fec00000p-1;
static const double D_LN2_LO = 0x1.d1cf79abc9e3bp-32;
static const double D_INV_LN2 = 0x1.71547652b82fep+0;

/* sin(r) = r * S(r^2), |r| <= pi/4; Taylor through r^19 (next term
 * < 2^-72, so truncation is far under the evaluation error). */
static const double D_SIN_C[10] = {
  0x1.0000000000000p+0 /* 1.0 */, -0x1.5555555555555p-3 /* -1/6 */,
  0x1.1111111111111p-7 /* 1/120 */, -0x1.a01a01a01a01ap-13 /* -1/5040 */,
  0x1.71de3a556c734p-19 /* 1/362880 */, -0x1.ae64567f544e4p-26 /* -1/39916800 */,
  0x1.6124613a86d09p-33 /* 1/6227020800 */, -0x1.ae7f3e733b81fp-41 /* -1/1307674368000 */,
  0x1.952c77030ad4ap-49 /* 1/355687428096000 */, -0x1.2f49b46814157p-57 /* -1/121645100408832000 */
};

/* cos(r) = C(r^2), |r| <= pi/4; Taylor through r^20. */
static const double D_COS_C[11] = {
  0x1.0000000000000p+0 /* 1.0 */, -0x1.0000000000000p-1 /* -0.5 */,
  0x1.5555555555555p-5 /* 1/24 */, -0x1.6c16c16c16c17p-10 /* -1/720 */,
  0x1.a01a01a01a01ap-16 /* 1/40320 */, -0x1.27e4fb7789f5cp-22 /* -1/3628800 */,
  0x1.1eed8eff8d898p-29 /* 1/479001600 */, -0x1.93974a8c07c9dp-37 /* -1/87178291200 */,
  0x1.ae7f3e733b81fp-45 /* 1/20922789888000 */, -0x1.6827863b97d97p-53 /* -1/6402373705728000 */,
  0x1.e542ba4020225p-62 /* 1/2432902008176640000 */
};

/* atan(r) = r * P(r^2), |r| <= 0.25; Taylor through r^27. */
static const double D_ATAN_C[14] = {
  0x1.0000000000000p+0 /* 1.0 */, -0x1.5555555555555p-2 /* -1/3 */,
  0x1.999999999999ap-3 /* 1/5 */, -0x1.2492492492492p-3 /* -1/7 */,
  0x1.c71c71c71c71cp-4 /* 1/9 */, -0x1.745d1745d1746p-4 /* -1/11 */,
  0x1.3b13b13b13b14p-4 /* 1/13 */, -0x1.1111111111111p-4 /* -1/15 */,
  0x1.e1e1e1e1e1e1ep-5 /* 1/17 */, -0x1.af286bca1af28p-5 /* -1/19 */,
  0x1.8618618618618p-5 /* 1/21 */, -0x1.642c8590b2164p-5 /* -1/23 */,
  0x1.47ae147ae147bp-5 /* 1/25 */, -0x1.2f684bda12f68p-5 /* -1/27 */
};

/* log(m) = 2s * Q(s^2), s = (m-1)/(m+1), m in [sqrt(1/2), sqrt(2)];
 * non-alternating log series through s^27 (u = s^2 <= 0.0295). */
static const double D_LOG_C[14] = {
  0x1.0000000000000p+0 /* 1.0 */, 0x1.5555555555555p-2 /* 1/3 */,
  0x1.999999999999ap-3 /* 1/5 */, 0x1.2492492492492p-3 /* 1/7 */,
  0x1.c71c71c71c71cp-4 /* 1/9 */, 0x1.745d1745d1746p-4 /* 1/11 */,
  0x1.3b13b13b13b14p-4 /* 1/13 */, 0x1.1111111111111p-4 /* 1/15 */,
  0x1.e1e1e1e1e1e1ep-5 /* 1/17 */, 0x1.af286bca1af28p-5 /* 1/19 */,
  0x1.8618618618618p-5 /* 1/21 */, 0x1.642c8590b2164p-5 /* 1/23 */,
  0x1.47ae147ae147bp-5 /* 1/25 */, 0x1.2f684bda12f68p-5 /* 1/27 */
};

/* exp(r) = E(r), |r| <= ln2/2 + eps; Taylor through r^15. */
static const double D_EXP_C[16] = {
  0x1.0000000000000p+0 /* 1/0! */, 0x1.0000000000000p+0 /* 1/1! */,
  0x1.0000000000000p-1 /* 1/2! */, 0x1.5555555555555p-3 /* 1/3! */,
  0x1.5555555555555p-5 /* 1/4! */, 0x1.1111111111111p-7 /* 1/5! */,
  0x1.6c16c16c16c17p-10 /* 1/6! */, 0x1.a01a01a01a01ap-13 /* 1/7! */,
  0x1.a01a01a01a01ap-16 /* 1/8! */, 0x1.71de3a556c734p-19 /* 1/9! */,
  0x1.27e4fb7789f5cp-22 /* 1/10! */, 0x1.ae64567f544e4p-26 /* 1/11! */,
  0x1.1eed8eff8d898p-29 /* 1/12! */, 0x1.6124613a86d09p-33 /* 1/13! */,
  0x1.93974a8c07c9dp-37 /* 1/14! */, 0x1.ae7f3e733b81fp-41 /* 1/15! */
};

/* Exact residual of a*b (Dekker/Veltkamp, no fma): *hi = a*b, and
 * *hi + *lo == a*b to within a final rounding; requires |a|,|b| < 2^996
 * and strictly-rounded operations (contraction off). */
static void d_two_product(double a, double b, double *hi, double *lo) {
  double p, t, ah, al, bh, bl;

  p = a * b;
  t = a * 134217729.0; /* 2^27 + 1 */
  ah = t - (t - a);
  al = a - ah;
  t = b * 134217729.0;
  bh = t - (t - b);
  bl = b - bh;
  *hi = p;
  *lo = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

/* n/d with the division rounding largely compensated (~0.5 ulp total,
 * vs 1 for a raw division); tan and pow use it on their critical paths. */
static double d_div_comp(double n, double d) {
  double q, ph, pl;

  q = n / d;
  d_two_product(q, d, &ph, &pl);
  return q + (n - ph - pl) / d;
}

static double d_sin_kernel(double r, double rlo) {
  double u, s;
  u = r * r;
  s = D_SIN_C[9];
  s = s * u + D_SIN_C[8];
  s = s * u + D_SIN_C[7];
  s = s * u + D_SIN_C[6];
  s = s * u + D_SIN_C[5];
  s = s * u + D_SIN_C[4];
  s = s * u + D_SIN_C[3];
  s = s * u + D_SIN_C[2];
  s = s * u + D_SIN_C[1];
  s = s * u + D_SIN_C[0];
  return r * s + rlo;
}

static double d_cos_kernel(double r, double rlo) {
  double u, c;
  u = r * r;
  c = D_COS_C[10];
  c = c * u + D_COS_C[9];
  c = c * u + D_COS_C[8];
  c = c * u + D_COS_C[7];
  c = c * u + D_COS_C[6];
  c = c * u + D_COS_C[5];
  c = c * u + D_COS_C[4];
  c = c * u + D_COS_C[3];
  c = c * u + D_COS_C[2];
  c = c * u + D_COS_C[1];
  c = c * u + D_COS_C[0];
  return c - rlo * r; /* cos(r+rlo) = cos(r) - rlo*sin(r) */
}

/* Cody-Waite reduction: r = x - n*(pi/2) as (rhi, rlo), *q = n mod 4.
 * Each chunk product n*chunk goes through Dekker two_product first, so
 * every subtraction below is either exact (Sterbenz) or its rounding is
 * recovered exactly by the residual line: the reduced argument carries
 * full double-double precision for |x| < D_TRIG_MAX.
 *
 * D_TRIG_MAX: for |x| below it, v = x*(2/pi) is under 2^52, so rint()'s
 * result n is exact (no lost quadrant bits; the count of leading n bits
 * stays <= 53 and ulp(n) <= 1). The double x*(2/pi) can then sit at most
 * ~1 away from the true quotient, and n*(pi/2) is computed exactly, so
 * r stays within [-pi/2, pi/2] and sin/cos/tan reduce with full precision.
 * At or above the bound the quadrant can no longer be recovered from n's
 * low bits without full Payne-Hanek, so the callers return NaN. */
#define D_TRIG_MAX 5.0e15

#define D_REM_STEP(term) \
  do { \
    t = s - (term); \
    *rlo += (s - t) - (term); \
    s = t; \
  } while (0)

static int d_rem_pio2(double x, double *rhi, double *rlo) {
  double fn, s, t, w, wl;
  long long n;

  fn = d_rint(x * D_INV_PIO2);
  n = (long long)fn;
  *rlo = 0.0;
  s = x;
  d_two_product(fn, PIO2_1, &w, &wl);
  D_REM_STEP(w);
  D_REM_STEP(wl);
  d_two_product(fn, PIO2_2, &w, &wl);
  D_REM_STEP(w);
  D_REM_STEP(wl);
  d_two_product(fn, PIO2_3, &w, &wl);
  D_REM_STEP(w);
  D_REM_STEP(wl);
  d_two_product(fn, PIO2_4, &w, &wl);
  D_REM_STEP(w);
  D_REM_STEP(wl);
  d_two_product(fn, PIO2_5, &w, &wl);
  D_REM_STEP(w);
  D_REM_STEP(wl);
  *rhi = s;
  return (int)(n & 3);
}

double sin(double x) {
  dshape_t b;
  uint64_t sp;
  double ax, rhi, rlo;
  int q;

  b.d = x;
  if (ho_trig_special(b.u, &sp)) {
    b.u = sp;
    return b.d;
  }
  if (x == 0.0)
    return x; /* +-0, sign preserved */
  ax = fabs(x);
  if (ax < 0.7853981633974483)
    return d_sin_kernel(x, 0.0);
  if (!(ax < D_TRIG_MAX)) { /* needs Payne-Hanek beyond: out of contract */
    b.u = HO_D_NAN;
    return b.d;
  }
  q = d_rem_pio2(x, &rhi, &rlo);
  switch (q) {
  case 0:
    return d_sin_kernel(rhi, rlo);
  case 1:
    return d_cos_kernel(rhi, rlo);
  case 2:
    return -d_sin_kernel(rhi, rlo);
  default:
    return -d_cos_kernel(rhi, rlo);
  }
}

double cos(double x) {
  dshape_t b;
  uint64_t sp;
  double ax, rhi, rlo;
  int q;

  b.d = x;
  if (ho_trig_special(b.u, &sp)) {
    b.u = sp;
    return b.d;
  }
  ax = fabs(x);
  if (ax < 0.7853981633974483)
    return d_cos_kernel(x, 0.0);
  if (!(ax < D_TRIG_MAX)) {
    b.u = HO_D_NAN;
    return b.d;
  }
  q = d_rem_pio2(x, &rhi, &rlo);
  switch (q) {
  case 0:
    return d_cos_kernel(rhi, rlo);
  case 1:
    return -d_sin_kernel(rhi, rlo);
  case 2:
    return -d_cos_kernel(rhi, rlo);
  default:
    return d_sin_kernel(rhi, rlo);
  }
}

double tan(double x) {
  dshape_t b;
  uint64_t sp;
  double ax, rhi, rlo, sn, cs;
  int q;

  b.d = x;
  if (ho_trig_special(b.u, &sp)) {
    b.u = sp;
    return b.d;
  }
  if (x == 0.0)
    return x;
  ax = fabs(x);
  if (ax < 0.7853981633974483)
    return d_div_comp(d_sin_kernel(x, 0.0), d_cos_kernel(x, 0.0));
  if (!(ax < D_TRIG_MAX)) {
    b.u = HO_D_NAN;
    return b.d;
  }
  q = d_rem_pio2(x, &rhi, &rlo);
  sn = d_sin_kernel(rhi, rlo);
  cs = d_cos_kernel(rhi, rlo);
  if (q & 1)
    return -d_div_comp(cs, sn); /* tan(q*pi/2 + r), q odd: -cot(r) */
  return d_div_comp(sn, cs);
}

/* atan on [0, 0.25]; all branch reductions below land here. */
static double d_atan_poly(double r) {
  double u, p;
  u = r * r;
  p = D_ATAN_C[13];
  p = p * u + D_ATAN_C[12];
  p = p * u + D_ATAN_C[11];
  p = p * u + D_ATAN_C[10];
  p = p * u + D_ATAN_C[9];
  p = p * u + D_ATAN_C[8];
  p = p * u + D_ATAN_C[7];
  p = p * u + D_ATAN_C[6];
  p = p * u + D_ATAN_C[5];
  p = p * u + D_ATAN_C[4];
  p = p * u + D_ATAN_C[3];
  p = p * u + D_ATAN_C[2];
  p = p * u + D_ATAN_C[1];
  p = p * u + D_ATAN_C[0];
  return r * p;
}

/* atan(x0) table, each entry a double-double (value, tail) so the
 * reconstruction adds at most the final rounding; the x0 set keeps
 * |r| = |(a-x0)/(1+a*x0)| <= 0.223 on every branch. */
static const double D_AT_50 = 0x1.dac670561bb4fp-2, D_AT_50L = 0x1.d4b176abf2eccp-56;
static const double D_AT_100 = 0x1.921fb54442d18p-1, D_AT_100L = 0x1.1a62633145c07p-55;
static const double D_AT_150 = 0x1.f730bd281f69bp-1, D_AT_150L = 0x1.8d38e17d29a08p-55;
static const double D_AT_200 = 0x1.1b6e192ebbe44p+0, D_AT_200L = 0x1.a536058649054p-54;
static const double D_AT_400 = 0x1.5368c951e9cfdp+0, D_AT_400L = -0x1.9857266def8b4p-54;

/* atan(a) for 0 <= a, finite. */
static double d_atan_pos(double a) {
  double r;

  if (a < 0.25)
    return d_atan_poly(a);
  if (a < 0.75) {
    r = (a - 0.5) / (1.0 + 0.5 * a);
    return D_AT_50 + (D_AT_50L + d_atan_poly(r));
  }
  if (a < 1.25) {
    r = (a - 1.0) / (1.0 + a);
    return D_AT_100 + (D_AT_100L + d_atan_poly(r));
  }
  if (a < 1.75) {
    r = (a - 1.5) / (1.0 + 1.5 * a);
    return D_AT_150 + (D_AT_150L + d_atan_poly(r));
  }
  if (a < 3.0) {
    r = (a - 2.0) / (1.0 + 2.0 * a);
    return D_AT_200 + (D_AT_200L + d_atan_poly(r));
  }
  if (a <= 6.0) {
    r = (a - 4.0) / (1.0 + 4.0 * a);
    return D_AT_400 + (D_AT_400L + d_atan_poly(r));
  }
  {
    double t = 1.0 / a; /* t <= 1/6 < 0.25 */
    return PIO2_HI + (PIO2_LO - d_atan_poly(t));
  }
}

double atan(double x) {
  dshape_t b;
  uint64_t sp;
  double r;

  b.d = x;
  if (ho_atan_special(b.u, &sp)) {
    b.u = sp;
    return b.d;
  }
  if (x == 0.0)
    return x;
  r = d_atan_pos(fabs(x));
  return (b.u >> 63) ? -r : r;
}

/* asin(a) for 0 <= a <= 0.5: a/sqrt(1-a^2) then atan. */
static double d_asin_small(double a) {
  double t = d_sqrt(1.0 - a * a);
  return d_atan_pos(a / t);
}

/* asin(a) for 0 <= a <= 1: half-angle form past 0.5 so arguments near 1
 * reconstruct on pi/2 with no cancellation ((1-a) is exact). */
static double d_asin_core(double a) {
  if (a > 0.5) {
    double s = d_sqrt((1.0 - a) * 0.5);
    double v = d_asin_small(s);
    return PIO2_HI + (PIO2_LO - 2.0 * v);
  }
  return d_asin_small(a);
}

double asin(double x) {
  dshape_t b;
  uint64_t sp;
  double r;

  b.d = x;
  if (ho_asin_special(b.u, &sp)) {
    b.u = sp;
    return b.d;
  }
  if (x == 0.0)
    return x;
  r = d_asin_core(fabs(x));
  return (b.u >> 63) ? -r : r;
}

double atan2(double y, double x) {
  dshape_t by, bx;
  uint64_t sp;
  double ay, ax, r;
  int ysign;

  by.d = y;
  bx.d = x;
  if (ho_atan2_special(by.u, bx.u, &sp)) {
    by.u = sp;
    return by.d;
  }
  ysign = ho_d_signbit(by.u);
  ay = fabs(y);
  ax = fabs(x);
  if (ax >= ay) {
    r = d_atan_pos(ay / ax); /* <= pi/4 */
    if (bx.u >> 63)
      r = PI_HI + (PI_LO - r); /* pi - r */
    return ysign ? -r : r;
  }
  r = d_atan_pos(ax / ay); /* < pi/4 */
  r = PIO2_HI + (PIO2_LO - r); /* pi/2 - atan(ax/ay) */
  if (bx.u >> 63)
    r = PI_HI + (PI_LO - r); /* pi/2 + atan(ax/ay), quadrant II */
  return ysign ? -r : r;
}

/* double-double helpers for the log pair: each returns the high part and
 * stores the low part; inputs are (hi, lo) pairs with |lo| <= 0.5 ulp(hi). */
static double d_two_sum(double a, double b, double *e) {
  double s, bb;

  s = a + b;
  bb = s - a;
  *e = (a - (s - bb)) + (b - bb);
  return s;
}

static double d_dd_addr(double ahi, double alo, double bhi, double blo, double *lo) {
  double e, s, t, ee;

  s = d_two_sum(ahi, bhi, &e);
  e += alo + blo;
  t = d_two_sum(s, e, &ee);
  *lo = ee;
  return t;
}

static double d_dd_mulr(double ahi, double alo, double b, double *lo) {
  double p, e, s, ee;

  d_two_product(ahi, b, &p, &e);
  e += alo * b;
  s = d_two_sum(p, e, &ee);
  *lo = ee;
  return s;
}

static double d_dd_mul(double ah, double al, double bh, double bl, double *lo) {
  double ph, pl, cross, s, e;

  d_two_product(ah, bh, &ph, &pl);
  cross = ah * bl + al * bh;
  pl += cross;
  s = d_two_sum(ph, pl, &e);
  *lo = e;
  return s;
}

/* log of a positive finite value as a two-double pair: the pair carries
 * ~2^-87..2^-100 relative accuracy (double-double Horner through Q, exact
 * products for k*ln2 and the LN2 tail), which is what lets pow survive
 * |y*log x| up to ~745 at ~1.5 ulp: a single-double pair would only reach
 * ~2^-57 and pow's error would grow with |y*log x|.  Subnormals are
 * scaled by 2^54 (exact) first; m lands in [sqrt(1/2), sqrt(2)] via the
 * exponent and one conditional halving. */
static void d_log_split(double x, double *hi_out, double *lo_out) {
  dshape_t b;
  double m, s, sc, u, qh, ql, th, tl, Lh, Ll, ah, bh, bl, ch, cl, cl2;
  int k, i;

  b.d = x;
  if ((b.u >> 52) == 0) { /* subnormal: scale into normal range */
    x *= 0x1p54;
    b.d = x;
    k = -54;
  } else {
    k = 0;
  }
  k += (int)((b.u >> 52) & 0x7ff) - 1023;
  b.u = (b.u & D_MANT_MASK) | D_ONE_EXP; /* m in [1, 2) */
  m = b.d;
  if (m > 1.4142135623730951) { /* > sqrt(2): halve into [0.707, 1) */
    m *= 0.5;
    k += 1;
  }
  /* s = (m-1)/(m+1) as a double-double: the plain quotient's 0.5 ulp is
   * the accuracy floor of the whole log (and pow amplifies it by |y|) */
  {
    double num, dp1, de, p, e, r;
    num = m - 1.0;               /* exact */
    dp1 = d_two_sum(m, 1.0, &de); /* m + 1 exact as a pair */
    s = num / dp1;
    d_two_product(s, dp1, &p, &e);
    r = (num - p) - e;           /* exact residual of the division */
    sc = (r - s * de) / dp1;
  }
  u = s * s;
  /* Q(u) = 1 + u/3 + u^2/5 + ... in double-double */
  qh = D_LOG_C[13];
  ql = 0.0;
  for (i = 12; i >= 0; i--) {
    qh = d_dd_mulr(qh, ql, u, &ql);
    qh = d_dd_addr(qh, ql, D_LOG_C[i], 0.0, &ql);
  }
  /* L = 2 s Q in double-double */
  th = d_dd_mul(qh, ql, s, sc, &tl);
  Lh = th + th;
  Ll = tl + tl;
  /* log = k*ln(2) + L: exact k*LN2_HI product, exact tail residual */
  ah = (double)k * D_LN2_HI;
  d_two_product((double)k, D_LN2_LO, &bh, &bl);
  ch = d_dd_addr(ah, 0.0, bh, bl, &cl);
  ch = d_dd_addr(ch, cl, Lh, Ll, &cl2);
  *hi_out = ch;
  *lo_out = cl2;
}

double log(double x) {
  dshape_t b;
  uint64_t sp;
  double hi, lo;

  b.d = x;
  if (ho_log_special(b.u, &sp)) {
    b.u = sp;
    return b.d;
  }
  d_log_split(x, &hi, &lo);
  return hi + lo;
}

/* exp(p) for |p| <= ~709.8 (double quality; also the pow workhorse).
 * Range reduction by k = rint(p/ln2) with the exact k*ln2_hi product;
 * scales in two steps near the subnormal floor. */
static double d_exp_core(double p) {
  double fn, r, t;
  int k;

  if (p > 709.9)
    return HUGE_VAL;
  if (p < -745.2)
    return 0.0;
  fn = d_rint(p * D_INV_LN2);
  k = (int)fn;
  r = p - fn * D_LN2_HI;
  r = r - fn * D_LN2_LO;
  t = D_EXP_C[15];
  t = t * r + D_EXP_C[14];
  t = t * r + D_EXP_C[13];
  t = t * r + D_EXP_C[12];
  t = t * r + D_EXP_C[11];
  t = t * r + D_EXP_C[10];
  t = t * r + D_EXP_C[9];
  t = t * r + D_EXP_C[8];
  t = t * r + D_EXP_C[7];
  t = t * r + D_EXP_C[6];
  t = t * r + D_EXP_C[5];
  t = t * r + D_EXP_C[4];
  t = t * r + D_EXP_C[3];
  t = t * r + D_EXP_C[2];
  t = t * r + D_EXP_C[1];
  t = t * r + D_EXP_C[0];
  if (k < -1022) { /* keep t normal through the subnormal scaling */
    t *= 0x1p-1000;
    k += 1000;
  }
  if (k > 1023) { /* 2^1024 overflows the bit-constructed scale */
    t *= 0.5;
    k -= 1;
  }
  {
    dshape_t s;
    s.u = (uint64_t)(k + 1023) << 52;
    return t * s.d;
  }
}

/* exp(phi+plo) ~ exp(phi)*(1+plo); pow feeds a genuinely small plo
 * (<= ~2 ulp of phi).  The guards also absorb phi = +-inf overflow from
 * extreme y*log(x) before plo can poison the multiply. */
static double d_exp_hi_lo(double phi, double plo) {
  double e;

  if (phi != phi)
    return phi;
  if (phi > 709.9)
    return HUGE_VAL;
  if (phi < -745.2)
    return 0.0;
  e = d_exp_core(phi);
  return e * (1.0 + plo);
}

float expf(float xin) {
  fshape_t fb;
  uint32_t sp;

  fb.f = xin;
  if (ho_expf_special(fb.u, &sp)) {
    fb.u = sp;
    return fb.f;
  }
  return (float)d_exp_hi_lo((double)xin, 0.0);
}

float tanhf(float xin) {
  fshape_t fb;
  uint32_t sp;
  double ax, e, r;

  fb.f = xin;
  if (ho_tanhf_special(fb.u, &sp)) {
    fb.u = sp;
    return fb.f;
  }
  ax = fabs((double)xin);
  if (ax < 0x1p-24)
    return xin; /* tanh(x) == x below a float ulp of correction */
  if (ax >= 20.0)
    return (xin < 0.0f) ? -1.0f : 1.0f; /* e^(2ax) must stay finite */
  e = d_exp_hi_lo(2.0 * ax, 0.0);
  r = (e - 1.0) / (e + 1.0);
  return (xin < 0.0f) ? (float)(-r) : (float)r;
}

/* a^i by repeated squaring; i >= 0 (pow's |y| <= 64 small-integer path;
 * exact for exactly-representable results like 2^10 or 10^3). */
static double d_powi(double a, int i) {
  double r = 1.0, base = a;

  while (i) {
    if (i & 1)
      r *= base;
    i >>= 1;
    if (i)
      base *= base;
  }
  return r;
}

double pow(double x, double y) {
  dshape_t bx, by;
  uint64_t sp;
  double ax, ay, r;
  int xsign, ysign, isint, isodd;

  bx.d = x;
  by.d = y;
  if (ho_pow_special(bx.u, by.u, &sp)) {
    bx.u = sp;
    return bx.d;
  }
  xsign = ho_d_signbit(bx.u);
  ysign = ho_d_signbit(by.u);
  ho_d_isint_odd(by.u, &isint, &isodd); /* x negative: isint guaranteed */
  ax = fabs(x);
  ay = fabs(y);
  if (isint && (ay <= 4.0 || ((bx.u & D_MANT_MASK) == 0 && ay <= 64.0))) {
    /* small-integer squaring path: < ~2 ulp, and exact whenever the
     * result is representable (x^2, powers of two like 2^10, 10^3) */
    int iy = (int)ay; /* exact: ay is an integer in range */
    r = d_powi(ax, iy);
    if (ysign)
      r = 1.0 / r;
  } else {
    /* x^y = exp(y*log(x)) with both products carried as exact residuals
     * (ph/pl from y*hi, th/tl from y*lo) so the exponent lands in a
     * two-sum whose correction term is genuinely <= ~2 ulp of the sum:
     * this is what keeps pow at ~1.5 ulp even for |y*log x| ~ 700. */
    double hi, lo, ph, pl, th, tl, ps, pt;
    d_log_split(ax, &hi, &lo);
    d_two_product(ay, hi, &ph, &pl);
    d_two_product(ay, lo, &th, &tl);
    ps = ph + th;
    pt = ((ph - ps) + th) + pl + tl;
    if (ysign) {
      ps = -ps;
      pt = -pt;
    }
    r = d_exp_hi_lo(ps, pt);
  }
  if (xsign && isodd)
    r = -r;
  return r;
}

/* Correctly-rounded square root from the hardware (both targets have an
 * IEEE-correct FP sqrt); sqrtf rides the same unit with a float->double
 * widening, which is exact and double-rounding-safe for sqrt. */
static double d_sqrt(double x) {
#if defined(__aarch64__)
  __asm__("fsqrt %d0, %d1" : "=w"(x) : "w"(x));
  return x;
#elif defined(__x86_64__)
  __asm__("sqrtsd %1, %0" : "=x"(x) : "x"(x));
  return x;
#else
  return __builtin_sqrt(x);
#endif
}

double sqrt(double x) { return d_sqrt(x); }

float sqrtf(float x) { return (float)d_sqrt((double)x); }

double modf(double x, double *iptr) {
  dshape_t b, t;
  uint64_t ip, fp;

  b.d = x;
  if (ho_d_modf_special(b.u, &ip, &fp)) {
    t.u = ip;
    *iptr = t.d;
    t.u = fp;
    return t.d;
  }
  t.d = d_trunc(x);
  *iptr = t.d;
  if (t.d == x) /* x = +-0 or |x| >= 2^52: fraction carries x's sign */
    return copysign(0.0, x);
  return x - t.d; /* exact */
}

float modff(float x, float *iptr) {
  double xd = (double)x, t;

  if (f_isnan(x)) {
    *iptr = x;
    return x;
  }
  if (__builtin_isinf(x)) {
    *iptr = x;
    return copysignf(0.0f, x);
  }
  t = d_trunc(xd);
  if (t == xd) {
    float z = copysignf(0.0f, x);
    *iptr = x;
    return z;
  }
  *iptr = (float)t;
  return (float)(xd - t);
}
