/*
 * HobbyOS Phase-1 host test: stdlib.h vs glibc.
 *
 * src/libc/src/stdlib.c compiled with -DHOST_TEST (hb_* names) is linked
 * alongside glibc and compared on literal + randomized inputs. The strtol
 * family is checked byte-exact (value, endptr, errno); qsort is checked on
 * sorted output (comparator call order is NOT part of the contract);
 * rand/srand on bounds + determinism; the environment table against its own
 * contract plus non-interference with glibc's; abort via fork + WIFSIGNALED.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern int hb_atoi(const char *);
extern long hb_atol(const char *);
extern long long hb_atoll(const char *);
extern long hb_strtol(const char *, char **, int);
extern long long hb_strtoll(const char *, char **, int);
extern unsigned long hb_strtoul(const char *, char **, int);
extern unsigned long long hb_strtoull(const char *, char **, int);
extern int hb_abs(int);
extern long hb_labs(long);
extern long long hb_llabs(long long);
extern void hb_qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern void *hb_bsearch(const void *, const void *, size_t, size_t,
                        int (*)(const void *, const void *));
extern int hb_rand(void);
extern void hb_srand(unsigned int);
extern char *hb_getenv(const char *);
extern int hb_setenv(const char *, const char *, int);
extern int hb_putenv(char *);
extern int hb_unsetenv(const char *);
extern void hb_abort(void) __attribute__((noreturn));
extern char **hb_environ;

static int a_rand_1 = 0; /* expected first rand() after srand(1), filled in main */

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

static unsigned int seeded_state = 12345;
static unsigned int prng(void)
{
    seeded_state = seeded_state * 1664525u + 1013904223u;
    return seeded_state;
}

static void rand_string(char *buf, int maxlen, unsigned int *st)
{
    int len = (int)(*st % (unsigned int)(maxlen + 1));
    int i;
    *st = *st * 1103515245u + 12345u;
    for (i = 0; i < len; i++) {
        int r = (int)(*st % 16u);
        *st = *st * 1103515245u + 12345u;
        if (r < 10) buf[i] = (char)('0' + r);
        else if (r == 10) buf[i] = '-';
        else if (r == 11) buf[i] = '+';
        else if (r == 12) buf[i] = 'x';
        else if (r == 13) buf[i] = 'a';
        else if (r == 14) buf[i] = 'Z';
        else buf[i] = ' ';
    }
    buf[len] = '\0';
}

