/*
 * math_test.c - MATH_T.BIN: in-wave EL0 acceptance for the P3.2
 * transcendental slice of the sysroot libc (sin, cos, tan, asin, atan,
 * atan2, log, pow, sqrt, modf, expf, tanhf).
 *
 * This is the end-to-end proof that libc.a exports the public symbols and
 * that the software kernels answer correctly on real ARM hardware: every
 * reference below is glibc's correctly-rounded double (probed on x86-64)
 * and every comparison is in ulps with the budget stated per check; where
 * exact references are awkward, an identity is checked with a stated
 * tolerance instead.  The kernel EL1 tier (src/kernel/math_test.c) covers
 * the integer dispatch; the host tier (src/host/math_transcend_test.c)
 * races the same functions against glibc over ~1M values.
 *
 * Budgets (all >= the measured worst case from the host race):
 *   sin/cos/atan/atan2/log: 2 ulp;  tan/asin/pow: 4 ulp;  sqrt: 1 ulp;
 *   expf/tanhf: 2 ulp;  identities: <= 16 ulp unless stated.
 */

#include <libc.h>
#include <math.h>
#include <stdint.h>

#define ULP_TRIG 2
#define ULP_TAN 4
#define ULP_ASIN 4
#define ULP_ATAN 2
#define ULP_LOG 2
#define ULP_POW 4
#define ULP_SQRT 1
#define ULP_F 2
#define ULP_ID 16

static int failures = 0;
static int checks = 0;

static uint64_t bits_of(double d) {
  union {
    double d;
    uint64_t u;
  } v;
  v.d = d;
  return v.u;
}

static uint32_t fbits_of(float f) {
  union {
    float f;
    uint32_t u;
  } v;
  v.f = f;
  return v.u;
}

static uint64_t ulp_dist(double a, double b) {
  uint64_t ua = bits_of(a), ub = bits_of(b);
  if (ua >> 63)
    ua = (uint64_t)0 - ua;
  if (ub >> 63)
    ub = (uint64_t)0 - ub;
  return ua > ub ? ua - ub : ub - ua;
}

static void phex64(uint64_t v) {
  static const char hexd[] = "0123456789abcdef";
  char buf[19];
  int i;

  buf[0] = '0';
  buf[1] = 'x';
  for (i = 0; i < 16; i++)
    buf[2 + i] = hexd[(v >> (60 - 4 * i)) & 0xf];
  buf[18] = 0;
  print_console(buf);
}

static void report(const char *name, int ok, const char *detail) {
  print_console("MATH_");
  print_console(name);
  print_console(ok ? ": PASS" : ": FAIL");
  if (!ok && detail) {
    print_console(" (");
    print_console(detail);
    print_console(")");
  }
  print_console("\n");
  checks++;
  if (!ok)
    failures++;
}

/* double check vs a glibc reference, in ulps */
static void check_d(const char *name, double got, double want, uint64_t budget) {
  uint64_t d = ulp_dist(got, want);

  if (d > budget) {
    print_console("  MATH_");
    print_console(name);
    print_console(" detail: got=");
    phex64(bits_of(got));
    print_console(" want=");
    phex64(bits_of(want));
    print_console(" ulp=");
    print_hex((long)(d > 1000000 ? 1000000 : d));
    print_console("\n");
  }
  report(name, d <= budget, 0);
}

/* bit-exact checks (special values) */
static void check_bits(const char *name, uint64_t got, uint64_t want) {
  if (got != want) {
    print_console("  MATH_");
    print_console(name);
    print_console(" detail: got=");
    phex64(got);
    print_console(" want=");
    phex64(want);
    print_console("\n");
  }
  report(name, got == want, 0);
}

static void check_fbits(const char *name, uint32_t got, uint32_t want) {
  report(name, got == want, 0);
}

