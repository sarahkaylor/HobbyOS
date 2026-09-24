/*
 * calc_test.c - Host unit tests for calc.c (HobbyOS Calculator).
 *
 * Compiled with -DHOST_TEST; calc.c is included directly (below) so the
 * tests can drive internal functions and state without the desktop event
 * loop.  Every check prints PASS/FAIL; the runner exits non-zero if any
 * check fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main calc_app_main
#include "../user/calc.c"
#undef main

/* ================================================================== */
/* Test framework                                                      */
/* ================================================================== */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
        g_checks++;                                                    \
        if (cond) {                                                    \
            printf("PASS: %s\n", (msg));                               \
        } else {                                                       \
            g_failures++;                                              \
            printf("FAIL: %s  [line %d]\n", (msg), __LINE__);          \
        }                                                              \
    } while (0)

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

/* Reset to a pristine state (empty expression, no result, memory 0). */
static void reset_state(void) {
  (void)calc_press('c');
  calc_memory = 0;
}

/* Type a string of keys. */
static void type_str(const char *s) {
  int i;
  for (i = 0; s[i] != '\0'; i++) (void)calc_press(s[i]);
}

/* Render the current state and return the screen text. */
static const char *screen_text(void) {
  static char buf[600];
  calc_render(buf, (int)sizeof(buf));
  return buf;
}

/* Copy rendered line `idx` (0-based) into out; returns 0 if missing. */
static int screen_line(const char *screen, int idx, char *out, int cap) {
  const char *p = screen;
  int i, j = 0;
  for (i = 0; i < idx; i++) {
    while (*p != '\0' && *p != '\n') p++;
    if (*p == '\n') p++;
    else return 0;
  }
  while (*p != '\0' && *p != '\n' && j < cap - 1) out[j++] = *p++;
  out[j] = '\0';
  return 1;
}

static int all_spaces(const char *s) {
  int i;
  for (i = 0; s[i] != '\0'; i++) if (s[i] != ' ') return 0;
  return 1;
}

/* True when `line` is exactly CALC_DISPLAY_W chars and ends with `suffix`,
 * space-padded on the left. */
static int right_aligned_ends(const char *line, const char *suffix) {
  int sl = (int)strlen(line);
  int fl = (int)strlen(suffix);
  int i;
  if (sl != CALC_DISPLAY_W || fl > sl) return 0;
  if (strcmp(line + sl - fl, suffix) != 0) return 0;
  for (i = 0; i < sl - fl; i++) if (line[i] != ' ') return 0;
  return 1;
}

/* Expected keypad grid, row-major. */
static const char KEYS_EXPECTED[CALC_KEYPAD_ROWS][CALC_KEYPAD_COLS] = {
  { '7', '8', '9', '+' },
  { '4', '5', '6', '-' },
  { '1', '2', '3', '*' },
  { '0', 'C', '=', '/' },
};

/* ================================================================== */
/* 1. Keypad geometry: key_at grid + out-of-range                      */
/* ================================================================== */

static void test_key_at_grid(void) {
  int r, c;
  char msg[96];
  printf("--- key_at grid ---\n");
  for (r = 0; r < CALC_KEYPAD_ROWS; r++) {
    for (c = 0; c < CALC_KEYPAD_COLS; c++) {
      snprintf(msg, sizeof msg, "key_at(%d,%d) == '%c'", r, c,
               KEYS_EXPECTED[r][c]);
      CHECK(key_at(r, c) == KEYS_EXPECTED[r][c], msg);
    }
  }
}

static void test_key_at_out_of_range(void) {
  char msg[96];
  printf("--- key_at out-of-range ---\n");
  snprintf(msg, sizeof msg, "key_at(-1,0) == 0");   CHECK(key_at(-1, 0) == 0, msg);
  snprintf(msg, sizeof msg, "key_at(0,-1) == 0");   CHECK(key_at(0, -1) == 0, msg);
  snprintf(msg, sizeof msg, "key_at(-1,-1) == 0");  CHECK(key_at(-1, -1) == 0, msg);
  snprintf(msg, sizeof msg, "key_at(4,0) == 0");    CHECK(key_at(4, 0) == 0, msg);
  snprintf(msg, sizeof msg, "key_at(0,4) == 0");    CHECK(key_at(0, 4) == 0, msg);
  snprintf(msg, sizeof msg, "key_at(4,4) == 0");    CHECK(key_at(4, 4) == 0, msg);
  snprintf(msg, sizeof msg, "key_at(99,99) == 0");  CHECK(key_at(99, 99) == 0, msg);
}

/* ================================================================== */
/* 2. Mouse hit-testing: key_at_click                                  */
/* ================================================================== */

