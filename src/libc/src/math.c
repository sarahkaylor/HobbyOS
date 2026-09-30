/*
 * HobbyOS libc: math.c — portable C99 floating-point subset (P3.2).
 *
 * Scope: exactly the functions libc++ and ported programs need so far, all
 * implemented with IEEE-754-exact integer/bit techniques so the host parity
 * test can race them byte-for-byte against glibc:
 *
 *   fabs family, fmin/fmax, copysign, trunc, floor, ceil, round,
 *   lround/llround, fmod, rint/nearbyint, fdim.
 *
 * The transcendentals (sin/cos/exp/pow/...) are declared in <math.h> but
 * deliberately NOT implemented yet: referencing them fails at link time
 * rather than returning wrong values (browser.md §6 P3.2 records this).
 *
 * Long double === double on this port (both targets would otherwise need
 * soft-float quad helpers that userland does not link); the *l entry
 * points below forward to the double implementations.
 *
 * HOST_TEST renames every symbol to hb_* so host tests can race glibc.
 */
#include <math.h>

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
