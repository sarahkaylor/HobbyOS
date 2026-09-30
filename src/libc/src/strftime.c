/*
 * HobbyOS libc: strftime.c — C-locale time formatting (P3.2).
 *
 * Implements the C99/POSIX directives with glibc's C-locale output, plus
 * the GNU padding flags ('-', '_', '0' and field widths) that ported GNU
 * sources use.  No timezone database: %Z is "UTC" and %z "+0000" (local
 * time is UTC everywhere in HobbyOS, matching time.c).
 *
 * asctime/ctime use the fixed C99 26-byte form.  ctime ==
 * asctime(localtime(...)) with localtime == gmtime here.
 *
 * HOST_TEST renames the public names to hb_* so the host test can race
 * glibc's output byte-for-byte (timezone-independent: %s uses timegm on the
 * device and glibc's strftime %s uses mktime, identical under TZ=UTC0).
 */
#ifdef HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#else
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"
#include "errno.h"
#endif

#ifdef HOST_TEST
#define strftime hb_strftime
#define strftime_l hb_strftime_l
#define asctime hb_asctime
#define asctime_r hb_asctime_r
#define ctime hb_ctime
#define ctime_r hb_ctime_r
#endif

static const char *const day_abbr[7] = {"Sun", "Mon", "Tue", "Wed",
                                        "Thu", "Fri", "Sat"};
static const char *const day_full[7] = {"Sunday",   "Monday", "Tuesday",
                                        "Wednesday", "Thursday", "Friday",
"Saturday"};
static const char *const mon_abbr[12] = {"Jan", "Feb", "Mar", "Apr",
                                         "May", "Jun", "Jul", "Aug",
                                         "Sep", "Oct", "Nov", "Dec"};
static const char *const mon_full[12] = {"January",   "February", "March",
                                         "April",     "May",      "June",
                                         "July",      "August",   "September",
                                         "October",   "November", "December"};

/* Formatting output state: bounded append; overflow flips `over`. */
typedef struct {
  char *dst;
  size_t cap; /* including NUL */
  size_t len;
  int over;
} sfmt_out;

static void sfmt_char(sfmt_out *o, char c) {
  if (o->len + 1 < o->cap)
    o->dst[o->len] = c;
  else
    o->over = 1;
  o->len++;
}

static void sfmt_str(sfmt_out *o, const char *s) {
  while (*s)
    sfmt_char(o, *s++);
}

/* Numeric field with glibc padding rules.  `pad` is the effective pad
 * character: '-' removes padding (width ignored), '_' and ' ' pad with
 * spaces, '0' zero-pads (sign kept left of the zeros).  tmp[] holds the
 * digits REVERSED (output reads it back to front), so the sign goes at
 * the high end when zero-padding and right after the digits otherwise. */
static void sfmt_num(sfmt_out *o, long v, int width, int pad) {
  char tmp[24];
  int n = 0;
  unsigned long uv;
  int neg = v < 0;

  if (pad == '-')
    width = 0;
  if (width > (int)sizeof(tmp) - 1)
    width = (int)sizeof(tmp) - 1;

  uv = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
  do {
    tmp[n++] = (char)('0' + (int)(uv % 10));
    uv /= 10;
  } while (uv != 0);

  if (pad == '0') {
    while (n < width - (neg ? 1 : 0))
      tmp[n++] = '0';
    if (neg)
      tmp[n++] = '-';
  } else {
    if (neg)
      tmp[n++] = '-';
    while (n < width)
      tmp[n++] = ' ';
  }
  while (n > 0)
    sfmt_char(o, tmp[--n]);
}

/* Weekday of Jan 1 of `year` given a tm consistent with it. */
static int jan1_wday(int year, int wday, int yday) {
  int w = (wday - yday) % 7;
  (void)year;
  return w < 0 ? w + 7 : w;
}

