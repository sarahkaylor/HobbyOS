/*
 * calc.c - Calculator for HobbyOS.
 *
 * Desktop calculator with an on-screen clickable 4x4 keypad.
 *
 * Semantics (signed 64-bit integer arithmetic only - NO floating point):
 *   - The user builds an expression from keypresses; '=' evaluates it.
 *   - Evaluation is strictly LEFT-TO-RIGHT, with no operator precedence:
 *       "2+3*4" = (2+3)*4 = 20.
 *   - '/' is integer division truncating toward zero: 7/2 = 3, -7/2 = -3.
 *   - Divide by zero enters an error state showing "Error: division by
 *     zero"; it persists until 'C' or any digit is pressed (spec).
 *   - A leading '-' or a '-' right after an operator is a unary sign:
 *       "-5+3" = -2, "5*-3" = -15, "5--3" = 8.
 *   - Overflow WRAPS (two's complement, mod 2^64); all arithmetic is done
 *     on uint64_t so the wrap is well defined on host and target alike.
 *     e.g. INT64_MAX + 1 = -9223372036854775808, INT64_MAX * 2 = -2.
 *   - '=' pressed twice in a row just re-displays the previous result
 *     (it does NOT repeat the operation).
 *   - A digit typed after '=' starts a fresh expression; an operator typed
 *     after '=' continues from the result ("8=" then "+2=" -> 10).
 *   - Enter is the same as '='.  Backspace deletes the last input char.
 *   - Whitespace / unknown keys are ignored.
 *
 * Memory (one int64 register):
 *   - Mem+ / Mem- add/subtract the CURRENT value, i.e. the result after
 *     '=' or, while typing, the live value of a complete expression.
 *   - MemRecall appends the memory value's decimal text to the expression.
 *     It is ignored in the error state or when it would not fit.
 *   - 'C' / the Clear menu item clear the expression, result and error
 *     state but KEEP memory (there is no other memory-clear affordance).
 *
 * Screen layout (window content text rows, 0-based):
 *   row 0            "=== Calculator ==="
 *   row 1            expression, right-aligned in CALC_DISPLAY_W columns
 *                    (rightmost CALC_DISPLAY_W chars if longer)
 *   row 2            "> N" or "> Error text", right-aligned the same way
 *   rows 3..6        the 4x4 keypad, every cell CALC_CELL_W chars wide
 *   row 7            "Memory: N"
 * The keypad's first row and cell geometry are constants shared by the
 * renderer and the mouse hit-testing so the two can never drift apart.
 */

#include "libc.h"
#include "gui.h"

/* ---- Configuration ---- */

#define CALC_EXPR_MAX    30   /* expression input cap (chars)             */
#define CALC_DISPLAY_W   30   /* width of the right-aligned display area  */

#define CALC_KEYPAD_ROWS  4
#define CALC_KEYPAD_COLS  4
#define CALC_CELL_W       5   /* every keypad cell renders as "[ X ]"     */

/* Layout rows, 0-based, inside the window content area. */
#define CALC_ROW_HEADER  0
#define CALC_ROW_EXPR    1
#define CALC_ROW_RESULT  2
#define CALC_ROW_KEYPAD  3
#define CALC_ROW_MEMORY  (CALC_ROW_KEYPAD + CALC_KEYPAD_ROWS)

/* Evaluation result / error status codes (shared). */
#define CALC_OK         0
#define CALC_ERR_DIV0   1
#define CALC_ERR_SYNTAX 2

/* Keypad key map: row-major, 4x4. */
static const char CALC_KEYS[CALC_KEYPAD_ROWS][CALC_KEYPAD_COLS] = {
  { '7', '8', '9', '+' },
  { '4', '5', '6', '-' },
  { '1', '2', '3', '*' },
  { '0', 'C', '=', '/' },
};

/* ---- State ---- */

static char    calc_expr[CALC_EXPR_MAX + 1]; /* current expression text  */
static int     calc_expr_len = 0;
static int64_t calc_result = 0;              /* last computed result     */
static int     calc_have_result = 0;
static int     calc_after_equals = 0;        /* '=' was just pressed     */
static int     calc_error = CALC_OK;         /* CALC_ERR_* or CALC_OK    */
static int64_t calc_memory = 0;              /* memory register          */

/* ---- Small helpers ---- */

static int calc_slen(const char *s) {
  int n = 0;
  while (s[n] != '\0') n++;
  return n;
}

static int calc_is_digit(char c) { return c >= '0' && c <= '9'; }

static int calc_is_op(char c) {
  return c == '+' || c == '-' || c == '*' || c == '/';
}

/* All arithmetic wraps (two's complement); done on uint64_t so the wrap is
 * well defined even in the HOST_TEST build. */
