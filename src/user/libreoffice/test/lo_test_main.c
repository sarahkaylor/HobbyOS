/*
 * src/user/libreoffice/test/lo_test_main.c
 *
 * LibreOffice Automated In-Guest E2E Test Suite Orchestrator.
 * Coordinates execution of all 78 tests across 4 tiers:
 *   - Tier 1: Feature Coverage (30 tests: 5 tests x 6 applications)
 *   - Tier 2: Boundary & Corner Cases (30 tests: 5 tests x 6 applications)
 *   - Tier 3: Cross-Feature Combinations (12 tests)
 *   - Tier 4: Real-World Scenarios (6 scenarios)
 *
 * Emits standardized serial log markers and initiates clean semihosting poweroff.
 */

#include "lo_test.h"
#include <sys/time.h>

static uint64_t bench_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void benchmark_one_app(const char *name, int (*fn)(int, char**, bool)) {
    char *argv[] = {(char*)name, "--headless", NULL};
    uint64_t max_us = 0;
    uint64_t total_us = 0;
    int iterations = 50;

    for (int i = 0; i < iterations; i++) {
        uint64_t t0 = bench_time_us();
        int rc = fn(2, argv, true);
        uint64_t elapsed = bench_time_us() - t0;
        if (rc != 0) {
            printf("[PROBE1] FAIL: %s returned error %d\n", name, rc);
            return;
        }
        if (elapsed > max_us) max_us = elapsed;
        total_us += elapsed;
    }

    uint64_t avg_us = total_us / (uint64_t)iterations;
    printf("[PROBE1] %s: errors=0, max=%llu us (%llu ms), avg=%llu us (%llu ms)\n",
           name, max_us, max_us / 1000, avg_us, avg_us / 1000);

    if (max_us > 10000) {
        printf("[PROBE1] FAIL: %s max time %llu us exceeded 10000 us (10ms) threshold!\n", name, max_us);
    }
    if (avg_us > 1000) {
        printf("[PROBE1] FAIL: %s avg time %llu us exceeded 1000 us (1ms) threshold!\n", name, avg_us);
    }
}

static void benchmark_soffice(void) {
    char *argv[] = {"soffice", "--headless", NULL};
    uint64_t max_us = 0;
    uint64_t total_us = 0;
    int iterations = 50;

    for (int i = 0; i < iterations; i++) {
        uint64_t t0 = bench_time_us();
        int rc = run_soffice_main(2, argv);
        uint64_t elapsed = bench_time_us() - t0;
        if (rc != 0) {
            printf("[PROBE1] FAIL: StartCenter returned error %d\n", rc);
            return;
        }
        if (elapsed > max_us) max_us = elapsed;
        total_us += elapsed;
    }

    uint64_t avg_us = total_us / (uint64_t)iterations;
    printf("[PROBE1] StartCenter: errors=0, max=%llu us (%llu ms), avg=%llu us (%llu ms)\n",
           max_us, max_us / 1000, avg_us, avg_us / 1000);

    if (max_us > 10000) {
        printf("[PROBE1] FAIL: StartCenter max time %llu us exceeded 10000 us (10ms) threshold!\n", max_us);
    }
    if (avg_us > 1000) {
        printf("[PROBE1] FAIL: StartCenter avg time %llu us exceeded 1000 us (1ms) threshold!\n", avg_us);
    }
}

static void run_headless_benchmarks(void) {
    printf("[PROBE1] STARTING HEADLESS FAST-PATH BENCHMARK\n");
    benchmark_one_app("Writer", run_writer_main);
    benchmark_one_app("Calc", run_calc_main);
    benchmark_one_app("Impress", run_impress_main);
    benchmark_one_app("Draw", run_draw_main);
    benchmark_one_app("Math", run_math_main);
    benchmark_one_app("Base", run_base_main);
    benchmark_soffice();
    printf("==================================================\n");
}

int lo_test_run_all(void) {
    run_headless_benchmarks();

    LO_LOG("==================================================");
    LO_LOG("[LO_TEST] STARTING LIBREOFFICE E2E SUITE");
    LO_LOG("==================================================");

    /* -------------------------------------------------------------------------
     * CORE FOUNDATION (ZIP, XML, VCL)
     * ------------------------------------------------------------------------- */
    run_tests_core();

    /* -------------------------------------------------------------------------
     * TIER 1: FEATURE COVERAGE (30 TESTS: 5 PER APPLICATION)
     * ------------------------------------------------------------------------- */
    LO_LOG("[LO_TEST] TIER 1 FEATURE COVERAGE: START");

    run_tests_writer_tier1();
    run_tests_calc_tier1();
    run_tests_impress_tier1();
    run_tests_draw_tier1();
    run_tests_math_tier1();
    run_tests_base_tier1();

    /* -------------------------------------------------------------------------
     * TIER 2: BOUNDARY & CORNER CASES (30 TESTS)
     * ------------------------------------------------------------------------- */
    g_current_suite = "TIER2_BOUNDARY";
    int tier2_failed_before = g_tests_failed;
    run_tests_writer_tier2();
    run_tests_calc_tier2();
    run_tests_impress_tier2();
    run_tests_draw_tier2();
    run_tests_math_tier2();
    run_tests_base_tier2();
    if (g_tests_failed == tier2_failed_before) {
        LO_LOG("[LO_TEST] TIER 2 BOUNDARY & CORNER: PASS (30/30 tests)");
    }

    /* -------------------------------------------------------------------------
     * TIER 3: CROSS-FEATURE COMBINATIONS (12 TESTS)
     * ------------------------------------------------------------------------- */
    run_tests_tier3_cross();

    /* -------------------------------------------------------------------------
     * TIER 4: REAL-WORLD SCENARIOS (6 SCENARIOS)
     * ------------------------------------------------------------------------- */
    run_tests_tier4_scenarios();

    /* -------------------------------------------------------------------------
     * ACTION VALIDATION & ZERO-DEAD-ITEM VERIFICATION (12 TESTS)
     * ------------------------------------------------------------------------- */
    run_tests_action_validation();

    /* -------------------------------------------------------------------------
     * TIER 5: LIVE IN-GUEST X11 GUI E2E (9 TESTS)
     * ------------------------------------------------------------------------- */
    run_tests_tier5_gui();

    /* -------------------------------------------------------------------------
     * SUMMARY & SHUTDOWN
     * ------------------------------------------------------------------------- */
    LO_LOG("==================================================");
    LO_LOG("Total Tests Run:    %d", g_tests_run);
    LO_LOG("Total Tests Passed: %d", g_tests_passed);
    LO_LOG("Total Tests Failed: %d", g_tests_failed);
    LO_LOG("==================================================");

    if (g_tests_failed == 0 && g_tests_run >= 78) {
        LO_LOG("[LO_TEST] ALL SUITES PASSED (%d/%d tests)", g_tests_passed, g_tests_run);
    } else {
        LO_LOG("[LO_TEST] TEST SUITE FAILED (%d failures)", g_tests_failed);
    }

    /* Clean QEMU semihosting poweroff */
    sys_poweroff();
    return (g_tests_failed == 0 ? 0 : 1);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return lo_test_run_all();
}