static void test_click_grid(void) {
  int r, c;
  char msg[96];
  printf("--- key_at_click: every grid cell ---\n");
  for (r = 0; r < CALC_KEYPAD_ROWS; r++) {
    for (c = 0; c < CALC_KEYPAD_COLS; c++) {
      int x = c * CALC_CELL_W + 2;          /* cell centre column */
      int y = CALC_ROW_KEYPAD + r;
      snprintf(msg, sizeof msg,
               "click(cell %d,%d) at x=%d y=%d -> '%c'", r, c, x, y,
               KEYS_EXPECTED[r][c]);
      CHECK(key_at_click(x, y) == KEYS_EXPECTED[r][c], msg);
    }
  }
}

static void test_click_boundaries(void) {
  printf("--- key_at_click: cell edges / outside ---\n");
  CHECK(key_at_click(0, CALC_ROW_KEYPAD) == '7', "x=0 first cell -> '7'");
  CHECK(key_at_click(4, CALC_ROW_KEYPAD) == '7', "x=4 last col of cell 0 -> '7'");
  CHECK(key_at_click(5, CALC_ROW_KEYPAD) == '8', "x=5 first col of cell 1 -> '8'");
  CHECK(key_at_click(9, CALC_ROW_KEYPAD) == '8', "x=9 last col of cell 1 -> '8'");
  CHECK(key_at_click(10, CALC_ROW_KEYPAD) == '9', "x=10 first col of cell 2 -> '9'");
  CHECK(key_at_click(15, CALC_ROW_KEYPAD) == '+', "x=15 first col of cell 3 -> '+'");
  CHECK(key_at_click(19, CALC_ROW_KEYPAD + 3) == '/', "x=19 on bottom row -> '/'");
  CHECK(key_at_click(19, CALC_ROW_KEYPAD) == '+', "x=19 on top row -> '+'");
  CHECK(key_at_click(20, CALC_ROW_KEYPAD) == 0, "x=20 past the keypad -> 0");
  CHECK(key_at_click(-1, CALC_ROW_KEYPAD) == 0, "x=-1 -> 0");
  CHECK(key_at_click(2, CALC_ROW_KEYPAD - 1) == 0, "row above keypad -> 0");
  CHECK(key_at_click(2, CALC_ROW_KEYPAD + CALC_KEYPAD_ROWS) == 0,
        "row below keypad -> 0");
  CHECK(key_at_click(2, CALC_ROW_EXPR) == 0, "click on expression row -> 0");
  CHECK(key_at_click(2, CALC_ROW_MEMORY) == 0, "click on memory row -> 0");
  CHECK(key_at_click(2, CALC_ROW_KEYPAD + 3) == '0', "bottom-left keypad cell -> '0'");
  CHECK(key_at_click(7, CALC_ROW_KEYPAD + 1) == '5', "middle cell (1,1) -> '5'");
}

/* ================================================================== */
/* 3. Evaluation: left-to-right, negatives, truncating division        */
/* ================================================================== */

static void check_eval(const char *expr, int64_t expected, const char *what) {
  char msg[128];
  reset_state();
  type_str(expr);
  (void)calc_press('=');
  snprintf(msg, sizeof msg, "%s: \"%s=\" -> %lld (got %lld)", what, expr,
           (long long)expected, (long long)calc_result);
  CHECK(calc_error == CALC_OK && calc_result == expected, msg);
}

static void test_evaluation(void) {
  printf("--- evaluation ---\n");
  check_eval("2+3*4", 20, "left-to-right, no precedence");
  check_eval("2*3+4", 10, "left-to-right multiply first");
  check_eval("10-4-3", 3, "chained subtraction");
  check_eval("1+2+3+4+5", 15, "five terms");
  check_eval("12+34*56", (12 + 34) * 56, "two-term chain");
  check_eval("100/7", 14, "integer division");
  check_eval("7/2", 3, "truncation 7/2");
  check_eval("-7/2", -3, "truncation toward zero -7/2");
  check_eval("-5+3", -2, "leading unary minus");
  check_eval("5*-3", -15, "unary minus after '*'");
  check_eval("5--3", 8, "unary minus after '-'");
  check_eval("5+-3", 2, "unary minus after '+'");
  check_eval("5/-2", -2, "unary minus as divisor");
  check_eval("0+0", 0, "zero result");
  check_eval("999999999*999999999", 999999998000000001LL, "large product");
}

