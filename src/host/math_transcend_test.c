/*
 * math_transcend_test.c - host-tier acceptance race for the P3.2
 * transcendentals in src/libc/src/math.c (compiled with -DHOST_TEST, so
 * every symbol is hb_*) against glibc's libm on this machine.
 *
 * ~1M deterministic values across the documented domains plus explicit
 * special-value bit checks (NaN signs, +-0, subnormals, domain errors),
 * where the expected answer is glibc's own live output.  Budgets are the
 * measured worst case with one ulp of headroom:
 *   sin/cos/atan/atan2/log/sqrt: 2 ulp;  tan/asin/pow: 4 ulp;
 *   expf/tanhf/sqrtf: 2 ulp;  modf/sqrt: exact where the reference is.
 *
 * The same source is exercised on-device by src/user/math_test.c
 * (MATH_T.BIN) and its integer dispatch by src/kernel/math_test.c (EL1);
 * this file is the dense-probability tier in between.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern double hb_sin(double);
extern double hb_cos(double);
extern double hb_tan(double);
extern double hb_asin(double);
extern double hb_atan(double);
extern double hb_atan2(double, double);
extern double hb_log(double);
extern double hb_pow(double, double);
extern double hb_sqrt(double);
extern double hb_modf(double, double *);
extern float hb_expf(float);
extern float hb_tanhf(float);
extern float hb_sqrtf(float);

static int failures = 0;

static uint64_t db(double x) {
  uint64_t u;
  memcpy(&u, &x, 8);
  return u;
}

static uint32_t fb(float x) {
  uint32_t u;
  memcpy(&u, &x, 4);
  return u;
}

static uint64_t ulp_d(double a, double b) {
  uint64_t ua = db(a), ub = db(b);
  if (ua >> 63)
    ua = (uint64_t)0 - ua;
  if (ub >> 63)
    ub = (uint64_t)0 - ub;
  return ua > ub ? ua - ub : ub - ua;
}

static uint32_t ulp_f(float a, float b) {
  uint32_t ua = fb(a), ub = fb(b);
  if (ua >> 31)
    ua = 0u - ua;
  if (ub >> 31)
    ub = 0u - ub;
  return ua > ub ? ua - ub : ub - ua;
}

/* race one unary double function over a corpus; returns the max ulp seen */
typedef double (*d1fn)(double);

static uint64_t race_d1(const char *name, d1fn mine, d1fn ref, uint64_t budget,
                        unsigned long long *n_out) {
  unsigned long long n = 0, over = 0, maxu = 0;
  double worst = 0.0;
  long i;

  for (i = 0; i <= 100000; i++) {
    double x = -40.0 + (80.0 / 100000.0) * (double)i;
    double a = mine(x), b = ref(x);
    uint64_t d = ulp_d(a, b);
    n++;
    if (d > maxu) {
      maxu = d;
      worst = x;
    }
    if (d > budget) {
      if (over < 4)
        printf("  %s OVER x=%.17g mine=%.17g ref=%.17g ulp=%llu\n", name, x, a,
               b, (unsigned long long)d);
      over++;
    }
  }
  for (i = 0; i <= 20000; i++) { /* scaled bands: reduction stress */
    double m = 1.0 + (double)i * 1.37;
    double x = ldexp(m, (i % 41) - 20) * ((i & 1) ? -1.0 : 1.0);
    double a = mine(x), b = ref(x);
    uint64_t d = ulp_d(a, b);
    n++;
    if (d > maxu)
      maxu = d;
    if (d > budget) {
      if (over < 4)
        printf("  %s OVER x=%.17g mine=%.17g ref=%.17g ulp=%llu\n", name, x, a,
               b, (unsigned long long)d);
      over++;
    }
  }
  {
    double pts[11] = {1e3 + 0.1, 1e4 + 0.3, 1e5 + 0.5, 1e6 + 0.7, -1e6 - 0.9,
                      6.5e6,      -6.5e6,     8388608.5, 1e8 + 0.25, 1e12 + 0.1,
                      5e15 - 1.0};
    for (i = 0; i < 11; i++) {
      double a = mine(pts[i]), b = ref(pts[i]);
      uint64_t d = ulp_d(a, b);
      n++;
      if (d > maxu)
        maxu = d;
      if (d > budget) {
        if (over < 4)
          printf("  %s OVER x=%.17g mine=%.17g ref=%.17g ulp=%llu\n", name,
                 pts[i], a, b, (unsigned long long)d);
        over++;
      }
    }
  }
  *n_out = n;
  if (over) {
    printf("MATH_HOST_%s: FAIL %llu over budget %llu (max %llu ulp)\n", name,
           (unsigned long long)over, (unsigned long long)budget,
           (unsigned long long)maxu);
    failures++;
  } else {
    printf("MATH_HOST_%s: PASS n=%llu max=%lluulp budget=%llu%s\n", name, n,
           (unsigned long long)maxu, (unsigned long long)budget,
           maxu >= budget ? " (at budget)" : "");
  }
  (void)worst;
  return maxu;
}

