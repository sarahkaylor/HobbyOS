/*
 * clock_test.c - Host unit tests for clock.c (Clock & Calendar).
 *
 * The app source is included directly (see src/host/pong_test.c) so the tests
 * can call internal helpers and inspect/manipulate the app state. The blocking
 * main loop is never run: helpers, event handling and buffer rendering are
 * exercised directly.
 *
 * Mocks come from src/host/compat.c: sysinfo(6) reports 2026-09-17 12:00:00
 * (Thursday) and sysinfo(1) reports 12345 ms of uptime. The fallback (no RTC)
 * path is driven by clearing clk.have_rtc / setting clk.uptime_ms directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main clock_app_main
#include "../user/clock.c"
#undef main

/* ================================================================== */
/* Tiny check harness                                                  */
/* ================================================================== */

static int checks_run = 0;
static int checks_passed = 0;
static int checks_failed = 0;

static void check(int cond, const char *name) {
  checks_run++;
  if (cond) {
    checks_passed++;
    printf("PASS: %s\n", name);
  } else {
    checks_failed++;
    printf("FAIL: %s\n", name);
  }
}

static void check_int(long got, long want, const char *name) {
  char msg[192];
  if (got == want) {
    check(1, name);
    return;
  }
  snprintf(msg, sizeof msg, "%s (got %ld, want %ld)", name, got, want);
  check(0, msg);
}

static void check_str(const char *got, const char *want, const char *name) {
  char msg[320];
  if (got && want && strcmp(got, want) == 0) {
    check(1, name);
    return;
  }
  snprintf(msg, sizeof msg, "%s (got \"%s\", want \"%s\")",
           name, got ? got : "(null)", want ? want : "(null)");
  check(0, msg);
}

static void check_has(const char *hay, const char *needle, const char *name) {
  char msg[320];
  if (hay && needle && strstr(hay, needle)) {
    check(1, name);
    return;
  }
  snprintf(msg, sizeof msg, "%s (substring \"%s\" missing)", name,
           needle ? needle : "(null)");
  check(0, msg);
}

static void check_lacks(const char *hay, const char *needle, const char *name) {
  char msg[320];
  if (hay && needle && !strstr(hay, needle)) {
    check(1, name);
    return;
  }
  snprintf(msg, sizeof msg, "%s (unexpected substring \"%s\" present)", name,
           needle ? needle : "(null)");
  check(0, msg);
}

static int count_char(const char *s, int c) {
  int n = 0;
  for (; *s; s++) if (*s == c) n++;
  return n;
}

/* ================================================================== */
/* Test-local golden data (typed independently from clock.c)            */
/* ================================================================== */

/* Expected 3x5 glyphs: 3 columns per row, '#' = on. */
static const char *const golden_glyph[10][5] = {
  { "###", "# #", "# #", "# #", "###" },  /* 0 */
  { " # ", "## ", " # ", " # ", "###" },  /* 1 */
  { "###", "  #", "###", "#  ", "###" },  /* 2 */
  { "###", "  #", "###", "  #", "###" },  /* 3 */
  { "# #", "# #", "###", "  #", "  #" },  /* 4 */
  { "###", "#  ", "###", "  #", "###" },  /* 5 */
  { "###", "#  ", "###", "# #", "###" },  /* 6 */
  { "###", "  #", "  #", "  #", "  #" },  /* 7 */
  { "###", "# #", "###", "# #", "###" },  /* 8 */
  { "###", "# #", "###", "  #", "###" },  /* 9 */
};

/* Assemble the expected big-time row (17 chars) from the golden glyphs. */
static void build_expected_row(int hour, int minute, int row, char *out) {
  const char *g;
  int j = 0;
  g = golden_glyph[(hour / 10) % 10][row];
  for (int i = 0; i < 3; i++) out[j++] = g[i];
  out[j++] = ' ';
  g = golden_glyph[hour % 10][row];
  for (int i = 0; i < 3; i++) out[j++] = g[i];
  out[j++] = ' ';
  out[j++] = (row == 1 || row == 3) ? '#' : ' ';
  out[j++] = ' ';
  g = golden_glyph[(minute / 10) % 10][row];
  for (int i = 0; i < 3; i++) out[j++] = g[i];
  out[j++] = ' ';
  g = golden_glyph[minute % 10][row];
  for (int i = 0; i < 3; i++) out[j++] = g[i];
  out[j] = '\0';
}

static char screen[CLK_SCREEN_MAX];
static char screen2[CLK_SCREEN_MAX];

static void reset_app(void) {
  clk_init();     /* mock RTC: 2026-09-17 12:00:00 Thursday */
}

/* Every line 110 chars or shorter (desktop limit). */
static int all_lines_short(const char *s, int max) {
  const char *p = s;
  int line = 0;
  while (*p) {
    const char *nl = strchr(p, '\n');
    int l = nl ? (int)(nl - p) : (int)strlen(p);
    if (l > max) {
      printf("  line %d is %d chars: \"%.*s\"\n", line, l, l, p);
      return 0;
    }
    line++;
    p = nl ? nl + 1 : p + l;
  }
  return 1;
}

/* Border rows ('|' or '+' at column 0) are exactly `width` chars wide. */
static int box_rows_aligned(const char *s, int width) {
  const char *p = s;
  while (*p) {
    const char *nl = strchr(p, '\n');
    int l = nl ? (int)(nl - p) : (int)strlen(p);
    if ((*p == '|' || *p == '+') && l != width) {
      printf("  box row %d wide (want %d): \"%.*s\"\n", l, width, l, p);
      return 0;
    }
    p = nl ? nl + 1 : p + l;
  }
  return 1;
}

