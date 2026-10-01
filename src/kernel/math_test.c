/*
 * math_test.c - EL1 kernel unit tests for the integer-only transcendental
 * dispatch (src/libc/src/math_dispatch.h).
 *
 * The kernel compiles with -mgeneral-regs-only (the F1.5 exception is
 * per-process FPU save/restore, not FP in kernel C), so this tier checks
 * exactly the part of the P3.2 math slice that is expressible in integer
 * bit-space: classification, the NaN/Inf/zero result matrices, the signed
 * results glibc answers with, and pow's integer/odd-integer predicate.
 * The FP value race (hb_* vs glibc, values and ulp budgets) lives in the
 * host tier (src/host/math_transcend_test.c) and the in-wave user program
 * (src/user/math_test.c).
 *
 * All expected bit patterns were probed from glibc's libm on x86-64 via
 * ctypes before this test was written.
 */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include <stdint.h>

#include "../libc/src/math_dispatch.h"

extern void uart_print_hex(uint64_t val);

/* handy bit patterns not in the dispatch header */
#define BITS_SNAN 0x7ff0000000000001ULL
#define BITS_QNAN_P 0x7ff8000000001234ULL
#define BITS_SNAN_P 0x7ff0000000001234ULL
#define BITS_SUB 0x0000000000000001ULL
#define BITS_NEG_SUB 0x8000000000000001ULL
#define BITS_MIN_NORM 0x0010000000000000ULL
#define BITS_TWO 0x4000000000000000ULL
#define BITS_2_5 0x4004000000000000ULL
#define BITS_THREE 0x4008000000000000ULL
#define BITS_FOUR 0x4010000000000000ULL
#define BITS_1_5 0x3ff8000000000000ULL
#define BITS_NEG_2 0xc000000000000000ULL
#define BITS_NEG_2_5 0xc004000000000000ULL
#define BITS_NEG_3 0xc008000000000000ULL
#define BITS_2_POW_53 0x4340000000000000ULL
#define BITS_1_0000000000000002 0x3ff0000000000001ULL

#define F_SNAN 0x7f800001u
#define F_QNAN_P 0x7fc01234u
#define F_ONE 0x3f800000u
#define F_F_2_5 0x40200000u
#define F_NEG_2_5 0xc0200000u

static int failures_at_section;

static void chk64(const char *name, uint64_t got, uint64_t want) {
  if (got != want) {
    tests_failed++;
    uart_puts("  MATH ");
    uart_puts(name);
    uart_puts(": FAIL got 0x");
    uart_print_hex(got);
    uart_puts(" want 0x");
    uart_print_hex(want);
    uart_puts("\n");
  } else {
    tests_run++;
  }
}

static void chk32(const char *name, uint32_t got, uint32_t want) {
  if (got != want) {
    tests_failed++;
    uart_puts("  MATH ");
    uart_puts(name);
    uart_puts(": FAIL got 0x");
    uart_print_hex(got);
    uart_puts(" want 0x");
    uart_print_hex(want);
    uart_puts("\n");
  } else {
    tests_run++;
  }
}

static void chkint(const char *name, int got, int want) {
  if (got != want) {
    tests_failed++;
    uart_puts("  MATH ");
    uart_puts(name);
    uart_puts(": FAIL got ");
    print_int(got);
    uart_puts(" want ");
    print_int(want);
    uart_puts("\n");
  } else {
    tests_run++;
  }
}

static void section_begin(void) { failures_at_section = tests_failed; }

static void section_end(const char *name) {
  if (tests_failed == failures_at_section) {
    uart_puts("  MATH ");
    uart_puts(name);
    uart_puts(": PASS\n");
  }
}

