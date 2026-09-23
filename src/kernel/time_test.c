/*
 * time_test.c - Kernel unit tests for the time/RTC helpers and the FAT-16
 * volume statistics syscall support (sysinfo cmd 6/7 backends).
 */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include <stdint.h>

extern int rtc_is_leap(int year);
extern int64_t rtc_days_from_civil(int year, int month, int day);
extern uint64_t rtc_make_epoch(int year, int month, int day, int hour, int minute, int second);
extern void rtc_epoch_to_time(uint64_t epoch, int *year, int *month, int *day,
                              int *hour, int *minute, int *second, int *weekday);
extern uint64_t rtc_read_epoch(void);
extern int fat16_stats(uint64_t *out_total, uint64_t *out_free);

static void check_time(uint64_t epoch, int y, int mo, int d, int h, int mi, int s, int wd) {
    int ay = 0, amo = 0, ad = 0, ah = 0, ami = 0, as = 0, awd = 0;
    rtc_epoch_to_time(epoch, &ay, &amo, &ad, &ah, &ami, &as, &awd);
    if (ay != y || amo != mo || ad != d || ah != h || ami != mi || as != s || awd != wd) {
        uart_puts("  epoch_to_time mismatch for epoch ");
        print_int((int)epoch);
        uart_puts(": got ");
        print_int(ay); uart_puts("-"); print_int(amo); uart_puts("-"); print_int(ad);
        uart_puts(" "); print_int(ah); uart_puts(":"); print_int(ami); uart_puts(":"); print_int(as);
        uart_puts(" wd="); print_int(awd);
        uart_puts(" expected ");
        print_int(y); uart_puts("-"); print_int(mo); uart_puts("-"); print_int(d);
        uart_puts(" "); print_int(h); uart_puts(":"); print_int(mi); uart_puts(":"); print_int(s);
        uart_puts(" wd="); print_int(wd);
        uart_puts("\n");
        tests_failed++;
        return;
    }
    tests_run++;
}

static void test_epoch_zero(void) {
    uart_puts("  Running test_epoch_zero...\n");
    /* 1970-01-01 00:00:00, a Thursday (weekday 4). */
    check_time(0, 1970, 1, 1, 0, 0, 0, 4);
}

static void test_known_dates(void) {
    uart_puts("  Running test_known_dates...\n");
    /* 2001-09-09 01:46:40 UTC, a Sunday. */
    check_time(1000000000ULL, 2001, 9, 9, 1, 46, 40, 0);
    /* 2026-09-17 12:00:00 UTC, a Thursday. */
    check_time(1789646400ULL, 2026, 9, 17, 12, 0, 0, 4);
    /* 2000-02-29 00:00:00 (leap day), a Tuesday. */
    check_time(951782400ULL, 2000, 2, 29, 0, 0, 0, 2);
}

static void test_make_epoch_roundtrip(void) {
    uart_puts("  Running test_make_epoch_roundtrip...\n");
    uint64_t e = rtc_make_epoch(2026, 9, 17, 12, 0, 0);
    EXPECT_EQ((e == 1789646400ULL), 1);
    e = rtc_make_epoch(1970, 1, 1, 0, 0, 0);
    EXPECT_EQ((e == 0), 1);
    e = rtc_make_epoch(2000, 2, 29, 6, 30, 15);
    int y, mo, d, h, mi, s, wd;
    rtc_epoch_to_time(e, &y, &mo, &d, &h, &mi, &s, &wd);
    EXPECT_EQ(y, 2000); EXPECT_EQ(mo, 2); EXPECT_EQ(d, 29);
    EXPECT_EQ(h, 6); EXPECT_EQ(mi, 30); EXPECT_EQ(s, 15);
}

