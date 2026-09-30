/*
 * HobbyOS libc: strtod.c — C99 decimal/hex floating-point string conversion
 * (P3.2, browser.md §6).
 *
 * Semantics follow C99 7.20.1.3 with glibc's C-locale behavior:
 *   [ws] [+|-] ( digits [ . digits ] [ (e|E) [+|-] digits ]
 *              | 0x hexdigits [ . hexdigits ] [ (p|P) [+|-] digits ]
 *              | inf | infinity | nan [ ( char* ) ] )
 * Endptr points past the last consumed character; with no conversion it is
 * the original nptr.  Overflow returns +-HUGE_VAL with ERANGE; underflow
 * sets ERANGE too (glibc does for tiny results).
 *
 * Rounding: correctly rounded (bit-identical to glibc) when the decimal
 * significand is exactly representable as a double (<= 15 significant
 * digits) and |exp10| <= 22 -- one multiply/divide, one rounding.  Outside
 * that window the bounded fallback (19-digit significand, power-of-ten
 * chunk scaling) may differ from glibc by <= 1 ulp; no table-driven
 * extended-precision rescaling (Eisel-Lemire) exists in this port yet.
 * src/host/libc_num_parity_test.c pins both: it asserts bit-exactness on
 * the exact subset and bounds the rest to <= 1 ulp.
 *
 * strtold: this port has no fp128 soft-float helpers on aarch64, so the
 * long double value is built bit-exactly from the parsed double (the
 * conversion is exact; long double has at least the range/precision of
 * double).  On x86_64 long double is x87 and the plain cast compiles
 * inline.
 *
 * HOST_TEST renames the entry points to hb_* for the glibc race test.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef HOST_TEST
#define strtod hb_strtod
#define strtof hb_strtof
#define strtold hb_strtold
#define atof hb_atof
#define strtod_l hb_strtod_l
#define strtof_l hb_strtof_l
#define strtold_l hb_strtold_l
#endif

/* Powers of ten, 10^0 .. 10^44.  Up to 10^22 each entry is EXACTLY
 * representable, so a single multiply/divide in that window is one
 * rounding of the true product -- correctly rounded.  Beyond 22 the
 * constant itself is rounded, but a single step is still much closer than
 * the two-rounding chunk walk, and it keeps the parity corpus exact. */
#define POW10_MAX 44
static const double pow10_dbl[POW10_MAX + 1] = {
  1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10,
  1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21,
  1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29, 1e30, 1e31, 1e32,
  1e33, 1e34, 1e35, 1e36, 1e37, 1e38, 1e39, 1e40, 1e41, 1e42, 1e43,
  1e44};

/* Scale a double by 10^e: a single multiply/divide while |e| <= POW10_MAX
 * (one rounding), chunk-wise through 10^44 beyond that (only reachable for
 * inputs whose result is subnormal or near HUGE_VAL). */
static double scale_pow10(double v, int e) {
  if (e == 0 || v == 0.0)
    return v;
  if (e > 0) {
    if (e <= POW10_MAX)
      return v * pow10_dbl[e];
    while (e > POW10_MAX) {
      v *= pow10_dbl[POW10_MAX];
      e -= POW10_MAX;
      if (v > 1.7976931348623157e308)
        break;
    }
    if (e > 0 && v <= 1.7976931348623157e308)
      v *= pow10_dbl[e];
  } else {
    e = -e;
    if (e <= POW10_MAX)
      return v / pow10_dbl[e];
    while (e > POW10_MAX) {
      v /= pow10_dbl[POW10_MAX];
      e -= POW10_MAX;
      if (v < 5e-324)
        break;
    }
    if (e > 0 && v >= 5e-324)
      v /= pow10_dbl[e];
  }
  return v;
}

/* Scale by 2^e exactly (multiplications by exact powers of two; 512-bit
 * chunks keep the loop bounded for extreme exponents). */
static double scale_pow2(double v, long e) {
  while (e >= 512) {
    v *= 1.3407807929942597e154; /* 2^512 */
    e -= 512;
  }
  while (e <= -512) {
    v *= 7.4583407312002070e-155; /* 2^-512 */
    e += 512;
  }
  while (e > 0) {
    v *= 2.0;
    e--;
  }
  while (e < 0) {
    v *= 0.5;
    e++;
  }
  return v;
}

