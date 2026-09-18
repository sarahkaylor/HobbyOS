/*
 * clock.c - Clock & Calendar for HobbyOS.
 *
 * A windowed text app that shows the current time as large ASCII digits,
 * the date, and a navigable month calendar (Monday-first) with today
 * highlighted.
 *
 * Usage contract (see gui.h):
 *   - Draw with print() after gui_clear() ("\f" clears the window)
 *   - Read input with gui_read_event_timeout() (1 s tick re-renders)
 *   - Register menus with gui_add_menu() (libc.h)
 *
 * Data source:
 *   sysinfo(6, &t, sizeof t) -> struct sys_time (RTC wall clock).
 *   If it returns < 0 there is no RTC: fall back to sysinfo(1) uptime (ms),
 *   print an "RTC unavailable - uptime HH:MM:SS" banner and hide the calendar.
 *
 * Screen layout (RTC available):
 *   === Clock ===
 *   <big ASCII-art HH:MM, 5 rows tall, 3 columns per digit>
 *   12:00:00
 *   Thursday, September 17, 2026
 *
 *   +-- September 2026 ------+
 *   +------------------------+
 *   | Mo Tu We Th Fr Sa Su   |
 *   |     1  2  3  4  5  6   |
 *   |  7  8  9 10 11 12 13   |
 *   | 14 15 16 [17] 18 19 20 |
 *   | 21 22 23 24 25 26 27   |
 *   | 28 29 30               |
 *   +------------------------+
 *   < > month  ^ v year  t=today
 *
 * Calendar cells are right-aligned in 2 columns and joined by one space (a
 * full week row of cells is 20 chars). Today's cell is bracketed ("[17]"),
 * so a row containing today is 1-2 chars wider than the others - the spec
 * allows this ("shifts that row"); the box is sized (CLK_CAL_W) so that such
 * a row still fits inside the border with a gap.
 *
 * Without an RTC the big art and the HH:MM:SS line show uptime (hours mod
 * 100 for the 4-digit art), followed by the banner; the calendar and the
 * navigation hint are hidden.
 *
 * All arithmetic is integer-only; rendering is deterministic for a given
 * state so host tests can compare exact strings.
 */

#include "libc.h"
#include "gui.h"

/* ---- Constants ---- */

#define CLK_YEAR_MIN     1970
#define CLK_YEAR_MAX     2099
#define CLK_GRID_CELLS   42     /* 6 weeks x 7 days, day numbers, 0 = blank */
#define CLK_CAL_W        26     /* calendar box width incl. "+" borders */
#define CLK_TICK_MS      1000   /* auto-refresh period (gui_read_event_timeout) */
#define CLK_SCREEN_MAX   2048   /* window text buffer size on the desktop */

/* ---- Startup contract (kept as constants so host tests can pin them) ---- */

static const char CLK_TITLE[]      = "Clock";
static const char CLK_MENU_NAME[]  = "Clock";
static const char CLK_MENU_ITEMS[] = "Today,Refresh";
static const char CLK_MARKER[]     = "[APP] CLOCK started\n";

/* ---- Pure state (kept global so host tests can inspect/manipulate it) ---- */

struct clk_state {
    int have_rtc;                   /* 1 = sysinfo(6) succeeded */
    unsigned long long epoch;       /* Unix seconds (RTC) */
    int year, month, day;           /* today (RTC), sanitized */
    int weekday;                    /* 0 = Sunday .. 6 = Saturday */
    int hour, minute, second;       /* wall clock (24h) */
    unsigned long long uptime_ms;   /* fallback: sysinfo(1) milliseconds */
    int disp_year, disp_month;      /* displayed calendar month */
};

struct clk_state clk;

/* ---- ASCII digit art: 10 digits x 5 rows x 3 columns, '#' = on ---- */

