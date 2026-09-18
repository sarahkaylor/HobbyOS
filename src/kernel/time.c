/*
 * time.c - Time/date helpers shared by all architectures.
 *
 * Converts between Unix epoch seconds and broken-down calendar time using
 * Howard Hinnant's civil-calendar algorithms (pure integer math, no libm).
 *
 * Used by the RTC drivers (arch/arm/rtc.c, arch/x64/rtc.c) to serve
 * sysinfo command 6.
 */

#include <stdint.h>

int rtc_is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* Days since 1970-01-01 for the given civil date. */
int64_t rtc_days_from_civil(int year, int month, int day) {
    int64_t y = year;
    y -= (month <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Build Unix epoch seconds from a civil UTC date/time. */
uint64_t rtc_make_epoch(int year, int month, int day, int hour, int minute, int second) {
    int64_t days = rtc_days_from_civil(year, month, day);
    int64_t secs = days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
    return (uint64_t)secs;
}

/* Break Unix epoch seconds into UTC civil time.
 * weekday: 0=Sunday .. 6=Saturday (1970-01-01 was a Thursday). */
void rtc_epoch_to_time(uint64_t epoch, int *year, int *month, int *day,
                       int *hour, int *minute, int *second, int *weekday) {
    int64_t days = (int64_t)(epoch / 86400ULL);
    uint64_t rem = epoch % 86400ULL;

    if (hour)   *hour = (int)(rem / 3600ULL);
    rem %= 3600ULL;
    if (minute) *minute = (int)(rem / 60ULL);
    if (second) *second = (int)(rem % 60ULL);

    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mm = mp + (mp < 10 ? 3 : -9);

    if (year)  *year = (int)(yy + (mm <= 2));
    if (month) *month = (int)mm;
    if (day)   *day = (int)dd;
    if (weekday) *weekday = (int)(((days % 7) + 4 + 7) % 7);
}