static void test_strto_parity(void)
{
    static const int bases[] = { 0, 2, 8, 10, 16, 36 };
    int iter;

    for (iter = 0; iter < 12000; iter++) {
        char buf[40];
        char *e1, *e2;
        long l1, l2;
        long long ll1, ll2;
        unsigned long u1, u2;
        unsigned long long ull1, ull2;
        int base = bases[prng() % (sizeof(bases) / sizeof(bases[0]))];
        int kind = (int)(prng() % 4);
        int en1, en2;
        int i;

        for (i = 0; i < 10; i++) {
            int r = (int)(prng() % 4);
            buf[i] = (char)((r < 2) ? '0' + prng() % 10
                                    : (r == 2 ? 'a' + prng() % 6
                                              : (char)(' ' + prng() % 20)));
        }
        buf[10] = '\0';
        if (prng() % 20 == 0) {
            buf[0] = '-';
            buf[1] = '9';
            buf[2] = '9';
            buf[3] = '9';
            buf[4] = '9';
            buf[5] = '9';
            buf[6] = '9';
            buf[7] = '9';
            buf[8] = '9';
            buf[9] = '9';
        }

        errno = 0;
        switch (kind) {
        case 0:
            l1 = hb_strtol(buf, &e1, base);
            en1 = errno;
            errno = 0;
            l2 = strtol(buf, &e2, base);
            en2 = errno;
            CHECK(l1 == l2, "strtol value");
            CHECK(e1 == e2, "strtol endptr");
            break;
        case 1:
            ll1 = hb_strtoll(buf, &e1, base);
            en1 = errno;
            errno = 0;
            ll2 = strtoll(buf, &e2, base);
            en2 = errno;
            CHECK(ll1 == ll2, "strtoll value");
            CHECK(e1 == e2, "strtoll endptr");
            break;
        case 2:
            u1 = hb_strtoul(buf, &e1, base);
            en1 = errno;
            errno = 0;
            u2 = strtoul(buf, &e2, base);
            en2 = errno;
            CHECK(u1 == u2, "strtoul value");
            CHECK(e1 == e2, "strtoul endptr");
            break;
        default:
            ull1 = hb_strtoull(buf, &e1, base);
            en1 = errno;
            errno = 0;
            ull2 = strtoull(buf, &e2, base);
            en2 = errno;
            CHECK(ull1 == ull2, "strtoull value");
            CHECK(e1 == e2, "strtoull endptr");
            break;
        }
        CHECK(en1 == en2, "strto* errno parity (ERANGE/0)");
    }

    /* atoi/atol/atoll parity on random decimal strings */
    {
        unsigned int st2 = 777;
        int i;
        for (i = 0; i < 3000; i++) {
            char buf[24];
            rand_string(buf, 22, &st2);
            CHECK(hb_atoi(buf) == atoi(buf), "atoi");
            CHECK(hb_atol(buf) == atol(buf), "atol");
            CHECK(hb_atoll(buf) == atoll(buf), "atoll");
        }
    }

    /* literal edge cases: limits, prefixes, separators, no-conversion */
    {
        char *ep;
        char lit[24];

        strcpy(lit, "9223372036854775807");
        errno = 0;
        CHECK(hb_strtol(lit, &ep, 10) == LONG_MAX && *ep == '\0' &&
                  errno == 0, "strtol LONG_MAX");
        strcpy(lit, "9223372036854775808");
        errno = 0;
        CHECK(hb_strtol(lit, &ep, 10) == LONG_MAX && errno == ERANGE,
              "strtol +overflow -> LONG_MAX/ERANGE");
        strcpy(lit, "-9223372036854775808");
        errno = 0;
        CHECK(hb_strtol(lit, &ep, 10) == LONG_MIN && *ep == '\0' &&
                  errno == 0, "strtol LONG_MIN");
        strcpy(lit, "-9223372036854775809");
        errno = 0;
        CHECK(hb_strtol(lit, &ep, 10) == LONG_MIN && errno == ERANGE,
              "strtol -overflow -> LONG_MIN/ERANGE");
        strcpy(lit, "0x1F");
        errno = 0;
        CHECK(hb_strtoll(lit, &ep, 16) == 31 && *ep == '\0' && errno == 0,
              "strtoll 0x prefix base16");
        strcpy(lit, "010");
        errno = 0;
        CHECK(hb_strtoll(lit, &ep, 0) == 8 && *ep == '\0' && errno == 0,
              "strtoll octal auto base0");
        strcpy(lit, "1_000");
        errno = 0;
        CHECK(hb_strtoll(lit, &ep, 10) == 1000 && *ep == '\0' && errno == 0,
              "strtoll underscore separator");
        strcpy(lit, "0x");
        errno = 0;
        CHECK(hb_strtoll(lit, &ep, 16) == 0 && ep == lit,
              "strtoll no digits after prefix -> endptr=nptr");
        strcpy(lit, "-1");
        errno = 0;
        CHECK(hb_strtoul(lit, &ep, 10) == ULONG_MAX && errno == 0,
              "strtoul(-1) wraps to ULONG_MAX");
        strcpy(lit, "18446744073709551616");
        errno = 0;
        CHECK(hb_strtoul(lit, &ep, 10) == ULONG_MAX && errno == ERANGE,
              "strtoul +overflow -> ULONG_MAX/ERANGE");
        strcpy(lit, "-18446744073709551616");
        errno = 0;
        CHECK(hb_strtoul(lit, &ep, 10) == ULONG_MAX && errno == ERANGE,
              "strtoul -overflow saturates ULONG_MAX/ERANGE");
        strcpy(lit, " \t +42xyz");
        errno = 0;
        CHECK(hb_strtol(lit, &ep, 10) == 42 && *ep == 'x' && errno == 0,
              "strtol ws/sign/prefix");
    }
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a;
    long y = *(const long *)b;
    return (x > y) - (x < y);
}