static void test_classes(void) {
  section_begin();
  chkint("DCLS-pos-zero", ho_d_class(HO_D_ZERO), HO_MC_ZERO);
  chkint("DCLS-neg-zero", ho_d_class(HO_D_NEG_ZERO), HO_MC_ZERO);
  chkint("DCLS-sub-min", ho_d_class(BITS_SUB), HO_MC_SUBNORMAL);
  chkint("DCLS-sub-neg", ho_d_class(BITS_NEG_SUB), HO_MC_SUBNORMAL);
  chkint("DCLS-min-norm", ho_d_class(BITS_MIN_NORM), HO_MC_NORMAL);
  chkint("DCLS-one", ho_d_class(HO_D_ONE), HO_MC_NORMAL);
  chkint("DCLS-pos-inf", ho_d_class(HO_D_INF), HO_MC_INF);
  chkint("DCLS-neg-inf", ho_d_class(HO_D_NEG_INF), HO_MC_INF);
  chkint("DCLS-qnan", ho_d_class(BITS_QNAN_P), HO_MC_NAN);
  chkint("DCLS-snan", ho_d_class(BITS_SNAN), HO_MC_NAN);
  chkint("FCLS-pos-zero", ho_f_class(0x00000000u), HO_MC_ZERO);
  chkint("FCLS-neg-zero", ho_f_class(0x80000000u), HO_MC_ZERO);
  chkint("FCLS-sub", ho_f_class(0x00000001u), HO_MC_SUBNORMAL);
  chkint("FCLS-one", ho_f_class(F_ONE), HO_MC_NORMAL);
  chkint("FCLS-pos-inf", ho_f_class(HO_F_INF), HO_MC_INF);
  chkint("FCLS-neg-inf", ho_f_class(0xff800000u), HO_MC_INF);
  chkint("FCLS-qnan", ho_f_class(F_QNAN_P), HO_MC_NAN);
  chkint("FCLS-snan", ho_f_class(F_SNAN), HO_MC_NAN);
  chkint("SIGNB-pos", ho_d_signbit(HO_D_ONE), 0);
  chkint("SIGNB-neg", ho_d_signbit(HO_D_NEG_ONE), 1);
  chk64("QUIET-snan", ho_d_quiet(BITS_SNAN), 0x7ff8000000000001ULL);
  chk64("QUIET-qnan-idempotent", ho_d_quiet(BITS_QNAN_P), BITS_QNAN_P);
  chk64("SZERO-pos", ho_d_sign_zero(HO_D_ONE), HO_D_ZERO);
  chk64("SZERO-neg", ho_d_sign_zero(HO_D_NEG_INF), HO_D_NEG_ZERO);
  section_end("classes");
}

static void test_trig_specials(void) {
  uint64_t out = 0;

  section_begin();
  chkint("TRIG-inf-handled", ho_trig_special(HO_D_INF, &out), 1);
  chk64("TRIG-inf-nan", out, HO_D_NAN_NEG);
  chkint("TRIG-neginf-handled", ho_trig_special(HO_D_NEG_INF, &out), 1);
  chk64("TRIG-neginf-nan", out, HO_D_NAN_NEG);
  chkint("TRIG-snan-handled", ho_trig_special(BITS_SNAN_P, &out), 1);
  chk64("TRIG-snan-quieted", out, 0x7ff8000000001234ULL);
  chkint("TRIG-qnan-handled", ho_trig_special(BITS_QNAN_P, &out), 1);
  chk64("TRIG-qnan-passthru", out, BITS_QNAN_P);
  chkint("TRIG-finite-open", ho_trig_special(BITS_FOUR, &out), 0);
  chkint("TRIG-zero-open", ho_trig_special(HO_D_ZERO, &out), 0);
  section_end("trig-specials");
}