static void test_trig(void) {
  check_d("SIN_ONE", sin(1.0), 0x1.aed548f090ceep-1, ULP_TRIG);
  check_d("SIN_HALF", sin(0.5), 0x1.eaee8744b05f0p-2, ULP_TRIG);
  check_d("SIN_NEG_P075", sin(-0.75), -0x1.5cffc16bf8f0dp-1, ULP_TRIG);
  check_d("COS_ONE", cos(1.0), 0x1.14a280fb5068cp-1, ULP_TRIG);
  check_d("COS_TWO", cos(2.0), -0x1.aa22657537205p-2, ULP_TRIG);
  check_d("COS_NEG_250", cos(-2.5), -0x1.9a2f7ef858b7dp-1, ULP_TRIG);
  check_d("COS_EIGHTH", cos(0.125), 0x1.fc015527d5bd3p-1, ULP_TRIG);
  check_d("TAN_ONE", tan(1.0), 0x1.8eb245cbee3a6p+0, ULP_TAN);
  check_d("TAN_HALF", tan(0.5), 0x1.17b4f5bf3474ap-1, ULP_TAN);
  check_d("TAN_NEG_THREE", tan(-3.0), 0x1.23ef71254b86fp-3, ULP_TAN);
  /* large arguments: full-precision Cody-Waite path */
  check_d("SIN_1E6", sin(1e6 + 0.7), 0x1.57d75dadc37f4p-2, ULP_TAN);
  check_d("SIN_123456789", sin(123456789.25), 0x1.fcf2314a9a314p-1, ULP_TAN);
  check_d("COS_1E9", cos(1e9 + 0.5), 0x1.e4fda92f2f1dcp-2, ULP_TAN);
  /* identities */
  check_d("IDENT_SIN2_COS2",
          sin(0.7) * sin(0.7) + cos(0.7) * cos(0.7), 1.0, ULP_ID);
  check_d("IDENT_TAN_RATIO", tan(0.9), sin(0.9) / cos(0.9), ULP_ID);
}

static void test_inverse_trig(void) {
  check_d("ASIN_HALF", asin(0.5), 0x1.0c152382d7366p-1, ULP_ASIN);
  check_d("ASIN_NEG_QTR", asin(-0.25), -0x1.02be9ce0b87cdp-2, ULP_ASIN);
  check_d("ATAN_ONE", atan(1.0), 0x1.921fb54442d18p-1, ULP_ATAN);
  check_d("ATAN_NEG_TWO", atan(-2.0), -0x1.1b6e192ebbe44p+0, ULP_ATAN);
  check_d("ATAN2_1_1", atan2(1.0, 1.0), 0x1.921fb54442d18p-1, ULP_ATAN);
  check_d("ATAN2_1_NEG1", atan2(1.0, -1.0), 0x1.2d97c7f3321d2p+1, ULP_ATAN);
  check_d("ATAN2_NEG1_NEG1", atan2(-1.0, -1.0), -0x1.2d97c7f3321d2p+1,
          ULP_ATAN);
  check_d("ATAN2_3_NEG4", atan2(3.0, -4.0), 0x1.3fc176b7a8560p+1, ULP_ATAN);
  /* identity: asin(sin(x)) == x for |x| <= pi/4 */
  check_d("IDENT_ASIN_SIN", asin(sin(0.6)), 0.6, ULP_ID);
  /* identity: atan2(y, x) == atan(y/x) for x > 0 */
  check_d("IDENT_ATAN2_ATAN", atan2(0.5, 2.0), atan(0.25), ULP_ID);
}

static void test_log(void) {
  check_d("LOG_TWO", log(2.0), 0x1.62e42fefa39efp-1, ULP_LOG);
  check_d("LOG_TEN", log(10.0), 0x1.26bb1bbb55516p+1, ULP_LOG);
  check_d("LOG_HALF", log(0.5), -0x1.62e42fefa39efp-1, ULP_LOG);
  check_d("LOG_1E300", log(1e300), 0x1.5963447f87fb5p+9, ULP_LOG);
  check_d("LOG_1E_M300", log(1e-300), -0x1.5963447f87fb5p+9, ULP_LOG);
  check_d("LOG_E", log(2.718281828459045), 1.0, ULP_LOG);
  /* identity: log(pow(2, y)) == y * log(2) for moderate y */
  check_d("IDENT_LOG_POW2", log(pow(2.0, 5.5)), 5.5 * 0x1.62e42fefa39efp-1,
          ULP_ID);
}