/* ================================================================== */
/* Leap years / month lengths                                          */
/* ================================================================== */

static void test_leap_years(void) {
  check_int(clk_is_leap(2000), 1, "is_leap(2000) - divisible by 400");
  check_int(clk_is_leap(2024), 1, "is_leap(2024)");
  check_int(clk_is_leap(2100), 0, "is_leap(2100) - century, not /400");
  check_int(clk_is_leap(1900), 0, "is_leap(1900) - century, not /400");
  check_int(clk_is_leap(2023), 0, "is_leap(2023)");
  check_int(clk_is_leap(1970), 0, "is_leap(1970)");
  check_int(clk_is_leap(2096), 1, "is_leap(2096)");
}

static void test_days_in_month(void) {
  check_int(clk_days_in_month(2024, 2), 29, "Feb 2024 = 29 days");
  check_int(clk_days_in_month(2026, 2), 28, "Feb 2026 = 28 days");
  check_int(clk_days_in_month(2100, 2), 28, "Feb 2100 = 28 days");
  check_int(clk_days_in_month(2000, 2), 29, "Feb 2000 = 29 days");
  check_int(clk_days_in_month(2026, 1), 31, "Jan 2026 = 31 days");
  check_int(clk_days_in_month(2026, 4), 30, "Apr 2026 = 30 days");
  check_int(clk_days_in_month(2026, 12), 31, "Dec 2026 = 31 days");
  check_int(clk_days_in_month(2026, 0), 0, "month 0 invalid -> 0");
  check_int(clk_days_in_month(2026, 13), 0, "month 13 invalid -> 0");
  check_int(clk_days_in_month(2026, -1), 0, "month -1 invalid -> 0");
}

/* ================================================================== */
/* Weekdays                                                            */
/* ================================================================== */

static void test_day_of_week(void) {
  check_int(clk_day_of_week(2026, 9, 17), 4, "2026-09-17 is Thursday (4)");
  check_int(clk_day_of_week(2000, 1, 1), 6, "2000-01-01 is Saturday (6)");
  check_int(clk_day_of_week(1970, 1, 1), 4, "1970-01-01 is Thursday (4)");
  check_int(clk_day_of_week(2026, 9, 1), 2, "2026-09-01 is Tuesday (2)");
  check_int(clk_day_of_week(2024, 2, 29), 4, "2024-02-29 is Thursday (4)");
  check_int(clk_day_of_week(2026, 2, 1), 0, "2026-02-01 is Sunday (0)");
  check_int(clk_day_of_week(2026, 0, 1), -1, "month 0 -> -1");
  check_int(clk_day_of_week(2026, 13, 1), -1, "month 13 -> -1");
}

static void test_first_weekday_index(void) {
  /* Monday-first column of the 1st of the month. */
  check_int(clk_first_weekday_index(2026, 9), 1, "Sept 2026 starts on column 1 (Tue)");
  check_int(clk_first_weekday_index(2026, 6), 0, "June 2026 starts on column 0 (Mon)");
  check_int(clk_first_weekday_index(2024, 2), 3, "Feb 2024 starts on column 3 (Thu)");
  check_int(clk_first_weekday_index(2026, 3), 6, "March 2026 starts on column 6 (Sun)");
  check_int(clk_first_weekday_index(2026, 0), -1, "invalid month -> -1");
}

/* ================================================================== */
/* Month grids                                                         */
/* ================================================================== */

static void test_month_grid_sept_2026(void) {
  int grid[CLK_GRID_CELLS];
  int i, ok;
  int weeks = clk_build_month_grid(2026, 9, grid);
  /* Sept 1 2026 is a Tuesday -> Monday-first column 1; 30 days -> 5 rows. */
  check_int(weeks, 5, "Sept 2026 grid needs 5 week rows");
  ok = 1;
  for (i = 0; i < CLK_GRID_CELLS; i++) {
    int want = (i >= 1 && i <= 30) ? i : 0;
    if (grid[i] != want) {
      printf("  cell %d: got %d want %d\n", i, grid[i], want);
      ok = 0;
    }
  }
  check(ok, "Sept 2026 grid: 1 blank cell, days 1..30, tail blank");
  check_int(grid[1], 1, "Sept 2026: first cell is day 1");
  check_int(grid[0], 0, "Sept 2026: leading blank cell");
  check_int(grid[30], 30, "Sept 2026: last day is 30");
  check_int(grid[31], 0, "Sept 2026: blank after day 30");
  check_int(grid[41], 0, "Sept 2026: final cell blank");
}

static void test_month_grid_feb_2024(void) {
  int grid[CLK_GRID_CELLS];
  int weeks = clk_build_month_grid(2024, 2, grid);
  check_int(weeks, 5, "Feb 2024 (leap) grid needs 5 week rows");
  check_int(grid[0], 0, "Feb 2024: starts after 3 blank cells? cell0 blank");
  check_int(grid[2], 0, "Feb 2024: cell2 blank (Thursday start)");
  check_int(grid[3], 1, "Feb 2024: day 1 in cell 3");
  check_int(grid[31], 29, "Feb 2024: day 29 in cell 31");
  check_int(grid[32], 0, "Feb 2024: blank after leap day");
}

static void test_month_grid_monday_start(void) {
  int grid[CLK_GRID_CELLS];
  int weeks = clk_build_month_grid(2026, 6, grid);
  check_int(weeks, 5, "June 2026 (Monday start) needs 5 week rows");
  check_int(grid[0], 1, "June 2026: day 1 in cell 0 (no leading blanks)");
  check_int(grid[29], 30, "June 2026: day 30 in cell 29");
  check_int(grid[30], 0, "June 2026: blank after day 30");
}