static void test_atan_specials(void) {
  uint64_t out = 0;

  section_begin();
  chkint("ATAN-inf-handled", ho_atan_special(HO_D_INF, &out), 1);
  chk64("ATAN-inf-pi2", out, HO_D_PI2);
  chkint("ATAN-neginf-handled", ho_atan_special(HO_D_NEG_INF, &out), 1);
  chk64("ATAN-neginf-negpi2", out, HO_D_NEG_PI2);
  chkint("ATAN-snan-handled", ho_atan_special(BITS_SNAN, &out), 1);
  chk64("ATAN-snan-quieted", out, 0x7ff8000000000001ULL);
  chkint("ATAN-finite-open", ho_atan_special(BITS_2_5, &out), 0);
  section_end("atan-specials");
}

static void test_log_specials(void) {
  uint64_t out = 0;

  section_begin();
  chkint("LOG-inf-handled", ho_log_special(HO_D_INF, &out), 1);
  chk64("LOG-inf", out, HO_D_INF);
  chkint("LOG-neginf-handled", ho_log_special(HO_D_NEG_INF, &out), 1);
  chk64("LOG-neginf-nan", out, HO_D_NAN_NEG);
  chkint("LOG-poszero-handled", ho_log_special(HO_D_ZERO, &out), 1);
  chk64("LOG-poszero-neg-inf", out, HO_D_NEG_INF);
  chkint("LOG-negzero-handled", ho_log_special(HO_D_NEG_ZERO, &out), 1);
  chk64("LOG-negzero-neg-inf", out, HO_D_NEG_INF);
  chkint("LOG-neg1-handled", ho_log_special(HO_D_NEG_ONE, &out), 1);
  chk64("LOG-neg1-nan", out, HO_D_NAN_NEG);
  chkint("LOG-neg25-handled", ho_log_special(BITS_NEG_2_5, &out), 1);
  chk64("LOG-neg25-nan", out, HO_D_NAN_NEG);
  chkint("LOG-snan-handled", ho_log_special(BITS_SNAN_P, &out), 1);
  chk64("LOG-snan-quieted", out, 0x7ff8000000001234ULL);
  chkint("LOG-pos25-open", ho_log_special(BITS_2_5, &out), 0);
  chkint("LOG-subnormal-open", ho_log_special(BITS_SUB, &out), 0);
  section_end("log-specials");
}

static void test_asin_specials(void) {
  uint64_t out = 0;

  section_begin();
  chkint("ASIN-two-handled", ho_asin_special(BITS_TWO, &out), 1);
  chk64("ASIN-two-pos-nan", out, HO_D_NAN);
  chkint("ASIN-neg25-handled", ho_asin_special(BITS_NEG_2_5, &out), 1);
  chk64("ASIN-neg25-pos-nan", out, HO_D_NAN);
  chkint("ASIN-inf-handled", ho_asin_special(HO_D_INF, &out), 1);
  chk64("ASIN-inf-pos-nan", out, HO_D_NAN);
  chkint("ASIN-neginf-handled", ho_asin_special(HO_D_NEG_INF, &out), 1);
  chk64("ASIN-neginf-pos-nan", out, HO_D_NAN);
  chkint("ASIN-justpast1-handled",
         ho_asin_special(BITS_1_0000000000000002, &out), 1);
  chk64("ASIN-justpast1-pos-nan", out, HO_D_NAN);
  chkint("ASIN-one-open", ho_asin_special(HO_D_ONE, &out), 0);
  chkint("ASIN-negone-open", ho_asin_special(HO_D_NEG_ONE, &out), 0);
  chkint("ASIN-half-open", ho_asin_special(HO_D_HALF, &out), 0);
  chkint("ASIN-snan-handled", ho_asin_special(BITS_SNAN, &out), 1);
  chk64("ASIN-snan-quieted", out, 0x7ff8000000000001ULL);
  section_end("asin-specials");
}