static const char clk_digit_art[10][5][3] = {
    /* 0 */ { {'#','#','#'}, {'#',' ','#'}, {'#',' ','#'}, {'#',' ','#'}, {'#','#','#'} },
    /* 1 */ { {' ','#',' '}, {'#','#',' '}, {' ','#',' '}, {' ','#',' '}, {'#','#','#'} },
    /* 2 */ { {'#','#','#'}, {' ',' ','#'}, {'#','#','#'}, {'#',' ',' '}, {'#','#','#'} },
    /* 3 */ { {'#','#','#'}, {' ',' ','#'}, {'#','#','#'}, {' ',' ','#'}, {'#','#','#'} },
    /* 4 */ { {'#',' ','#'}, {'#',' ','#'}, {'#','#','#'}, {' ',' ','#'}, {' ',' ','#'} },
    /* 5 */ { {'#','#','#'}, {'#',' ',' '}, {'#','#','#'}, {' ',' ','#'}, {'#','#','#'} },
    /* 6 */ { {'#','#','#'}, {'#',' ',' '}, {'#','#','#'}, {'#',' ','#'}, {'#','#','#'} },
    /* 7 */ { {'#','#','#'}, {' ',' ','#'}, {' ',' ','#'}, {' ',' ','#'}, {' ',' ','#'} },
    /* 8 */ { {'#','#','#'}, {'#',' ','#'}, {'#','#','#'}, {'#',' ','#'}, {'#','#','#'} },
    /* 9 */ { {'#','#','#'}, {'#',' ','#'}, {'#','#','#'}, {' ',' ','#'}, {'#','#','#'} },
};

/* ================================================================== */
/* Date helpers                                                        */
/* ================================================================== */

int clk_is_leap(int y) {
    if (y % 4 != 0) return 0;
    if (y % 100 != 0) return 1;
    return (y % 400 == 0) ? 1 : 0;
}