static int d_isdigit(int c) { return c >= '0' && c <= '9'; }
static int d_ishex(int c) {
  return d_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hexval(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

/* Match an ASCII case-insensitive prefix; return 1 and advance *pp. */
static int match_ci(const char **pp, const char *word) {
  const char *p = *pp;
  while (*word) {
    char c = *p;
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    if (c != *word)
      return 0;
    p++;
    word++;
  }
  *pp = p;
  return 1;
}

static double parse_double(const char *nptr, char **endptr) {
  const char *p = nptr;
  int neg = 0;

  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '\f' ||
         *p == '\r')
    p++;

  if (*p == '+' || *p == '-') {
    neg = (*p == '-');
    p++;
  }

  /* inf / infinity / nan */
  {
    const char *q = p;
    if (match_ci(&q, "infinity") || match_ci(&q, "inf")) {
      if (endptr)
        *endptr = (char *)q;
      return neg ? -HUGE_VAL : HUGE_VAL;
    }
    q = p;
    if (match_ci(&q, "nan")) {
      if (*q == '(') { /* glibc accepts nan(chars) */
        const char *r = q + 1;
        while (*r && *r != ')')
          r++;
        if (*r == ')')
          q = r + 1;
      }
      if (endptr)
        *endptr = (char *)q;
      return NAN;
    }
  }

  /* hex float: 0x... */
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    const char *q = p + 2;
    unsigned long long mant = 0;
    int digits = 0, fxdigits = 0;
    long fexp = 0;

    while (*q == '0') { /* skip leading zeros */
      digits++;
      q++;
    }
    while (d_ishex(*q)) {
      if (digits < 19) {
        mant = mant * 16 + (unsigned)hexval(*q);
        digits++;
      } else {
        fexp += 4;
      }
      q++;
    }
    if (*q == '.') {
      q++;
      while (d_ishex(*q)) {
        if (digits < 19) {
          mant = mant * 16 + (unsigned)hexval(*q);
          digits++;
          fexp -= 4;
        }
        fxdigits++;
        q++;
      }
    }
    if (digits > 0 || fxdigits > 0) {
      long pexp = 0, pneg = 0;
      const char *r = q;
      if (*r == 'p' || *r == 'P') {
        r++;
        if (*r == '+' || *r == '-') {
          pneg = (*r == '-');
          r++;
        }
        if (d_isdigit(*r)) {
          while (d_isdigit(*r)) {
            pexp = pexp * 10 + (*r - '0');
            if (pexp > 100000)
              pexp = 100000;
            r++;
          }
          q = r;
        } else {
          pneg = 0; /* no exponent digits: p does not consume */
        }
      }
      {
        double v = (double)mant;
        long e = fexp + (pneg ? -pexp : pexp);
        v = scale_pow2(v, e);
        /* glibc's hex path sets ERANGE for overflow and for any subnormal
         * or zero result (probed: 0x1p1024 -> inf+ERANGE, 0x1p-1075 ->
         * 0.0+ERANGE, 0x1p-1074 -> subnormal+ERANGE). */
        if (__builtin_isinf(v))
          errno = ERANGE;
        else if (v != 0.0 && v < 2.2250738585072014e-308)
          errno = ERANGE;
        else if (v == 0.0 && mant != 0)
          errno = ERANGE;
        if (endptr)
          *endptr = (char *)q;
        return neg ? -v : v;
      }
    }
    /* "0x" with no digits: glibc parses the leading 0 and stops at 'x'. */
    p += 1;
    if (endptr)
      *endptr = (char *)p;
    return neg ? -0.0 : 0.0;
  }

  /* decimal */
  {
    unsigned long long mant = 0;
    int ndig = 0, any = 0;
    int exp10 = 0;

    while (*p == '0') { /* leading zeros: not significant */
      any = 1;
      p++;
    }
    while (d_isdigit(*p)) {
      any = 1;
      if (ndig < 19) {
        mant = mant * 10 + (unsigned)(*p - '0');
        ndig++;
      } else {
        exp10++;
      }
      p++;
    }
    if (*p == '.') {
      p++;
      /* Leading fraction zeros are not significant: skip them without
       * consuming the 19-significant-digit budget (each still moves the
       * decimal exponent).  Without this, "0.0000000000000000000000001"
       * (25 fraction digits) filled the budget with zeros and returned
       * +0.0 -- caught by the host parity test. */
      while (*p == '0' && ndig == 0) {
        any = 1;
        if (exp10 > -1000000)
          exp10--;
        p++;
      }
      while (d_isdigit(*p)) {
        any = 1;
        if (ndig < 19) {
          mant = mant * 10 + (unsigned)(*p - '0');
          ndig++;
          exp10--;
        }
        p++;
      }
    }
    if (!any) { /* no digits at all */
      if (endptr)
        *endptr = (char *)nptr;
      return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
      const char *r = p + 1;
      int eneg = 0, edig = 0;
      long e = 0;
      if (*r == '+' || *r == '-') {
        eneg = (*r == '-');
        r++;
      }
      while (d_isdigit(*r)) {
        e = e * 10 + (*r - '0');
        if (e > 100000)
          e = 100000;
        edig = 1;
        r++;
      }
      if (edig) {
        exp10 += (int)(eneg ? -e : e);
        p = r;
      }
    }
    {
      double v = (double)mant;
      v = scale_pow10(v, exp10);
      if (v > 1.7976931348623157e308 || (v == HUGE_VAL)) {
        errno = ERANGE;
        v = HUGE_VAL;
      } else if (v != 0.0 && v < 2.2250738585072014e-308) {
        errno = ERANGE;
      } else if (v == 0.0 && mant != 0) {
        errno = ERANGE;
      }
      if (endptr)
        *endptr = (char *)p;
      return neg ? -v : v;
    }
  }
}