static void test_float_specials(void) {
  uint32_t out = 0;

  section_begin();
  chkint("EXPF-posinf-handled", ho_expf_special(HO_F_INF, &out), 1);
  chk32("EXPF-posinf", out, HO_F_INF);
  chkint("EXPF-neginf-handled", ho_expf_special(0xff800000u, &out), 1);
  chk32("EXPF-neginf-poszero", out, HO_F_ZERO);
  chkint("EXPF-snan-handled", ho_expf_special(F_SNAN, &out), 1);
  chk32("EXPF-snan-quieted", out, 0x7fc00001u);
  chkint("EXPF-one-open", ho_expf_special(F_ONE, &out), 0);
  chkint("TANHF-posinf-handled", ho_tanhf_special(HO_F_INF, &out), 1);
  chk32("TANHF-posinf-one", out, HO_F_ONE);
  chkint("TANHF-neginf-handled", ho_tanhf_special(0xff800000u, &out), 1);
  chk32("TANHF-neginf-negone", out, HO_F_NEG_ONE);
  chkint("TANHF-snan-handled", ho_tanhf_special(F_SNAN, &out), 1);
  chk32("TANHF-snan-quieted", out, 0x7fc00001u);
  chkint("TANHF-finite-open", ho_tanhf_special(F_F_2_5, &out), 0);
  section_end("float-specials");
}

static void test_modf_specials(void) {
  uint64_t ip = 0, fp = 0;

  section_begin();
  chkint("MODF-inf-handled", ho_d_modf_special(HO_D_INF, &ip, &fp), 1);
  chk64("MODF-inf-int", ip, HO_D_INF);
  chk64("MODF-inf-frac-poszero", fp, HO_D_ZERO);
  chkint("MODF-neginf-handled", ho_d_modf_special(HO_D_NEG_INF, &ip, &fp), 1);
  chk64("MODF-neginf-int", ip, HO_D_NEG_INF);
  chk64("MODF-neginf-frac-negzero", fp, HO_D_NEG_ZERO);
  chkint("MODF-snan-handled", ho_d_modf_special(BITS_SNAN_P, &ip, &fp), 1);
  chk64("MODF-snan-int-quieted", ip, 0x7ff8000000001234ULL);
  chk64("MODF-snan-frac-quieted", fp, 0x7ff8000000001234ULL);
  chkint("MODF-finite-open", ho_d_modf_special(BITS_2_5, &ip, &fp), 0);
  section_end("modf-specials");
}

static void test_isint_odd(void) {
  int isint, isodd;

  section_begin();
  ho_d_isint_odd(HO_D_ZERO, &isint, &isodd);
  chkint("ISINT-zero-int", isint, 1);
  chkint("ISINT-zero-notodd", isodd, 0);
  ho_d_isint_odd(HO_D_HALF, &isint, &isodd);
  chkint("ISINT-half-notint", isint, 0);
  ho_d_isint_odd(BITS_2_5, &isint, &isodd);
  chkint("ISINT-25-notint", isint, 0);
  ho_d_isint_odd(BITS_THREE, &isint, &isodd);
  chkint("ISINT-3-int", isint, 1);
  chkint("ISINT-3-odd", isodd, 1);
  ho_d_isint_odd(BITS_NEG_3, &isint, &isodd);
  chkint("ISINT-neg3-int", isint, 1);
  chkint("ISINT-neg3-odd", isodd, 1);
  ho_d_isint_odd(BITS_FOUR, &isint, &isodd);
  chkint("ISINT-4-int", isint, 1);
  chkint("ISINT-4-even", isodd, 0);
  ho_d_isint_odd(BITS_2_POW_53, &isint, &isodd);
  chkint("ISINT-2p53-int", isint, 1);
  chkint("ISINT-2p53-even", isodd, 0);
  ho_d_isint_odd(BITS_SUB, &isint, &isodd);
  chkint("ISINT-sub-notint", isint, 0);
  ho_d_isint_odd(HO_D_INF, &isint, &isodd);
  chkint("ISINT-inf-notint", isint, 0);
  ho_d_isint_odd(BITS_QNAN_P, &isint, &isodd);
  chkint("ISINT-nan-notint", isint, 0);
  section_end("isint-odd");
}

