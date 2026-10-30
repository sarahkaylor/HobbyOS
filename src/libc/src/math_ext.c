/*
 * HobbyOS libc: frexp / ldexp / scalbn families (icewm.md M2, lane xlibs-ext).
 *
 * <math.h> has declared these since P3.2, but math.c deliberately left them
 * link-time holes until a port needed them.  The X11 client stack does:
 * libpng's png_ascii_from_fp (pulled by freetype/Xft/IceWM) calls frexp.
 *
 * All exact: frexp/frexpf are pure bit manipulation (subnormals normalised
 * by an exact power-of-two multiply first); scalbn/ldexp use musl's
 * construction (at most two exact pre-scalings, then ONE rounding multiply,
 * with the 2^53 / 2^24 guard so subnormal results are not double-rounded).
 * errno is not set on overflow/underflow (math_errhandling is
 * MATH_ERREXCEPT-style on this port, as in math.c).
 * Long double === double on this port: the *l forms forward.
 *
 * HOST_TEST renames every symbol to hb_* so src/host/x11ext_libc_test.c
 * can race them bit-for-bit against glibc.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef HOST_TEST
#define HB_NAME(name) hb_##name
#else
#define HB_NAME(name) name
#endif

static inline uint64_t d2u(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
static inline double u2d(uint64_t u) { double x; memcpy(&x, &u, 8); return x; }
static inline uint32_t f2u(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static inline float u2f(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }

double HB_NAME(frexp)(double x, int *e) {
  uint64_t u = d2u(x);
  int ee = (int)((u >> 52) & 0x7ff);
  if (ee == 0) {
    if ((u << 1) == 0) { *e = 0; return x; }   /* +-0 */
    x = HB_NAME(frexp)(x * 0x1p64, e);          /* subnormal: exact rescale */
    *e -= 64;
    return x;
  }
  if (ee == 0x7ff) { *e = 0; return x; }        /* inf / nan */
  *e = ee - 0x3fe;
  u &= 0x800fffffffffffffull;
  u |= 0x3fe0000000000000ull;
  return u2d(u);
}

float HB_NAME(frexpf)(float x, int *e) {
  uint32_t u = f2u(x);
  int ee = (int)((u >> 23) & 0xff);
  if (ee == 0) {
    if ((u << 1) == 0) { *e = 0; return x; }
    x = HB_NAME(frexpf)(x * 0x1p64f, e);
    *e -= 64;
    return x;
  }
  if (ee == 0xff) { *e = 0; return x; }
  *e = ee - 0x7e;
  u &= 0x807fffffu;
  u |= 0x3f000000u;
  return u2f(u);
}

double HB_NAME(scalbn)(double x, int n) {
  double y = x;
  if (n > 1023) {
    y *= 0x1p1023;
    n -= 1023;
    if (n > 1023) {
      y *= 0x1p1023;
      n -= 1023;
      if (n > 1023) n = 1023;
    }
  } else if (n < -1022) {
    /* 0x1p-1022 * 0x1p53 keeps y normal so the final multiply rounds once. */
    y *= 0x1p-1022 * 0x1p53;
    n += 1022 - 53;
    if (n < -1022) {
      y *= 0x1p-1022 * 0x1p53;
      n += 1022 - 53;
      if (n < -1022) n = -1022;
    }
  }
  return y * u2d((uint64_t)(0x3ff + n) << 52);
}

float HB_NAME(scalbnf)(float x, int n) {
  float y = x;
  if (n > 127) {
    y *= 0x1p127f;
    n -= 127;
    if (n > 127) {
      y *= 0x1p127f;
      n -= 127;
      if (n > 127) n = 127;
    }
  } else if (n < -126) {
    y *= 0x1p-126f * 0x1p24f;
    n += 126 - 24;
    if (n < -126) {
      y *= 0x1p-126f * 0x1p24f;
      n += 126 - 24;
      if (n < -126) n = -126;
    }
  }
  return y * u2f((uint32_t)(0x7f + n) << 23);
}

double HB_NAME(ldexp)(double x, int n) { return HB_NAME(scalbn)(x, n); }
float HB_NAME(ldexpf)(float x, int n) { return HB_NAME(scalbnf)(x, n); }

#ifndef HOST_TEST
long double frexpl(long double x, int *e) { return frexp((double)x, e); }
long double ldexpl(long double x, int n) { return scalbn((double)x, n); }
long double scalbnl(long double x, int n) { return scalbn((double)x, n); }
#endif
