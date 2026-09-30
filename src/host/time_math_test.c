/*
 * HobbyOS host test: pure calendar math (src/libc/src/time_math.c).
 *
 * time_math.c is compiled with -DHOST_TEST (its functions land as hb_*)
 * and every case is raced against glibc's timegm()/gmtime_r() on the same
 * inputs.  The epoch table is hardcoded and was generated from an
 * independent reference (Python calendar.timegm on explicit civil-date
 * tuples; normalization rows via datetime + timedelta), covering epoch 0,
 * pre-1970 (negative) epochs, the 1900/2000/2100 leap-century rules, the
 * 2038 boundary, and late-2026 dates.
 *
 * Exit 0 on full pass, non-zero with a FAIL count otherwise.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The HobbyOS implementations (renamed by the HOST_TEST block). */
extern time_t hb_timegm(struct tm *tm);
extern struct tm *hb_gmtime_r(const time_t *timep, struct tm *result);
extern struct tm *hb_localtime_r(const time_t *timep, struct tm *result);
extern time_t hb_mktime(struct tm *tm);

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                 \
  do {                                                    \
    checks++;                                             \
    if (!(cond)) {                                        \
      failures++;                                         \
      printf("FAIL %s (line %d)\n", what, __LINE__);      \
    }                                                     \
  } while (0)

struct case_t {
  int year, mon, mday, hour, min, sec; /* mon is 0-11 (struct tm form) */
  long long epoch;
  const char *what;
};

/* Civil date -> epoch (calendar.timegm reference, UTC). */
static const struct case_t CASES[] = { { 1970,  0,  1,  0,  0,  0,            0LL, "epoch" }, { 1970,  0,  1,  0,  0,  1,            1LL, "epoch+1s" }, { 1969, 11, 31, 23, 59, 59,           -1LL, "pre-epoch -1s" }, { 1969, 11, 31,  0,  0,  0,       -86400LL, "pre-epoch -1day" }, { 1969,  6, 20, 20, 17, 40,    -14182940LL, "pre-epoch 1969-07-20" }, { 1960,  0,  1,  0,  0,  0,   -315619200LL, "1960-01-01" }, {    1,  0,  1,  0,  0,  0, -62135596800LL, "year 1" }, { 1900,  1, 28, 23, 59, 59,  -2203891201LL, "1900 non-leap Feb 28" }, { 1900,  2,  1,  0,  0,  0,  -2203891200LL, "1900 non-leap Mar 1" }, { 2000,  1, 29, 12, 34, 56,    951827696LL, "2000-02-29 leap noon" }, { 2000,  1, 29, 23, 59, 59,    951868799LL, "2000-02-29 end" }, { 2000,  2,  1,  0,  0,  0,    951868800LL, "2000-03-01" }, { 2024,  1, 29,  0,  0,  0,   1709164800LL, "2024-02-29" }, { 2038,  0, 19,  3, 14,  7,   2147483647LL, "2038-01-19 03:14:07" }, { 2038,  0, 19,  3, 14,  8,   2147483648LL, "2038-01-19 03:14:08" }, { 2100,  1, 28, 23, 59, 59,   4107542399LL, "2100 non-leap Feb 28 end" }, { 2100,  2,  1,  0,  0,  0,   4107542400LL, "2100 non-leap Mar 1" }, { 2026,  8, 29,  0,  0,  0,   1790640000LL, "2026-09-29 start" }, { 2026,  8, 29, 12,  0,  0,   1790683200LL, "2026-09-29 noon" }, { 2026, 11, 31, 23, 59, 59,   1798761599LL, "2026-12-31 end" }, { 2099, 11, 31, 23, 59, 59,   4102444799LL, "2099-12-31 end" },
};

/* Out-of-range fields: timegm/mktime fold them (datetime + timedelta
 * reference) — the input struct must come back normalized, glibc-style. */
static const struct case_t NORM[] = { { 2000,  0, 32,  0,  0,  0,    949363200LL, "mday 32 -> Feb 1" }, { 2026, 12,  1,  0,  0,  0,   1798761600LL, "tm_mon 13 -> Jan next year" }, { 1970,  0,  1, -1,  0,  0,        -3600LL, "hour -1" }, { 2026,  0,  1,  0, 60, 60,   1767229260LL, "minute 60 + second 60" }, { 2026, 11, 31, 23, 59, 60,   1798761600LL, "second 60 rolls the day" }, { 2026,  1, 29,  0,  0,  0,   1772323200LL, "Feb 29 in a non-leap year" }, { 2026, -1, 15,  0,  0,  0,   1765756800LL, "tm_mon -1 -> Dec prev year" }, { 2026, -2, 15,  0,  0,  0,   1763164800LL, "tm_mon -2 -> Nov prev year" },
};

static struct tm mk_tm(const struct case_t *c) {
  struct tm t;
  memset(&t, 0, sizeof t);
  t.tm_year = c->year - 1900;
  t.tm_mon = c->mon;
  t.tm_mday = c->mday;
  t.tm_hour = c->hour;
  t.tm_min = c->min;
  t.tm_sec = c->sec;
  return t;
}