double strtod(const char *nptr, char **endptr) {
  return parse_double(nptr, endptr);
}

float strtof(const char *nptr, char **endptr) {
  double d = parse_double(nptr, endptr);
  float f = (float)d;

  /* parse_double only knows the double range; glibc's strtof reports
   * ERANGE when the *float* result overflows, becomes subnormal or
   * underflows to zero (probed: "1e39" -> inf+ERANGE, "1e-45" ->
   * subnormal+ERANGE, "1e-46" -> 0+ERANGE). */
  if (d != 0.0) {
    if (__builtin_isinf(f) && !__builtin_isinf(d))
      errno = ERANGE;
    else if ((double)f == 0.0)
      errno = ERANGE;
    else if (__builtin_fabs(d) < 1.1754943508222875e-38)
      errno = ERANGE;
  }
  return f;
}

/* Build a long double from a double without fp128 arithmetic (see file
 * header).  On x86_64 the compiler conversion is inline, so only aarch64
 * takes the bit-construction path. */
long double strtold(const char *nptr, char **endptr) {
  double d = parse_double(nptr, endptr);
#if defined(__aarch64__)
  {
    /* IEEE 754 binary128 layout: [sign:1][exp:15][frac:112].  Every double
     * value is representable exactly; convert bit-exactly. */
    union {
      double d;
      unsigned long long u;
    } du;
    union {
      long double ld;
      struct {
        unsigned long long lo, hi;
      } w;
    } lu;
    unsigned long long fr;
    unsigned sign;
    int e;

    du.d = d;
    sign = (unsigned)(du.u >> 63);
    e = (int)((du.u >> 52) & 0x7FF);
    fr = du.u & 0xFFFFFFFFFFFFFULL;

    if (e == 0x7FF) { /* inf / nan */
      lu.w.hi = ((unsigned long long)sign << 63) | (0x7FFFULL << 48) |
                (fr >> 4) | (fr ? (1ULL << 47) : 0);
      lu.w.lo = fr << 60;
      return lu.ld;
    }
    if (e == 0 && fr == 0) { /* +-0 */
      lu.w.hi = (unsigned long long)sign << 63;
      lu.w.lo = 0;
      return lu.ld;
    }
    if (e == 0) { /* subnormal double: normalize into binary128 */
      int p = 63 - __builtin_clzll(fr); /* msb index of fr (0..51) */
      unsigned exp128 = (unsigned)(-1074 + p + 16383 + 1);
      /* value = 1.frac * 2^(-1074+p): put fr's msb at bit 111 below the
       * implicit 1. */
      unsigned long long f128 = fr << (111 - p);
      lu.w.hi = ((unsigned long long)sign << 63) | ((unsigned long long)exp128 << 48) |
                (f128 >> 64);
      lu.w.lo = f128 & 0xFFFFFFFFFFFFFFFFULL;
      return lu.ld;
    }
    /* normal: exp128 = e - 1023 + 16383, frac <<= 60 */
    lu.w.hi = ((unsigned long long)sign << 63) |
              ((unsigned long long)(e + 15360) << 48) | (fr >> 4);
    lu.w.lo = fr << 60;
    return lu.ld;
  }
#else
  return (long double)d;
#endif
}

double atof(const char *nptr) { return parse_double(nptr, NULL); }

#ifndef HOST_TEST
/* xlocale variants (C locale: locale ignored).  Host builds skip these —
 * glibc already provides them and the race test compares the base
 * functions. */
double strtod_l(const char *nptr, char **endptr, locale_t loc) {
  (void)loc;
  return parse_double(nptr, endptr);
}
float strtof_l(const char *nptr, char **endptr, locale_t loc) {
  (void)loc;
  return strtof(nptr, endptr);
}
long double strtold_l(const char *nptr, char **endptr, locale_t loc) {
  (void)loc;
  return strtold(nptr, endptr);
}
#endif