static void test_can_append(void) {
  printf("--- input acceptance (calc_can_append) ---\n");
  reset_state();
  CHECK(calc_can_append('-') == 1, "leading '-' accepted");
  CHECK(calc_can_append('+') == 0, "leading '+' rejected");
  CHECK(calc_can_append('*') == 0, "leading '*' rejected");
  CHECK(calc_can_append('7') == 1, "leading digit accepted");
  type_str("-");
  CHECK(calc_can_append('-') == 0, "lone '-' cannot take a second sign");
  CHECK(calc_can_append('3') == 1, "'-' followed by digit ok");
  reset_state();
  type_str("5");
  CHECK(calc_can_append('+') == 1, "'5' + operator ok");
  CHECK(calc_can_append('*') == 1, "'5' + '*' ok");
  type_str("*");
  CHECK(calc_can_append('-') == 1, "'5*' + unary '-' ok");
  CHECK(calc_can_append('+') == 0, "'5*' + '+' rejected");
  type_str("-");
  CHECK(calc_can_append('-') == 0, "'5*-' cannot take another sign");
  CHECK(calc_can_append('7') == 1, "'5*-' + digit ok");
  /* And it actually evaluates: "5*-7" */
  type_str("7");
  (void)calc_press('=');
  CHECK(calc_result == -35, "\"5*-7=\" -> -35");
}

/* ================================================================== */
/* 4. '=' semantics                                                    */
/* ================================================================== */

static void test_equals_semantics(void) {
  printf("--- '=' semantics ---\n");

  /* second '=' re-displays, does not repeat the operation */
  reset_state();
  type_str("2+3");
  CHECK(calc_press('=') == 1, "first '=' changes state");
  CHECK(calc_result == 5, "\"2+3=\" -> 5");
  CHECK(calc_press('=') == 0, "second '=' is a no-op (re-display)");
  CHECK(calc_result == 5, "result unchanged by second '='");
  CHECK(calc_after_equals == 1, "still in result state");

  /* digit after '=' starts a fresh expression */
  reset_state();
  type_str("2+3");
  (void)calc_press('=');           /* result 5, expr cleared */
  CHECK(calc_expr_len == 0, "expression cleared after '='");
  (void)calc_press('7');
  CHECK(strcmp(calc_expr, "7") == 0, "digit after '=' starts fresh ('7')");
  (void)calc_press('=');
  CHECK(calc_result == 7, "\"7=\" after previous '=' -> 7");

  /* operator after '=' continues from the result */
  reset_state();
  type_str("8");
  (void)calc_press('=');
  CHECK(calc_press('/') == 1, "operator after '=' accepted");
  CHECK(strcmp(calc_expr, "8/") == 0, "expr rebuilt from result: \"8/\"");
  (void)calc_press('2');
  (void)calc_press('=');
  CHECK(calc_result == 4, "\"8=\" then \"/2=\" -> 4");

  /* negative result continued */
  reset_state();
  type_str("-5");
  (void)calc_press('=');
  (void)calc_press('+');
  CHECK(strcmp(calc_expr, "-5+") == 0, "negative result becomes \"-5+\"");
  (void)calc_press('2');
  (void)calc_press('=');
  CHECK(calc_result == -3, "\"-5=\" then \"+2=\" -> -3");

  /* empty expression + '=' */
  reset_state();
  CHECK(calc_press('=') == 1, "empty + '=' changes state");
  CHECK(calc_error == CALC_OK, "empty + '=' is not an error");
  CHECK(calc_result == 0 && calc_have_result == 1, "empty + '=' -> 0");
  CHECK(calc_press('=') == 0, "'=' again still a no-op");

  /* Enter behaves like '=' */
  reset_state();
  type_str("6*7");
  (void)calc_press('\n');
  CHECK(calc_result == 42, "Enter evaluates like '=' (6*7=42)");
  reset_state();
  type_str("1+1");
  (void)calc_press('\r');
  CHECK(calc_result == 2, "Carriage return evaluates like '='");
}

/* ================================================================== */
/* 5. Error states                                                     */
/* ================================================================== */

static void test_divide_by_zero(void) {
  const char *s;
  printf("--- divide by zero ---\n");
  reset_state();
  type_str("5/0");
  CHECK(calc_press('=') == 1, "'=' on 5/0 enters error state");
  CHECK(calc_error == CALC_ERR_DIV0, "error code is CALC_ERR_DIV0");
  s = screen_text();
  CHECK(strstr(s, "Error: division by zero") != NULL,
        "screen shows \"Error: division by zero\"");

  /* error persists through '=', operators, backspace, recall, memory menu */
  CHECK(calc_press('=') == 0, "'=' ignored in error state");
  CHECK(calc_error == CALC_ERR_DIV0, "still in error after '='");
  CHECK(calc_press('+') == 0, "operator ignored in error state");
  CHECK(calc_press('\b') == 0, "backspace ignored in error state");
  CHECK(calc_press('m') == 0, "MemRecall key ignored in error state");
  CHECK(calc_menu(0, 3) == 0, "MemRecall menu ignored in error state");
  CHECK(calc_error == CALC_ERR_DIV0, "error state unchanged");

  /* recovery via a digit: fresh expression */
  CHECK(calc_press('4') == 1, "digit recovers from error");
  CHECK(calc_error == CALC_OK, "error cleared by digit");
  CHECK(calc_expr_len == 1 && calc_expr[0] == '4', "fresh expression \"4\"");
  (void)calc_press('=');
  CHECK(calc_result == 4, "\"4\" after recovery evaluates to 4");
  s = screen_text();
  CHECK(strstr(s, "Error") == NULL, "no error text after recovery");

  /* recovery via 'C' */
  reset_state();
  type_str("8/0");
  (void)calc_press('=');
  CHECK(calc_error == CALC_ERR_DIV0, "8/0 errors too");
  CHECK(calc_press('c') == 1, "'c' clears the error");
  CHECK(calc_error == CALC_OK && calc_expr_len == 0, "state cleared by 'c'");

  /* 0/0 errors as well */
  reset_state();
  type_str("0/0");
  (void)calc_press('=');
  CHECK(calc_error == CALC_ERR_DIV0, "0/0 is division by zero");
}