static void test_month_grid_six_weeks(void) {
  int grid[CLK_GRID_CELLS];
  int weeks = clk_build_month_grid(2026, 3, grid);
  check_int(weeks, 6, "March 2026 (Sunday start, 31 days) needs 6 week rows");
  check_int(grid[5], 0, "March 2026: cell5 blank (Sunday start)");
  check_int(grid[6], 1, "March 2026: day 1 in cell 6");
  check_int(grid[36], 31, "March 2026: day 31 in cell 36");
  check_int(grid[37], 0, "March 2026: blank after day 31");
}

static void test_month_grid_four_weeks(void) {
  int grid[CLK_GRID_CELLS];
  int weeks = clk_build_month_grid(2027, 2, grid);
  check_int(weeks, 4, "Feb 2027 (Monday start, 28 days) needs 4 week rows");
  check_int(grid[0], 1, "Feb 2027: day 1 in cell 0");
  check_int(grid[27], 28, "Feb 2027: day 28 in cell 27");
  check_int(grid[28], 0, "Feb 2027: blank after day 28");
}

static void test_month_grid_invalid(void) {
  int grid[CLK_GRID_CELLS];
  int i, ok = 1;
  check_int(clk_build_month_grid(2026, 13, grid), 0, "invalid month -> 0 weeks");
  for (i = 0; i < CLK_GRID_CELLS; i++) {
    if (grid[i] != 0) ok = 0;
  }
  check(ok, "invalid month -> grid cleared");
}

/* ================================================================== */
/* Calendar cell rows                                                  */
/* ================================================================== */

static void test_cal_cells(void) {
  char buf[64];
  int week0[7] = { 0, 1, 2, 3, 4, 5, 6 };
  int week2[7] = { 14, 15, 16, 17, 18, 19, 20 };
  int week3[7] = { 21, 22, 23, 24, 25, 26, 27 };
  int tail[7] = { 28, 29, 30, 0, 0, 0, 0 };

  clk_cal_cells(buf, week0, 0);
  check_str(buf, "    1  2  3  4  5  6", "week row: blank(Mon) then 1..6");
  check_int(strlen(buf), 20, "normal week row is 20 chars");

  clk_cal_cells(buf, week2, 0);
  check_str(buf, "14 15 16 17 18 19 20", "week row without highlight");

  clk_cal_cells(buf, week2, 17);
  check_str(buf, "14 15 16 [17] 18 19 20", "today 17 is bracketed");
  check_int(strlen(buf), 22, "highlighted week row is 22 chars (documented shift)");

  clk_cal_cells(buf, tail, 30);
  check(strncmp(buf, "28 29 [30]", 10) == 0 && (int)strlen(buf) == 22
            && strspn(buf + 10, " ") == 12 && buf[22] == '\0',
        "highlight on the last day pads the tail with blanks");

  clk_cal_cells(buf, week3, 0);
  check_str(buf, "21 22 23 24 25 26 27", "week row 4");

  clk_cal_cells(buf, week0, 1);
  check_has(buf, "[1]", "single-digit today is bracketed");
  check_int(strlen(buf), 21, "single-digit highlight row is 21 chars");
}

/* ================================================================== */
/* ASCII art                                                           */
/* ================================================================== */

static void test_art_digits(void) {
  char row[8];
  int i, r, ok;

  /* Structural: every glyph row is exactly 3 chars of '#' / ' '. */
  ok = 1;
  for (i = 0; i < 10; i++) {
    for (r = 0; r < 5; r++) {
      int n = clk_art_digit_row(i, r, row);
      if (n != 3 || strlen(row) != 3) {
        printf("  digit %d row %d: length %d\n", i, r, n);
        ok = 0;
      }
      for (int k = 0; k < 3; k++) {
        if (row[k] != '#' && row[k] != ' ') {
          printf("  digit %d row %d: bad char '%c'\n", i, r, row[k]);
          ok = 0;
        }
      }
    }
  }
  check(ok, "all 10 digits x 5 rows are exactly 3 chars of '#'/' '");

  /* Golden comparison against the test-local table (independent copy). */
  ok = 1;
  for (i = 0; i < 10; i++) {
    for (r = 0; r < 5; r++) {
      clk_art_digit_row(i, r, row);
      if (strcmp(row, golden_glyph[i][r]) != 0) {
        printf("  digit %d row %d: got \"%s\" want \"%s\"\n",
               i, r, row, golden_glyph[i][r]);
        ok = 0;
      }
    }
  }
  check(ok, "all glyphs match the golden table");

  /* All ten glyphs must be distinct (no copy/paste duplicates). */
  ok = 1;
  for (i = 0; i < 10; i++) {
    char glyph[20];
    int j = 0;
    for (r = 0; r < 5; r++) {
      clk_art_digit_row(i, r, row);
      for (int k = 0; k < 3; k++) glyph[j++] = row[k];
    }
    glyph[j] = '\0';
    for (int d = 0; d < i; d++) {
      char other[20];
      int o = 0;
      for (r = 0; r < 5; r++) {
        clk_art_digit_row(d, r, row);
        for (int k = 0; k < 3; k++) other[o++] = row[k];
      }
      other[o] = '\0';
      if (strcmp(glyph, other) == 0) {
        printf("  glyph %d identical to %d\n", i, d);
        ok = 0;
      }
    }
  }
  check(ok, "all ten glyphs are distinct");

  /* A few exact hand-verified cells. */
  clk_art_digit_row(1, 0, row);
  check_str(row, " # ", "glyph '1' top row is \" # \"");
  clk_art_digit_row(1, 1, row);
  check_str(row, "## ", "glyph '1' second row is \"## \"");
  clk_art_digit_row(0, 2, row);
  check_str(row, "# #", "glyph '0' middle row is \"# #\"");
  clk_art_digit_row(7, 4, row);
  check_str(row, "  #", "glyph '7' bottom row is \"  #\"");
  clk_art_digit_row(9, 0, row);
  check_str(row, "###", "glyph '9' top row is \"###\"");

  /* Out-of-range inputs. */
  check_int(clk_art_digit_row(10, 0, row), -1, "digit 10 -> -1");
  check_int(clk_art_digit_row(-1, 0, row), -1, "digit -1 -> -1");
  check_int(clk_art_digit_row(0, 5, row), -1, "row 5 -> -1");
}

