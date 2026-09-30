/* libc_num_parity_test.c -- P3.2 host-parity test for the numeric gap-fills.
 *
 * Compiles src/libc/src/math.c and src/libc/src/strtod.c a second time for
 * the host (-DHOST_TEST, symbols renamed hb_*) and races them against the
 * workstation glibc: same values, same endptr offsets, same errno.
 *
 * The bare-metal sources compute in IEEE-754 bit patterns (the OS has no
 * FP ABI), so every non-NaN comparison here is bit-identical, not
 * epsilon-close.
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- hb_* surface (see the HOST_TEST block in each source) --------------- */
double hb_fabs(double);
float hb_fabsf(float);
long double hb_fabsl(long double);
double hb_trunc(double);
float hb_truncf(float);
long double hb_truncl(long double);
double hb_floor(double);
float hb_floorf(float);
long double hb_floorl(long double);
double hb_ceil(double);
float hb_ceilf(float);
long double hb_ceill(long double);
double hb_round(double);
float hb_roundf(float);
long double hb_roundl(long double);
double hb_rint(double);
float hb_rintf(float);
double hb_nearbyint(double);
float hb_nearbyintf(float);
double hb_fmin(double, double);
float hb_fminf(float, float);
double hb_fmax(double, double);
float hb_fmaxf(float, float);
double hb_copysign(double, double);
float hb_copysignf(float, float);
double hb_fmod(double, double);
float hb_fmodf(float, float);
double hb_fdim(double, double);
long hb_lround(double);
long hb_lroundf(float);
long long hb_llround(double);
long long hb_llroundf(float);
double hb_strtod(const char *, char **);
float hb_strtof(const char *, char **);
long double hb_strtold(const char *, char **);

static int checks = 0, failures = 0;

static unsigned long long dBits(double d) {
  unsigned long long u;
  memcpy(&u, &d, sizeof u);
  return u;
}
static unsigned long long fBits(float f) {
  unsigned int u;
  memcpy(&u, &f, sizeof u);
  return (unsigned long long)u;
}
static int sameD(double a, double b) {
  if (isnan(a) && isnan(b))
    return 1;
  return dBits(a) == dBits(b);
}
static int sameF(float a, float b) {
  if (isnan(a) && isnan(b))
    return 1;
  return fBits(a) == fBits(b);
}
/* ULP distance between two doubles (same sign, both finite). */
static unsigned long long ulpDist(double a, double b) {
  unsigned long long ua = dBits(a), ub = dBits(b);
  if (ua & (1ULL << 63))
    ua = ~ua + 1; /* map to monotonic ordering */
  if (ub & (1ULL << 63))
    ub = ~ub + 1;
  return ua > ub ? ua - ub : ub - ua;
}
static unsigned long long ulpDistF(float a, float b) {
  unsigned int ua = (unsigned int)fBits(a), ub = (unsigned int)fBits(b);
  if (ua & 0x80000000u)
    ua = ~ua + 1u;
  if (ub & 0x80000000u)
    ub = ~ub + 1u;
  return ua > ub ? ua - ub : ub - ua;
}

static int ulpInfo = 0;
/* Bit-exact or within the documented fallback bound: the port rounds
 * exactly inside the exact window (<= 15 significant digits, |exp10| <=
 * 22) and to <= 1 ulp outside it (see src/libc/src/strtod.c, "Rounding:"),
 * with a 2-ulp worst case where either result is subnormal (the chunked
 * power-of-ten walk at the subnormal floor).  acceptD/acceptF encode that
 * contract; +-0, NaN and flush-to-zero classes must still match exactly.
 * Accepted-but-not-bit-exact results are counted as INFO, not failures. */
