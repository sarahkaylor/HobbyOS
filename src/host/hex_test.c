/*
 * hex_test.c - Host unit tests for the HobbyOS Hex Viewer (hex.c).
 *
 * Includes the app source directly (same pattern as src/host/pong_test.c)
 * so the tests drive the internal helpers and state without a desktop.
 * Covers: hex digits + ASCII filtering, row formatting (full rows, short
 * rows, empty rows, rows past the end, tiny output buffers), row counting,
 * scroll/page clamping, offset parsing, goto clamping, the offset
 * indicator, whole-screen rendering (closed, empty, 16-byte, 20-byte,
 * truncated, small buffer) and event handling.  File loading is exercised
 * against real files under /tmp.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main hex_app_main
#include "../user/hex.c"
#undef main

/* ---- check framework ---- */

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *msg) {
    checks_run++;
    if (ok) {
        printf("PASS: %s\n", msg);
    } else {
        printf("FAIL: %s\n", msg);
        checks_failed++;
    }
}

/* ---- helpers ---- */

static int count_lines(const char *s) {
    int n = 0;
    for (int i = 0; s[i]; i++) if (s[i] == '\n') n++;
    return n;
}

static int max_line_len(const char *s) {
    int best = 0, cur = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') {
            if (cur > best) best = cur;
            cur = 0;
        } else {
            cur++;
        }
    }
    if (cur > best) best = cur;
    return best;
}

/* Fill `dst` with `n` spaces (used to build expected padded rows).
 * Caller-provided buffers so one call cannot clobber another's result. */
static void spaces_into(char *dst, int n) {
    memset(dst, ' ', (size_t)n);
    dst[n] = '\0';
}

/* Put `n` bytes into the viewer state without touching the filesystem. */
static void set_buffer(const unsigned char *bytes, int n, const char *name) {
    hex_close_file();
    for (int i = 0; i < n; i++) hex_data[i] = bytes[i];
    hex_len = n;
    hex_truncated = 0;
    hex_loaded = 1;
    hex_top_row = 0;
    snprintf(hex_fname, sizeof(hex_fname), "%s", name);
}

static void write_file(const char *path, const void *data, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        check(0, "could not create test file");
        return;
    }
    if (n > 0) fwrite(data, 1, (size_t)n, f);
    fclose(f);
}

/* A 20-byte buffer: 16 bytes of "Hello World!\nBCD" + "EFGH".
 * Row 0 is full, row 1 holds 4 bytes. */
static const unsigned char T20[20] = {
    'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '!', 0x0A,
    'B', 'C', 'D', 'E', 'F', 'G', 'H'
};

#define ROW0_TEXT \
    "00000000  48 65 6C 6C 6F 20 57 6F 72 6C 64 21 0A 42 43 44" \
    "  |Hello World!.BCD|"

static const char *OFF_TAIL = ")  ^v=scroll  <> =page  g=goto  c=close";

/* ---- hex digit / ASCII filter ---- */

static void test_hex_digit(void) {
    check(hex_digit(0) == '0', "hex_digit(0) == '0'");
    check(hex_digit(9) == '9', "hex_digit(9) == '9'");
    check(hex_digit(10) == 'A', "hex_digit(10) == 'A'");
    check(hex_digit(15) == 'F', "hex_digit(15) == 'F'");
    check(hex_digit(-1) == '?' && hex_digit(16) == '?',
          "hex_digit out of range == '?'");
}

static void test_hex_ascii(void) {
    check(hex_ascii(32) == ' ', "ASCII 0x20 (space) kept");
    check(hex_ascii(33) == '!', "ASCII 0x21 kept");
    check(hex_ascii(126) == '~', "ASCII 0x7E ('~') kept");
    check(hex_ascii(127) == '.', "ASCII 0x7F (DEL) -> '.'");
    check(hex_ascii(31) == '.', "ASCII 0x1F -> '.'");
    check(hex_ascii(0) == '.', "ASCII 0x00 -> '.'");
    check(hex_ascii(128) == '.', "ASCII 0x80 -> '.'");
    check(hex_ascii(255) == '.', "ASCII 0xFF -> '.'");
}