static void test_leap_years(void) {
    uart_puts("  Running test_leap_years...\n");
    EXPECT_EQ(rtc_is_leap(2000), 1);
    EXPECT_EQ(rtc_is_leap(2024), 1);
    EXPECT_EQ(rtc_is_leap(1900), 0);
    EXPECT_EQ(rtc_is_leap(2100), 0);
    EXPECT_EQ(rtc_is_leap(2026), 0);
}

static void test_days_from_civil(void) {
    uart_puts("  Running test_days_from_civil...\n");
    EXPECT_EQ((rtc_days_from_civil(1970, 1, 1) == 0), 1);
    EXPECT_EQ((rtc_days_from_civil(1970, 1, 2) == 1), 1);
    EXPECT_EQ((rtc_days_from_civil(2000, 1, 1) == 10957), 1);
    EXPECT_EQ((rtc_days_from_civil(2026, 9, 17) == 20713), 1);
}

static void test_fat16_stats(void) {
    uart_puts("  Running test_fat16_stats...\n");
    tests_run++;
    uint64_t total = 0, freeb = 0;
    int rc = fat16_stats(&total, &freeb);
    EXPECT_EQ(rc, 0);
    /* The 64 MB image must report a plausible data area. */
    EXPECT_EQ((total > 16ULL * 1024 * 1024), 1);
    EXPECT_EQ((total < 256ULL * 1024 * 1024), 1);
    EXPECT_EQ((freeb > 0), 1);
    EXPECT_EQ((freeb <= total), 1);
    /* Sectors are 512 bytes; the data area must be sector-aligned. */
    EXPECT_EQ((total % 512) == 0, 1);
    EXPECT_EQ((freeb % 512) == 0, 1);
    /* The shipped image holds userland binaries, so some space is used. */
    EXPECT_EQ((total - freeb) > 0, 1);
    uart_puts("  [stats] total=");
    print_int((int)(total / (1024 * 1024)));
    uart_puts("MB free=");
    print_int((int)(freeb / (1024 * 1024)));
    uart_puts("MB\n");
}

static void test_rtc_read(void) {
    uart_puts("  Running test_rtc_read...\n");
    tests_run++;
    uint64_t e1 = rtc_read_epoch();
    if (e1 == 0) {
        uart_puts("  [rtc] RTC reads 0 (no RTC/not set) - sysinfo(6) will report unavailable\n");
        return;
    }
    /* Plausibility: between 2000-01-01 and 2100-01-01. */
    EXPECT_EQ((e1 >= 946684800ULL), 1);
    EXPECT_EQ((e1 < 4102444800ULL), 1);
    int y, mo, d, h, mi, s, wd;
    rtc_epoch_to_time(e1, &y, &mo, &d, &h, &mi, &s, &wd);
    uart_puts("  [rtc] epoch=");
    print_int((int)e1);
    uart_puts(" -> ");
    print_int(y); uart_puts("-"); print_int(mo); uart_puts("-"); print_int(d);
    uart_puts(" "); print_int(h); uart_puts(":"); print_int(mi); uart_puts(":"); print_int(s);
    uart_puts("\n");
    EXPECT_EQ((y >= 2000 && y < 2100), 1);
    EXPECT_EQ((mo >= 1 && mo <= 12), 1);
    EXPECT_EQ((d >= 1 && d <= 31), 1);
    EXPECT_EQ((h >= 0 && h <= 23), 1);
    EXPECT_EQ((mi >= 0 && mi <= 59), 1);
    EXPECT_EQ((s >= 0 && s <= 59), 1);
    EXPECT_EQ((wd >= 0 && wd <= 6), 1);
    /* Monotonicity: a second read must not go backwards. */
    uint64_t e2 = rtc_read_epoch();
    EXPECT_EQ((e2 >= e1), 1);
}

void time_test_suite(void) {
    uart_puts("time_test_suite:\n");
    test_epoch_zero();
    test_known_dates();
    test_make_epoch_roundtrip();
    test_leap_years();
    test_days_from_civil();
    test_fat16_stats();
    test_rtc_read();
}

#endif // KERNEL_MODE_UNIT_TEST
