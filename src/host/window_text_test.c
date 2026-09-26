/*
 * window_text_test.c - Host unit tests for the window text capture helpers
 * in src/user/graphics/window.c (wm_text_putc / wm_text_backspace /
 * wm_text_clear).
 *
 * These helpers back the desktop's window output capture.  A window is a
 * terminal tail: once the buffer is full the oldest lines slide off so the
 * newest output always lands.  The plain drop-when-full behavior they
 * replace froze a long output mid-word - reported with `grep --help` in the
 * console window, where the 4060 bytes of help text were cut off at
 * "  -I                        equi" once the 2048-byte buffer filled.
 *
 * window.c is included into this single translation unit (same pattern as
 * window_damage_test.c), so the assertions drive the real capture code.
 */

#include <stdio.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"
#include "../user_include/graphics/window.h"

#include "../user/graphics/graphics.c"
#include "../user/graphics/window.c"

static int tests_run = 0;
static int tests_failed = 0;
static int checks_run = 0;

#define CHECK(cond, msg) do { \
    checks_run++; \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
  } while (0)

#define RUN(fn) do { tests_run++; printf("  Test: %s ...\n", #fn); fn(); } while (0)

static struct window *fresh_window(void) {
  wm_init();
  wm_create_window(0, -1, -1, -1);
  return &windows[0];
}

static void append_bytes(struct window *w, const char *s, int n) {
  for (int i = 0; i < n; i++) wm_text_putc(w, s[i]);
}

/* ====================================================================== */

static void test_below_cap_appends_verbatim(void) {
  struct window *w = fresh_window();
  const char *s = "alpha\nbravo\ncharlie";
  append_bytes(w, s, (int)strlen(s));

  CHECK(strcmp(w->text, s) == 0, "below the cap the text is intact verbatim");
  CHECK(w->text_len == (int)strlen(s), "below the cap the length is exact");
}

static void test_backspace_and_clear(void) {
  struct window *w = fresh_window();

  append_bytes(w, "abc", 3);
  wm_text_backspace(w);
  CHECK(strcmp(w->text, "ab") == 0 && w->text_len == 2,
        "backspace removes exactly the last byte");

  wm_text_backspace(w);
  wm_text_backspace(w);
  CHECK(w->text_len == 0 && w->text[0] == '\0', "backspace down to empty");

  wm_text_backspace(w);
  CHECK(w->text_len == 0, "backspace on empty text is a no-op");

  append_bytes(w, "xy", 2);
  wm_text_clear(w);
  CHECK(w->text_len == 0 && w->text[0] == '\0', "clear empties the buffer");
}

/* 400 lines of 64 bytes: 25,600 bytes > 8191, so the buffer must slide. */
static char stream[400 * 64];

static int build_stream(char *dst, int lines) {
  int n = 0;
  for (int i = 0; i < lines; i++) {
    n += sprintf(dst + n, "L%03d ", i);
    while (n % 64 != 63) dst[n++] = '.';
    dst[n++] = '\n';
  }
  dst[n] = '\0';
  return n;
}

static void test_overflow_slides_a_clean_suffix(void) {
  struct window *w = fresh_window();
  int total = build_stream(stream, 400);

  int overflowed = 0;
  for (int i = 0; i < total; i++) {
    wm_text_putc(w, stream[i]);
    if (w->text_len > MAX_TEXT - 1) {
      overflowed = 1;
      break;
    }
  }
  CHECK(!overflowed, "the buffer never exceeds MAX_TEXT - 1");

  int off = total - w->text_len;
  CHECK(off > 0, "the stream exceeded the buffer, so bytes were dropped");
  CHECK(memcmp(w->text, stream + off, w->text_len + 1) == 0,
        "the captured text is a clean suffix of the stream");

  CHECK(off == 0 || stream[off - 1] == '\n',
        "the oldest surviving byte starts a line");
  CHECK(strstr(w->text, "L399 ") != NULL, "the newest line landed");
  CHECK(strstr(w->text, "L000 ") == NULL, "the oldest line slid off");
  CHECK(w->text_len > MAX_TEXT / 4, "a healthy scrollback survives");
}

/* A single line longer than the whole buffer: no line boundary exists, so
 * the slide falls back to a byte cut and never overflows. */
static void test_giant_single_line(void) {
  struct window *w = fresh_window();
  static char big[20000];
  memset(big, 'x', sizeof big);

  append_bytes(w, big, sizeof big);
  CHECK(w->text_len <= MAX_TEXT - 1 && w->text_len > MAX_TEXT / 2,
        "giant line: length inside the buffer");
  CHECK(w->text[w->text_len] == '\0', "giant line: terminated");
  CHECK(memcmp(w->text, big + sizeof big - w->text_len, w->text_len) == 0,
        "giant line: newest bytes are a clean suffix");
}

/* The reported regression: a grep-help-sized command output (prompt + the
 * ~4060-byte help) must be captured whole, not cut off mid-line. */
static void test_grep_help_sized_output_fits(void) {
  struct window *w = fresh_window();
  static char g[4200];
  int n = 0;
  n += sprintf(g + n, "user@hobbyos:/$ grep --help\n");
  for (int i = 0; i < 58; i++) {   /* 58 x 70 bytes = 4060 */
    n += sprintf(g + n, "  --option-%02d ", i);
    while (n % 70 != 69) g[n++] = ' ';
    g[n++] = '\n';
  }
  g[n] = '\0';
  CHECK(n > 4000 && n < MAX_TEXT - 1, "fixture is grep-help sized");

  append_bytes(w, g, n);
  CHECK(w->text_len == n, "no byte of the help-sized output was dropped");
  CHECK(strcmp(w->text, g) == 0, "the whole output is captured verbatim");
}

/* ====================================================================== */

int main(void) {
  RUN(test_below_cap_appends_verbatim);
  RUN(test_backspace_and_clear);
  RUN(test_overflow_slides_a_clean_suffix);
  RUN(test_giant_single_line);
  RUN(test_grep_help_sized_output_fits);

  printf("=== Results: %d run, %d failed ===\n", tests_run, tests_failed);
  printf("=== Checks: %d individual capture checks ===\n", checks_run);
  if (tests_failed) {
    printf("FAILED\n");
    return 1;
  }
  printf("ALL WINDOW TEXT TESTS PASSED\n");
  return 0;
}