static int is_leap(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int weeks_in_year(int jan1, int leap) {
  /* 53 ISO weeks iff Jan 1 is a Thursday, or a leap year starting on
   * Wednesday (its Dec 31 is then a Thursday). */
  return 52 + (jan1 == 4 || (leap && jan1 == 3));
}

/* ISO 8601 week number + week-based year. */
static int iso_week(const struct tm *tm, int *iso_year) {
  int year = tm->tm_year + 1900;
  int wday = tm->tm_wday == 0 ? 7 : tm->tm_wday; /* Mon=1..Sun=7 */
  int ord = tm->tm_yday + 1;
  int week, j1;

  *iso_year = year;
  week = (ord - wday + 10) / 7;
  if (week < 1) {
    /* Belongs to the last week of the previous year. */
    int prev_leap = is_leap(year - 1);
    j1 = jan1_wday(year - 1, tm->tm_wday,
                   tm->tm_yday + (prev_leap ? -366 : -365));
    *iso_year = year - 1;
    return weeks_in_year(j1, prev_leap);
  }
  j1 = jan1_wday(year, tm->tm_wday, tm->tm_yday);
  if (week > weeks_in_year(j1, is_leap(year))) {
    /* Falls into week 1 of the next year. */
    *iso_year = year + 1;
    return 1;
  }
  return week;
}

static int wday12(int hour) {
  int h = hour % 12;
  return h == 0 ? 12 : h;
}

size_t strftime(char *s, size_t max, const char *format,
                const struct tm *tm) {
  sfmt_out o;
  const char *p;

  o.dst = s;
  o.cap = max;
  o.len = 0;
  o.over = 0;

  for (p = format; *p != '\0'; p++) {
    int pad = 0; /* 0 = default, '0', '_' or '-' (no pad) */
    int width = 0;
    char c;

    if (*p != '%') {
      sfmt_char(&o, *p);
      continue;
    }
    p++;
    /* GNU flags: - _ 0 */
    for (;;) {
      if (*p == '-')
        pad = '-';
      else if (*p == '_')
        pad = '_';
      else if (*p == '0')
        pad = '0';
      else
        break;
      p++;
    }
    while (*p >= '0' && *p <= '9')
      width = width * 10 + (*p++ - '0');
    c = *p;

    switch (c) {
    case 'a':
      sfmt_str(&o, day_abbr[tm->tm_wday % 7]);
      break;
    case 'A':
      sfmt_str(&o, day_full[tm->tm_wday % 7]);
      break;
    case 'b':
    case 'h':
      sfmt_str(&o, mon_abbr[tm->tm_mon % 12]);
      break;
    case 'B':
      sfmt_str(&o, mon_full[tm->tm_mon % 12]);
      break;
    case 'c':
      sfmt_str(&o, day_abbr[tm->tm_wday % 7]);
      sfmt_char(&o, ' ');
      sfmt_str(&o, mon_abbr[tm->tm_mon % 12]);
      sfmt_char(&o, ' ');
      sfmt_num(&o, tm->tm_mday, 2, ' ');
      sfmt_char(&o, ' ');
      sfmt_num(&o, tm->tm_hour, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_min, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_sec, 2, '0');
      sfmt_char(&o, ' ');
      sfmt_num(&o, tm->tm_year + 1900, 0, '0');
      break;
    case 'C':
      sfmt_num(&o, (tm->tm_year + 1900) / 100, 2,
               pad ? pad : '0');
      break;
    case 'd':
      sfmt_num(&o, tm->tm_mday, 2,
               pad ? pad : '0');
      break;
    case 'D':
      sfmt_num(&o, tm->tm_mon + 1, 2, '0');
      sfmt_char(&o, '/');
      sfmt_num(&o, tm->tm_mday, 2, '0');
      sfmt_char(&o, '/');
      sfmt_num(&o, (tm->tm_year + 1900) % 100, 2, '0');
      break;
    case 'e':
      sfmt_num(&o, tm->tm_mday, 2, ' ');
      break;
    case 'F':
      sfmt_num(&o, tm->tm_year + 1900, 4, '0');
      sfmt_char(&o, '-');
      sfmt_num(&o, tm->tm_mon + 1, 2, '0');
      sfmt_char(&o, '-');
      sfmt_num(&o, tm->tm_mday, 2, '0');
      break;
    case 'g':
    case 'G': {
        int iso_y;
        (void)iso_week(tm, &iso_y);
        sfmt_num(&o, c == 'g' ? iso_y % 100 : iso_y, c == 'g' ? 2 : 4, '0');
        break;
      }
    case 'H':
      sfmt_num(&o, tm->tm_hour, 2,
               pad ? pad : '0');
      break;
    case 'I':
      sfmt_num(&o, wday12(tm->tm_hour), 2,
               pad ? pad : '0');
      break;
    case 'j':
      sfmt_num(&o, tm->tm_yday + 1, 3, '0');
      break;
    case 'k':
      sfmt_num(&o, tm->tm_hour, 2, ' ');
      break;
    case 'l':
      sfmt_num(&o, wday12(tm->tm_hour), 2, ' ');
      break;
    case 'm':
      sfmt_num(&o, tm->tm_mon + 1, 2,
               pad ? pad : '0');
      break;
    case 'M':
      sfmt_num(&o, tm->tm_min, 2,
               pad ? pad : '0');
      break;
    case 'n':
      sfmt_char(&o, '\n');
      break;
    case 'p':
      sfmt_str(&o, tm->tm_hour < 12 ? "AM" : "PM");
      break;
    case 'P':
      sfmt_str(&o, tm->tm_hour < 12 ? "am" : "pm");
      break;
    case 'r':
      sfmt_num(&o, wday12(tm->tm_hour), 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_min, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_sec, 2, '0');
      sfmt_char(&o, ' ');
      sfmt_str(&o, tm->tm_hour < 12 ? "AM" : "PM");
      break;
    case 'R':
      sfmt_num(&o, tm->tm_hour, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_min, 2, '0');
      break;
    case 's': {
        struct tm copy = *tm;
        time_t secs = timegm(&copy);
        sfmt_num(&o, (long)secs, 0, '0');
        break;
      }
    case 'S':
      sfmt_num(&o, tm->tm_sec, 2,
               pad ? pad : '0');
      break;
    case 't':
      sfmt_char(&o, '\t');
      break;
    case 'T':
      sfmt_num(&o, tm->tm_hour, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_min, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_sec, 2, '0');
      break;
    case 'u':
      sfmt_num(&o, tm->tm_wday == 0 ? 7 : tm->tm_wday, 1, '0');
      break;
    case 'U': /* week of year, Sunday as first day */
      sfmt_num(&o, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0');
      break;
    case 'V': {
        int iso_y;
        sfmt_num(&o, iso_week(tm, &iso_y), 2, '0');
        break;
      }
    case 'w':
      sfmt_num(&o, tm->tm_wday, 1, '0');
      break;
    case 'W': /* week of year, Monday as first day */
      sfmt_num(&o, (tm->tm_yday + 7 - (tm->tm_wday == 0 ? 6 : tm->tm_wday - 1)) / 7,
               2, '0');
      break;
    case 'x':
      sfmt_num(&o, tm->tm_mon + 1, 2, '0');
      sfmt_char(&o, '/');
      sfmt_num(&o, tm->tm_mday, 2, '0');
      sfmt_char(&o, '/');
      sfmt_num(&o, (tm->tm_year + 1900) % 100, 2, '0');
      break;
    case 'X':
      sfmt_num(&o, tm->tm_hour, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_min, 2, '0');
      sfmt_char(&o, ':');
      sfmt_num(&o, tm->tm_sec, 2, '0');
      break;
    case 'y':
      sfmt_num(&o, (tm->tm_year + 1900) % 100, 2,
               pad ? pad : '0');
      break;
    case 'Y':
      sfmt_num(&o, tm->tm_year + 1900, 4,
               pad ? pad : '0');
      break;
    case 'z':
      sfmt_str(&o, "+0000");
      break;
    case 'Z':
      sfmt_str(&o, "UTC");
      break;
    case '%':
      sfmt_char(&o, '%');
      break;
    case '\0': /* trailing '%': glibc copies the '%' literally */
      sfmt_char(&o, '%');
      goto finish;
    default: /* unknown directive: glibc copies it literally */
      sfmt_char(&o, '%');
      sfmt_char(&o, c);
      break;
    }
  }

finish:
  if (o.over || o.cap == 0)
    return 0;
  s = o.dst;
  s[o.len] = '\0';
  return o.len;
}

size_t strftime_l(char *s, size_t max, const char *format,
                  const struct tm *tm, locale_t loc) {
  (void)loc;
  return strftime(s, max, format, tm);
}

/* wcsftime is not host-tested (the host build must not collide with
 * glibc's wcsftime, which has no hb_ rename); the device build compiles
 * the whole block. */
#ifndef HOST_TEST
/* wcsftime: the format and result are ASCII here (C locale); the wide
 * form is handled with a narrow staging buffer. */
size_t wcsftime(wchar_t *s, size_t maxsize, const wchar_t *format,
                const struct tm *timeptr) {
  char *narrow;
  size_t i, n;

  for (i = 0; format[i] != 0; i++) {
    if (format[i] < 0 || format[i] >= 0x80) {
      errno = EILSEQ;
      return 0;
    }
  }
  if (maxsize == 0)
    return 0;
  narrow = malloc(maxsize);
  if (narrow == NULL)
    return 0;

  {
    char *nfmt = malloc(i + 1);
    if (nfmt == NULL) {
      free(narrow);
      return 0;
    }
    for (i = 0; format[i] != 0; i++)
      nfmt[i] = (char)format[i];
    nfmt[i] = '\0';

    n = strftime(narrow, maxsize, nfmt, timeptr);
    free(nfmt);
  }
  if (n == 0) {
    free(narrow);
    return 0;
  }
  for (i = 0; i <= n; i++)
    s[i] = (wchar_t)(unsigned char)narrow[i];
  free(narrow);
  return n;
}
#endif /* !HOST_TEST */

/* ---------------- asctime / ctime ---------------- */

static char *asctime_into(const struct tm *tm, char *buf) {
  /* C99: "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n" */
  char *d = buf;
  const char *src;
  int v;

  src = day_abbr[tm->tm_wday % 7];
  *d++ = src[0];
  *d++ = src[1];
  *d++ = src[2];
  *d++ = ' ';
  src = mon_abbr[tm->tm_mon % 12];
  *d++ = src[0];
  *d++ = src[1];
  *d++ = src[2];

  /* C99 "%3d": the day is right-aligned in a three-wide field, which is
   * why glibc prints "Jul 14" and "Jan  1". */
  v = tm->tm_mday;
  *d++ = ' ';
  *d++ = v < 10 ? ' ' : (char)('0' + (v / 10) % 10);
  *d++ = (char)('0' + v % 10);
  *d++ = ' ';

  v = tm->tm_hour;
  *d++ = (char)('0' + (v / 10) % 10);
  *d++ = (char)('0' + v % 10);
  *d++ = ':';
  v = tm->tm_min;
  *d++ = (char)('0' + (v / 10) % 10);
  *d++ = (char)('0' + v % 10);
  *d++ = ':';
  v = tm->tm_sec;
  *d++ = (char)('0' + (v / 10) % 10);
  *d++ = (char)('0' + v % 10);
  *d++ = ' ';

  /* Year: C99 leaves this as a plain %d (no zero padding). */
  v = tm->tm_year + 1900;
  {
    char tmp[8];
    int n = 0;
    do {
      tmp[n++] = (char)('0' + v % 10);
      v /= 10;
    } while (v != 0);
    while (n > 0)
      *d++ = tmp[--n];
  }
  *d++ = '\n';
  *d = '\0';
  return buf;
}

static char hb_asctime_buf[26];

char *asctime(const struct tm *tm) {
  return asctime_into(tm, hb_asctime_buf);
}

char *asctime_r(const struct tm *tm, char *buf) {
  return asctime_into(tm, buf);
}

char *ctime(const time_t *timep) {
  struct tm tm;
  if (localtime_r(timep, &tm) == NULL)
    return NULL;
  return asctime_into(&tm, hb_asctime_buf);
}

char *ctime_r(const time_t *timep, char *buf) {
  struct tm tm;
  if (localtime_r(timep, &tm) == NULL)
    return NULL;
  return asctime_into(&tm, buf);
}