static void test_art_time_row(void) {
  char got[32];
  char want[32];
  int r, ok;

  ok = 1;
  for (r = 0; r < 5; r++) {
    int n = clk_art_time_row(12, 34, r, got);
    if (n != 17 || strlen(got) != 17) {
      printf("  row %d length %d\n", r, n);
      ok = 0;
    }
  }
  check(ok, "big HH:MM rows are exactly 17 chars");

  /* Colon column: '#' dots at rows 1 and 3, blank elsewhere. */
  ok = 1;
  for (r = 0; r < 5; r++) {
    clk_art_time_row(12, 34, r, got);
    int want_ch = (r == 1 || r == 3) ? '#' : ' ';
    if (got[8] != want_ch) {
      printf("  row %d colon column is '%c' want '%c'\n", r, got[8], want_ch);
      ok = 0;
    }
  }
  check(ok, "colon column 8 shows dots on rows 1 and 3 only");

  /* Golden cross-check for a spread of times. */
  ok = 1;
  for (int t = 0; t < 5; t++) {
    int hours[5] = { 0, 5, 12, 23, 9 };
    int mins[5] = { 0, 9, 34, 59, 12 };
    for (r = 0; r < 5; r++) {
      clk_art_time_row(hours[t], mins[t], r, got);
      build_expected_row(hours[t], mins[t], r, want);
      if (strcmp(got, want) != 0) {
        printf("  %02d:%02d row %d: got \"%s\" want \"%s\"\n",
               hours[t], mins[t], r, got, want);
        ok = 0;
      }
    }
  }
  check(ok, "big time rows match golden glyphs (5 times x 5 rows)");

  /* Exact string check for one full row (hand verified). */
  clk_art_time_row(12, 0, 0, got);
  check_str(got, " #  ###   ### ###", "12:00 top row exact art");
  clk_art_time_row(23, 59, 4, got);
  check_str(got, "### ###   ### ###", "23:59 bottom row exact art");

  check_int(clk_art_time_row(12, 0, 5, got), -1, "art row 5 -> -1");
  check_int(clk_art_time_row(12, 0, -1, got), -1, "art row -1 -> -1");
}

/* ================================================================== */
/* Formatting helpers                                                  */
/* ================================================================== */

static void test_fmt_hms(void) {
  char buf[24];
  clk_fmt_hms(0, 0, 0, buf);
  check_str(buf, "00:00:00", "fmt_hms(0,0,0)");
  clk_fmt_hms(9, 5, 3, buf);
  check_str(buf, "09:05:03", "fmt_hms(9,5,3) zero padded");
  clk_fmt_hms(23, 59, 59, buf);
  check_str(buf, "23:59:59", "fmt_hms(23,59,59)");
  clk_fmt_hms(13, 4, 9, buf);
  check_str(buf, "13:04:09", "fmt_hms(13,4,9) 24h");
  clk_fmt_hms(-1, -2, -3, buf);
  check_str(buf, "00:00:00", "fmt_hms clamps negatives");
  check_int(clk_fmt_hms(23, 59, 59, buf), 8, "fmt_hms always 8 chars");
}

static void test_names(void) {
  check_str(clk_weekday_name(0), "Sunday", "weekday_name(0)");
  check_str(clk_weekday_name(4), "Thursday", "weekday_name(4)");
  check_str(clk_weekday_name(6), "Saturday", "weekday_name(6)");
  check_str(clk_weekday_name(7), "?", "weekday_name(7) -> ?");
  check_str(clk_weekday_name(-1), "?", "weekday_name(-1) -> ?");
  check_str(clk_month_name(1), "January", "month_name(1)");
  check_str(clk_month_name(9), "September", "month_name(9)");
  check_str(clk_month_name(12), "December", "month_name(12)");
  check_str(clk_month_name(0), "?", "month_name(0) -> ?");
  check_str(clk_month_name(13), "?", "month_name(13) -> ?");
}

static void test_fmt_date_line(void) {
  char buf[64];
  check_int(clk_fmt_date_line(4, 2026, 9, 17, buf),
            (long)strlen("Thursday, September 17, 2026"), "date line length");
  check_str(buf, "Thursday, September 17, 2026", "date line for mock RTC date");
  clk_fmt_date_line(6, 2000, 1, 1, buf);
  check_str(buf, "Saturday, January 1, 2000", "date line: no zero padding on day");
  clk_fmt_date_line(0, 1970, 1, 1, buf);
  check_str(buf, "Sunday, January 1, 1970", "date line 1970");
  clk_fmt_date_line(7, 2026, 9, 17, buf);
  check_has(buf, "?, September 17, 2026", "bad weekday falls back to '?'");
}