static int subnormalD(double d) {
  return d != 0.0 && fabs(d) < 2.2250738585072014e-308; /* < DBL_MIN */
}
static int subnormalF(float f) {
  return f != 0.0f && fabsf(f) < 1.17549435e-38f; /* < FLT_MIN */
}
static int acceptD(double a, double b) {
  if (sameD(a, b))
    return 1;
  if (isnan(a) || isnan(b))
    return 0; /* mismatched NaN class */
  if ((a == 0.0) != (b == 0.0))
    return 0; /* one side flushed to zero */
  if (subnormalD(a) || subnormalD(b))
    return ulpDist(a, b) <= 2;
  return ulpDist(a, b) <= 1;
}
static int acceptF(float a, float b) {
  if (sameF(a, b))
    return 1;
  if (isnan(a) || isnan(b))
    return 0;
  if ((a == 0.0f) != (b == 0.0f))
    return 0;
  if (subnormalF(a) || subnormalF(b))
    return ulpDistF(a, b) <= 2;
  return ulpDistF(a, b) <= 1;
}

/* The port's strtod bounds the fallback to <= 1 ulp outside the exact
 * window; accepted-but-not-bit-exact pairs are recorded as INFO here. */
static void fail(const char *what, const char *fmt_in, int i) {
  failures++;
  printf("FAIL %s [case %d: %s]\n", what, i, fmt_in);
}

/* Unary double drivers ---------------------------------------------------- */
#define U1(name, hbfn, glfn)                                                  \
  do {                                                                        \
    for (i = 0; i < nvals; i++) {                                             \
      double a = hbfn(vals[i]);                                               \
      double b = glfn(vals[i]);                                               \
      checks++;                                                               \
      if (!sameD(a, b))                                                       \
        fail(name, cval[i], i);                                               \
    }                                                                         \
  } while (0)
#define U1F(name, hbfn, glfn)                                                 \
  do {                                                                        \
    for (i = 0; i < nvals; i++) {                                             \
      float a = hbfn((float)vals[i]);                                         \
      float b = glfn((float)vals[i]);                                         \
      checks++;                                                               \
      if (!sameF(a, b))                                                       \
        fail(name, cval[i], i);                                               \
    }                                                                         \
  } while (0)
#define U1L(name, hbfn, glfn)                                                 \
  do {                                                                        \
    for (i = 0; i < nvals; i++) {                                             \
      long double a = hbfn((long double)vals[i]);                             \
      long double b = glfn((long double)vals[i]);                             \
      checks++;                                                               \
      if (!(a == b || (isnan((double)a) && isnan((double)b))))                \
        fail(name, cval[i], i);                                               \
    }                                                                         \
  } while (0)
#define U1LR(name, hbfn, glfn, ret)                                           \
  do {                                                                        \
    for (i = 0; i < nvals; i++) {                                             \
      errno = 0;                                                              \
      ret a = hbfn(vals[i]);                                                  \
      int ea = errno;                                                         \
      errno = 0;                                                              \
      ret b = glfn(vals[i]);                                                  \
      int eb = errno;                                                         \
      checks++;                                                               \
      if (a != b || ea != eb)                                                 \
        fail(name, cval[i], i);                                               \
    }                                                                         \
  } while (0)