static void test_total_rows(void) {
    check(hex_total_rows(0) == 0, "0 bytes -> 0 rows");
    check(hex_total_rows(1) == 1, "1 byte -> 1 row");
    check(hex_total_rows(15) == 1, "15 bytes -> 1 row");
    check(hex_total_rows(16) == 1, "16 bytes -> 1 row (no partial row)");
    check(hex_total_rows(17) == 2, "17 bytes -> 2 rows");
    check(hex_total_rows(20) == 2, "20 bytes -> 2 rows");
    check(hex_total_rows(256) == 16, "256 bytes -> 16 rows");
    check(hex_total_rows(HEX_MAX_BYTES) == 2048, "32768 bytes -> 2048 rows");
}

/* ---- row formatting ---- */

static void test_row_formatting(void) {
    char row[HEX_ROW_LINE];
    char expect[128];
    int n;
    int blanks;

    set_buffer(T20, 20, "HEXBUF.BIN");

    /* Row 0: all 16 bytes present. */
    n = hex_format_row(row, sizeof(row), hex_data, hex_len, 0);
    check(n == 77 && strlen(row) == 77, "row 0 is 77 chars and NUL-terminated");
    check(row[59] == '|' && row[76] == '|',
          "row 0 has '|' at columns 59 and 76");
    check(strcmp(row, ROW0_TEXT) == 0,
          "row 0 exact text (uppercase hex, 0x0A as '.')");
    check(strncmp(row + 10, "48 65 6C", 8) == 0, "row 0 hex bytes start at col 10");
    check(strncmp(row + 60, "Hello World!.BCD", 16) == 0,
          "row 0 ASCII column is 16 printable chars");

    /* Row 1: only 4 bytes, 12 padded slots. */
    n = hex_format_row(row, sizeof(row), hex_data, hex_len, 16);
    check(n == 77 && strlen(row) == 77, "short row is still 77 chars");
    {
        char pad1[64], pad2[64];
        spaces_into(pad1, 37);   /* slot separator + 12 missing slots x 3 */
        spaces_into(pad2, 12);   /* missing ASCII cells                   */
        snprintf(expect, sizeof(expect), "00000010  45 46 47 48%s |EFGH%s|",
                 pad1, pad2);
    }
    check(strcmp(row, expect) == 0,
          "short row exact text (4 bytes + 12 slots of 3 spaces + ASCII gaps)");
    check(strncmp(row + 10, "45 46 47 48", 11) == 0, "short row hex bytes in place");
    blanks = 1;
    for (int i = 22; i <= 57; i++) if (row[i] != ' ') blanks = 0;
    check(blanks, "short row missing hex slots are spaces (cols 22..57)");
    check(strncmp(row + 60, "EFGH", 4) == 0, "short row ASCII starts with EFGH");
    blanks = 1;
    for (int i = 64; i <= 75; i++) if (row[i] != ' ') blanks = 0;
    check(blanks, "short row missing ASCII slots are spaces (cols 64..75)");
    check(row[59] == '|' && row[76] == '|', "short row bars keep the ASCII column aligned");

    /* A row entirely past the end of the data. */
    n = hex_format_row(row, sizeof(row), hex_data, hex_len, 32);
    check(n == 77 && row[59] == '|' && row[76] == '|',
          "row past end of data is still a full 77-char frame");
    blanks = 1;
    for (int i = 10; i <= 58; i++) if (row[i] != ' ') blanks = 0;
    for (int i = 60; i <= 75; i++) if (row[i] != ' ') blanks = 0;
    check(blanks, "row past end of data is blank hex + blank ASCII");

    /* Output buffer smaller than one row: truncated, never overrun. */
    memset(row, 'X', sizeof(row));
    n = hex_format_row(row, 20, hex_data, hex_len, 0);
    check(n == 19 && row[19] == '\0' && strncmp(row, ROW0_TEXT, 19) == 0,
          "small row buffer holds the first 19 chars and is terminated");
}