static void test_fmt_month_year(void) {
  char buf[32];
  clk_fmt_month_year(2026, 9, buf);
  check_str(buf, "September 2026", "month-year title for Sept 2026");
  clk_fmt_month_year(1970, 1, buf);
  check_str(buf, "January 1970", "month-year title for Jan 1970");
}

static void test_uptime(void) {
  char buf[24];
  int h, m, s;
  clk_fmt_uptime(0, buf);
  check_str(buf, "00:00:00", "uptime 0 ms");
  clk_fmt_uptime(12345, buf);
  check_str(buf, "00:00:12", "uptime 12345 ms (mock)");
  clk_fmt_uptime(3661000, buf);
  check_str(buf, "01:01:01", "uptime 1h 1m 1s");
  clk_fmt_uptime(3599999, buf);
  check_str(buf, "00:59:59", "uptime 59m 59.999s truncates");
  clk_fmt_uptime(86400000ULL, buf);
  check_str(buf, "24:00:00", "uptime exactly 24h");
  clk_fmt_uptime(90000000ULL, buf);
  check_str(buf, "25:00:00", "uptime > 24h (25h)");
  clk_fmt_uptime(360000000ULL, buf);
  check_str(buf, "100:00:00", "uptime >= 100h keeps 3 digits");

  clk_split_uptime(90000000ULL, &h, &m, &s);
  check(h == 25 && m == 0 && s == 0, "split_uptime(25h) = 25/0/0");
  clk_split_uptime(12345ULL, &h, &m, &s);
  check(h == 0 && m == 0 && s == 12, "split_uptime(12345ms) = 0/0/12");
  clk_split_uptime(7322000ULL, &h, &m, &s);
  check(h == 2 && m == 2 && s == 2, "split_uptime(2h2m2s) = 2/2/2");
}

/* ================================================================== */
/* State: refresh / sanitize / clamp                                   */
/* ================================================================== */

static void test_refresh_and_init(void) {
  reset_app();
  check_int(clk.have_rtc, 1, "clk_init() sees the mock RTC");
  check_int(clk.year, 2026, "RTC year 2026");
  check_int(clk.month, 9, "RTC month 9");
  check_int(clk.day, 17, "RTC day 17");
  check_int(clk.hour, 12, "RTC hour 12");
  check_int(clk.minute, 0, "RTC minute 0");
  check_int(clk.second, 0, "RTC second 0");
  check_int(clk.weekday, 4, "RTC weekday Thursday(4)");
  check(clk.epoch == 1789646400ULL, "RTC epoch copied (1789646400)");
  check_int(clk.disp_year, 2026, "calendar initialised to RTC year");
  check_int(clk.disp_month, 9, "calendar initialised to RTC month");
  check_int(clk.weekday, clk_day_of_week(clk.year, clk.month, clk.day),
            "RTC weekday agrees with day_of_week()");
}

static void test_apply_time_sanitize(void) {
  struct sys_time t;
  memset(&t, 0, sizeof t);
  t.year = 2026; t.month = 13; t.day = 31;
  t.hour = 99; t.minute = 99; t.second = 99; t.weekday = 9;
  clk_apply_time(&t);
  check_int(clk.month, 12, "month 13 clamped to 12");
  check_int(clk.day, 31, "Dec 31 is valid");
  check_int(clk.hour, 3, "hour 99 wraps into 0..23 (99 -> 3)");
  check_int(clk.minute, 39, "minute 99 -> 39");
  check_int(clk.second, 39, "second 99 -> 39");
  check_int(clk.weekday, clk_day_of_week(2026, 12, 31),
            "bad weekday recomputed from the date");

  memset(&t, 0, sizeof t);
  t.year = 2026; t.month = 2; t.day = 31; t.hour = 5; t.weekday = 2;
  clk_apply_time(&t);
  check_int(clk.day, 28, "Feb 31 clamped to 28 (2026 not leap)");
  check_int(clk.weekday, 2, "valid weekday (2) is kept");

  memset(&t, 0, sizeof t);
  t.year = 1965; t.month = 1; t.day = 1;
  clk_apply_time(&t);
  check_int(clk.year, CLK_YEAR_MIN, "year 1965 clamped to 1970");
  memset(&t, 0, sizeof t);
  t.year = 2500; t.month = 1; t.day = 1;
  clk_apply_time(&t);
  check_int(clk.year, CLK_YEAR_MAX, "year 2500 clamped to 2099");
}

static void test_clamp_disp(void) {
  clk.disp_year = 1500;
  clk.disp_month = 33;
  clk_clamp_disp();
  check_int(clk.disp_year, CLK_YEAR_MIN, "disp year 1500 clamped to 1970");
  check_int(clk.disp_month, 12, "disp month 33 clamped to 12");

  clk.disp_year = 2200;
  clk.disp_month = 0;
  clk_clamp_disp();
  check_int(clk.disp_year, CLK_YEAR_MAX, "disp year 2200 clamped to 2099");
  check_int(clk.disp_month, 1, "disp month 0 clamped to 1");
}

/* ================================================================== */
/* Navigation                                                          */
/* ================================================================== */