static void test_pow_sqrt(void) {
  check_d("POW_2_HALF", pow(2.0, 0.5), 0x1.6a09e667f3bcdp+0, ULP_POW);
  check_d("POW_3_HALF", pow(3.0, 0.5), 0x1.bb67ae8584caap+0, ULP_POW);
  check_d("POW_15_725", pow(1.5, 7.25), 0x1.2e8a070829b99p+4, ULP_POW);
  check_bits("POW_EXACT_10", bits_of(pow(2.0, 10.0)), bits_of(1024.0));
  check_bits("POW_EXACT_NEG2", bits_of(pow(2.0, -2.0)), bits_of(0.25));
  check_bits("POW_EXACT_1E3", bits_of(pow(10.0, 3.0)), bits_of(1000.0));
  check_bits("POW_NEGINT", bits_of(pow(-2.0, 3.0)), bits_of(-8.0));
  check_d("POW_2_1000", pow(2.0, 1000.0), 0x1.0000000000000p+1000, ULP_POW);
  check_bits("POW_OVERFLOW", bits_of(pow(2.0, 2000.0)), bits_of(INFINITY));
  check_bits("POW_UNDERFLOW", bits_of(pow(0.5, 4000.0)), bits_of(0.0));
  check_bits("SQRT_EXACT", bits_of(sqrt(4.0)), bits_of(2.0));
  check_d("SQRT_TWO", sqrt(2.0), 0x1.6a09e667f3bcdp+0, ULP_SQRT);
  check_d("SQRT_TINY", sqrt(1e-300), 0x1.a2fe76a3f9475p-499, ULP_SQRT);
  check_d("SQRT_ZERO", sqrt(0.0), 0.0, 0);
}

static void test_modf(void) {
  double ip;
  double fp;

  fp = modf(3.75, &ip);
  check_bits("MODF_IP", bits_of(ip), bits_of(3.0));
  check_bits("MODF_FP", bits_of(fp), bits_of(0.75));
  fp = modf(-3.75, &ip);
  check_bits("MODF_NEG_IP", bits_of(ip), bits_of(-3.0));
  check_bits("MODF_NEG_FP", bits_of(fp), bits_of(-0.75));
  fp = modf(INFINITY, &ip);
  check_bits("MODF_INF_IP", bits_of(ip), bits_of(INFINITY));
  check_bits("MODF_INF_FP", bits_of(fp), bits_of(0.0));
}

static void test_float_funcs(void) {
  check_fbits("EXPF_ONE", fbits_of(expf(1.0f)), fbits_of(2.7182817459106445f));
  check_fbits("EXPF_NEG10", fbits_of(expf(-10.0f)), fbits_of(4.539993096841499e-05f));
  check_fbits("EXPF_HALF", fbits_of(expf(0.5f)), fbits_of(1.6487212181091309f));
  check_fbits("TANHF_ONE", fbits_of(tanhf(1.0f)), fbits_of(0.7615941762924194f));
  check_fbits("TANHF_NEG_HALF", fbits_of(tanhf(-0.5f)), fbits_of(-0.46211716532707214f));
  check_fbits("TANHF_LARGE", fbits_of(tanhf(20.0f)), fbits_of(1.0f));
}

static void test_specials(void) {
  check_bits("SIN_INF_NAN", bits_of(sin(INFINITY)), 0xfff8000000000000ULL);
  check_bits("COS_INF_NAN", bits_of(cos(INFINITY)), 0xfff8000000000000ULL);
  check_bits("TAN_NEGINF_NAN", bits_of(tan(-INFINITY)), 0xfff8000000000000ULL);
  check_bits("LOG_NEG_NAN", bits_of(log(-1.0)), 0xfff8000000000000ULL);
  check_bits("LOG_ZERO_NEGINF", bits_of(log(0.0)), 0xfff0000000000000ULL);
  check_bits("ASIN_DOMAIN_NAN", bits_of(asin(2.0)), 0x7ff8000000000000ULL);
  check_bits("ATAN_INF", bits_of(atan(INFINITY)), 0x3ff921fb54442d18ULL);
  check_bits("ATAN2_00", bits_of(atan2(0.0, -1.0)), 0x400921fb54442d18ULL);
  check_bits("POW_DOMAIN_NAN", bits_of(pow(-2.0, 2.5)), 0xfff8000000000000ULL);
  check_bits("EXPF_NEGINF", (uint64_t)fbits_of(expf(-INFINITY)), 0x00000000ULL);
  check_bits("TANHF_INF", (uint64_t)fbits_of(tanhf(INFINITY)), 0x3f800000ULL);
  check_bits("SIN_NEG_ZERO", bits_of(sin(-0.0)), 0x8000000000000000ULL);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  print_console("MATH_T.BIN: P3.2 transcendental acceptance (EL0, "
                "libc.a)\n");
  test_trig();
  test_inverse_trig();
  test_log();
  test_pow_sqrt();
  test_modf();
  test_float_funcs();
  test_specials();
  if (failures == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
  } else {
    print_console("TESTS FAILED: ");
    print_dec(failures);
    print_console(" of ");
    print_dec(checks);
    print_console("\n");
  }
  return failures;
}