static void test_ascii_boundaries_in_row(void) {
    static const unsigned char bound[4] = { 0x20, 0x7E, 0x7F, 0x00 };
    char row[HEX_ROW_LINE];
    set_buffer(bound, 4, "BOUND.BIN");
    hex_format_row(row, sizeof(row), hex_data, hex_len, 0);
    check(strncmp(row + 10, "20 7E 7F 00", 11) == 0, "boundary bytes hex-rendered");
    check(strncmp(row + 60, " ~..", 4) == 0,
          "boundary bytes ASCII: 0x20 space, 0x7E '~', 0x7F '.', 0x00 '.'");
    check(row[64] == ' ' && row[75] == ' ', "ASCII column padded to 16 cells");
}

static void test_empty_buffer_row(void) {
    char row[HEX_ROW_LINE];
    int n;
    int blanks;

    check(hex_total_rows(0) == 0, "empty buffer has 0 rows");
    n = hex_format_row(row, sizeof(row), hex_data, 0, 0);
    check(n == 77, "empty buffer row is still 77 chars");
    check(strncmp(row, "00000000  ", 10) == 0, "empty buffer row keeps the offset digits");
    blanks = 1;
    for (int i = 10; i <= 58; i++) if (row[i] != ' ') blanks = 0;
    for (int i = 60; i <= 75; i++) if (row[i] != ' ') blanks = 0;
    check(blanks, "empty buffer row is all blanks between the bars");
    check(row[59] == '|' && row[76] == '|', "empty buffer row keeps both bars");
}

/* ---- whole-screen rendering ---- */

static void test_render_closed(void) {
    char out[HEX_SCREEN_MAX];
    int n;

    hex_close_file();
    n = hex_render(out, sizeof(out));
    check(n == (int)strlen(out), "render returns the length it wrote");
    check(strncmp(out, "=== Hex Viewer ===\n", 19) == 0, "closed: '=== Hex Viewer ===' first line");
    check(strstr(out, "No file open - File > Open (or press o)\n") != NULL,
          "closed: no-file hint line");
    check(count_lines(out) == 2, "closed: exactly 2 lines");
    check(strstr(out, "Offset:") == NULL, "closed: no offset line");
    check(strstr(out, "|") == NULL, "closed: no dump rows");
}

static void test_render_open(void) {
    char out[HEX_SCREEN_MAX];
    char out2[HEX_SCREEN_MAX];

    set_buffer(T20, 20, "HEXBUF.BIN");
    hex_render(out, sizeof(out));
    check(count_lines(out) == 21, "open: 21 lines (title + file + 18 rows + offset)");
    check(strstr(out, "File: HEXBUF.BIN  (20 bytes)\n") != NULL,
          "open: file line shows name and byte count");
    check(strstr(out, ROW0_TEXT "\n") != NULL, "open: full first row present");
    check(strstr(out, "00000010  45 46 47 48") != NULL, "open: short second row present");
    check(strstr(out, "Offset: 0x00000000 (row 1/2)") != NULL, "open: offset line present");
    check(max_line_len(out) < 110, "open: no line exceeds 110 chars");

    hex_render(out2, sizeof(out2));
    check(strcmp(out, out2) == 0, "open: rendering is deterministic");

    hex_scroll(1);
    hex_render(out2, sizeof(out2));
    check(strstr(out2, "Offset: 0x00000010 (row 2/2)") != NULL,
          "scrolled: offset line follows the top row");
    check(strstr(out2, "00000000") == NULL, "scrolled: first row is off screen");
    check(strstr(out2, ROW0_TEXT) == NULL, "scrolled: first row text gone");
    hex_scroll(-1);
}

