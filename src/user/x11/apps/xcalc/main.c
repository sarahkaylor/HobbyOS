/*
 * __                _     _
 *  / _|_   _____ __ _| | __| | ___ _ __ __ _ _ __
 * | |_\ \ / / __/ _` | |/ _` |/ _ \ '__/ _` | '_ \
 * |  _|\ V / (_| (_| | | (_| |  __/ | | (_| | | | |
 * |_|   \_/ \___\__,_|_|\__,_|\___|_|  \__,_|_| |_|
 *
 * xcalc - Skalculator for HobbyOS.
 *
 * A port of Skalculator (https://github.com/fvcalderan/Skalculator), a
 * single-file RPN calculator by Felipe V. Calderan (BSD 3-Clause; the
 * original license ships alongside as LICENSE.txt).  The original is a
 * pure-Xlib program; here it runs on HobbyOS's X11 support library
 * (src/user/x11/, a small Xlib on the desktop's pixel-mode windows) and
 * therefore on the tiling desktop.
 *
 * Platform adaptations (all documented in README.md):
 *
 *   - Fixed-point core.  HobbyOS user space has no floating point
 *     (-mgeneral-regs-only), so the double stack became an int64
 *     fixed-point stack in milliunits (FIX = 1000).  Values saturate at
 *     +/-2^31-1 milliunits (about +/-2.1e6), which keeps every operation
 *     inside int64; '^' supports integer exponents only and divisions by
 *     zero display "inf" like the original's double math did.
 *
 *   - Tile-responsive layout.  The keypad grid and the four text lines
 *     (three stack rows + entry) scale to whatever content rectangle the
 *     desktop hands the window, the grid is capped and centered on big
 *     tiles, and everything relayouts/redraws when the tiling changes
 *     (Expose/ConfigureNotify).
 *
 *   - Keyboard support (the original is mouse-only): digits, '.', the
 *     operators, Enter (push), Backspace (clear entry), Esc (all clear),
 *     plus single letters for SWAP/POP/inverse/percentage/+/-.
 *
 *   - Non-interactive mode reads its file through the HobbyOS libc
 *     (open/read) instead of stdio's fopen/fgets.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <fcntl.h>
#include <libc.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* ---- constants (the original's, retuned for the 8x8 font) ------------ */

#define DISPLAY_LEN 40      /* entry/staging text (original: 256) */
#define STACK_SIZE 64       /* RPN stack depth (original: 256) */
#define MAX_TOKENS 64       /* non-interactive tokens (original: 1024) */
#define MAX_BUF_SIZE 1024   /* input line size (original: 4096) */

static const char *const buttons[6][4] = {
  { "C", "AC", "POP", "SWAP" },
  { "%", "^", "1/x", "+/-" },
  { "7", "8", "9", "+" },
  { "4", "5", "6", "-" },
  { "1", "2", "3", "*" },
  { ".", "0", "ENTER", "/" },
};

/* ---- fixed-point core ------------------------------------------------ */

#define FIX 1000LL                    /* milliunits */
#define FIX_MAX 2147483647LL          /* +/-2^31-1: every op stays in int64 */

static long long fix_sat(long long v) {
  if (v > FIX_MAX) return FIX_MAX;
  if (v < -FIX_MAX) return -FIX_MAX;
  return v;
}

/* a*b in milliunits (|a|,|b| <= 2^31, so |a*b| <= 2^62: no overflow). */
static long long fix_mul(long long a, long long b) {
  return fix_sat((a * b) / FIX);
}

/* a/b in milliunits; returns 0 on division by zero (caller shows "inf"). */
static int fix_div(long long a, long long b, long long *out) {
  if (b == 0) {
    *out = 0;
    return 0;
  }
  *out = fix_sat((a * FIX) / b);
  return 1;
}

/* base^exp for integer exponents in [-64, 64]; 0 when unsupported or 0^-n. */
static int fix_pow(long long base, long long exp_fix, long long *out) {
  if (exp_fix % FIX != 0) return 0;             /* fractional exponent */
  long long e = exp_fix / FIX;
  if (e < -64 || e > 64) return 0;
  int neg = e < 0;
  if (neg) e = -e;
  long long result = FIX;
  for (long long i = 0; i < e; i++) result = fix_mul(result, base);
  if (neg) {
    long long inv;
    if (!fix_div(FIX, result, &inv)) return 0;
    result = inv;
  }
  *out = result;
  return 1;
}