/* Length of month m (1-12) of year y; 0 for an invalid month. */
int clk_days_in_month(int y, int m) {
    static const int len[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 0;
    if (m == 2 && clk_is_leap(y)) return 29;
    return len[m - 1];
}

/* Day of week for y-m-d: 0 = Sunday .. 6 = Saturday (Gregorian, Sakamoto).
 * Returns -1 for an invalid month. */
int clk_day_of_week(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy;
    long w;
    if (m < 1 || m > 12) return -1;
    yy = y;
    if (m < 3) yy -= 1;
    w = (long)yy + (long)(yy / 4) - (long)(yy / 100) + (long)(yy / 400)
        + (long)t[m - 1] + (long)d;
    int r = (int)(w % 7);
    if (r < 0) r += 7;
    return r;
}

/* Column of the 1st of the month in a Monday-first week: 0 = Monday .. 6 = Sunday. */
int clk_first_weekday_index(int y, int m) {
    int w = clk_day_of_week(y, m, 1);
    if (w < 0) return -1;
    return (w + 6) % 7;
}

/* Fill out[42] with day numbers (0 = blank cell) for the month, Monday-first.
 * Returns the number of week rows (0 for an invalid month, else 4..6). */
int clk_build_month_grid(int y, int m, int *out) {
    int nd, start, weeks;
    for (int i = 0; i < CLK_GRID_CELLS; i++) out[i] = 0;
    nd = clk_days_in_month(y, m);
    if (nd <= 0) return 0;
    start = clk_first_weekday_index(y, m);
    if (start < 0) return 0;
    for (int d = 1; d <= nd; d++) out[start + d - 1] = d;
    weeks = (start + nd + 6) / 7;
    if (weeks > 6) weeks = 6;
    return weeks;
}

/* ================================================================== */
/* Names and formatting                                                */
/* ================================================================== */

const char *clk_month_name(int m) {
    static const char *const names[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    if (m < 1 || m > 12) return "?";
    return names[m - 1];
}

const char *clk_weekday_name(int w) {
    static const char *const names[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    if (w < 0 || w > 6) return "?";
    return names[w];
}

/* Zero-pad to at least 2 digits ("7" -> "07"); returns length. */
int clk_pad2(int v, char *out) {
    if (v < 0) v = 0;
    return gui_uitoa_z((unsigned long)v, out, 2);
}

/* "HH:MM:SS" from clock fields (guards negatives; no mod: fields are 24h). */
int clk_fmt_hms(int h, int m, int s, char *out) {
    int j = 0;
    if (h < 0) h = 0;
    if (m < 0) m = 0;
    if (s < 0) s = 0;
    j += clk_pad2(h, out + j);
    out[j++] = ':';
    j += clk_pad2(m, out + j);
    out[j++] = ':';
    j += clk_pad2(s, out + j);
    out[j] = '\0';
    return j;
}

/* Split milliseconds into hours (may exceed 24), minutes, seconds. */
void clk_split_uptime(unsigned long long ms, int *out_h, int *out_m, int *out_s) {
    unsigned long long total = ms / 1000ULL;
    if (out_h) *out_h = (int)(total / 3600ULL);
    if (out_m) *out_m = (int)((total / 60ULL) % 60ULL);
    if (out_s) *out_s = (int)(total % 60ULL);
}

/* "HH:MM:SS" from uptime ms; hours >= 100 are printed with all digits. */
int clk_fmt_uptime(unsigned long long ms, char *out) {
    int h, m, s, j = 0;
    clk_split_uptime(ms, &h, &m, &s);
    if (h < 100) j += gui_uitoa_z((unsigned long)h, out + j, 2);
    else j += gui_uitoa((unsigned long)h, out + j);
    out[j++] = ':';
    j += clk_pad2(m, out + j);
    out[j++] = ':';
    j += clk_pad2(s, out + j);
    out[j] = '\0';
    return j;
}

/* "Thursday, September 17, 2026" (buffer >= 48). */
int clk_fmt_date_line(int weekday, int y, int m, int d, char *out) {
    const char *wn = clk_weekday_name(weekday);
    const char *mn = clk_month_name(m);
    int j = 0;
    for (int i = 0; wn[i]; i++) out[j++] = wn[i];
    out[j++] = ',';
    out[j++] = ' ';
    for (int i = 0; mn[i]; i++) out[j++] = mn[i];
    out[j++] = ' ';
    j += gui_uitoa((unsigned long)d, out + j);
    out[j++] = ',';
    out[j++] = ' ';
    j += gui_uitoa((unsigned long)y, out + j);
    out[j] = '\0';
    return j;
}

/* "September 2026" (buffer >= 24). */
int clk_fmt_month_year(int y, int m, char *out) {
    const char *mn = clk_month_name(m);
    int j = 0;
    for (int i = 0; mn[i]; i++) out[j++] = mn[i];
    out[j++] = ' ';
    j += gui_uitoa((unsigned long)y, out + j);
    out[j] = '\0';
    return j;
}

/* ================================================================== */
/* Big ASCII-art time                                                  */
/* ================================================================== */

/* One 3-column row of a digit glyph; returns 3 or -1 on bad input. */
int clk_art_digit_row(int digit, int row, char *out) {
    const char *s;
    if (digit < 0 || digit > 9 || row < 0 || row > 4) {
        out[0] = '\0';
        return -1;
    }
    s = clk_digit_art[digit][row];
    out[0] = s[0];
    out[1] = s[1];
    out[2] = s[2];
    out[3] = '\0';
    return 3;
}

/* One row of the big "HH:MM" time (17 chars): 4 glyphs separated by one
 * space, with a 1-column ':' between the pairs ('#' dots on rows 1 and 3). */
int clk_art_time_row(int hour, int minute, int row, char *out) {
    char g[4];
    int j = 0;
    if (row < 0 || row > 4) {
        out[0] = '\0';
        return -1;
    }
    if (hour < 0) hour = 0;
    if (minute < 0) minute = 0;
    clk_art_digit_row((hour / 10) % 10, row, g);
    for (int i = 0; i < 3; i++) out[j++] = g[i];
    out[j++] = ' ';
    clk_art_digit_row(hour % 10, row, g);
    for (int i = 0; i < 3; i++) out[j++] = g[i];
    out[j++] = ' ';
    out[j++] = (row == 1 || row == 3) ? '#' : ' ';
    out[j++] = ' ';
    clk_art_digit_row((minute / 10) % 10, row, g);
    for (int i = 0; i < 3; i++) out[j++] = g[i];
    out[j++] = ' ';
    clk_art_digit_row(minute % 10, row, g);
    for (int i = 0; i < 3; i++) out[j++] = g[i];
    out[j] = '\0';
    return j;
}

/* ================================================================== */
/* Calendar rows                                                       */
/* ================================================================== */

/* Render 7 day cells (right-aligned in 2 columns, space separated) into out.
 * 0 = blank cell; `today` is bracketed as "[17]" (0 = no highlight).
 * Returns the length (20 normally, 1-2 chars more when today is shown). */
int clk_cal_cells(char *out, const int *days, int today) {
    int j = 0;
    for (int i = 0; i < 7; i++) {
        int d = days[i];
        if (i > 0) out[j++] = ' ';
        if (d <= 0) {
            out[j++] = ' ';
            out[j++] = ' ';
        } else if (d == today) {
            out[j++] = '[';
            j += gui_uitoa((unsigned long)d, out + j);
            out[j++] = ']';
        } else {
            j += gui_itoa_pad((long)d, out + j, 2);
        }
    }
    out[j] = '\0';
    return j;
}

/* ================================================================== */
/* App state                                                           */
/* ================================================================== */

void clk_clamp_disp(void) {
    if (clk.disp_year < CLK_YEAR_MIN) clk.disp_year = CLK_YEAR_MIN;
    if (clk.disp_year > CLK_YEAR_MAX) clk.disp_year = CLK_YEAR_MAX;
    if (clk.disp_month < 1) clk.disp_month = 1;
    if (clk.disp_month > 12) clk.disp_month = 12;
}

/* Copy an RTC reading into the state, sanitizing out-of-range fields so a
 * broken RTC cannot produce a nonsensical calendar. */
void clk_apply_time(const struct sys_time *t) {
    int y = t->year, mo = t->month, d = t->day;
    int h, mi, s, dim;
    if (y < CLK_YEAR_MIN) y = CLK_YEAR_MIN;
    if (y > CLK_YEAR_MAX) y = CLK_YEAR_MAX;
    if (mo < 1) mo = 1;
    if (mo > 12) mo = 12;
    dim = clk_days_in_month(y, mo);
    if (d < 1) d = 1;
    if (d > dim) d = dim;
    h = t->hour % 24;   if (h < 0) h += 24;
    mi = t->minute % 60; if (mi < 0) mi += 60;
    s = t->second % 60; if (s < 0) s += 60;

    clk.epoch = t->epoch;
    clk.year = y;
    clk.month = mo;
    clk.day = d;
    clk.hour = h;
    clk.minute = mi;
    clk.second = s;
    if (t->weekday >= 0 && t->weekday <= 6) clk.weekday = t->weekday;
    else clk.weekday = clk_day_of_week(y, mo, d);
    clk.have_rtc = 1;
}

/* Re-read the RTC (or, on failure, the uptime fallback). */
void clk_refresh(void) {
    struct sys_time t;
    if (sysinfo(6, &t, (int)sizeof(t)) >= 0) {
        clk_apply_time(&t);
    } else {
        int up = sysinfo(1, 0, 0);
        clk.have_rtc = 0;
        clk.uptime_ms = (up > 0) ? (unsigned long long)up : 0ULL;
        clk.epoch = 0;
        clk.year = 0; clk.month = 0; clk.day = 0; clk.weekday = 0;
        clk.hour = 0; clk.minute = 0; clk.second = 0;
    }
}

/* Startup: read the clock and point the calendar at the RTC month. */
void clk_init(void) {
    clk_refresh();
    if (clk.have_rtc) {
        clk.disp_year = clk.year;
        clk.disp_month = clk.month;
    } else {
        clk.disp_year = CLK_YEAR_MIN;
        clk.disp_month = 1;
    }
    clk_clamp_disp();
}

/* Navigate months (wraps the year, clamped to 1970..2099).
 * Returns 1 if the displayed month changed, 0 if it was clamped. */
int clk_month_delta(int delta) {
    int y, m;
    clk_clamp_disp();
    y = clk.disp_year;
    m = clk.disp_month;
    if (delta > 0) {
        if (m == 12) {
            if (y >= CLK_YEAR_MAX) return 0;
            y++;
            m = 1;
        } else {
            m++;
        }
    } else if (delta < 0) {
        if (m == 1) {
            if (y <= CLK_YEAR_MIN) return 0;
            y--;
            m = 12;
        } else {
            m--;
        }
    } else {
        return 0;
    }
    clk.disp_year = y;
    clk.disp_month = m;
    return 1;
}

/* Navigate years, clamped to 1970..2099. Returns 1 if it changed. */
int clk_year_delta(int delta) {
    int y;
    clk_clamp_disp();
    y = clk.disp_year + delta;
    if (y < CLK_YEAR_MIN) y = CLK_YEAR_MIN;
    if (y > CLK_YEAR_MAX) y = CLK_YEAR_MAX;
    if (y == clk.disp_year) return 0;
    clk.disp_year = y;
    return 1;
}

/* Point the calendar at the RTC month (no refresh). Returns 1 if there is
 * an RTC to jump to, 0 if the displayed month was left alone. */
int clk_jump_to_rtc_month(void) {
    if (!clk.have_rtc) return 0;
    clk.disp_year = clk.year;
    clk.disp_month = clk.month;
    clk_clamp_disp();
    return 1;
}

/* Jump the calendar to today's month. Returns 1 (a redraw is worthwhile). */
int clk_jump_today(void) {
    clk_refresh();
    return clk_jump_to_rtc_month();
}

/* Day to highlight in the displayed month, or 0 for none. */
static int clk_today_day(void) {
    if (!clk.have_rtc) return 0;
    if (clk.disp_year != clk.year || clk.disp_month != clk.month) return 0;
    return clk.day;
}

/* Handle one input event. Returns 1 when the screen must be redrawn. */
int clk_handle_event(const struct gui_event *ev) {
    switch (ev->type) {
        case GUI_EV_LEFT:
            return clk_month_delta(-1);
        case GUI_EV_RIGHT:
            return clk_month_delta(1);
        case GUI_EV_UP:
            return clk_year_delta(-1);
        case GUI_EV_DOWN:
            return clk_year_delta(1);
        case GUI_EV_CHAR:
            if (ev->ch == 't' || ev->ch == 'T') return clk_jump_today();
            if (ev->ch == 'r' || ev->ch == 'R') { clk_refresh(); return 1; }
            return 0;
        case GUI_EV_MENU:
            if (ev->menu == 0 && ev->item == 0) return clk_jump_today();
            if (ev->menu == 0 && ev->item == 1) { clk_refresh(); return 1; }
            return 0;
        default:
            return 0;
    }
}

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

struct clk_out {
    char *buf;
    int cap;        /* capacity including the NUL */
    int len;
    int truncated;  /* 1 if any write did not fit */
};

static void clk_out_init(struct clk_out *o, char *buf, int cap) {
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    o->truncated = 0;
    if (cap > 0) buf[0] = '\0';
}

static void clk_out_putc(struct clk_out *o, char c) {
    if (o->len + 1 >= o->cap) {
        o->truncated = 1;
        return;
    }
    o->buf[o->len++] = c;
    o->buf[o->len] = '\0';
}

static void clk_out_puts(struct clk_out *o, const char *s) {
    for (int i = 0; s[i]; i++) clk_out_putc(o, s[i]);
}

/* Bordered month calendar. */
static void clk_render_calendar(struct clk_out *o, int year, int month, int today) {
    static const char *const hdr = " Mo Tu We Th Fr Sa Su";
    char title[32];
    char row[80];
    int grid[CLK_GRID_CELLS];
    int weeks, w, j;

    clk_fmt_month_year(year, month, title);
    gui_box_row(row, CLK_CAL_W, title);
    clk_out_puts(o, row);
    clk_out_putc(o, '\n');
    gui_rule(row, CLK_CAL_W);
    clk_out_puts(o, row);
    clk_out_putc(o, '\n');

    /* Weekday header, padded to the box inner width. */
    j = 0;
    row[j++] = '|';
    for (int i = 0; hdr[i]; i++) row[j++] = hdr[i];
    while (j < CLK_CAL_W - 1) row[j++] = ' ';
    row[j++] = '|';
    row[j] = '\0';
    clk_out_puts(o, row);
    clk_out_putc(o, '\n');

    weeks = clk_build_month_grid(year, month, grid);
    for (w = 0; w < weeks; w++) {
        char cells[32];
        clk_cal_cells(cells, &grid[w * 7], today);
        j = 0;
        row[j++] = '|';
        row[j++] = ' ';
        for (int i = 0; cells[i] && j < CLK_CAL_W - 1; i++) row[j++] = cells[i];
        while (j < CLK_CAL_W - 1) row[j++] = ' ';
        row[j++] = '|';
        row[j] = '\0';
        clk_out_puts(o, row);
        clk_out_putc(o, '\n');
    }

    gui_rule(row, CLK_CAL_W);
    clk_out_puts(o, row);
    clk_out_putc(o, '\n');
}

/* Render the complete screen into `out` (NUL-terminated, never overflows).
 * Returns the number of characters written. */
int clk_render(char *out, int cap) {
    struct clk_out o;
    char line[64];

    clk_out_init(&o, out, cap);
    clk_clamp_disp();

    clk_out_puts(&o, "=== Clock ===\n");

    if (clk.have_rtc) {
        for (int r = 0; r < 5; r++) {
            clk_art_time_row(clk.hour, clk.minute, r, line);
            clk_out_puts(&o, line);
            clk_out_putc(&o, '\n');
        }
        clk_fmt_hms(clk.hour, clk.minute, clk.second, line);
        clk_out_puts(&o, line);
        clk_out_putc(&o, '\n');
        clk_fmt_date_line(clk.weekday, clk.year, clk.month, clk.day, line);
        clk_out_puts(&o, line);
        clk_out_putc(&o, '\n');
        clk_out_putc(&o, '\n');

        clk_render_calendar(&o, clk.disp_year, clk.disp_month, clk_today_day());
        clk_out_puts(&o, "< > month  ^ v year  t=today\n");
    } else {
        int h, m, s;
        clk_split_uptime(clk.uptime_ms, &h, &m, &s);
        for (int r = 0; r < 5; r++) {
            clk_art_time_row(h % 100, m, r, line);
            clk_out_puts(&o, line);
            clk_out_putc(&o, '\n');
        }
        clk_fmt_uptime(clk.uptime_ms, line);
        clk_out_puts(&o, line);
        clk_out_putc(&o, '\n');
        clk_out_puts(&o, "RTC unavailable - uptime ");
        clk_out_puts(&o, line);
        clk_out_putc(&o, '\n');
    }

    return o.len;
}

/* Clear the window and print the whole screen (desktop redraw strategy). */
void clk_redraw(void) {
    static char screen[CLK_SCREEN_MAX];
    clk_render(screen, (int)sizeof(screen));
    gui_clear();
    print(screen);
}

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
    gui_set_title(CLK_TITLE);
    gui_add_menu(0, CLK_MENU_NAME, CLK_MENU_ITEMS);
    print_console(CLK_MARKER);

    clk_init();
    clk_redraw();

    for (;;) {
        struct gui_event ev;
        if (gui_read_event_timeout(&ev, CLK_TICK_MS)) {
            if (clk_handle_event(&ev)) clk_redraw();
        } else {
            /* Tick: re-read the clock and re-render. */
            clk_refresh();
            clk_redraw();
        }
    }
}