static void test_render_16_bytes(void) {
    unsigned char b[16];
    char out[HEX_SCREEN_MAX];
    for (int i = 0; i < 16; i++) b[i] = (unsigned char)('A' + i);
    set_buffer(b, 16, "SIXTEEN.BIN");

    check(hex_total_rows(hex_len) == 1, "16-byte buffer is exactly 1 row");
    char row[HEX_ROW_LINE];
    hex_format_row(row, sizeof(row), hex_data, hex_len, 0);
    check(strcmp(row,
                 "00000000  41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 50"
                 "  |ABCDEFGHIJKLMNOP|") == 0,
          "16-byte row has no padding slots");

    hex_render(out, sizeof(out));
    check(count_lines(out) == 21, "16-byte file renders 21 lines");
    check(strstr(out, "|ABCDEFGHIJKLMNOP|\n") != NULL, "16-byte ASCII column rendered");
    check(strstr(out, "00000010") == NULL, "16-byte file has no partial second row");
    check(strstr(out, "(row 1/1)") != NULL, "16-byte file indicator is row 1/1");
}

static void test_render_one_byte(void) {
    unsigned char b = 'A';
    char row[HEX_ROW_LINE];
    int n;
    int blanks;

    set_buffer(&b, 1, "ONE.BIN");
    n = hex_format_row(row, sizeof(row), hex_data, hex_len, 0);
    check(n == 77 && strlen(row) == 77, "1-byte row padded to 77 chars");
    check(strncmp(row, "00000000  41", 12) == 0, "1-byte row: offset + '41'");
    blanks = 1;
    for (int i = 12; i <= 58; i++) if (row[i] != ' ') blanks = 0;
    check(blanks, "1-byte row: 15 missing bytes become 3 spaces each");
    check(row[59] == '|' && row[60] == 'A', "1-byte row: bar at 59, ASCII 'A' at 60");
    blanks = 1;
    for (int i = 61; i <= 75; i++) if (row[i] != ' ') blanks = 0;
    check(blanks && row[76] == '|', "1-byte row: ASCII column padded and closed");
}

static void test_render_empty_file(void) {
    char out[HEX_SCREEN_MAX];
    set_buffer(NULL, 0, "EMPTY.BIN");

    hex_render(out, sizeof(out));
    check(count_lines(out) == 21, "0-byte file still draws the 18-row frame");
    check(strstr(out, "File: EMPTY.BIN  (0 bytes)\n") != NULL, "0-byte file line");
    check(strstr(out, "(row 0/0)") != NULL, "0-byte file indicator is row 0/0");
    check(strstr(out, "Offset: 0x00000000") != NULL, "0-byte file offset is 0");

    hex_scroll(5);
    check(hex_top_row == 0, "0-byte file cannot scroll down");
    hex_scroll(-5);
    check(hex_top_row == 0, "0-byte file cannot scroll up");
    hex_goto_offset(500);
    check(hex_top_row == 0, "0-byte file goto stays at row 0");
}

static void test_render_truncated(void) {
    char out[HEX_SCREEN_MAX];
    set_buffer(NULL, 0, "TRUNC.BIN");
    hex_loaded = 1;
    hex_len = HEX_MAX_BYTES;
    hex_truncated = 1;
    hex_top_row = 0;

    check(hex_total_rows(hex_len) == 2048, "32768 bytes -> 2048 rows");
    hex_render(out, sizeof(out));
    check(strstr(out, "File: TRUNC.BIN  (truncated at 32768)\n") != NULL,
          "truncated: header says truncated at 32768");
    check(count_lines(out) == 21, "truncated: 21 lines");

    hex_scroll(100000);
    check(hex_top_row == 2047, "big file scroll clamps to the last row (2047)");
    hex_render(out, sizeof(out));
    check(strstr(out, "(row 2048/2048)") != NULL, "big file indicator row 2048/2048 (1-based)");
    check(strstr(out, "Offset: 0x00007FF0") != NULL, "big file offset 0x7FF0 (2047*16)");
    check(max_line_len(out) < 110, "big file: no line exceeds 110 chars");
    check((int)strlen(out) < 1800, "big file: screen stays under the 1800-char budget");
}