/* Parse the entry text ("-12.5") into milliunits, saturating. */
static long long parse_fixed(const char *s) {
  int sign = 1;
  int i = 0;
  if (s[0] == '-') {
    sign = -1;
    i = 1;
  }
  long long ip = 0;
  int digits = 0;
  while (s[i] >= '0' && s[i] <= '9' && digits < 9) {
    ip = ip * 10 + (s[i] - '0');
    i++;
    digits++;
  }
  while (s[i] >= '0' && s[i] <= '9') i++;       /* saturate the rest */
  long long fp = 0;
  int fdigits = 0;
  if (s[i] == '.') {
    i++;
    while (s[i] >= '0' && s[i] <= '9' && fdigits < 3) {
      fp = fp * 10 + (s[i] - '0');
      i++;
      fdigits++;
    }
    while (fdigits < 3) {
      fp *= 10;
      fdigits++;
    }
  }
  return fix_sat(sign * (ip * FIX + fp));
}

/* Render milliunits like the original's %g did for its ranges:
 * "-12.5", "0", "1.234"; trailing zeros trimmed, no exponent form. */
static void format_fixed(long long v, char *buf) {
  int j = 0;
  if (v < 0) {
    buf[j++] = '-';
    v = -v;
  }
  long long ip = v / FIX;
  long long fp = v % FIX;
  char tmp[24];
  int d = 0;
  if (ip == 0) {
    tmp[d++] = '0';
  }
  while (ip > 0) {
    tmp[d++] = (char)('0' + (int)(ip % 10));
    ip /= 10;
  }
  while (d > 0) buf[j++] = tmp[--d];
  if (fp) {
    char f[3];
    f[0] = (char)('0' + (int)((fp / 100) % 10));
    f[1] = (char)('0' + (int)((fp / 10) % 10));
    f[2] = (char)('0' + (int)(fp % 10));
    int n = 3;
    while (n > 1 && f[n - 1] == '0') n--;
    buf[j++] = '.';
    for (int k = 0; k < n; k++) buf[j++] = f[k];
  }
  buf[j] = '\0';
}

/* ---- state (the original's, in fixed point) -------------------------- */

static Display *display;
static Window window;
static GC gc;
static XEvent event;

static char display_text[DISPLAY_LEN] = "0";
static char display_text_aux[DISPLAY_LEN];
static long long stack[STACK_SIZE];
static int head = 0;

/* ---- layout: everything scales to the content rectangle ------------- */

struct key_rect {
  int x, y, w, h;
};

struct xcalc_layout {
  int header_h;                 /* stack + entry line area, in pixels */
  int line_spacing;             /* baseline spacing of the four lines */
  struct key_rect keys[6][4];
};

#define KEY_W_MAX 120             /* caps so a full-screen tile is not */
#define KEY_H_MAX 64              /* four absurdly giant keys           */

/* Pure geometry: fit the 4x6 keypad plus the four text lines into a
 * cw x ch content rectangle.  Recomputable at any size (reflows). */
static void layout_compute(int cw, int ch, struct xcalc_layout *L) {
  if (cw < 8) cw = 8;
  if (ch < 8) ch = 8;

  /* Header: 16% of the height for four 8px text lines; kept sane for
   * very short tiles. */
  int header = ch * 16 / 100;
  int hmax = ch - 40;
  if (hmax < 24) hmax = 24;
  if (header > hmax) header = hmax;
  if (header < 24) header = 24;
  L->header_h = header;
  L->line_spacing = header / 4;
  if (L->line_spacing < 8) L->line_spacing = 8;

  int margin = 4;
  int pad_top = header + 2;
  int avail_h = ch - pad_top - margin;
  if (avail_h < 6) avail_h = 6;
  int avail_w = cw - 2 * margin;
  if (avail_w < 4) avail_w = 4;

  int cell_w = avail_w / 4;
  int cell_h = avail_h / 6;
  if (cell_w > KEY_W_MAX) cell_w = KEY_W_MAX;
  if (cell_h > KEY_H_MAX) cell_h = KEY_H_MAX;

  int grid_w = cell_w * 4;
  int gx = (cw - grid_w) / 2;                 /* centered on big tiles */
  for (int row = 0; row < 6; row++) {
    for (int col = 0; col < 4; col++) {
      L->keys[row][col].x = gx + col * cell_w;
      L->keys[row][col].y = pad_top + row * cell_h;
      L->keys[row][col].w = cell_w;
      L->keys[row][col].h = cell_h;
    }
  }
}