static void test_incomplete_expression(void) {
  const char *s;
  printf("--- incomplete expression ---\n");
  reset_state();
  type_str("5+");
  (void)calc_press('=');
  CHECK(calc_error == CALC_ERR_SYNTAX, "\"5+=\" -> syntax error");
  s = screen_text();
  CHECK(strstr(s, "Error: incomplete expression") != NULL,
        "screen shows \"Error: incomplete expression\"");
  (void)calc_press('9'); /* recovers */
  CHECK(calc_error == CALC_OK && strcmp(calc_expr, "9") == 0,
        "digit recovers from syntax error");

  reset_state();
  type_str("-");
  (void)calc_press('=');
  CHECK(calc_error == CALC_ERR_SYNTAX, "lone '-' + '=' -> syntax error");

  reset_state();
  type_str("5+-");
  (void)calc_press('=');
  CHECK(calc_error == CALC_ERR_SYNTAX, "\"5+-=\" (trailing sign) -> syntax error");

  reset_state();
  type_str("12*");
  (void)calc_press('=');
  CHECK(calc_error == CALC_ERR_SYNTAX, "\"12*=\" (trailing op) -> syntax error");
}

/* ================================================================== */
/* 6. Editing: backspace and the expression length cap                 */
/* ================================================================== */

static void test_backspace(void) {
  printf("--- backspace ---\n");
  reset_state();
  type_str("123");
  CHECK(calc_press('\b') == 1, "backspace deletes");
  CHECK(strcmp(calc_expr, "12") == 0, "\"123\" + BS -> \"12\"");
  CHECK(calc_press(127) == 1, "DEL (127) deletes too");
  CHECK(strcmp(calc_expr, "1") == 0, "\"12\" + DEL -> \"1\"");
  CHECK(calc_press('\b') == 1, "backspace to empty");
  CHECK(calc_expr_len == 0, "expression now empty");
  CHECK(calc_press('\b') == 0, "backspace on empty is a no-op");
  CHECK(calc_press('\b') == 0, "backspace on empty is still a no-op");

  reset_state();
  type_str("5+");
  CHECK(calc_press('\b') == 1, "backspace after operator");
  CHECK(strcmp(calc_expr, "5") == 0, "\"5+\" + BS -> \"5\"");

  reset_state();
  type_str("9");
  (void)calc_press('=');
  CHECK(calc_press('\b') == 0, "backspace after '=' is a no-op");
  CHECK(calc_after_equals == 1, "still in result state after backspace");

  reset_state();
  type_str("123456789012345678901234567890"); /* 30 chars */
  CHECK(calc_expr_len == CALC_EXPR_MAX, "typed up to the cap");
  CHECK(calc_press('\b') == 1, "backspace at the cap works");
  CHECK(calc_expr_len == CALC_EXPR_MAX - 1, "length is cap-1 after BS");
}

static void test_expression_cap(void) {
  const char *s;
  printf("--- expression length cap ---\n");
  reset_state();
  type_str("1234567890123456789012345678901234567890"); /* 40 digits */
  CHECK(calc_expr_len == CALC_EXPR_MAX, "expression capped at 30 chars");
  CHECK(strcmp(calc_expr, "123456789012345678901234567890") == 0,
        "kept the first 30 chars");
  CHECK(calc_press('9') == 0, "digit past the cap is ignored");
  CHECK(calc_expr_len == CALC_EXPR_MAX, "length still 30");
  CHECK(calc_press('+') == 0, "operator past the cap is ignored");
  CHECK(calc_expr_len == CALC_EXPR_MAX, "length still 30 after '+'");
  (void)calc_press('=');
  /* The 30-digit number wraps mod 2^64; verify against expected wrap. */
  {
    uint64_t u = 0;
    int i;
    const char *d = "123456789012345678901234567890";
    for (i = 0; d[i]; i++) u = u * 10ULL + (uint64_t)(d[i] - '0');
    CHECK(calc_result == (int64_t)u, "30-digit value wraps modulo 2^64");
  }
  /* the screen still renders after a huge expression */
  s = screen_text();
  CHECK(strstr(s, "=== Calculator ===") != NULL, "screen still renders");
}