static void test_render_small_buffer(void) {
    char small[8];
    char full[HEX_SCREEN_MAX];
    int n;

    set_buffer(T20, 20, "HEXBUF.BIN");
    hex_render(full, sizeof(full));
    n = hex_render(small, sizeof(small));
    check(n == 7 && small[7] == '\0', "small render target is terminated, not overrun");
    check(strncmp(small, full, 7) == 0, "small render target holds the screen prefix");
    check(hex_render(NULL, 0) == 0, "zero-size render target is safe");
}

/* ---- scroll / goto ---- */

static unsigned char BIG[300];
static void make_big(void) {
    for (int i = 0; i < 300; i++) BIG[i] = (unsigned char)(i & 0xFF);
}

static void test_scroll_clamp(void) {
    make_big();
    set_buffer(BIG, 300, "BIG.BIN");
    check(hex_total_rows(hex_len) == 19, "300 bytes -> 19 rows");
    check(HEX_VIS_ROWS == 18 && HEX_PAGE_ROWS == 16,
          "18 visible rows, 16-row page step");
    check(hex_top_row == 0, "scroll starts at row 0");

    hex_scroll(1);
    check(hex_top_row == 1, "down one row");
    hex_scroll(1);
    check(hex_top_row == 2, "down another row");
    hex_scroll(-1);
    check(hex_top_row == 1, "up one row");
    hex_scroll(-1);
    check(hex_top_row == 0, "back to row 0");
    hex_scroll(-1);
    check(hex_top_row == 0, "up at the top is clamped");

    hex_scroll(HEX_PAGE_ROWS);
    check(hex_top_row == 16, "page down moves 16 rows");
    hex_scroll(HEX_PAGE_ROWS);
    check(hex_top_row == 18, "page down clamps to the last row (18)");
    hex_scroll(HEX_PAGE_ROWS);
    check(hex_top_row == 18, "page down at the bottom is clamped");
    hex_scroll(-HEX_PAGE_ROWS);
    check(hex_top_row == 2, "page up moves 16 rows");
    hex_scroll(-HEX_PAGE_ROWS);
    check(hex_top_row == 0, "page up at the top is clamped");
    hex_scroll(-100);
    check(hex_top_row == 0, "large up scroll clamps");
    hex_scroll(1000);
    check(hex_top_row == 18, "large down scroll clamps to the last row");
}

static void test_clamp_helpers(void) {
    check(hex_clamp_row(-1, 5) == 0 && hex_clamp_row(5, 5) == 4,
          "hex_clamp_row clamps both ends");
    check(hex_clamp_row(3, 0) == 0 && hex_clamp_row(3, -7) == 0,
          "hex_clamp_row is 0 when there are no rows");
    check(hex_clamp_offset(-1, 2) == 0, "hex_clamp_offset clamps negatives");
    check(hex_clamp_offset(9999, 2) == 16, "hex_clamp_offset clamps to the last row start");
    check(hex_clamp_offset(9999, 0) == 0, "hex_clamp_offset is 0 when empty");
    check(hex_clamp_offset(16, 2) == 16, "hex_clamp_offset keeps valid offsets");
}

static void test_offset_parse(void) {
    check(hex_parse_offset("1F") == 31, "\"1F\" -> 31");
    check(hex_parse_offset("0x1f") == 31, "\"0x1f\" -> 31");
    check(hex_parse_offset("0X1F") == 31, "\"0X1F\" -> 31");
    check(hex_parse_offset("0") == 0, "\"0\" -> 0");
    check(hex_parse_offset(" 2A ") == 42, "surrounding spaces are ignored");
    check(hex_parse_offset("FFFFFFFF") == 4294967295L, "8 hex digits accepted");
    check(hex_parse_offset("") == -1, "empty string -> -1");
    check(hex_parse_offset(NULL) == -1, "NULL -> -1");
    check(hex_parse_offset("zz") == -1, "non-hex text -> -1");
    check(hex_parse_offset("0x") == -1, "\"0x\" with no digits -> -1");
    check(hex_parse_offset("12g") == -1, "trailing junk -> -1");
    check(hex_parse_offset("123456789") == -1, "9 digits (too long) -> -1");
    check(hex_parse_offset("-1") == -1, "negative value -> -1");
}