static struct xcalc_layout g_layout;
static int g_layout_valid = 0;

/* Layout for the current content size (kept fresh through reflows). */
static const struct xcalc_layout *current_layout(void) {
  XWindowAttributes wa;
  if (XGetWindowAttributes(display, window, &wa) &&
      (wa.width != 0 || wa.height != 0)) {
    layout_compute(wa.width, wa.height, &g_layout);
    g_layout_valid = 1;
  }
  if (!g_layout_valid) layout_compute(320, 480, &g_layout);
  return &g_layout;
}

/* Which key label sits at content pixel (x,y)?  NULL outside the keypad. */
static const char *key_at(const struct xcalc_layout *L, int x, int y) {
  for (int row = 0; row < 6; row++) {
    for (int col = 0; col < 4; col++) {
      const struct key_rect *k = &L->keys[row][col];
      if (x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h) {
        return buttons[row][col];
      }
    }
  }
  return 0;
}

/* ---- drawing --------------------------------------------------------- */

static void draw_text_at(int x, int y, const char *s) {
  XDrawString(display, window, gc, x, y, s, (int)strlen(s));
}

static void draw_screen(void) {
  const struct xcalc_layout *L = current_layout();

  XClearWindow(display, window);

  /* Print stack (three lines, oldest at the top, like the original). */
  int sp = L->line_spacing;
  for (int i = 2; i >= 0; i--) {
    if (head - i >= 0) {
      format_fixed(stack[head - i], display_text_aux);
    } else {
      strcpy(display_text_aux, "0");
    }
    draw_text_at(10, (3 - i) * sp, display_text_aux);
  }

  /* Write the entry line under the stack. */
  draw_text_at(10, 4 * sp, display_text);

  /* Draw the keypad. */
  for (int row = 0; row < 6; row++) {
    for (int col = 0; col < 4; col++) {
      const struct key_rect *k = &L->keys[row][col];
      XDrawRectangle(display, window, gc, k->x, k->y, k->w - 1, k->h - 1);
      const char *label = buttons[row][col];
      int len = (int)strlen(label);
      int lx = k->x + (k->w - 8 * len) / 2;
      int ly = k->y + (k->h + 6) / 2;         /* 8px glyphs, centered */
      draw_text_at(lx, ly, label);
    }
  }
}

/* ---- calculator behaviour (faithful to the original) ----------------- */

static void update_display(char *text) {
  if (strcmp(display_text, "0") == 0) {
    strncpy(display_text, text, sizeof(display_text) - 1);
    display_text[sizeof(display_text) - 1] = '\0';
  } else {
    strncat(display_text, text,
            sizeof(display_text) - strlen(display_text) - 1);
  }
  draw_screen();
}

static void clear_stack(void) {
  for (int i = 0; i < STACK_SIZE; i++) stack[i] = 0;
}

/* Push the entry to the stack. */
static void push(void) {
  head++;
  if (head >= STACK_SIZE) head = STACK_SIZE - 1;   /* bounded vs original */
  stack[head] = parse_fixed(display_text);
  strcpy(display_text, "0");
  draw_screen();
}

/* Pop the head of the stack.  (head == 0 reads stack[1] like the
 * original did -- both are 0 after clear_stack.) */
static void pop(void) {
  if (head > 0) {
    head--;
    format_fixed(stack[head + 1], display_text);
  } else {
    clear_stack();
    format_fixed(stack[head + 1], display_text);
  }
  draw_screen();
}

/* Swap the entry with the head of the stack. */
static void swap(void) {
  long long curr_head = stack[head];
  stack[head] = parse_fixed(display_text);
  format_fixed(curr_head, display_text);
  draw_screen();
}

/* Add a decimal point (only if there is none yet). */
static void decimal(void) {
  if (strchr(display_text, '.') == NULL) {
    size_t len = strlen(display_text);
    if (len + 1 < sizeof(display_text)) {
      display_text[len] = '.';
      display_text[len + 1] = '\0';
    }
  }
  draw_screen();
}