static void test_pow_specials(void) {
  uint64_t out = 0;

  section_begin();
  /* y == +-0 and x == 1 win over every NaN/Inf handling (IEEE, glibc) */
  chkint("POW-x5-y0-handled", ho_pow_special(BITS_2_5, HO_D_ZERO, &out), 1);
  chk64("POW-x5-y0-one", out, HO_D_ONE);
  chkint("POW-xnan-y0-handled", ho_pow_special(BITS_QNAN_P, HO_D_ZERO, &out), 1);
  chk64("POW-xnan-y0-one", out, HO_D_ONE);
  chkint("POW-x1-ynan-handled", ho_pow_special(HO_D_ONE, BITS_QNAN_P, &out), 1);
  chk64("POW-x1-ynan-one", out, HO_D_ONE);
  chkint("POW-x1-yinf-handled", ho_pow_special(HO_D_ONE, HO_D_INF, &out), 1);
  chk64("POW-x1-yinf-one", out, HO_D_ONE);
  /* NaN propagation order: y's NaN first, then x's */
  chkint("POW-x2-ynan-handled", ho_pow_special(BITS_TWO, BITS_QNAN_P, &out), 1);
  chk64("POW-x2-ynan-quieted-y", out, BITS_QNAN_P);
  chkint("POW-xnan-y2-handled", ho_pow_special(BITS_SNAN, BITS_TWO, &out), 1);
  chk64("POW-xnan-y2-quieted-x", out, 0x7ff8000000000001ULL);
  /* y == +-Inf */
  chkint("POW-neg1-pinf-handled", ho_pow_special(HO_D_NEG_ONE, HO_D_INF, &out), 1);
  chk64("POW-neg1-pinf-one", out, HO_D_ONE);
  chkint("POW-neg1-ninf-handled", ho_pow_special(HO_D_NEG_ONE, HO_D_NEG_INF, &out), 1);
  chk64("POW-neg1-ninf-one", out, HO_D_ONE);
  chkint("POW-x2-pinf-handled", ho_pow_special(BITS_TWO, HO_D_INF, &out), 1);
  chk64("POW-x2-pinf-inf", out, HO_D_INF);
  chkint("POW-x2-ninf-handled", ho_pow_special(BITS_TWO, HO_D_NEG_INF, &out), 1);
  chk64("POW-x2-ninf-zero", out, HO_D_ZERO);
  chkint("POW-xhalf-pinf-handled", ho_pow_special(HO_D_HALF, HO_D_INF, &out), 1);
  chk64("POW-xhalf-pinf-zero", out, HO_D_ZERO);
  chkint("POW-xhalf-ninf-handled", ho_pow_special(HO_D_HALF, HO_D_NEG_INF, &out), 1);
  chk64("POW-xhalf-ninf-inf", out, HO_D_INF);
  chkint("POW-xneg2-pinf-handled", ho_pow_special(BITS_NEG_2, HO_D_INF, &out), 1);
  chk64("POW-xneg2-pinf-inf", out, HO_D_INF);
  /* x == +-0 */
  chkint("POW-poszero-y3-handled", ho_pow_special(HO_D_ZERO, BITS_THREE, &out), 1);
  chk64("POW-poszero-y3-poszero", out, HO_D_ZERO);
  chkint("POW-negzero-y3-handled", ho_pow_special(HO_D_NEG_ZERO, BITS_THREE, &out), 1);
  chk64("POW-negzero-y3-negzero", out, HO_D_NEG_ZERO);
  chkint("POW-negzero-y2-handled", ho_pow_special(HO_D_NEG_ZERO, BITS_TWO, &out), 1);
  chk64("POW-negzero-y2-poszero", out, HO_D_ZERO);
  chkint("POW-negzero-yneg3-handled",
         ho_pow_special(HO_D_NEG_ZERO, BITS_NEG_3, &out), 1);
  chk64("POW-negzero-yneg3-neg-inf", out, HO_D_NEG_INF);
  chkint("POW-poszero-yneg3-handled",
         ho_pow_special(HO_D_ZERO, BITS_NEG_3, &out), 1);
  chk64("POW-poszero-yneg3-inf", out, HO_D_INF);
  /* x == +-Inf */
  chkint("POW-posinf-y3-handled", ho_pow_special(HO_D_INF, BITS_THREE, &out), 1);
  chk64("POW-posinf-y3-inf", out, HO_D_INF);
  chkint("POW-neginf-y3-handled", ho_pow_special(HO_D_NEG_INF, BITS_THREE, &out), 1);
  chk64("POW-neginf-y3-neg-inf", out, HO_D_NEG_INF);
  chkint("POW-neginf-y2-handled", ho_pow_special(HO_D_NEG_INF, BITS_TWO, &out), 1);
  chk64("POW-neginf-y2-inf", out, HO_D_INF);
  chkint("POW-neginf-yneg3-handled",
         ho_pow_special(HO_D_NEG_INF, BITS_NEG_3, &out), 1);
  chk64("POW-neginf-yneg3-negzero", out, HO_D_NEG_ZERO);
  chkint("POW-posinf-yneg3-handled",
         ho_pow_special(HO_D_INF, BITS_NEG_3, &out), 1);
  chk64("POW-posinf-yneg3-zero", out, HO_D_ZERO);
  /* negative base, non-integer exponent -> -NaN (FP path otherwise) */
  chkint("POW-neg2-y25-handled", ho_pow_special(BITS_NEG_2, BITS_2_5, &out), 1);
  chk64("POW-neg2-y25-neg-nan", out, HO_D_NAN_NEG);
  chkint("POW-neg2-y3-open", ho_pow_special(BITS_NEG_2, BITS_THREE, &out), 0);
  chkint("POW-neg2-y4-open", ho_pow_special(BITS_NEG_2, BITS_FOUR, &out), 0);
  chkint("POW-pos2-y25-open", ho_pow_special(BITS_TWO, BITS_2_5, &out), 0);
  section_end("pow-specials");
}