static void test_goto_offset(void) {
    make_big();
    set_buffer(BIG, 300, "BIG.BIN");

    hex_goto_offset(0);
    check(hex_top_row == 0, "goto 0 -> row 0");
    hex_goto_offset(15);
    check(hex_top_row == 0, "goto 15 -> row 0");
    hex_goto_offset(16);
    check(hex_top_row == 1, "goto 16 -> row 1");
    hex_goto_offset(288);
    check(hex_top_row == 18, "goto 288 -> last row");
    hex_goto_offset(100000);
    check(hex_top_row == 18, "goto past the end clamps to the last row");
    hex_goto_offset(-5);
    check(hex_top_row == 0, "negative goto clamps to row 0");

    hex_close_file();
    hex_goto_offset(64);
    check(hex_top_row == 0 && !hex_loaded, "goto with no file is inert");
    hex_scroll(3);
    check(hex_top_row == 0, "scroll with no file is inert");
}

static void test_offset_indicator(void) {
    char line[HEX_ROW_LINE];
    int n = hex_format_offset_line(line, sizeof(line), 0, 2);
    check(n == 66 && strlen(line) == 66, "offset line is 66 chars");
    check(strcmp(line, "Offset: 0x00000000 (row 1/2)  ^v=scroll  <> =page  g=goto  c=close") == 0,
          "offset line row 1/2 exact");

    hex_format_offset_line(line, sizeof(line), 1, 2);
    check(strcmp(line, "Offset: 0x00000010 (row 2/2)  ^v=scroll  <> =page  g=goto  c=close") == 0,
          "offset line row 2/2 exact");
    hex_format_offset_line(line, sizeof(line), 18, 19);
    check(strcmp(line, "Offset: 0x00000120 (row 19/19)  ^v=scroll  <> =page  g=goto  c=close") == 0,
          "offset line row 19/19 exact (0x120 = 288)");
    hex_format_offset_line(line, sizeof(line), 0, 0);
    check(strcmp(line, "Offset: 0x00000000 (row 0/0)  ^v=scroll  <> =page  g=goto  c=close") == 0,
          "offset line for an empty buffer is row 0/0");
    hex_format_offset_line(line, sizeof(line), 99, 3);
    check(strcmp(line, "Offset: 0x00000020 (row 3/3)  ^v=scroll  <> =page  g=goto  c=close") == 0,
          "out-of-range top row is clamped in the indicator");
    hex_format_offset_line(line, sizeof(line), 0, 2);
    check(strstr(line, OFF_TAIL) != NULL, "offset line keeps the key help text");
}

/* ---- event handling ---- */

