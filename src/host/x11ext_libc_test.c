/*
 * Host test (icewm.md M2, lane xlibs-ext): src/libc/src/math_ext.c compiled
 * as hb_* and raced bit-for-bit against glibc's frexp/frexpf/ldexp/ldexpf/
 * scalbn/scalbnf on literal edge cases (zeros, subnormals, inf, nan,
 * overflow/underflow boundaries) plus randomized bit patterns.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double hb_frexp(double, int *);
float hb_frexpf(float, int *);
double hb_scalbn(double, int);
float hb_scalbnf(float, int);
double hb_ldexp(double, int);
float hb_ldexpf(float, int);

static int fails;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int same_d(double a, double b) {
  if (isnan(a) && isnan(b)) return 1;
  uint64_t x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); return x == y;
}
static int same_f(float a, float b) {
  if (isnan(a) && isnan(b)) return 1;
  uint32_t x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); return x == y;
}

static void chk_d(double x, int n) {
  int e1 = 0, e2 = 0;
  double a = hb_frexp(x, &e1), b = frexp(x, &e2);
  if (!same_d(a, b) || (isfinite(x) && e1 != e2)) {
    if (fails++ < 20) printf("FAIL frexp(%a): %a/%d want %a/%d\n", x, a, e1, b, e2);
  }
  a = hb_scalbn(x, n); b = scalbn(x, n);
  if (!same_d(a, b) && fails++ < 20) printf("FAIL scalbn(%a,%d): %a want %a\n", x, n, a, b);
  a = hb_ldexp(x, n); b = ldexp(x, n);
  if (!same_d(a, b) && fails++ < 20) printf("FAIL ldexp(%a,%d): %a want %a\n", x, n, a, b);
}
static void chk_f(float x, int n) {
  int e1 = 0, e2 = 0;
  float a = hb_frexpf(x, &e1), b = frexpf(x, &e2);
  if (!same_f(a, b) || (isfinite(x) && e1 != e2)) {
    if (fails++ < 20) printf("FAIL frexpf(%a): %a/%d want %a/%d\n", x, a, e1, b, e2);
  }
  a = hb_scalbnf(x, n); b = scalbnf(x, n);
  if (!same_f(a, b) && fails++ < 20) printf("FAIL scalbnf(%a,%d): %a want %a\n", x, n, a, b);
  a = hb_ldexpf(x, n); b = ldexpf(x, n);
  if (!same_f(a, b) && fails++ < 20) printf("FAIL ldexpf(%a,%d): %a want %a\n", x, n, a, b);
}

int main(void) {
  static const double lits[] = {0.0, -0.0, 1.0, -1.0, 0.5, 3.0, 0x1p-1074, -0x1p-1074, 0x1p-1022,
                                0x1.fffffffffffffp-1023, 0x1.fffffffffffffp1023, INFINITY, -INFINITY,
                                NAN, 1e300, 1e-300, 123.456, -7.25e-310};
  static const int ns[] = {0, 1, -1, 52, -52, 1023, -1022, -1074, -1075, 1024, 2046, -2098, 4000, -4000,
                           2147483647, -2147483647 - 1};
  for (size_t i = 0; i < sizeof lits / sizeof lits[0]; i++)
    for (size_t j = 0; j < sizeof ns / sizeof ns[0]; j++) {
      chk_d(lits[i], ns[j]);
      chk_f((float)lits[i], ns[j]);
    }
  for (int i = 0; i < 2000000; i++) {
    uint64_t r = next();
    double d; memcpy(&d, &r, 8);
    uint32_t r32 = (uint32_t)(r >> 17);
    float f; memcpy(&f, &r32, 4);
    int n = (int)(next() % 4400) - 2200;
    chk_d(d, n);
    chk_f(f, n % 330);
  }
  if (fails) { printf("x11ext_libc_test: %d FAILURES\n", fails); return 1; }
  printf("x11ext_libc_test: frexp/ldexp/scalbn (+f) bit-exact vs glibc: PASS\n");
  return 0;
}