static void test_month_nav(void) {
  reset_app();
  check_int(clk_month_delta(1), 1, "next month reports a change");
  check(clk.disp_year == 2026 && clk.disp_month == 10, "Sept -> Oct 2026");
  check_int(clk_month_delta(-1), 1, "prev month reports a change");
  check(clk.disp_year == 2026 && clk.disp_month == 9, "Oct -> Sept 2026");
  check_int(clk_month_delta(0), 0, "zero delta is a no-op");

  /* Dec -> next wraps the year. */
  clk.disp_year = 2026;
  clk.disp_month = 12;
  check_int(clk_month_delta(1), 1, "Dec -> next year reports a change");
  check(clk.disp_year == 2027 && clk.disp_month == 1, "Dec 2026 -> Jan 2027");

  /* Jan -> prev wraps the year back. */
  check_int(clk_month_delta(-1), 1, "Jan -> prev year reports a change");
  check(clk.disp_year == 2026 && clk.disp_month == 12, "Jan 2027 -> Dec 2026");

  /* Year bounds clamp and report no change. */
  clk.disp_year = 2099;
  clk.disp_month = 12;
  check_int(clk_month_delta(1), 0, "Dec 2099 -> next is clamped (no change)");
  check(clk.disp_year == 2099 && clk.disp_month == 12, "still Dec 2099");
  clk.disp_year = 1970;
  clk.disp_month = 1;
  check_int(clk_month_delta(-1), 0, "Jan 1970 -> prev is clamped (no change)");
  check(clk.disp_year == 1970 && clk.disp_month == 1, "still Jan 1970");

  /* Same year, month wrap inside the bounds still works at the edges. */
  clk.disp_year = 2099;
  clk.disp_month = 6;
  check_int(clk_month_delta(1), 1, "Jun 2099 -> Jul 2099 allowed");
  check(clk.disp_year == 2099 && clk.disp_month == 7, "Jun 2099 -> Jul 2099");
}

static void test_year_nav(void) {
  reset_app();
  check_int(clk_year_delta(1), 1, "next year reports a change");
  check_int(clk.disp_year, 2027, "2026 -> 2027");
  check_int(clk.disp_month, 9, "month is preserved when navigating years");
  check_int(clk_year_delta(-1), 1, "prev year reports a change");
  check_int(clk.disp_year, 2026, "2027 -> 2026");
  check_int(clk_year_delta(0), 0, "zero delta is a no-op");

  clk.disp_year = 2099;
  check_int(clk_year_delta(1), 0, "2099 -> 2100 clamped (no change)");
  check_int(clk.disp_year, 2099, "still 2099");
  clk.disp_year = 1970;
  check_int(clk_year_delta(-1), 0, "1970 -> 1969 clamped (no change)");
  check_int(clk.disp_year, 1970, "still 1970");
  check_int(clk_year_delta(1), 1, "1970 -> 1971 allowed");
  check_int(clk.disp_year, 1971, "1971 after down/up");
}

static void test_events(void) {
  struct gui_event ev;
  memset(&ev, 0, sizeof ev);

  reset_app();
  ev.type = GUI_EV_RIGHT;
  check_int(clk_handle_event(&ev), 1, "RIGHT redraws and moves forward");
  check_int(clk.disp_month, 10, "RIGHT -> October 2026");
  ev.type = GUI_EV_LEFT;
  check_int(clk_handle_event(&ev), 1, "LEFT redraws and moves back");
  check_int(clk.disp_month, 9, "LEFT -> September 2026");
  ev.type = GUI_EV_UP;
  check_int(clk_handle_event(&ev), 1, "UP redraws and moves a year back");
  check_int(clk.disp_year, 2025, "UP -> 2025");
  ev.type = GUI_EV_DOWN;
  check_int(clk_handle_event(&ev), 1, "DOWN redraws and moves a year forward");
  check_int(clk.disp_year, 2026, "DOWN -> 2026");

  /* 't' jumps back to the RTC month from anywhere. */
  clk.disp_year = 2030;
  clk.disp_month = 1;
  ev.type = GUI_EV_CHAR;
  ev.ch = 't';
  check_int(clk_handle_event(&ev), 1, "'t' requests a redraw");
  check(clk.disp_year == 2026 && clk.disp_month == 9, "'t' jumps to the RTC month");

  /* 'T' works too; 'r' refreshes the clock. */
  clk.disp_month = 5;
  ev.ch = 'T';
  check_int(clk_handle_event(&ev), 1, "'T' also jumps to today");
  check_int(clk.disp_month, 9, "'T' -> September");

  clk.hour = 7; clk.minute = 7; clk.second = 7;
  ev.ch = 'r';
  check_int(clk_handle_event(&ev), 1, "'r' requests a redraw");
  check(clk.hour == 12 && clk.minute == 0 && clk.second == 0, "'r' re-reads the clock");

  /* Menu 0 "Clock" -> Today, Refresh. */
  clk.disp_year = 2028;
  clk.disp_month = 2;
  ev.type = GUI_EV_MENU;
  ev.menu = 0;
  ev.item = 0;
  check_int(clk_handle_event(&ev), 1, "menu Today requests a redraw");
  check(clk.disp_year == 2026 && clk.disp_month == 9, "menu Today jumps to the RTC month");

  clk.hour = 3;
  ev.item = 1;
  check_int(clk_handle_event(&ev), 1, "menu Refresh requests a redraw");
  check_int(clk.hour, 12, "menu Refresh re-reads the clock");

  /* Unknown selections/keys are ignored. */
  ev.menu = 1;
  ev.item = 0;
  check_int(clk_handle_event(&ev), 0, "unknown menu ignored");
  ev.menu = 0;
  ev.item = 7;
  check_int(clk_handle_event(&ev), 0, "unknown menu item ignored");
  ev.type = GUI_EV_CHAR;
  ev.ch = 'x';
  check_int(clk_handle_event(&ev), 0, "unknown key ignored");
  ev.type = GUI_EV_NONE;
  check_int(clk_handle_event(&ev), 0, "GUI_EV_NONE ignored");
  ev.type = GUI_EV_ESC;
  check_int(clk_handle_event(&ev), 0, "ESC ignored");
  ev.type = GUI_EV_MOUSE;
  check_int(clk_handle_event(&ev), 0, "mouse click ignored (no mouse UI)");

  /* Without an RTC there is no "today" to jump to (no refresh here, so the
   * mock cannot silently restore the RTC). */
  clk.have_rtc = 0;
  clk.disp_year = 2020;
  clk.disp_month = 3;
  check_int(clk_jump_to_rtc_month(), 0, "jump without an RTC is a no-op");
  check(clk.disp_year == 2020 && clk.disp_month == 3, "displayed month unchanged");

  /* With an RTC the jump works, and 't' still refreshes first. */
  clk.have_rtc = 1;
  clk.year = 2026;
  clk.month = 9;
  check_int(clk_jump_to_rtc_month(), 1, "jump with an RTC reports a change");
  check(clk.disp_year == 2026 && clk.disp_month == 9, "jump lands on the RTC month");

  /* The next refresh recovers the RTC (mock always has one). */
  clk.have_rtc = 0;
  clk_refresh();
  check_int(clk.have_rtc, 1, "clk_refresh() restores the RTC state");
  check(clk.year == 2026 && clk.month == 9 && clk.day == 17, "restored date is correct");
}

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