static void test_events(void) {
    struct gui_event ev;
    make_big();

    set_buffer(BIG, 300, "BIG.BIN");

    memset(&ev, 0, sizeof(ev));
    ev.type = GUI_EV_CHAR;
    ev.ch = 'q';
    check(hex_handle_event(&ev) == HEX_ACT_QUIT, "'q' asks the app to exit");
    ev.ch = 'x';
    check(hex_handle_event(&ev) == HEX_ACT_NONE, "unknown key ignored");

    ev.type = GUI_EV_DOWN;
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && hex_top_row == 1,
          "down arrow scrolls one row");
    ev.type = GUI_EV_UP;
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && hex_top_row == 0,
          "up arrow scrolls back");
    ev.type = GUI_EV_RIGHT;
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && hex_top_row == 16,
          "right arrow pages down 16 rows");
    ev.type = GUI_EV_LEFT;
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && hex_top_row == 0,
          "left arrow pages up to row 0");

    ev.type = GUI_EV_ESC;
    check(hex_handle_event(&ev) == HEX_ACT_NONE, "ESC ignored");
    ev.type = GUI_EV_MOUSE;
    ev.x = 3; ev.y = 4; ev.button = 1;
    check(hex_handle_event(&ev) == HEX_ACT_NONE, "mouse click ignored");
    ev.type = GUI_EV_MENU;
    ev.menu = 1; ev.item = 0;
    check(hex_handle_event(&ev) == HEX_ACT_NONE, "selection from an unknown menu ignored");
    ev.menu = 0; ev.item = 5;
    check(hex_handle_event(&ev) == HEX_ACT_NONE, "unknown File menu item ignored");

    ev.menu = 0; ev.item = 1;
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && !hex_loaded &&
          hex_len == 0 && hex_fname[0] == '\0',
          "File > Close drops the file");
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && !hex_loaded,
          "File > Close on an empty viewer is harmless");

    set_buffer(BIG, 300, "BIG.BIN");
    ev.type = GUI_EV_CHAR;
    ev.ch = 'c';
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && !hex_loaded && hex_top_row == 0,
          "'c' closes the file");
    ev.ch = 'C';
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && !hex_loaded, "'C' closes the file too");

    /* 'g' with nothing loaded must return immediately (no dialog) */
    ev.ch = 'g';
    check(hex_handle_event(&ev) == HEX_ACT_REDRAW && !hex_loaded,
          "'g' with no file open is inert (never opens a dialog)");

    ev.ch = 'q';
    check(hex_handle_event(&ev) == HEX_ACT_QUIT, "'q' quits even with no file open");
}

/* ---- real file loading ---- */

#define TMPFILE "/tmp/hex_test_data.bin"

static void test_file_loading(void) {
    unsigned char data[64];
    char out[HEX_SCREEN_MAX];

    for (int i = 0; i < 64; i++) data[i] = (unsigned char)('A' + (i % 26));

    write_file(TMPFILE, data, 20);
    hex_close_file();
    check(hex_load(TMPFILE) == 1, "hex_load opens a real file");
    check(hex_loaded == 1 && hex_len == 20 && hex_truncated == 0,
          "hex_load records 20 bytes, not truncated");
    check(strcmp(hex_fname, TMPFILE) == 0, "hex_load records the file name");
    check(hex_top_row == 0, "hex_load resets the scroll position");
    hex_render(out, sizeof(out));
    check(strstr(out, "(20 bytes)") != NULL, "render shows the loaded byte count");
    check(strncmp(out + 19 + strlen("File: " TMPFILE), "  (20 bytes)\n", 13) == 0,
          "render file line ends with the byte count");

    check(hex_load("/no/such/dir/hex_missing.bin") == 0, "hex_load fails on a missing path");
    check(hex_loaded == 1 && hex_len == 20, "failed open keeps the previous file loaded");
    check(hex_load("") == 0, "hex_load(\"\") fails");
    check(hex_load(NULL) == 0, "hex_load(NULL) fails");

    /* Exactly the cap: not truncated. */
    static unsigned char cap[HEX_MAX_BYTES];
    for (int i = 0; i < HEX_MAX_BYTES; i++) cap[i] = (unsigned char)(i * 7 + 3);
    write_file(TMPFILE, cap, HEX_MAX_BYTES);
    check(hex_load(TMPFILE) == 1 && hex_len == HEX_MAX_BYTES && hex_truncated == 0,
          "exactly 32768 bytes is not truncated");

    /* One byte more: truncated at the cap. */
    FILE *f = fopen(TMPFILE, "ab");
    if (f) { fputc('X', f); fclose(f); }
    check(hex_load(TMPFILE) == 1 && hex_len == HEX_MAX_BYTES && hex_truncated == 1,
          "32769 bytes is truncated at 32768");
    hex_render(out, sizeof(out));
    check(strstr(out, "  (truncated at 32768)\n") != NULL, "render shows the truncation");
    check(strstr(out, "File: " TMPFILE) != NULL, "render shows the full file name");

    /* Empty file. */
    write_file(TMPFILE, NULL, 0);
    check(hex_load(TMPFILE) == 1 && hex_len == 0 && hex_truncated == 0,
          "0-byte file loads with 0 bytes");
    hex_render(out, sizeof(out));
    check(strstr(out, "(0 bytes)") != NULL, "render shows 0 bytes");
    check(strstr(out, "(row 0/0)") != NULL, "0-byte file indicator is row 0/0");

    remove(TMPFILE);
}