/* Compute the operation between the stack head and the entry. */
static void compute_operation(char op) {
  long long other = parse_fixed(display_text);
  long long result = 0;
  int finite = 1;

  switch (op) {
  case '+':
    result = fix_sat(stack[head] + other);
    break;
  case '-':
    result = fix_sat(stack[head] - other);
    break;
  case '*':
    result = fix_mul(stack[head], other);
    break;
  case '/':
    finite = fix_div(stack[head], other, &result);
    break;
  case '^':
    finite = fix_pow(stack[head], other, &result);
    break;
  default:
    break;
  }

  if (head > 0) {
    head--;
  } else {
    clear_stack();
  }

  if (finite) {
    format_fixed(result, display_text);
  } else if (op == '/') {
    strcpy(display_text, "inf");        /* x/0, like the original's %g */
  } else {
    strcpy(display_text, "E");          /* unsupported power */
  }
  draw_screen();
}

/* Percentage of the stack head (a * b/100, like the original): b/100 is a
 * ratio, so the divisor is 100 in *value* space = 100*FIX in milliunits. */
static void percentage(void) {
  long long d = parse_fixed(display_text);
  long long result = fix_sat((stack[head] * d) / (100 * FIX));
  format_fixed(result, display_text);
  draw_screen();
}

/* 1/x of the entry. */
static void inverse(void) {
  long long d = parse_fixed(display_text);
  long long result = 0;
  if (fix_div(FIX, d, &result)) {
    format_fixed(result, display_text);
  } else {
    strcpy(display_text, "inf");
  }
  draw_screen();
}

/* Clear the entry and the whole stack. */
static void all_clear(void) {
  strcpy(display_text, "0");
  clear_stack();
  draw_screen();
}

/* Clear the entry. */
static void clear(void) {
  strcpy(display_text, "0");
  draw_screen();
}

/* Switch between +display and -display. */
static void plus_minus(void) {
  if (strcmp(display_text, "0") == 0) return;

  if (strcmp(display_text, "inf") == 0 || strcmp(display_text, "E") == 0) {
    return;
  }

  size_t length = strlen(display_text);

  if (display_text[0] == '-') {
    for (size_t i = 0; i < length; i++) {
      display_text[i] = display_text[i + 1];
    }
  } else {
    if (length + 1 < sizeof(display_text)) {
      for (int i = (int)length; i >= 0; i--) {
        display_text[i + 1] = display_text[i];
      }
      display_text[0] = '-';
    }
  }

  draw_screen();
}

/* Process a pressed key label (mouse or keyboard both end up here). */
static void process_label(const char *label) {
  if (strcmp(label, "C") == 0) {
    clear();
  } else if (strcmp(label, "AC") == 0) {
    all_clear();
  } else if (strcmp(label, "POP") == 0) {
    pop();
  } else if (strcmp(label, "SWAP") == 0) {
    swap();
  } else if (strcmp(label, "%") == 0) {
    percentage();
  } else if (strcmp(label, "1/x") == 0) {
    inverse();
  } else if (strcmp(label, "+/-") == 0) {
    plus_minus();
  } else if (strcmp(label, ".") == 0) {
    decimal();
  } else if (strcmp(label, "ENTER") == 0) {
    push();
  } else if (strcmp(label, "+") == 0 || strcmp(label, "-") == 0 ||
             strcmp(label, "*") == 0 || strcmp(label, "/") == 0 ||
             strcmp(label, "^") == 0) {
    compute_operation(label[0]);
  } else {
    update_display((char *)label);
  }
}

/* ---- non-interactive mode (argv, like the original) ------------------ */

typedef struct {
  char **tokens;
  int token_count;
} Tokenized;

static Tokenized tokenize(char *str) {
  static char *tokens[MAX_TOKENS];
  int token_count = 0;
  char *token = strtok(str, " \t\n");
  while (token != NULL && token_count < MAX_TOKENS) {
    tokens[token_count++] = token;
    token = strtok(NULL, " \t\n");
  }
  Tokenized tk = { tokens, token_count };
  return tk;
}

/* One line of a file, through the HobbyOS libc (no stdio here). */
static char *load_file(const char *filename) {
  char *buffer = malloc(MAX_BUF_SIZE);
  if (!buffer) exit(1);
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    print_console("xcalc: cannot open input file\n");
    exit(1);
  }
  int n = read(fd, buffer, MAX_BUF_SIZE - 1);
  close(fd);
  if (n < 0) n = 0;
  buffer[n] = '\0';
  for (int i = 0; i < n; i++) {
    if (buffer[i] == '\n') {
      buffer[i] = '\0';
      break;
    }
  }
  return buffer;
}

