/*
 * HobbyOS Phase-1 host test: header-only sysroot set (stdarg.h, limits.h,
 * stdbool.h, inttypes.h). Under -Isrc/libc/include (first on the path)
 * the HobbyOS wrappers are what resolves, so compiling this TU with that
 * flag and passing the checks proves the sysroot headers work on a real
 * toolchain — including include_next delegation and the format macros.
 */
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>

static int total_checks = 0;
static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        total_checks++;                                                       \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s (line %d)\n", msg, __LINE__);                     \
        }                                                                     \
    } while (0)

/* limits.h */
static void cl_limits(void)
{
    CHECK(CHAR_BIT == 8, "CHAR_BIT");
    CHECK(INT_MAX == 2147483647, "INT_MAX");
    CHECK(INT_MIN == (-INT_MAX - 1), "INT_MIN");
    CHECK(LONG_MAX > 2147483647L, "LONG_MAX 64-bit (LP64)");
    CHECK(UCHAR_MAX == 255, "UCHAR_MAX");
}

/* stdbool.h */
static void cl_stdbool(void)
{
    bool t = true;
    bool f = false;
    CHECK(t && !f, "bool true/false");
    CHECK(sizeof(bool) == 1, "sizeof(bool) == 1");
#ifdef __bool_true_false_are_defined
    CHECK(1, "__bool_true_false_are_defined");
#else
    CHECK(0, "__bool_true_false_are_defined");
#endif
}

/* stdarg.h round trip */
static int va_sum(int n, ...)
{
    va_list ap;
    int i, s = 0;
    va_start(ap, n);
    for (i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static void cl_stdarg(void)
{
    CHECK(va_sum(4, 1, 2, 3, 4) == 10, "va_arg round trip");
    CHECK(va_sum(1, 42) == 42, "va_arg single");
}

/* inttypes.h: printf/scanf format macros (used in real format strings) */
static void cl_inttypes(void)
{
    char buf[64];
    /* compile-time: macros are string literals that static-concat */
#if __LP64__
    const char *f64 = "%" PRId64;
    CHECK(strcmp(f64, "%ld") == 0, "PRId64 literal (LP64)");
    strcpy(buf, "%" PRIu64);
    CHECK(strcmp(buf, "%lu") == 0, "PRIu64 literal (LP64)");
#else
    const char *f64 = "%" PRId64;
    CHECK(strcmp(f64, "%lld") == 0, "PRId64 literal (ILP32)");
    strcpy(buf, "%" PRIu64);
    CHECK(strcmp(buf, "%llu") == 0, "PRIu64 literal (ILP32)");
#endif
    strcpy(buf, "%" PRIuMAX);
    CHECK(strcmp(buf, __LP64__ ? "%lu" : "%llu") == 0, "PRIuMAX literal");
    strcpy(buf, "%" PRIxPTR);
    CHECK(strcmp(buf, "%lx") == 0, "PRIxPTR literal");
    CHECK(sizeof(intmax_t) == 8, "sizeof(intmax_t) == 8");
    CHECK(sizeof(uintmax_t) == 8, "sizeof(uintmax_t) == 8");
    CHECK(sizeof(imaxdiv_t) == 16, "sizeof(imaxdiv_t) == 16");

    /* real format usage */
    snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)-1234567890123LL);
    CHECK(strcmp(buf, "-1234567890123") == 0, "PRId64 in snprintf");
    snprintf(buf, sizeof(buf), "%" PRIu64, (uint64_t)18446744073709551615ULL);
    CHECK(strcmp(buf, "18446744073709551615") == 0, "PRIu64 full width");
    snprintf(buf, sizeof(buf), "%" PRIx32, (uint32_t)0xDEADBEEF);
    CHECK(strcmp(buf, "deadbeef") == 0, "PRIx32 hex");
    snprintf(buf, sizeof(buf), "%" PRId32, (int32_t)-7);
    CHECK(strcmp(buf, "-7") == 0, "PRId32");
    snprintf(buf, sizeof(buf), "%" PRId16, (int16_t)-32768);
    CHECK(strcmp(buf, "-32768") == 0, "PRId16");
    snprintf(buf, sizeof(buf), "%" PRIu8, (uint8_t)255);
    CHECK(strcmp(buf, "255") == 0, "PRIu8");

    /* SCN round trip */
    {
        int64_t v = 0;
        int n = sscanf("-987654321", "%" SCNd64, &v);
        CHECK(n == 1 && v == -987654321LL, "SCNd64");
    }
    {
        unsigned long w = 0;
        int n = sscanf("abc", "%" SCNxPTR, &w);
        CHECK(n == 1 && w == 0xabcUL, "SCNxPTR");
    }
}

int main(void)
{
    cl_limits();
    cl_stdbool();
    cl_stdarg();
    cl_inttypes();
    printf("libc_headers_test: %d checks, %d failures\n", total_checks,
           failures);
    if (failures == 0) {
        printf("LIB C HEADERS TEST PASSED\n");
        return 0;
    }
    printf("LIB C HEADERS TEST FAILED\n");
    return 1;
}