static int64_t calc_wrap_add(int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a + (uint64_t)b);
}
static int64_t calc_wrap_sub(int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a - (uint64_t)b);
}
static int64_t calc_wrap_mul(int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a * (uint64_t)b);
}
static int64_t calc_wrap_neg(int64_t a) {
  return (int64_t)(0ULL - (uint64_t)a);
}

/* Decimal text of a signed 64-bit integer.  buf must hold >= 21 bytes. */
static int calc_i64_to_str(int64_t v, char *buf) {
  char tmp[20];
  int n = 0, j = 0;
  uint64_t u;
  if (v < 0) u = (uint64_t)(-(v + 1)) + 1ULL; /* safe for INT64_MIN */
  else       u = (uint64_t)v;
  if (u == 0) tmp[n++] = '0';
  while (u > 0) {
    tmp[n++] = (char)('0' + (int)(u % 10));
    u /= 10;
  }
  if (v < 0) buf[j++] = '-';
  while (n > 0) buf[j++] = tmp[--n];
  buf[j] = '\0';
  return j;
}

/* ---- Expression evaluation ---- */

/* Evaluate s[0..len) left-to-right with no operator precedence.  Grammar:
 *   expr := ['-'] digits { op ['-'] digits }
 * Returns CALC_OK (with *out set), CALC_ERR_DIV0 or CALC_ERR_SYNTAX. */
static int calc_eval(const char *s, int len, int64_t *out) {
  int i = 0, have = 0, pending = 0;
  int64_t acc = 0;
  for (;;) {
    int neg = 0;
    if (i < len && s[i] == '-') { neg = 1; i++; }
    if (i >= len || !calc_is_digit(s[i])) return CALC_ERR_SYNTAX;
    int64_t num = 0;
    while (i < len && calc_is_digit(s[i])) {
      num = calc_wrap_add(calc_wrap_mul(num, 10), s[i] - '0');
      i++;
    }
    if (neg) num = calc_wrap_neg(num);
    if (have) {
      if (pending == '+')      acc = calc_wrap_add(acc, num);
      else if (pending == '-') acc = calc_wrap_sub(acc, num);
      else if (pending == '*') acc = calc_wrap_mul(acc, num);
      else { /* '/' */
        if (num == 0) return CALC_ERR_DIV0;
        if (acc == INT64_MIN && num == -1) acc = INT64_MIN; /* wraps */
        else acc = acc / num;
      }
    } else {
      acc = num;
      have = 1;
    }
    if (i >= len) break;
    if (!calc_is_op(s[i])) return CALC_ERR_SYNTAX;
    pending = s[i];
    i++;
  }
  *out = acc;
  return CALC_OK;
}

/* Would appending `key` to the current expression keep it a valid
 * expression prefix?  Used to reject impossible input like "5++" or
 * "5---" while still allowing a unary '-' right after an operator. */
static int calc_can_append(char key) {
  int run = 0;
  if (calc_is_digit(key)) return 1;
  if (!calc_is_op(key)) return 0;
  while (run < calc_expr_len && calc_is_op(calc_expr[calc_expr_len - 1 - run])) run++;
  if (calc_expr_len == 0) return key == '-'; /* leading unary sign only */
  if (run == 0) return 1;                    /* after a digit: any op  */
  if (run >= 2) return 0;                    /* "5--": no third sign   */
  if (run == calc_expr_len) return 0;        /* lone "-": no more sign */
  return key == '-';                         /* after an op: unary '-' */
}

/* ---- State transitions ---- */

static void calc_append_ch(char c) {
  if (calc_expr_len >= CALC_EXPR_MAX) return;
  calc_expr[calc_expr_len++] = c;
  calc_expr[calc_expr_len] = '\0';
}

/* Clear the calculation (expression, result, error); memory is kept. */
static void calc_clear_calculation(void) {
  calc_expr_len = 0;
  calc_expr[0] = '\0';
  calc_result = 0;
  calc_have_result = 0;
  calc_after_equals = 0;
  calc_error = CALC_OK;
}

/* The "current value": the result after '=', else the live value of a
 * complete expression, else the last result (0 if there never was one). */
static int64_t calc_current_value(void) {
  int64_t v = 0;
  if (calc_error != CALC_OK) return 0;
  if (calc_after_equals) return calc_result;
  if (calc_expr_len > 0 && calc_eval(calc_expr, calc_expr_len, &v) == CALC_OK) return v;
  return calc_have_result ? calc_result : 0;
}

/* ---- Memory ---- */

static void calc_mem_add(int64_t v) { calc_memory = calc_wrap_add(calc_memory, v); }
static void calc_mem_sub(int64_t v) { calc_memory = calc_wrap_sub(calc_memory, v); }

/* MemRecall: append the memory value's decimal text to the expression.
 * Ignored in the error state or when it would not fit the cap. */