static void test_atan2_specials(void) {
  uint64_t out = 0;

  section_begin();
  chkint("ATAN2-ynan-handled", ho_atan2_special(BITS_QNAN_P, HO_D_ONE, &out), 1);
  chk64("ATAN2-ynan-quieted", out, BITS_QNAN_P);
  chkint("ATAN2-xnan-handled", ho_atan2_special(HO_D_ONE, BITS_SNAN, &out), 1);
  chk64("ATAN2-xnan-quieted", out, 0x7ff8000000000001ULL);
  chkint("ATAN2-pinf-pinf-handled",
         ho_atan2_special(HO_D_INF, HO_D_INF, &out), 1);
  chk64("ATAN2-pinf-pinf-pi4", out, HO_D_PI4);
  chkint("ATAN2-pinf-ninf-handled",
         ho_atan2_special(HO_D_INF, HO_D_NEG_INF, &out), 1);
  chk64("ATAN2-pinf-ninf-3pi4", out, HO_D_3PI4);
  chkint("ATAN2-ninf-pinf-handled",
         ho_atan2_special(HO_D_NEG_INF, HO_D_INF, &out), 1);
  chk64("ATAN2-ninf-pinf-neg-pi4", out, HO_D_NEG_PI4);
  chkint("ATAN2-ninf-ninf-handled",
         ho_atan2_special(HO_D_NEG_INF, HO_D_NEG_INF, &out), 1);
  chk64("ATAN2-ninf-ninf-neg-3pi4", out, HO_D_NEG_3PI4);
  chkint("ATAN2-pinf-x2-handled",
         ho_atan2_special(HO_D_INF, BITS_TWO, &out), 1);
  chk64("ATAN2-pinf-x2-pi2", out, HO_D_PI2);
  chkint("ATAN2-ninf-xneg3-handled",
         ho_atan2_special(HO_D_NEG_INF, BITS_NEG_3, &out), 1);
  chk64("ATAN2-ninf-xneg3-neg-pi2", out, HO_D_NEG_PI2);
  chkint("ATAN2-y1-pinf-handled",
         ho_atan2_special(HO_D_ONE, HO_D_INF, &out), 1);
  chk64("ATAN2-y1-pinf-poszero", out, HO_D_ZERO);
  chkint("ATAN2-y1-ninf-handled",
         ho_atan2_special(HO_D_ONE, HO_D_NEG_INF, &out), 1);
  chk64("ATAN2-y1-ninf-pi", out, HO_D_PI);
  chkint("ATAN2-yneg1-pinf-handled",
         ho_atan2_special(HO_D_NEG_ONE, HO_D_INF, &out), 1);
  chk64("ATAN2-yneg1-pinf-negzero", out, HO_D_NEG_ZERO);
  chkint("ATAN2-yneg1-ninf-handled",
         ho_atan2_special(HO_D_NEG_ONE, HO_D_NEG_INF, &out), 1);
  chk64("ATAN2-yneg1-ninf-neg-pi", out, HO_D_NEG_PI);
  chkint("ATAN2-poszero-x2-handled",
         ho_atan2_special(HO_D_ZERO, BITS_TWO, &out), 1);
  chk64("ATAN2-poszero-x2-poszero", out, HO_D_ZERO);
  chkint("ATAN2-negzero-x2-handled",
         ho_atan2_special(HO_D_NEG_ZERO, BITS_TWO, &out), 1);
  chk64("ATAN2-negzero-x2-negzero", out, HO_D_NEG_ZERO);
  chkint("ATAN2-poszero-xneg2-handled",
         ho_atan2_special(HO_D_ZERO, BITS_NEG_2, &out), 1);
  chk64("ATAN2-poszero-xneg2-pi", out, HO_D_PI);
  chkint("ATAN2-negzero-xneg2-handled",
         ho_atan2_special(HO_D_NEG_ZERO, BITS_NEG_2, &out), 1);
  chk64("ATAN2-negzero-xneg2-neg-pi", out, HO_D_NEG_PI);
  chkint("ATAN2-y2-poszero-handled",
         ho_atan2_special(BITS_TWO, HO_D_ZERO, &out), 1);
  chk64("ATAN2-y2-poszero-pi2", out, HO_D_PI2);
  chkint("ATAN2-yneg2-poszero-handled",
         ho_atan2_special(BITS_NEG_2, HO_D_ZERO, &out), 1);
  chk64("ATAN2-yneg2-poszero-neg-pi2", out, HO_D_NEG_PI2);
  chkint("ATAN2-y2-negzero-handled",
         ho_atan2_special(BITS_TWO, HO_D_NEG_ZERO, &out), 1);
  chk64("ATAN2-y2-negzero-pi2", out, HO_D_PI2);
  chkint("ATAN2-yneg2-negzero-handled",
         ho_atan2_special(BITS_NEG_2, HO_D_NEG_ZERO, &out), 1);
  chk64("ATAN2-yneg2-negzero-neg-pi2", out, HO_D_NEG_PI2);
  chkint("ATAN2-finite-open", ho_atan2_special(BITS_THREE, BITS_FOUR, &out), 0);
  section_end("atan2-specials");
}

void math_test_suite(void) {
  uart_puts("  Running math_test_suite (integer-only transcendental "
            "dispatch)...\n");
  test_classes();
  test_trig_specials();
  test_atan_specials();
  test_log_specials();
  test_asin_specials();
  test_float_specials();
  test_modf_specials();
  test_isint_odd();
  test_pow_specials();
  test_atan2_specials();
}

#endif // KERNEL_MODE_UNIT_TEST