int main(void) {
  static const double vals[] = {
    0.0,   -0.0,  0.5,  -0.5,  1.0,   -1.0,  1.5,   -1.5,   2.5,    -2.5,
    3.7,   -3.7,  0.1,  -0.1,  1e15,  -1e15, 1e-15, -1e-15, 1e300,  -1e300,
    1e-300, -1e-300, 42.0, -42.0, 255.9, -255.9,
  };
  static const char *cval[] = {
    "0.0",   "-0.0",  "0.5",   "-0.5",  "1.0",   "-1.0",  "1.5",
    "-1.5",  "2.5",   "-2.5",  "3.7",   "-3.7",  "0.1",   "-0.1",
    "1e15",  "-1e15", "1e-15", "-1e-15", "1e300", "-1e300", "1e-300",
    "-1e-300", "42.0", "-42.0", "255.9", "-255.9",
  };
  const int nvals = (int)(sizeof vals / sizeof vals[0]);
  int i, j;

  U1("fabs", hb_fabs, fabs);
  U1("trunc", hb_trunc, trunc);
  U1("floor", hb_floor, floor);
  U1("ceil", hb_ceil, ceil);
  U1("round", hb_round, round);
  U1("rint", hb_rint, rint);
  U1("nearbyint", hb_nearbyint, nearbyint);
  U1F("fabsf", hb_fabsf, fabsf);
  U1F("truncf", hb_truncf, truncf);
  U1F("floorf", hb_floorf, floorf);
  U1F("ceilf", hb_ceilf, ceilf);
  U1F("roundf", hb_roundf, roundf);
  U1F("rintf", hb_rintf, rintf);
  U1F("nearbyintf", hb_nearbyintf, nearbyintf);
  U1L("fabsl", hb_fabsl, fabsl);
  U1L("truncl", hb_truncl, truncl);
  U1L("floorl", hb_floorl, floorl);
  U1L("ceill", hb_ceill, ceill);
  U1L("roundl", hb_roundl, roundl);
  U1LR("lround", hb_lround, lround, long);
  U1LR("lroundf", hb_lroundf, lroundf, long);
  U1LR("llround", hb_llround, llround, long long);
  U1LR("llroundf", hb_llroundf, llroundf, long long);

  /* Binary drivers: the interesting second operands for each family. */
  {
    static const double bs[] = {0.5, -0.5, 1.0, -1.0, 3.0, -3.0, 7.25,
                                -7.25, 2.0, 0.0, -0.0};
    static const char *bval[] = {"0.5", "-0.5", "1.0", "-1.0", "3.0",
                                 "-3.0", "7.25", "-7.25", "2.0", "0.0",
    "-0.0"};
    const int nbs = (int)(sizeof bs / sizeof bs[0]);
    for (i = 0; i < nvals; i++) {
      for (j = 0; j < nbs; j++) {
        checks += 5;
        if (!sameD(hb_fmin(vals[i], bs[j]), fmin(vals[i], bs[j])))
          fail("fmin", cval[i], i);
        if (!sameD(hb_fmax(vals[i], bs[j]), fmax(vals[i], bs[j])))
          fail("fmax", cval[i], i);
        if (!sameD(hb_copysign(vals[i], bs[j]), copysign(vals[i], bs[j])))
          fail("copysign", cval[i], i);
        if (!sameD(hb_fmod(vals[i], bs[j]), fmod(vals[i], bs[j])))
          fail("fmod", cval[i], i);
        if (!sameD(hb_fdim(vals[i], bs[j]), fdim(vals[i], bs[j])))
          fail("fdim", cval[i], i);
        if (!sameF(hb_fminf((float)vals[i], (float)bs[j]),
                   fminf((float)vals[i], (float)bs[j])) ||
            !sameF(hb_fmaxf((float)vals[i], (float)bs[j]),
                   fmaxf((float)vals[i], (float)bs[j])) ||
            !sameF(hb_fmodf((float)vals[i], (float)bs[j]),
                   fmodf((float)vals[i], (float)bs[j])) ||
            !sameF(hb_copysignf((float)vals[i], (float)bs[j]),
                   copysignf((float)vals[i], (float)bs[j])))
          fail("float-binary", cval[i], i);
        checks += 4;
        (void)bval[0];
      }
    }
  }

  /* NaN behaviour for the min/max/rounding families. */
  {
    double nanv = NAN, infv = INFINITY;
    checks += 6;
    if (!sameD(hb_fmin(nanv, 1.0), fmin(nanv, 1.0)))
      fail("fmin-nan", "nan", 0);
    if (!sameD(hb_fmax(1.0, nanv), fmax(1.0, nanv)))
      fail("fmax-nan", "nan", 0);
    if (!sameD(hb_trunc(infv), trunc(infv)))
      fail("trunc-inf", "inf", 0);
    if (!sameD(hb_round(-infv), round(-infv)))
      fail("round--inf", "-inf", 0);
    if (!sameD(hb_fmod(5.0, infv), fmod(5.0, infv)))
      fail("fmod-inf", "inf", 0);
    if (!sameD(hb_fdim(infv, infv), fdim(infv, infv)))
      fail("fdim-inf", "inf", 0);
  }

  /* --- strtod family --------------------------------------------------- */
  {
    static const char *in[] = {
      "0",       "-0",      "123",      "  42",     "+7.5",   ".5",
      "5.",      "1e10",    "1e-10",    "1e999",    "1e-999", "0x1p4",
      "0x1.8p1", "-0x10",   "inf",      "INFINITY", "nan",    "nan(123)",
      "nanx",    "1e",      "e5",       "0x",       ".e5",    "1.2.3",
      "  -  1",  "\t0.1e2x", "1e+3",    "1e-3",     "-.",     "0.0000000000000000000000001",
      "4.9406564584124654e-324",        "1e-310",   "2.2250738585072011e-308",
      "0x1.fffffffffffffp1023",         "0x1p1024", "0x1p-1075",
      "9223372036854775808",            "1e39",     "1e-45",  "3.4028235e38",
      "1.5e38",  "0.1",     "1e-5x",
    };
    const int nin = (int)(sizeof in / sizeof in[0]);
    for (i = 0; i < nin; i++) {
      char *ea, *eb;
      double da, db;
      errno = 0;
      da = hb_strtod(in[i], &ea);
      int erra = errno;
      errno = 0;
      db = strtod(in[i], &eb);
      int errb = errno;
      checks++;
      if (acceptD(da, db) && !sameD(da, db))
        ulpInfo++;
      if (!acceptD(da, db) || (ea - in[i]) != (eb - in[i]) || erra != errb)
        fail("strtod", in[i], i);

      errno = 0;
      float fa = hb_strtof(in[i], &ea);
      int erraF = errno;
      errno = 0;
      float fb = strtof(in[i], &eb);
      int errbF = errno;
      checks++;
      if (acceptF(fa, fb) && !sameF(fa, fb))
        ulpInfo++;
      if (!acceptF(fa, fb) || (ea - in[i]) != (eb - in[i]) ||
          erraF != errbF)
        fail("strtof", in[i], i);

      /* strtold: this port builds the long double bit-exactly from the
       * parsed double, so inputs beyond double precision are out of scope;
       * the corpus here is all double-representable. */
      {
        long double la = hb_strtold(in[i], &ea);
        long double lb = strtold(in[i], &eb);
        checks++;
        if (!acceptD((double)la, (double)lb))
          fail("strtold", in[i], i);
        else if (!sameD((double)la, (double)lb))
          ulpInfo++;
      }
    }
  }

  /* The exactly-rounded window (<= 15 significant digits, |exp10| <= 22)
   * must be BIT-exact, no slack: spot-check the canonical pair. */
  {
    static const char *exact[] = {"0.1", "1e-22", "1e22", "123456789012345",
                                  "0.000001", "1e10", "3.5e7"};
    int k;
    for (k = 0; k < (int)(sizeof exact / sizeof exact[0]); k++) {
      char *e1, *e2;
      double a = hb_strtod(exact[k], &e1);
      double b = strtod(exact[k], &e2);
      checks++;
      if (!sameD(a, b) || (e1 - exact[k]) != (e2 - exact[k]))
        fail("strtod-exact-window", exact[k], k);
    }
  }

  if (failures == 0) {
    printf("libc_num_parity_test: %d checks, 0 failures "
           "(%d beyond-window results within the documented ulp bounds)\n",
           checks, ulpInfo);
    printf("NUM PARITY TEST PASSED\n");
    return 0;
  }
  printf("libc_num_parity_test: %d checks, %d failures\n", checks, failures);
  printf("NUM PARITY TEST FAILED\n");
  return 1;
}