static void test_render_rtc(void) {
  int n;
  reset_app();
  n = clk_render(screen, (int)sizeof screen);
  check(n > 0 && n < (int)sizeof screen - 1, "render fits the window buffer");
  check_int((int)strlen(screen), n, "render return value == strlen");
  check_has(screen, "=== Clock ===\n", "screen starts with the Clock title");
  check(screen[0] == '=' && strncmp(screen, "=== Clock ===", 13) == 0,
        "first line is exactly '=== Clock ==='");
  check_has(screen, "\n12:00:00\n", "HH:MM:SS line from struct fields");
  check_has(screen, "Thursday, September 17, 2026\n", "full weekday/month date line");
  check_has(screen, "September 2026", "calendar title shows the RTC month");
  check_has(screen, "+-- September 2026 ------+", "calendar box title row is 26 chars wide");
  check_has(screen, "Mo Tu We Th Fr Sa Su", "Monday-first weekday header");
  check_has(screen, "| Mo Tu We Th Fr Sa Su   |", "weekday header row is padded to the box");
  check_has(screen, "[17]", "today is bracketed in the calendar");
  check_int(count_char(screen, '['), 1, "exactly one day is highlighted");
  check_has(screen, "< > month  ^ v year  t=today\n", "navigation hint line");
  check_has(screen, " #  ###   ### ###", "big art top row for 12:00");
  check_has(screen, "\n### ###   ### ###\n", "big art bottom row for 12:00");
  check_has(screen, "| 14 15 16 [17] 18 19 20 |", "highlighted week row is rendered");
  check_has(screen, "|     1  2  3  4  5  6   |", "first week row starts on Tuesday");
  check(screen[n - 1] == '\n', "screen ends with a newline");

  /* Deterministic: same state -> byte-identical output. */
  memset(screen2, 'Z', sizeof screen2);
  clk_render(screen2, (int)sizeof screen2);
  check_str(screen2, screen, "rendering is deterministic for the same state");
}

static void test_render_shape(void) {
  reset_app();
  clk_render(screen, (int)sizeof screen);
  check(all_lines_short(screen, 110), "no rendered line exceeds 110 chars");
  check(box_rows_aligned(screen, CLK_CAL_W), "calendar border rows are all 25 chars");
  check((int)strlen(screen) < 1800, "one full screen stays under 1800 chars");
  check(count_char(screen, '\n') <= 24, "screen fits in 24 text rows");

  /* Every month of every year in range renders and stays aligned. */
  int ok = 1;
  for (int y = CLK_YEAR_MIN; y <= CLK_YEAR_MAX && ok; y += 7) {
    for (int m = 1; m <= 12 && ok; m++) {
      clk.disp_year = y;
      clk.disp_month = m;
      clk_render(screen, (int)sizeof screen);
      if (!all_lines_short(screen, 110) || !box_rows_aligned(screen, CLK_CAL_W)) {
        printf("  bad render for %04d-%02d\n", y, m);
        ok = 0;
        break;
      }
    }
  }
  check(ok, "every sampled month (1970..2099) renders within limits and stays aligned");

  /* Months other than the RTC month carry no highlight. */
  clk.disp_year = 2026;
  clk.disp_month = 8;
  clk_render(screen, (int)sizeof screen);
  check_has(screen, "August 2026", "August 2026 title");
  check_int(count_char(screen, '['), 0, "no highlight outside the RTC month");
  clk.disp_year = 2030;
  clk.disp_month = 1;
  clk_render(screen, (int)sizeof screen);
  check_has(screen, "January 2030", "January 2030 title");
  check_int(count_char(screen, '['), 0, "no highlight in a future month");
}