/* The 9 standard fields (glibc's struct tm carries extras we don't write). */
static int tm_fields_eq(const struct tm *a, const struct tm *b) {
  return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min &&
         a->tm_hour == b->tm_hour && a->tm_mday == b->tm_mday &&
         a->tm_mon == b->tm_mon && a->tm_year == b->tm_year &&
         a->tm_wday == b->tm_wday && a->tm_yday == b->tm_yday &&
         a->tm_isdst == b->tm_isdst;
}

static void test_cases(void) {
  for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
    const struct case_t *c = &CASES[i];
    struct tm t = mk_tm(c), g, rt, lt, mt;

    errno = 123;
    long long e = (long long)hb_timegm(&t);
    CHECK(e == c->epoch, "timegm matches the hardcoded reference");
    CHECK(errno == 123, "timegm success leaves errno alone");
    g = mk_tm(c);
    long long e_glibc = (long long)timegm(&g);
    CHECK(e == e_glibc, "timegm epoch matches glibc");
    CHECK(tm_fields_eq(&t, &g), "normalized struct matches glibc's");

    time_t te = (time_t)c->epoch;
    memset(&rt, 0xAA, sizeof rt);
    CHECK(hb_gmtime_r(&te, &rt) == &rt, "gmtime_r returns result");
    CHECK(rt.tm_year == c->year - 1900 && rt.tm_mon == c->mon &&
              rt.tm_mday == c->mday && rt.tm_hour == c->hour &&
              rt.tm_min == c->min && rt.tm_sec == c->sec,
          "gmtime_r breaks the epoch back to the input date");
    {
      struct tm gt;
      memset(&gt, 0xAA, sizeof gt);
      CHECK(gmtime_r(&te, &gt) == &gt, "reference gmtime_r works");
      CHECK(tm_fields_eq(&rt, &gt), "gmtime_r fields match glibc");
    }

    mt = mk_tm(c);
    CHECK((long long)hb_mktime(&mt) == c->epoch, "mktime == timegm");
    CHECK(tm_fields_eq(&mt, &t), "mktime normalizes like timegm");

    memset(&lt, 0xAA, sizeof lt);
    hb_localtime_r(&te, &lt);
    CHECK(tm_fields_eq(&lt, &rt), "localtime_r == gmtime_r (no tzdb)");

    /* Round trip through the broken-down form. */
    {
      struct tm back = rt;
      CHECK((long long)hb_timegm(&back) == c->epoch,
            "timegm(gmtime_r(e)) round-trips");
      CHECK(tm_fields_eq(&back, &rt), "round trip is stable");
    }
  }
}

static void test_normalization(void) {
  for (size_t i = 0; i < sizeof NORM / sizeof NORM[0]; i++) {
    const struct case_t *c = &NORM[i];
    struct tm t = mk_tm(c), g = mk_tm(c);
    struct tm ref;

    long long e = (long long)hb_timegm(&t);
    CHECK(e == c->epoch, "normalized epoch matches the reference");
    long long e_glibc = (long long)timegm(&g);
    CHECK(e == e_glibc, "normalized epoch matches glibc");
    CHECK(tm_fields_eq(&t, &g), "normalized struct matches glibc's");

    /* The normalized struct must agree with gmtime_r(e). */
    time_t te = (time_t)e;
    hb_gmtime_r(&te, &ref);
    CHECK(tm_fields_eq(&t, &ref), "normalized struct re-broken equals input");
  }
}

static void test_edge(void) {
  struct tm t;
  errno = 0;
  CHECK(hb_gmtime_r(NULL, &t) == NULL && errno == EINVAL,
        "gmtime_r(NULL) fails with EINVAL");
  errno = 0;
  CHECK(hb_gmtime_r((time_t[]){0}, NULL) == NULL && errno == EINVAL,
        "gmtime_r(NULL result) fails with EINVAL");
  errno = 0;
  CHECK(hb_timegm(NULL) == (time_t)-1 && errno == EINVAL,
        "timegm(NULL) fails with EINVAL");
  errno = 0;
  CHECK(hb_localtime_r(NULL, &t) == NULL && errno == EINVAL,
        "localtime_r(NULL) fails with EINVAL");

  /* Pre-epoch floor division: -1 must be 1969-12-31 23:59:59, not
   * 1970-01-01 00:00:00 truncated toward zero. */
  {
    time_t neg = -1;
    struct tm rt;
    hb_gmtime_r(&neg, &rt);
    CHECK(rt.tm_year == 69 && rt.tm_mon == 11 && rt.tm_mday == 31 &&
              rt.tm_hour == 23 && rt.tm_min == 59 && rt.tm_sec == 59,
          "-1 floors to the previous day");
    CHECK(rt.tm_wday == 3, "1969-12-31 was a Wednesday");
    CHECK(rt.tm_yday == 364, "1969-12-31 is day 364");
  }

  /* Leap-day weekday sanity: 2000-02-29 was a Tuesday (2). */
  {
    time_t leap = (time_t)951868799;
    struct tm rt;
    hb_gmtime_r(&leap, &rt);
    CHECK(rt.tm_wday == 2, "2000-02-29 was a Tuesday");
    CHECK(rt.tm_yday == 59, "2000-02-29 is day 59");
  }
}

int main(void) {
  test_cases();
  test_normalization();
  test_edge();

  printf("time_math_test: %d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("PASS\n");
  return failures ? 1 : 0;
}