/* ================================================================== */
/* 7. Memory operations                                                */
/* ================================================================== */

static void test_memory(void) {
  const char *s;
  printf("--- memory ---\n");
  reset_state();
  type_str("5");
  (void)calc_press('=');
  CHECK(calc_menu(0, 1) == 1, "Mem+ handled");
  CHECK(calc_memory == 5, "Mem+ adds the result (5)");
  CHECK(calc_menu(0, 1) == 1, "Mem+ again");
  CHECK(calc_memory == 10, "Mem+ accumulates (10)");
  CHECK(calc_menu(0, 2) == 1, "Mem- handled");
  CHECK(calc_memory == 5, "Mem- subtracts (5)");
  s = screen_text();
  CHECK(strstr(s, "Memory: 5") != NULL, "screen shows \"Memory: 5\"");

  /* repeated Mem- goes negative and the display follows */
  reset_state();
  type_str("0");
  (void)calc_press('=');
  for (int i = 0; i < 3; i++) (void)calc_menu(0, 2);
  CHECK(calc_memory == 0, "Mem- of 0 keeps memory at 0");
  type_str("2");
  (void)calc_press('=');
  (void)calc_menu(0, 2);
  (void)calc_menu(0, 2);
  (void)calc_menu(0, 2);
  CHECK(calc_memory == -6, "three Mem- of 2 -> -6");
  s = screen_text();
  CHECK(strstr(s, "Memory: -6") != NULL, "screen shows \"Memory: -6\"");

  /* Mem+ uses the live value of an un-evaluated expression */
  reset_state();
  type_str("2+3");
  (void)calc_menu(0, 1);
  CHECK(calc_memory == 5, "Mem+ with live expression 2+3 adds 5");

  /* Mem+ with an incomplete expression adds 0 */
  reset_state();
  type_str("5+");
  (void)calc_menu(0, 1);
  CHECK(calc_memory == 0, "Mem+ with incomplete expression adds nothing");

  /* unknown menu / item are ignored */
  reset_state();
  CHECK(calc_menu(1, 0) == 0, "unknown menu index ignored");
  CHECK(calc_menu(0, 9) == 0, "unknown menu item ignored");
  CHECK(calc_memory == 0, "memory untouched by unknown menu");

  /* Clear keeps memory */
  reset_state();
  type_str("7");
  (void)calc_press('=');
  (void)calc_menu(0, 1);
  CHECK(calc_memory == 7, "memory is 7");
  (void)calc_press('c');
  CHECK(calc_memory == 7, "'C' keeps memory");
  CHECK(calc_expr_len == 0 && calc_result == 0 && calc_error == CALC_OK,
        "'C' clears expression/result/error");
}

static void test_mem_recall(void) {
  printf("--- memory recall ---\n");
  reset_state();
  type_str("7");
  (void)calc_press('=');
  (void)calc_menu(0, 1);           /* memory = 7 */
  (void)calc_press('c');           /* clear calc, keep memory */
  CHECK(calc_press('m') == 1, "'m' recalls");
  CHECK(strcmp(calc_expr, "7") == 0, "recall inserted \"7\"");
  (void)calc_press('=');
  CHECK(calc_result == 7, "recalled value evaluates to 7");

  /* menu item 3 is the same as 'm' */
  (void)calc_press('c');
  CHECK(calc_menu(0, 3) == 1, "MemRecall menu item works");
  CHECK(strcmp(calc_expr, "7") == 0, "menu recall inserted \"7\"");

  /* negative memory recalls with its sign */
  reset_state();
  type_str("9");
  (void)calc_press('=');
  (void)calc_menu(0, 2);           /* memory = -9 */
  (void)calc_press('c');
  CHECK(calc_memory == -9, "memory is -9");
  (void)calc_press('m');
  CHECK(strcmp(calc_expr, "-9") == 0, "recall inserted \"-9\"");
  (void)calc_press('=');
  CHECK(calc_result == -9, "recalled -9 evaluates to -9");

  /* recall into a pending operator */
  (void)calc_press('c');
  type_str("5+");
  CHECK(calc_press('m') == 1, "recall after operator accepted");
  CHECK(strcmp(calc_expr, "5+-9") == 0, "expr is \"5+-9\"");
  (void)calc_press('=');
  CHECK(calc_result == -4, "\"5+-9=\" -> 5 + (-9) = -4");

  /* recall after '=' starts a fresh expression */
  (void)calc_press('c');
  type_str("3");
  (void)calc_press('=');
  CHECK(calc_after_equals == 1, "in result state");
  (void)calc_press('m');
  CHECK(calc_after_equals == 0 && strcmp(calc_expr, "-9") == 0,
        "recall after '=' starts fresh with \"-9\"");

  /* recall at the cap is ignored */
  reset_state();
  type_str("7");
  (void)calc_press('=');
  (void)calc_menu(0, 1);           /* memory = 7 */
  (void)calc_press('c');
  type_str("123456789012345678901234567890"); /* at the 30-char cap */
  CHECK(strcmp(calc_expr, "123456789012345678901234567890") == 0, "expr at cap");
  CHECK(calc_press('m') == 0, "recall at the cap is ignored");
  CHECK(strcmp(calc_expr, "123456789012345678901234567890") == 0,
        "expression unchanged by ignored recall");
}