static void test_qsort_bsearch(void)
{
    int iter;
    for (iter = 0; iter < 400; iter++) {
        long arr[256];
        long ref[256];
        long key;
        int n = 1 + (int)(prng() % 256);
        int i;
        for (i = 0; i < n; i++) {
            long v = (long)(prng() % 100000);
            arr[i] = v;
            ref[i] = v;
        }
        hb_qsort(arr, n, sizeof(long), cmp_long);
        qsort(ref, n, sizeof(long), cmp_long);
        for (i = 0; i < n; i++) {
            if (arr[i] != ref[i]) {
                CHECK(0, "qsort output vs glibc");
                break;
            }
        }
        key = (prng() % 100000);
        {
            long *f1 = (long *)bsearch(&key, arr, (size_t)n, sizeof(long), cmp_long);
            long *f2 = (long *)hb_bsearch(&key, arr, (size_t)n, sizeof(long), cmp_long);
            /* presence must agree; actual element must compare equal */
            CHECK((f1 == NULL) == (f2 == NULL), "bsearch presence");
            if (f1 != NULL && f2 != NULL) CHECK(*f1 == *f2, "bsearch value");
        }
    }
}

static void test_abs_rand(void)
{
    CHECK(hb_abs(-5) == 5, "abs(-5)");
    CHECK(hb_abs(5) == 5, "abs(5)");
    CHECK(hb_abs(0) == 0, "abs(0)");
    CHECK(hb_labs(-2147483648L) == 2147483648L, "labs(INT_MIN)");
    /* llabs(LLONG_MIN) is UB that wraps to LLONG_MIN in both glibc and
     * our implementation (0ULL - 2^63 casts back) — assert the wrap. */
    CHECK(hb_llabs(-9223372036854775807LL - 1) == LLONG_MIN, "llabs(LLONG_MIN) wraps");

    hb_srand(1);
    {
        int a = hb_rand();
        int b = hb_rand();
        CHECK(a >= 0 && a <= 2147483647, "rand bounds");
        CHECK(b >= 0 && b <= 2147483647, "rand bounds 2");
    }
    hb_srand(1);
    CHECK(hb_rand() == a_rand_1, "rand deterministic");
}

static void test_env(void)
{
    char *r;

    CHECK(hb_getenv("HB_NO_SUCH_VAR") == NULL, "getenv missing");
    CHECK(hb_setenv("HB_K", "v1", 0) == 0, "setenv new");
    r = hb_getenv("HB_K");
    CHECK(r != NULL && strcmp(r, "v1") == 0, "getenv after setenv");
    CHECK(hb_setenv("HB_K", "v2", 0) == 0, "setenv no-overwrite");
    r = hb_getenv("HB_K");
    CHECK(r != NULL && strcmp(r, "v1") == 0, "setenv no-overwrite keeps old");
    CHECK(hb_setenv("HB_K", "v2", 1) == 0, "setenv overwrite");
    r = hb_getenv("HB_K");
    CHECK(r != NULL && strcmp(r, "v2") == 0, "setenv overwrite applied");

    CHECK(hb_putenv((char *)"HB_P=pp") == 0, "putenv");
    r = hb_getenv("HB_P");
    CHECK(r != NULL && strcmp(r, "pp") == 0, "getenv after putenv");
    CHECK(hb_unsetenv("HB_P") == 0, "unsetenv");
    CHECK(hb_getenv("HB_P") == NULL, "getenv after unsetenv");
    CHECK(hb_unsetenv("HB_MISSING_TOO") == 0, "unsetenv missing = 0");
    CHECK(hb_setenv("HB_BAD=NAME", "x", 1) == -1, "setenv rejects '='");
    CHECK(hb_setenv("", "x", 1) == -1, "setenv rejects empty name");

    /* no cross-contamination with glibc's environment */
    setenv("GLB_ONLY", "g", 1);
    CHECK(hb_getenv("GLB_ONLY") == NULL, "hb env isolated from glibc");
    hb_setenv("HB_ONLY", "h", 1);
    CHECK(getenv("HB_ONLY") == NULL, "glibc env isolated from hb");
}

static void test_abort(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        hb_abort();
        _exit(99); /* unreachable */
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
              "abort raises SIGABRT");
    } else {
        CHECK(0, "fork failed");
    }
}

int main(void)
{
    /* fill the deterministic-rand expected value */
    hb_srand(1);
    a_rand_1 = hb_rand();

    test_strto_parity();
    test_qsort_bsearch();
    test_abs_rand();
    test_env();
    test_abort();

    printf("libc_stdlib_test: %d checks, %d failures\n", total_checks,
           failures);
    if (failures == 0) {
        printf("LIB C STDLIB TEST PASSED\n");
        return 0;
    }
    printf("LIB C STDLIB TEST FAILED\n");
    return 1;
}
