/*
 * src/user/libreoffice/test/lo_test.h
 *
 * Common header for LibreOffice Automated In-Guest E2E Test Suite on HobbyOS.
 * Declares global test state, assertion macros, helper functions, and suite runners.
 */

#ifndef LO_TEST_H
#define LO_TEST_H

#include "libc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

/* LibreOffice subsystem headers */
#include "lo_core.h"
#include "lo_zip.h"
#include "lo_xml.h"
#include "lo_vcl.h"
#include "lo_writer.h"
#include "lo_calc.h"
#include "lo_impress.h"
#include "lo_draw.h"
#include "lo_math.h"
#include "lo_base.h"
#include "soffice.h"

/* -------------------------------------------------------------------------
 * Global Test Statistics & State
 * ------------------------------------------------------------------------- */
extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;
extern const char *g_current_suite;
extern const char *g_current_test;
extern bool g_current_test_failed;

/* -------------------------------------------------------------------------
 * Logging & Assertion Framework
 * ------------------------------------------------------------------------- */
#define LO_LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)

#define LO_TEST_FAIL(msg) do { \
    printf("  [FAIL] %s::%s: %s (at %s:%d)\n", g_current_suite, g_current_test, msg, __FILE__, __LINE__); \
    g_current_test_failed = true; \
    return; \
} while (0)

#define LO_TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        LO_TEST_FAIL(msg); \
    } \
} while (0)

#define LO_TEST_ASSERT_EQ(actual, expected, msg) do { \
    long long _act = (long long)(actual); \
    long long _exp = (long long)(expected); \
    if (_act != _exp) { \
        printf("  [FAIL] %s::%s: %s (expected %lld, got %lld at %s:%d)\n", \
               g_current_suite, g_current_test, msg, _exp, _act, __FILE__, __LINE__); \
        g_current_test_failed = true; \
        return; \
    } \
} while (0)

#define LO_TEST_ASSERT_STR_EQ(actual, expected, msg) do { \
    const char *_act = (const char *)(actual); \
    const char *_exp = (const char *)(expected); \
    if (!_act || !_exp || strcmp(_act, _exp) != 0) { \
        printf("  [FAIL] %s::%s: %s (expected \"%s\", got \"%s\" at %s:%d)\n", \
               g_current_suite, g_current_test, msg, _exp ? _exp : "(null)", _act ? _act : "(null)", __FILE__, __LINE__); \
        g_current_test_failed = true; \
        return; \
    } \
} while (0)

#define LO_TEST_ASSERT_STR_CONTAINS(actual, substring, msg) do { \
    const char *_act = (const char *)(actual); \
    const char *_sub = (const char *)(substring); \
    if (!_act || !_sub || strstr(_act, _sub) == NULL) { \
        printf("  [FAIL] %s::%s: %s (\"%s\" not in \"%s\" at %s:%d)\n", \
               g_current_suite, g_current_test, msg, _sub ? _sub : "(null)", _act ? _act : "(null)", __FILE__, __LINE__); \
        g_current_test_failed = true; \
        return; \
    } \
} while (0)

#define LO_TEST_ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        LO_TEST_FAIL(msg); \
    } \
} while (0)

#define LO_TEST_ASSERT_NULL(ptr, msg) do { \
    if ((ptr) != NULL) { \
        LO_TEST_FAIL(msg); \
    } \
} while (0)

#define LO_TEST_ASSERT_NEAR(actual, expected, eps, msg) do { \
    double _diff = fabs((double)(actual) - (double)(expected)); \
    if (_diff > (eps)) { \
        printf("  [FAIL] %s::%s: %s (expected %f, got %f, diff %f > %f at %s:%d)\n", \
               g_current_suite, g_current_test, msg, (double)(expected), (double)(actual), _diff, (double)(eps), __FILE__, __LINE__); \
        g_current_test_failed = true; \
        return; \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * Helper Function Types & Prototypes
 * ------------------------------------------------------------------------- */
typedef void (*test_func_t)(void);

void run_single_test(const char *test_name, test_func_t fn);
bool verify_odf_package(const char *path, const char *expected_mime);
void sys_poweroff(void);

/* Sub-Suite Test Runners */
void run_tests_core(void);
void run_tests_writer_tier1(void);
void run_tests_writer_tier2(void);

void run_tests_calc_tier1(void);
void run_tests_calc_tier2(void);

void run_tests_impress_tier1(void);
void run_tests_impress_tier2(void);

void run_tests_draw_tier1(void);
void run_tests_draw_tier2(void);

void run_tests_math_tier1(void);
void run_tests_math_tier2(void);

void run_tests_base_tier1(void);
void run_tests_base_tier2(void);

void run_tests_tier3_cross(void);
void run_tests_tier4_scenarios(void);
void run_tests_action_validation(void);
void run_tests_tier5_gui(void);

int lo_test_run_all(void);

#endif /* LO_TEST_H */