/* ================================================================== */
/* 8. Unknown keys and event dispatch                                  */
/* ================================================================== */

static void test_unknown_keys(void) {
  printf("--- unknown / whitespace keys ---\n");
  reset_state();
  CHECK(calc_press(' ') == 0, "space ignored");
  CHECK(calc_press('\t') == 0, "tab ignored");
  CHECK(calc_press('z') == 0, "'z' ignored");
  CHECK(calc_press('?') == 0, "'?' ignored");
  CHECK(calc_press('\x01') == 0, "control char ignored");
  CHECK(calc_expr_len == 0 && calc_error == CALC_OK, "state unchanged");

  /* but 'x' is also not a key while typing an expression */
  type_str("12");
  CHECK(calc_press('z') == 0, "'z' ignored mid-expression");
  CHECK(strcmp(calc_expr, "12") == 0, "expression unchanged");
  CHECK(calc_press('X') == 0, "'X' ignored mid-expression");
  CHECK(strcmp(calc_expr, "12") == 0, "expression unchanged (2)");
}

static void test_handle_event(void) {
  struct gui_event ev;
  printf("--- calc_handle_event dispatch ---\n");

  reset_state();
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_CHAR;
  ev.ch = '7';
  CHECK(calc_handle_event(&ev) == 1, "CHAR event handled");
  CHECK(strcmp(calc_expr, "7") == 0, "CHAR '7' typed");

  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_CHAR;
  ev.ch = ' ';
  CHECK(calc_handle_event(&ev) == 0, "CHAR space ignored");

  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_UP;
  CHECK(calc_handle_event(&ev) == 0, "arrow event ignored");
  ev.type = GUI_EV_ESC;
  CHECK(calc_handle_event(&ev) == 0, "ESC event ignored");

  /* menu dispatch through the event struct */
  reset_state();
  type_str("4");
  (void)calc_press('=');
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MENU;
  ev.menu = 0;
  ev.item = 1;                     /* Mem+ */
  CHECK(calc_handle_event(&ev) == 1, "MENU event handled");
  CHECK(calc_memory == 4, "MENU Mem+ added 4");
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MENU;
  ev.menu = 2;
  ev.item = 0;
  CHECK(calc_handle_event(&ev) == 0, "MENU with unknown index ignored");

  /* mouse dispatch: click the '6' cell */
  reset_state();
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MOUSE;
  ev.x = 2 * CALC_CELL_W + 2;      /* cell (1,2) -> '6' */
  ev.y = CALC_ROW_KEYPAD + 1;
  ev.button = 1;
  CHECK(calc_handle_event(&ev) == 1, "left-click on '6' handled");
  CHECK(strcmp(calc_expr, "6") == 0, "click typed '6'");

  /* click the '=' cell evaluates */
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MOUSE;
  ev.x = 2 * CALC_CELL_W + 2;      /* cell (3,2) -> '=' */
  ev.y = CALC_ROW_KEYPAD + 3;
  ev.button = 1;
  CHECK(calc_handle_event(&ev) == 1, "left-click on '=' handled");
  CHECK(calc_result == 6, "clicking '=' evaluated 6");

  /* 'C' cell clears */
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MOUSE;
  ev.x = 1 * CALC_CELL_W + 2;      /* cell (3,1) -> 'C' */
  ev.y = CALC_ROW_KEYPAD + 3;
  ev.button = 1;
  CHECK(calc_handle_event(&ev) == 1, "left-click on 'C' handled");
  CHECK(calc_expr_len == 0 && calc_result == 0, "clicking 'C' cleared");

  /* right-click and off-keypad clicks are ignored */
  reset_state();
  memset(&ev, 0, sizeof ev);
  ev.type = GUI_EV_MOUSE;
  ev.x = 2;
  ev.y = CALC_ROW_KEYPAD;
  ev.button = 2;
  CHECK(calc_handle_event(&ev) == 0, "right-click ignored");
  ev.button = 1;
  ev.y = 0;                        /* header row */
  CHECK(calc_handle_event(&ev) == 0, "click outside the keypad ignored");
  CHECK(calc_expr_len == 0, "no input from ignored clicks");
}