static void test_render_fallback(void) {
  int n;
  reset_app();
  /* No RTC: sysinfo(6) failed on the device; simulate the state. */
  clk.have_rtc = 0;
  clk.uptime_ms = 12345ULL;
  n = clk_render(screen, (int)sizeof screen);
  check(n > 0 && (int)strlen(screen) == n, "fallback render returns its length");
  check_has(screen, "RTC unavailable - uptime 00:00:12", "fallback banner with uptime");
  check_has(screen, "00:00:12", "fallback HH:MM:SS line");
  check_lacks(screen, "Mo Tu", "calendar hidden without an RTC");
  check_lacks(screen, "September", "month name hidden without an RTC");
  check_lacks(screen, "t=today", "navigation hint hidden without an RTC");
  check_int(count_char(screen, '\n'), 8, "fallback screen: title + 5 art + time + banner");

  /* >24h uptime keeps all digits and drives the big art. */
  clk.uptime_ms = 90000000ULL;   /* 25:00:00 */
  clk_render(screen, (int)sizeof screen);
  check_has(screen, "RTC unavailable - uptime 25:00:00", "uptime banner > 24h");
  char want[32];
  build_expected_row(25 % 100, 0, 1, want);
  check_has(screen, want, "big art shows 25:00 (hours mod 100)");
  build_expected_row(25 % 100, 0, 0, want);
  check_has(screen, want, "big art row 0 for 25:00");

  /* Uptime with 3-digit hours still renders. */
  clk.uptime_ms = 360000000ULL;  /* 100:00:00 */
  clk_render(screen, (int)sizeof screen);
  check_has(screen, "RTC unavailable - uptime 100:00:00", "uptime banner 100h");
  check(all_lines_short(screen, 110), "fallback screen respects the line limit");
}

static void test_render_buffer_safety(void) {
  char tiny[64];
  int n;
  reset_app();
  memset(tiny, 'X', sizeof tiny);
  n = clk_render(tiny, 16);
  check(n >= 0 && n <= 15, "small buffer: returned length <= cap-1");
  check(tiny[n] == '\0', "small buffer: NUL terminated at the returned length");
  check(tiny[16] == 'X' && tiny[63] == 'X', "small buffer: no writes past cap-1");

  memset(tiny, 'Y', sizeof tiny);
  n = clk_render(tiny, 1);
  check_int(n, 0, "cap 1 leaves room only for the NUL");
  check(tiny[0] == '\0', "cap 1 writes just the NUL");
  check(tiny[1] == 'Y', "cap 1 writes nothing beyond the NUL");

  memset(tiny, 'Y', sizeof tiny);
  n = clk_render(tiny, 0);
  check_int(n, 0, "cap 0 returns 0 without writing");
  check(tiny[0] == 'Y' && tiny[63] == 'Y', "cap 0 leaves the buffer untouched");
}

/* ================================================================== */
/* Startup contract                                                    */
/* ================================================================== */

static void test_startup_contract(void) {
  /* These strings are what the desktop / acceptance test look for. */
  check_str(CLK_TITLE, "Clock", "window title is \"Clock\"");
  check_str(CLK_MENU_NAME, "Clock", "menu 0 is named \"Clock\"");
  check_str(CLK_MENU_ITEMS, "Today,Refresh", "menu 0 offers Today,Refresh");
  check_str(CLK_MARKER, "[APP] CLOCK started\n", "console marker line");
  check_int((int)sizeof(CLK_MARKER) - 1, 20, "marker is a single 20-char line");
  check(CLK_TICK_MS == 1000, "auto-refresh ticks once per second");
  check(CLK_YEAR_MIN == 1970 && CLK_YEAR_MAX == 2099, "calendar year bounds 1970..2099");
}

/* Print the screens as evidence for the human reader (not a check). */
static void print_screens(void) {
  printf("\n--- RTC screen (mock: 2026-09-17 12:00:00 Thursday) ---\n");
  reset_app();
  clk_render(screen, (int)sizeof screen);
  printf("%s", screen);
  printf("--- Fallback screen (no RTC, uptime 25:00:00) ---\n");
  clk.have_rtc = 0;
  clk.uptime_ms = 90000000ULL;
  clk_render(screen, (int)sizeof screen);
  printf("%s", screen);
  printf("--- end ---\n\n");
}

/* ================================================================== */

int main(void) {
  printf("=== Clock & Calendar host tests ===\n\n");

  printf("[date helpers]\n");
  test_leap_years();
  test_days_in_month();
  test_day_of_week();
  test_first_weekday_index();

  printf("\n[month grids]\n");
  test_month_grid_sept_2026();
  test_month_grid_feb_2024();
  test_month_grid_monday_start();
  test_month_grid_six_weeks();
  test_month_grid_four_weeks();
  test_month_grid_invalid();
  test_cal_cells();

  printf("\n[ascii art]\n");
  test_art_digits();
  test_art_time_row();

  printf("\n[formatting]\n");
  test_fmt_hms();
  test_names();
  test_fmt_date_line();
  test_fmt_month_year();
  test_uptime();

  printf("\n[state]\n");
  test_refresh_and_init();
  test_apply_time_sanitize();
  test_clamp_disp();

  printf("\n[navigation]\n");
  test_month_nav();
  test_year_nav();
  test_events();

  printf("\n[rendering]\n");
  test_render_rtc();
  test_render_shape();
  test_render_fallback();
  test_render_buffer_safety();

  printf("\n[startup contract]\n");
  test_startup_contract();

  print_screens();

  printf("=== Results ===\n");
  printf("checks run:    %d\n", checks_run);
  printf("checks passed: %d\n", checks_passed);
  printf("checks failed: %d\n", checks_failed);

  if (checks_failed == 0) {
    printf("\nALL TESTS PASSED\n");
    return 0;
  }
  printf("\n%d CHECK(S) FAILED\n", checks_failed);
  return 1;
}
