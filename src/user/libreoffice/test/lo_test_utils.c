/*
 * src/user/libreoffice/test/lo_test_utils.c
 *
 * Test utilities, assertion runners, ODF container verification,
 * and clean QEMU semihosting shutdown for LibreOffice E2E test harness.
 */

#include "lo_test.h"

/* -------------------------------------------------------------------------
 * Global Test State
 * ------------------------------------------------------------------------- */
int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;

const char *g_current_suite = "";
const char *g_current_test = "";
bool g_current_test_failed = false;

/* -------------------------------------------------------------------------
 * Single Test Execution Wrapper
 * ------------------------------------------------------------------------- */
void run_single_test(const char *test_name, test_func_t fn) {
    g_current_test = test_name;
    g_current_test_failed = false;
    g_tests_run++;

    fn();

    if (!g_current_test_failed) {
        g_tests_passed++;
    } else {
        g_tests_failed++;
    }
}

/* -------------------------------------------------------------------------
 * Clean QEMU Shutdown Mechanism (sys_poweroff)
 * ------------------------------------------------------------------------- */
void sys_poweroff(void) {
    printf("[LO_TEST] Executing sys_poweroff()...\n");
#if defined(__aarch64__)
    /*
     * ARM64 QEMU Virt Semihosting Shutdown.
     * x0 = 0x18 (SYS_EXIT_EXTENDED)
     * x1 = pointer to parameter block: [0x20026, 0]
     * Under 'qemu -semihosting -action shutdown=poweroff', this directly halts QEMU cleanly.
     */
    uint64_t param_block[2];
    param_block[0] = 0x20026; /* ADP_ExitReason_ApplicationExit + ADP_Stop_NoError */
    param_block[1] = 0;       /* Subcode 0 */

    __asm__ volatile (
        "mov x0, #0x18\n"
        "mov x1, %0\n"
        "hlt #0xf000\n"
        :
        : "r"(param_block)
        : "x0", "x1", "memory"
    );
#elif defined(__x86_64__)
    /* x86_64 QEMU ACPI / debug-exit shutdown */
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "d"((uint16_t)0x604));
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x00), "d"((uint16_t)0x501));
#endif

    /* Standard process exit fallback -> kernel scheduler drain */
    exit(g_tests_failed == 0 ? 0 : 1);
}

/* -------------------------------------------------------------------------
 * ODF Package Verification Helper (ISO/IEC 26300 Conformance)
 * ------------------------------------------------------------------------- */
bool verify_odf_package(const char *path, const char *expected_mime) {
    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_READ);
    if (!zip) return false;

    /* 1. Verify mimetype entry exists and matches */
    void *mime_buf = NULL;
    size_t mime_len = 0;
    if (lo_zip_read_file(zip, "mimetype", &mime_buf, &mime_len) != 0 || !mime_buf) {
        lo_zip_close(zip);
        return false;
    }
    if (mime_len != strlen(expected_mime) || memcmp(mime_buf, expected_mime, mime_len) != 0) {
        free(mime_buf);
        lo_zip_close(zip);
        return false;
    }
    free(mime_buf);

    /* 2. Verify META-INF/manifest.xml exists */
    if (!lo_zip_has_entry(zip, "META-INF/manifest.xml")) {
        lo_zip_close(zip);
        return false;
    }

    /* 3. Verify content.xml exists */
    if (!lo_zip_has_entry(zip, "content.xml")) {
        lo_zip_close(zip);
        return false;
    }

    lo_zip_close(zip);
    return true;
}