/* ================================================================== */
/* 9. Rendering                                                        */
/* ================================================================== */

static void test_render_layout(void) {
  const char *s;
  char line[80];
  int i;
  printf("--- render: layout ---\n");

  reset_state();
  s = screen_text();
  CHECK(screen_line(s, CALC_ROW_HEADER, line, sizeof line) &&
        strcmp(line, "=== Calculator ===") == 0,
        "row 0 is \"=== Calculator ===\"");

  CHECK(screen_line(s, CALC_ROW_KEYPAD + 0, line, sizeof line) &&
        strcmp(line, "[ 7 ][ 8 ][ 9 ][ + ]") == 0,
        "keypad row 0 == \"[ 7 ][ 8 ][ 9 ][ + ]\"");
  CHECK(screen_line(s, CALC_ROW_KEYPAD + 1, line, sizeof line) &&
        strcmp(line, "[ 4 ][ 5 ][ 6 ][ - ]") == 0,
        "keypad row 1 == \"[ 4 ][ 5 ][ 6 ][ - ]\"");
  CHECK(screen_line(s, CALC_ROW_KEYPAD + 2, line, sizeof line) &&
        strcmp(line, "[ 1 ][ 2 ][ 3 ][ * ]") == 0,
        "keypad row 2 == \"[ 1 ][ 2 ][ 3 ][ * ]\"");
  CHECK(screen_line(s, CALC_ROW_KEYPAD + 3, line, sizeof line) &&
        strcmp(line, "[ 0 ][ C ][ = ][ / ]") == 0,
        "keypad row 3 == \"[ 0 ][ C ][ = ][ / ]\"");

  CHECK(screen_line(s, CALC_ROW_MEMORY, line, sizeof line) &&
        strcmp(line, "Memory: 0") == 0,
        "memory row == \"Memory: 0\"");

  /* expression right-aligned in exactly CALC_DISPLAY_W columns */
  reset_state();
  type_str("1+2");
  s = screen_text();
  CHECK(screen_line(s, CALC_ROW_EXPR, line, sizeof line), "expression row present");
  CHECK((int)strlen(line) == CALC_DISPLAY_W, "expression row is exactly 30 wide");
  CHECK(right_aligned_ends(line, "1+2"),
        "expression right-aligned (ends with \"1+2\")");
  for (i = 0; i < CALC_DISPLAY_W - 3; i++) {
    if (line[i] != ' ') break;
  }
  CHECK(i == CALC_DISPLAY_W - 3, "expression row space-padded on the left");

  /* empty expression renders as all spaces */
  reset_state();
  s = screen_text();
  CHECK(screen_line(s, CALC_ROW_EXPR, line, sizeof line) &&
        (int)strlen(line) == CALC_DISPLAY_W && all_spaces(line),
        "empty expression row is 30 spaces");

  /* result line: "> N" right-aligned */
  reset_state();
  type_str("1+2");
  (void)calc_press('=');
  s = screen_text();
  CHECK(screen_line(s, CALC_ROW_RESULT, line, sizeof line), "result row present");
  CHECK((int)strlen(line) == CALC_DISPLAY_W, "result row is exactly 30 wide");
  CHECK(right_aligned_ends(line, "> 3"), "result right-aligned shows \"> 3\"");

  /* negative result */
  reset_state();
  type_str("-5");
  (void)calc_press('=');
  s = screen_text();
  (void)screen_line(s, CALC_ROW_RESULT, line, sizeof line);
  CHECK(right_aligned_ends(line, "> -5"),
        "negative result right-aligned shows \"> -5\"");

  /* error line */
  reset_state();
  type_str("5/0");
  (void)calc_press('=');
  s = screen_text();
  (void)screen_line(s, CALC_ROW_RESULT, line, sizeof line);
  CHECK(right_aligned_ends(line, "> Error: division by zero"),
        "error right-aligned in the result row");

  /* live preview while typing */
  reset_state();
  type_str("20/3");
  s = screen_text();
  (void)screen_line(s, CALC_ROW_RESULT, line, sizeof line);
  CHECK(right_aligned_ends(line, "> 6"),
        "live value shown while typing (20/3 -> 6)");

  /* deterministic rendering */
  {
    char b1[600], b2[600];
    int n1, n2;
    n1 = calc_render(b1, (int)sizeof b1);
    n2 = calc_render(b2, (int)sizeof b2);
    CHECK(n1 == n2 && strcmp(b1, b2) == 0, "rendering is deterministic");
  }
}