static int calc_mem_recall(void) {
  char num[24];
  int n, i;
  if (calc_error != CALC_OK) return 0;
  n = calc_i64_to_str(calc_memory, num);
  if (calc_after_equals) { /* recall after '=' starts a fresh expression */
    calc_after_equals = 0;
    calc_expr_len = 0;
    calc_expr[0] = '\0';
  }
  if (calc_expr_len + n > CALC_EXPR_MAX) return 0;
  for (i = 0; i < n; i++) calc_append_ch(num[i]);
  return 1;
}

/* ---- Input ---- */

/* Handle one key.  Returns 1 when the screen needs a redraw. */
static int calc_press(char key) {
  if (calc_error != CALC_OK) {
    /* Error state: only 'C' and any digit recover (per spec). */
    if (calc_is_digit(key)) {
      calc_error = CALC_OK;
      calc_after_equals = 0;
      calc_expr_len = 0;
      calc_expr[0] = '\0';
      calc_append_ch(key);
      return 1;
    }
    if (key == 'c' || key == 'C') {
      calc_clear_calculation();
      return 1;
    }
    return 0;
  }

  if (calc_is_digit(key)) {
    if (calc_after_equals) { /* digit after '=' starts fresh */
      calc_after_equals = 0;
      calc_expr_len = 0;
      calc_expr[0] = '\0';
    }
    if (calc_expr_len >= CALC_EXPR_MAX) return 0; /* length cap */
    calc_append_ch(key);
    return 1;
  }

  if (key == 'c' || key == 'C') {
    calc_clear_calculation();
    return 1;
  }

  if (key == '\b' || key == 127) { /* Backspace / DEL */
    if (calc_after_equals) return 0; /* result is not editable text */
    if (calc_expr_len == 0) return 0;
    calc_expr[--calc_expr_len] = '\0';
    return 1;
  }

  if (key == 'm' || key == 'M') return calc_mem_recall();

  if (key == '=' || key == '\n' || key == '\r') { /* '=' or Enter */
    if (calc_after_equals) return 0; /* 2nd '=' re-displays the result */
    if (calc_expr_len == 0) {        /* empty expression + '=' -> 0 */
      calc_result = 0;
      calc_have_result = 1;
      calc_after_equals = 1;
      return 1;
    }
    {
      int64_t v = 0;
      int st = calc_eval(calc_expr, calc_expr_len, &v);
      if (st == CALC_OK) {
        calc_result = v;
        calc_have_result = 1;
        calc_after_equals = 1;
        calc_expr_len = 0;
        calc_expr[0] = '\0';
        return 1;
      }
      calc_error = st; /* CALC_ERR_DIV0 / CALC_ERR_SYNTAX */
      calc_after_equals = 0;
      return 1;
    }
  }

  if (calc_is_op(key)) {
    if (calc_after_equals) { /* continue from the result */
      char num[24];
      int n = calc_i64_to_str(calc_result, num);
      int i;
      if (n + 1 > CALC_EXPR_MAX) return 0;
      calc_expr_len = 0;
      calc_expr[0] = '\0';
      for (i = 0; i < n; i++) calc_append_ch(num[i]);
      calc_append_ch(key);
      calc_after_equals = 0;
      return 1;
    }
    if (!calc_can_append(key)) return 0;
    if (calc_expr_len >= CALC_EXPR_MAX) return 0;
    calc_append_ch(key);
    return 1;
  }

  return 0; /* whitespace / unknown keys are ignored */
}

/* Menu 0 "Calc": 0=Clear, 1=Mem+, 2=Mem-, 3=MemRecall. */
static int calc_menu(int menu, int item) {
  if (menu != 0) return 0;
  switch (item) {
  case 0: /* Clear */
    calc_clear_calculation();
    return 1;
  case 1: /* Mem+ */
    calc_mem_add(calc_current_value());
    return 1;
  case 2: /* Mem- */
    calc_mem_sub(calc_current_value());
    return 1;
  case 3: /* MemRecall */
    return calc_mem_recall();
  default:
    return 0;
  }
}

/* ---- Keypad geometry / hit-testing ---- */

/* Key at a keypad grid cell; 0 when out of range. */
static char key_at(int row, int col) {
  if (row < 0 || row >= CALC_KEYPAD_ROWS) return 0;
  if (col < 0 || col >= CALC_KEYPAD_COLS) return 0;
  return CALC_KEYS[row][col];
}

/* Map a window-relative text-cell click to a keypad key; 0 when the click
 * is outside the keypad. */
static char key_at_click(int x, int y) {
  int row, col;
  if (x < 0 || x >= CALC_KEYPAD_COLS * CALC_CELL_W) return 0;
  if (y < CALC_ROW_KEYPAD || y >= CALC_ROW_KEYPAD + CALC_KEYPAD_ROWS) return 0;
  row = y - CALC_ROW_KEYPAD;
  col = x / CALC_CELL_W;
  return key_at(row, col);
}