static Tokenized process_args(int argc, char **argv) {
  Tokenized empty = { 0, 0 };
  if (argc < 2) return empty;

  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    print_console("xcalc - Skalculator for HobbyOS\n");
    print_console("Usage: xcalc [-h, --help] [-f FILE_NAME, --file FILE_NAME]\n");
    print_console("       xcalc '2 3 +'\n");
    exit(0);
  }

  if (strcmp(argv[1], "-f") == 0 || strcmp(argv[1], "--file") == 0) {
    if (argc > 2) {
      return tokenize(load_file(argv[2]));
    }
    print_console("xcalc: --file (-f) expects a file name.\n");
    exit(1);
  }

  return tokenize(argv[1]);
}

/* ---- keyboard (new: the original is mouse-only) ---------------------- */

static void handle_key(KeySym ks, const char *buf, int n) {
  if (ks == XK_Return) {
    push();
    return;
  }
  if (ks == XK_Escape) {
    all_clear();
    return;
  }
  if (ks == XK_BackSpace) {
    clear();
    return;
  }
  if (n < 1) return;
  char c = buf[0];
  if (c >= '0' && c <= '9') {
    char tmp[2] = { c, '\0' };
    update_display(tmp);
    return;
  }
  switch (c) {
  case '.': decimal(); break;
  case '+': case '-': case '*': case '/': case '^':
    compute_operation(c);
    break;
  case '%': percentage(); break;
  case 's': case 'S': swap(); break;
  case 'p': case 'P': pop(); break;
  case 'c': case 'C': clear(); break;
  case 'a': case 'A': all_clear(); break;
  case 'i': case 'I': inverse(); break;
  case 'n': case 'N': plus_minus(); break;
  default: break;
  }
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv) {
  print_console("[APP] XCALC started\n");

  /* Get non-interactive tokens (argv), like the original. */
  Tokenized tk = process_args(argc, argv);

  /* Start display. */
  display = XOpenDisplay(NULL);
  if (display == NULL) {
    print_console("xcalc: cannot open display\n");
    exit(1);
  }

  /* Set screen and window: a comfortable minimum; the desktop tiles it
   * to whatever the reflow gives us (the layout adapts). */
  int screen = DefaultScreen(display);
  window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                               320, 480, 0, WhitePixel(display, screen),
                               BlackPixel(display, screen));

  /* Setup input (StructureNotify so reflows also redraw). */
  XSelectInput(display, window,
               ExposureMask | KeyPressMask | ButtonPressMask |
               StructureNotifyMask);
  XStoreName(display, window, "xcalc");
  XMapWindow(display, window);

  /* Create context and set the foreground colour. */
  gc = XCreateGC(display, window, 0, 0);
  XSetForeground(display, gc, WhitePixel(display, screen));

  /* Run non-interactive tokens before the event loop. */
  if (tk.tokens != NULL) {
    for (int i = 0; i < tk.token_count; i++) {
      /* A valid number token is followed by an implicit ENTER.  (Unlike
       * the original's set, 'e' is not accepted: no exponent notation in
       * the fixed-point core -- see README.md.) */
      if (strcmp(tk.tokens[i], "+") != 0 && strcmp(tk.tokens[i], "-") != 0 &&
          strspn(tk.tokens[i], "0123456789+-.") == strlen(tk.tokens[i]) &&
          strlen(tk.tokens[i]) > 0) {
        process_label("ENTER");
      }
      process_label(tk.tokens[i]);
    }
  }

  /* Loop. */
  while (1) {
    XNextEvent(display, &event);

    /* Redraw on expose and on every reflow. */
    if (event.type == Expose || event.type == ConfigureNotify) {
      draw_screen();
    }

    /* Mouse: exact keypad hit areas (scaled with the tile). */
    if (event.type == ButtonPress) {
      const char *label = key_at(current_layout(), event.xbutton.x,
                                 event.xbutton.y);
      if (label != NULL) process_label(label);
    }

    /* Keyboard. */
    if (event.type == KeyPress) {
      KeySym ks = 0;
      char buf[8];
      int n = XLookupString(&event.xkey, buf, sizeof(buf), &ks, 0);
      handle_key(ks, buf, n);
    }
  }

  XCloseDisplay(display);
  return 0;
}