int main(void) {
  unsigned long long n;
  long i;

  printf("math_transcend_test: hb_* vs glibc libm (x86-64)\n");

  race_d1("SIN", hb_sin, sin, 2, &n);
  race_d1("COS", hb_cos, cos, 2, &n);
  race_d1("TAN", hb_tan, tan, 4, &n);
  race_d1("ASIN", hb_asin, asin, 4, &n);
  race_d1("ATAN", hb_atan, atan, 2, &n);
  race_d1("LOG", hb_log, log, 2, &n);

  /* asin over its domain (race_d1's [-40,40] is mostly the domain error) */
  {
    unsigned long long over = 0, maxu = 0;
    for (i = 0; i <= 100000; i++) {
      double x = -1.0 + (2.0 / 100000.0) * (double)i;
      uint64_t d = ulp_d(hb_asin(x), asin(x));
      if (d > maxu)
        maxu = d;
      if (d > 4) {
        if (over < 4)
          printf("  ASIN OVER x=%.17g ulp=%llu\n", x, (unsigned long long)d);
        over++;
      }
    }
    if (over) {
      printf("MATH_HOST_ASIN_DOMAIN: FAIL %llu over (max %llu ulp)\n", over,
             (unsigned long long)maxu);
      failures++;
    } else {
      printf("MATH_HOST_ASIN_DOMAIN: PASS n=100001 max=%lluulp\n",
             (unsigned long long)maxu);
    }
  }

  /* log over a wide positive range */
  {
    unsigned long long over = 0, maxu = 0;
    for (i = 0; i <= 100000; i++) {
      double x = exp(((double)i / 100000.0) * 600.0 - 300.0);
      uint64_t d = ulp_d(hb_log(x), log(x));
      if (d > maxu)
        maxu = d;
      if (d > 2) {
        if (over < 4)
          printf("  LOG OVER x=%.17g ulp=%llu\n", x, (unsigned long long)d);
        over++;
      }
    }
    if (over) {
      printf("MATH_HOST_LOG_RANGE: FAIL %llu over (max %llu ulp)\n", over,
             (unsigned long long)maxu);
      failures++;
    } else {
      printf("MATH_HOST_LOG_RANGE: PASS n=100001 max=%lluulp\n",
             (unsigned long long)maxu);
    }
  }

  /* atan2 pairs */
  {
    double ys[6] = {1.0, -1.0, 3.0, -0.25, 1e10, -1e-10};
    double xs[6] = {1.0, -1.0, 4.0, -2.0, 1e-10, -1e10};
    unsigned long long over = 0, maxu = 0, cnt = 0;
    long a, b;

    for (a = 0; a < 6; a++)
      for (b = 0; b < 6; b++)
        for (i = 0; i <= 500; i++) {
          double y = ys[a] * (1.0 + (double)i / 500.0);
          double x = xs[b] * (1.0 + (double)i / 331.0);
          uint64_t d = ulp_d(hb_atan2(y, x), atan2(y, x));
          cnt++;
          if (d > maxu)
            maxu = d;
          if (d > 2) {
            if (over < 4)
              printf("  ATAN2 OVER y=%.17g x=%.17g ulp=%llu\n", y, x,
                     (unsigned long long)d);
            over++;
          }
        }
    if (over) {
      printf("MATH_HOST_ATAN2: FAIL %llu over (max %llu ulp)\n", over,
             (unsigned long long)maxu);
      failures++;
    } else {
      printf("MATH_HOST_ATAN2: PASS n=%llu max=%lluulp\n", cnt,
             (unsigned long long)maxu);
    }
  }

  /* pow over the integer sweep that stresses |y*log x| */
  {
    double base[14] = {2.0,  10.0,     0.5,   1.5,    0.1,  3.7,       100.0,
                       1e-5, 1234.5,   0.9999, 7.0,   1e10, 1e-10,   1.0000000001};
    unsigned long long over = 0, maxu = 0, cnt = 0;
    long a;
    int j;

    for (a = 0; a < 14; a++)
      for (j = -70; j <= 70; j++) {
        double y = (double)j;
        uint64_t d = ulp_d(hb_pow(base[a], y), pow(base[a], y));
        cnt++;
        if (d > maxu)
          maxu = d;
        if (d > 4) {
          if (over < 4)
            printf("  POW OVER base=%.17g y=%d ulp=%llu\n", base[a], j,
                   (unsigned long long)d);
          over++;
        }
      }
    {
      double frac[8] = {0.5, 0.25, 2.5, 0.3333333333333333, 1.5,
                        -0.75, 7.25, 1e-3};
      for (a = 0; a < 8; a++) {
        uint64_t d = ulp_d(hb_pow(1.5, frac[a]), pow(1.5, frac[a]));
        cnt++;
        if (d > maxu)
          maxu = d;
        if (d > 4) {
          if (over < 4)
            printf("  POW OVER y=%.17g ulp=%llu\n", frac[a],
                   (unsigned long long)d);
          over++;
        }
      }
    }
    if (over) {
      printf("MATH_HOST_POW: FAIL %llu over (max %llu ulp)\n", over,
             (unsigned long long)maxu);
      failures++;
    } else {
      printf("MATH_HOST_POW: PASS n=%llu max=%lluulp\n", cnt,
             (unsigned long long)maxu);
    }
  }

  /* sqrt: exact for perfect squares, correctly rounded otherwise */
  {
    unsigned long long over = 0, maxu = 0, cnt = 0;
    for (i = 0; i <= 60000; i++) {
      double x = (double)(i * i);
      uint64_t d = ulp_d(hb_sqrt(x), (double)i);
      cnt++;
      if (d > maxu)
        maxu = d;
      if (d)
        over++;
    }
    for (i = 0; i <= 40000; i++) {
      double x = 1e-300 * (1.0 + (double)i);
      uint64_t d = ulp_d(hb_sqrt(x), sqrt(x));
      cnt++;
      if (d > maxu)
        maxu = d;
      if (d > 1) {
        if (over < 4)
          printf("  SQRT OVER x=%.17g ulp=%llu\n", x, (unsigned long long)d);
        over++;
      }
    }
    if (over) {
      printf("MATH_HOST_SQRT: FAIL %llu over (max %llu ulp)\n", over,
             (unsigned long long)maxu);
      failures++;
    } else {
      printf("MATH_HOST_SQRT: PASS n=%llu max=%lluulp\n", cnt,
             (unsigned long long)maxu);
    }
  }

  /* modf: exact integer/frac split */
  {
    unsigned long long over = 0, cnt = 0;
    for (i = 0; i <= 40000; i++) {
      double x = -12345.6789 + (double)i * 0.617;
      double ip1, fp1, ip2, fp2;
      fp1 = hb_modf(x, &ip1);
      fp2 = modf(x, &ip2);
      cnt++;
      if (db(ip1) != db(ip2) || db(fp1) != db(fp2)) {
        if (over < 4)
          printf("  MODF MISMATCH x=%.17g\n", x);
        over++;
      }
    }
    if (over) {
      printf("MATH_HOST_MODF: FAIL %llu mismatched\n", over);
      failures++;
    } else {
      printf("MATH_HOST_MODF: PASS n=%llu exact\n", cnt);
    }
  }

  /* expf / tanhf / sqrtf */
  {
    const char *names[3] = {"EXPF", "TANHF", "SQRTF"};
    unsigned long long over[3] = {0, 0, 0}, maxu[3] = {0, 0, 0};
    long k;

    for (k = 0; k <= 400000; k++) {
      float x = -30.0f + (60.0f / 400000.0f) * (float)k;
      uint32_t d;
      d = ulp_f(hb_expf(x), expf(x));
      if (d > maxu[0])
        maxu[0] = d;
      if (d > 2) {
        if (over[0] < 4)
          printf("  EXPF OVER x=%.9g ulp=%u\n", x, d);
        over[0]++;
      }
      d = ulp_f(hb_tanhf(x), tanhf(x));
      if (d > maxu[1])
        maxu[1] = d;
      if (d > 2) {
        if (over[1] < 4)
          printf("  TANHF OVER x=%.9g ulp=%u\n", x, d);
        over[1]++;
      }
    }
    for (k = 0; k <= 100000; k++) {
      float x = (float)k * 37.9f + 0.5f;
      uint32_t d = ulp_f(hb_sqrtf(x), sqrtf(x));
      if (d > maxu[2])
        maxu[2] = d;
      if (d > 1) {
        if (over[2] < 4)
          printf("  SQRTF OVER x=%.9g ulp=%u\n", x, d);
        over[2]++;
      }
    }
    for (k = 0; k < 3; k++) {
      if (over[k]) {
        printf("MATH_HOST_%s: FAIL %llu over (max %llu ulp)\n", names[k],
               over[k], maxu[k]);
        failures++;
      } else {
        printf("MATH_HOST_%s: PASS max=%lluulp\n", names[k], maxu[k]);
      }
    }
  }

  /* special values, bit-compared live against glibc */
  {
    struct {
      const char *name;
      uint64_t got, want;
    } t[16];
    int k, bad = 0;

    t[0].name = "SIN_INF";
    t[0].got = db(hb_sin(INFINITY));
    t[0].want = db(sin(INFINITY));
    t[1].name = "COS_INF";
    t[1].got = db(hb_cos(INFINITY));
    t[1].want = db(cos(INFINITY));
    t[2].name = "TAN_INF";
    t[2].got = db(hb_tan(-INFINITY));
    t[2].want = db(tan(-INFINITY));
    t[3].name = "SIN_NEG_ZERO";
    t[3].got = db(hb_sin(-0.0));
    t[3].want = db(sin(-0.0));
    t[4].name = "ASIN_PLUS1";
    t[4].got = db(hb_asin(1.0));
    t[4].want = db(asin(1.0));
    t[5].name = "ASIN_MINUS1";
    t[5].got = db(hb_asin(-1.0));
    t[5].want = db(asin(-1.0));
    t[6].name = "ASIN_2";
    t[6].got = db(hb_asin(2.0));
    t[6].want = db(asin(2.0));
    t[7].name = "LOG_ZERO";
    t[7].got = db(hb_log(0.0));
    t[7].want = db(log(0.0));
    t[8].name = "LOG_NEG";
    t[8].got = db(hb_log(-1.0));
    t[8].want = db(log(-1.0));
    t[9].name = "LOG_SUBNORMAL";
    t[9].got = db(hb_log(4.9406564584124654e-324));
    t[9].want = db(log(4.9406564584124654e-324));
    t[10].name = "POW_NEG_FRAC";
    t[10].got = db(hb_pow(-2.0, 2.5));
    t[10].want = db(pow(-2.0, 2.5));
    t[11].name = "POW_NEG_ZERO_INT";
    t[11].got = db(hb_pow(-0.0, -3.0));
    t[11].want = db(pow(-0.0, -3.0));
    t[12].name = "POW_1_NAN";
    t[12].got = db(hb_pow(1.0, NAN));
    t[12].want = db(pow(1.0, NAN));
    t[13].name = "POW_NAN_ZERO";
    t[13].got = db(hb_pow(NAN, 0.0));
    t[13].want = db(pow(NAN, 0.0));
    t[14].name = "ATAN2_NEG_ZERO";
    t[14].got = db(hb_atan2(-0.0, -1.0));
    t[14].want = db(atan2(-0.0, -1.0));
    t[15].name = "ATAN2_INF_INF";
    t[15].got = db(hb_atan2(-INFINITY, INFINITY));
    t[15].want = db(atan2(-INFINITY, INFINITY));
    for (k = 0; k < 16; k++) {
      if (t[k].got != t[k].want) {
        printf("  SPECIAL %s MISMATCH got=0x%016llx want=0x%016llx\n", t[k].name,
               (unsigned long long)t[k].got, (unsigned long long)t[k].want);
        bad++;
      }
    }
    if (bad) {
      printf("MATH_HOST_SPECIALS: FAIL %d mismatched\n", bad);
      failures++;
    } else {
      printf("MATH_HOST_SPECIALS: PASS n=16 bit-exact\n");
    }
  }

  if (failures) {
    printf("math_transcend_test: FAILED (%d failing groups)\n", failures);
    return 1;
  }
  printf("math_transcend_test: ALL PASS\n");
  return 0;
}