/* ---- Event dispatch ---- */

/* Returns 1 when the screen needs a redraw. */
static int calc_handle_event(const struct gui_event *ev) {
  if (ev == 0) return 0;
  if (ev->type == GUI_EV_CHAR) return calc_press((char)ev->ch);
  if (ev->type == GUI_EV_MENU) return calc_menu(ev->menu, ev->item);
  if (ev->type == GUI_EV_MOUSE && ev->button == 1 && ev->state == GUI_MOUSE_PRESS) {
    char k = key_at_click(ev->x, ev->y);
    if (k != 0) return calc_press(k);
  }
  return 0;
}

/* ---- Rendering ---- */

struct calc_sb {
  char *buf;
  int   cap;
  int   len;
};

static void sb_init(struct calc_sb *b, char *buf, int cap) {
  b->buf = buf;
  b->cap = cap;
  b->len = 0;
  if (cap > 0) buf[0] = '\0';
}

static void sb_char(struct calc_sb *b, char c) {
  if (b->len < b->cap - 1) {
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
  }
}

static void sb_str(struct calc_sb *b, const char *s) {
  int i;
  for (i = 0; s[i] != '\0'; i++) sb_char(b, s[i]);
}

/* Right-align `text` in exactly `width` columns; if it is longer, the
 * rightmost `width` chars are kept.  out needs width+1 bytes. */
static int calc_right_align(char *out, int width, const char *text) {
  int len = calc_slen(text);
  int pad, i;
  if (len > width) { text += len - width; len = width; }
  pad = width - len;
  for (i = 0; i < pad; i++) out[i] = ' ';
  for (i = 0; i < len; i++) out[pad + i] = text[i];
  out[width] = '\0';
  return width;
}

/* The "> N" / "> Error text" result line.  Returns its length. */
static int calc_result_text(char *out, int cap) {
  char num[24];
  const char *body;
  int j = 0, i;
  if (cap <= 0) return 0;
  out[j++] = '>';
  out[j++] = ' ';
  if (calc_error == CALC_ERR_DIV0) body = "Error: division by zero";
  else if (calc_error == CALC_ERR_SYNTAX) body = "Error: incomplete expression";
  else {
    calc_i64_to_str(calc_current_value(), num);
    body = num;
  }
  for (i = 0; body[i] != '\0' && j < cap - 1; i++) out[j++] = body[i];
  out[j] = '\0';
  return j;
}

/* Render the complete screen into `out` (NUL-terminated) and return the
 * number of characters written, excluding the NUL.  Deterministic for a
 * given state; the host tests depend on that. */
static int calc_render(char *out, int cap) {
  struct calc_sb b;
  char line[CALC_DISPLAY_W + 1];
  char res[48];
  char mb[24];
  int r, c;

  if (out == 0 || cap <= 0) return 0;
  sb_init(&b, out, cap);

  /* Header */
  sb_str(&b, "=== Calculator ===");
  sb_char(&b, '\n');

  /* Expression, right-aligned */
  calc_right_align(line, CALC_DISPLAY_W, calc_expr);
  sb_str(&b, line);
  sb_char(&b, '\n');

  /* Result / error line, right-aligned */
  calc_result_text(res, (int)sizeof(res));
  calc_right_align(line, CALC_DISPLAY_W, res);
  sb_str(&b, line);
  sb_char(&b, '\n');

  /* 4x4 keypad; cells are CALC_CELL_W chars wide, 4 cells per row */
  for (r = 0; r < CALC_KEYPAD_ROWS; r++) {
    for (c = 0; c < CALC_KEYPAD_COLS; c++) {
      sb_char(&b, '[');
      sb_char(&b, ' ');
      sb_char(&b, key_at(r, c));
      sb_char(&b, ' ');
      sb_char(&b, ']');
    }
    sb_char(&b, '\n');
  }

  /* Memory line */
  sb_str(&b, "Memory: ");
  calc_i64_to_str(calc_memory, mb);
  sb_str(&b, mb);
  sb_char(&b, '\n');

  return b.len;
}

/* Clear the window and print the complete screen. */
static void calc_redraw(void) {
  static char screen[600];
  gui_clear();
  calc_render(screen, (int)sizeof(screen));
  print(screen);
}

/* ---- Entry point ---- */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
  gui_set_title("Calculator");
  gui_add_menu(0, "Calc", "Clear,Mem+,Mem-,MemRecall");
  gui_enable_mouse();
  print_console("[APP] CALC started\n");

  calc_redraw();

  for (;;) {
    struct gui_event ev;
    if (gui_read_event(&ev) && calc_handle_event(&ev)) {
      calc_redraw();
    }
  }

#ifdef HOST_TEST
  return 0;
#endif
}