/* ---- the real entry point, driven in a child process ---- */

/* Run hex_app_main() in a forked child with pipes as its stdin/stdout, send
 * it 'q' and check what it wrote: the startup marker, the window title, the
 * menu registration and the initial screen. This exercises the parts the
 * desktop normally provides (menu/title protocol, event loop, exit path)
 * without hanging the test on the desktop's blocking event loop. */
static void test_entry_point_child(void) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        check(0, "entry point: pipe creation failed");
        return;
    }

    fflush(NULL);
    hex_close_file();   /* the child inherits state: start it with no file open */
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "entry point: fork failed");
        return;
    }
    if (pid == 0) {
        /* Child: the app's own entry point with the pipes as stdin/stdout. */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        hex_app_main();
        _exit(0);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write(in_pipe[1], "q", 1) != 1) check(0, "entry point: could not send 'q'");
    close(in_pipe[1]);

    static char captured[4096];
    int total = 0;
    for (;;) {
        fd_set set;
        struct timeval tv;
        FD_ZERO(&set);
        FD_SET(out_pipe[0], &set);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        if (select(out_pipe[0] + 1, &set, NULL, NULL, &tv) <= 0) break;
        int r = (int)read(out_pipe[0], captured + total,
                          (int)sizeof(captured) - 1 - total);
        if (r <= 0) break;
        total += r;
        if (total >= (int)sizeof(captured) - 1) break;
    }
    captured[total] = '\0';
    close(out_pipe[0]);

    int status = 0;
    kill(pid, SIGKILL);   /* no-op if the child already exited */
    waitpid(pid, &status, 0);

    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "entry point: 'q' makes the app exit(0)");
    check(strstr(captured, "[APP] HEX started\n") != NULL,
          "entry point: prints the '[APP] HEX started' startup marker");
    check(strstr(captured, "\033]THex Viewer~") != NULL,
          "entry point: sets the 'Hex Viewer' window title");
    check(strstr(captured, "M0;File;Open,Close") != NULL,
          "entry point: registers the File > Open,Close menu");
    check(strstr(captured, "=== Hex Viewer ===\n") != NULL,
          "entry point: draws the initial screen");
    check(strstr(captured, "No file open - File > Open (or press o)") != NULL,
          "entry point: initial screen shows the no-file hint");
}

/* ---- runner ---- */

int main(void) {
    printf("=== Hex Viewer Host Tests ===\n\n");

    test_hex_digit();
    test_hex_ascii();
    test_total_rows();
    test_row_formatting();
    test_ascii_boundaries_in_row();
    test_empty_buffer_row();
    test_render_closed();
    test_render_open();
    test_render_16_bytes();
    test_render_one_byte();
    test_render_empty_file();
    test_render_truncated();
    test_render_small_buffer();
    test_scroll_clamp();
    test_clamp_helpers();
    test_offset_parse();
    test_goto_offset();
    test_offset_indicator();
    test_events();
    test_file_loading();
    test_entry_point_child();

    printf("\n=== Results ===\n");
    printf("Checks run:    %d\n", checks_run);
    printf("Checks failed: %d\n", checks_failed);
    if (checks_failed == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("SOME TESTS FAILED\n");
    return 1;
}