static void test_render_line_lengths(void) {
  char big[600];
  const char *p;
  int len = 0, max = 0;
  printf("--- render: line lengths ---\n");
  reset_state();
  type_str("123456789012345678901234567890");
  (void)calc_press('+');
  calc_render(big, (int)sizeof big);
  p = big;
  for (; *p != '\0'; p++) {
    if (*p == '\n') {
      if (len > max) max = len;
      len = 0;
    } else {
      len++;
    }
  }
  if (len > max) max = len;
  CHECK(max <= 110, "no rendered line exceeds 110 chars");
  CHECK(max <= CALC_DISPLAY_W + 1, "widest line is the 30-col display area");
}

/* ================================================================== */
/* 10. Right-alignment helper + overflow                               */
/* ================================================================== */

static void test_right_align(void) {
  char ab[CALC_DISPLAY_W + 8];
  printf("--- right alignment helper ---\n");
  calc_right_align(ab, CALC_DISPLAY_W, "abc");
  CHECK((int)strlen(ab) == CALC_DISPLAY_W, "right-align output is width wide");
  CHECK(strcmp(ab, "                           abc") == 0,
        "\"abc\" is padded to the right edge");
  calc_right_align(ab, CALC_DISPLAY_W, "012345678901234567890123456789");
  CHECK(strcmp(ab, "012345678901234567890123456789") == 0,
        "exactly-width text is unchanged");
  calc_right_align(ab, CALC_DISPLAY_W, "01234567890123456789012345678901234");
  CHECK(strcmp(ab, "567890123456789012345678901234") == 0,
        "overlong text keeps the rightmost 30 chars");
  calc_right_align(ab, CALC_DISPLAY_W, "");
  CHECK((int)strlen(ab) == CALC_DISPLAY_W && all_spaces(ab),
        "empty text -> all spaces");
}

static void test_overflow_wrap(void) {
  printf("--- overflow wraps (documented) ---\n");
  reset_state();
  type_str("9223372036854775807+1");
  (void)calc_press('=');
  CHECK(calc_error == CALC_OK, "INT64_MAX + 1 does not error");
  CHECK(calc_result == INT64_MIN, "INT64_MAX + 1 wraps to INT64_MIN");

  reset_state();
  type_str("9223372036854775807*2");
  (void)calc_press('=');
  CHECK(calc_result == -2, "INT64_MAX * 2 wraps to -2");

  reset_state();
  type_str("9223372036854775808"); /* 2^63 as an unsigned literal */
  (void)calc_press('=');
  CHECK(calc_result == INT64_MIN, "9223372036854775808 wraps to INT64_MIN");

  reset_state();
  type_str("-9223372036854775808/-1"); /* INT64_MIN / -1 must not crash */
  (void)calc_press('=');
  CHECK(calc_error == CALC_OK, "INT64_MIN / -1 does not error");
  CHECK(calc_result == INT64_MIN, "INT64_MIN / -1 wraps to INT64_MIN");

  reset_state();
  type_str("-9223372036854775808"); /* exactly representable */
  (void)calc_press('=');
  CHECK(calc_result == INT64_MIN, "INT64_MIN literal round-trips");

  reset_state();
  type_str("9223372036854775807*4");
  (void)calc_press('=');
  CHECK(calc_error == CALC_OK, "huge product wraps without error");
  CHECK(calc_result == -4, "INT64_MAX * 4 wraps to -4");

  reset_state();
  type_str("4611686018427387904*4"); /* 2^62 * 4 = 2^64 -> 0 */
  (void)calc_press('=');
  CHECK(calc_result == 0, "2^62 * 4 wraps to 0");
}

/* ================================================================== */
/* Runner                                                              */
/* ================================================================== */

int main(void) {
  printf("=== Calc Host Test ===\n\n");

  test_key_at_grid();
  test_key_at_out_of_range();
  test_click_grid();
  test_click_boundaries();
  test_evaluation();
  test_can_append();
  test_equals_semantics();
  test_divide_by_zero();
  test_incomplete_expression();
  test_backspace();
  test_expression_cap();
  test_memory();
  test_mem_recall();
  test_unknown_keys();
  test_handle_event();
  test_render_layout();
  test_render_line_lengths();
  test_right_align();
  test_overflow_wrap();

  printf("\n=== Test Results ===\n");
  printf("Checks run:    %d\n", g_checks);
  printf("Checks passed: %d\n", g_checks - g_failures);
  printf("Checks failed: %d\n", g_failures);
  if (g_failures == 0) {
    printf("\nALL TESTS PASSED\n");
    return 0;
  }
  printf("\n%d CHECK(S) FAILED\n", g_failures);
  return 1;
}
