/* hb_curses.c — the HobbyOS curses shim (see curses.h for the design).
 *
 * Renders full-screen "curses" programs into a HobbyOS desktop window by
 * driving its terminal mode (ANSI in, key sequences out) and keeping a
 * cell grid + physical-screen model so each doupdate() sends only the
 * bytes that changed.
 *
 * Everything here is deliberately testable: the transport is an injectable
 * function table (hb_set_io) so host tests can run a whole curses program
 * against a captured byte stream without a desktop.
 */

#include "curses.h"

#ifdef HOST_TEST
#include <string.h>
#include <stdlib.h>
#else
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
/* HobbyOS syscall wrapper (implementation in user/libc.c, ships in libc.a):
 * returns the number of bytes readable on fd, 0 when the pipe is empty,
 * and -1 when every writer has closed (i.e. the window is gone). */
extern int available(int fd);
#endif

/* The desktop's terminal grid maximum (must match window.h's
 * TERM_MAX_ROWS/TERM_MAX_COLS: the desktop never sends a larger size). */
#define HBC_MAX_ROWS 72
#define HBC_MAX_COLS 128
#define HBC_CELLS (HBC_MAX_ROWS * HBC_MAX_COLS)

/* ====================================================================== */
/* Transport                                                              */
/* ====================================================================== */

#ifndef HOST_TEST
static int def_read_block(char *buf, int cap) {
  for (;;) {
    int a = available(0);
    if (a < 0) return -1;                 /* writers gone: EOF */
    if (a > 0) {
      if (a > cap) a = cap;
      int r = (int)read(0, buf, a);
      if (r > 0) return r;
      if (r == 0) return -1;              /* treat as EOF */
      return r;                           /* real error */
    }
    usleep(1000);
  }
}

static int def_read_now(char *buf, int cap) {
  int a = available(0);
  if (a < 0) return -1;
  if (a == 0) return 0;
  if (a > cap) a = cap;
  int r = (int)read(0, buf, a);
  return r;
}

static int def_write(const char *buf, int len) {
  int done = 0;
  while (done < len) {
    int w = (int)write(1, buf + done, len - done);
    if (w <= 0) return -1;
    done += w;
  }
  return done;
}

static void def_sleep_ms(int ms) { if (ms > 0) usleep((unsigned int)ms * 1000u); }
#else
static int def_read_block(char *buf, int cap) { (void)buf; (void)cap; return -1; }
static int def_read_now(char *buf, int cap) { (void)buf; (void)cap; return 0; }
static int def_write(const char *buf, int len) { (void)buf; (void)len; return len; }
static void def_sleep_ms(int ms) { (void)ms; }
#endif

static int (*io_read_block)(char *, int) = def_read_block;
static int (*io_read_now)(char *, int) = def_read_now;
static int (*io_write)(const char *, int) = def_write;
static void (*io_sleep)(int) = def_sleep_ms;

void hb_set_io(const struct hb_io *io) {
  if (!io) {
    io_read_block = def_read_block;
    io_read_now = def_read_now;
    io_write = def_write;
    io_sleep = def_sleep_ms;
    return;
  }
  if (io->read_block) io_read_block = io->read_block;
  if (io->read_now) io_read_now = io->read_now;
  if (io->write) io_write = io->write;
  if (io->sleep_ms) io_sleep = io->sleep_ms;
}

/* ====================================================================== */
/* Output emitter                                                         */
/* ====================================================================== */

static char obuf[4096];
static int olen = 0;

static void oflush(void) {
  if (olen > 0) {
    io_write(obuf, olen);
    olen = 0;
  }
}
static void oputc(char c) {
  if (olen >= (int)sizeof obuf - 1) oflush();
  obuf[olen++] = c;
}
static void oputs(const char *s) {
  while (*s) oputc(*s++);
}
static void oput_int(int v) {
  char tmp[12];
  int n = 0;
  if (v < 0) { oputc('-'); v = -v; }
  if (v == 0) tmp[n++] = '0';
  while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  while (n > 0) oputc(tmp[--n]);
}

/* ====================================================================== */
/* Screen model                                                           */
/* ====================================================================== */

int LINES = 24, COLS = 80;

static unsigned char vs_ch[HBC_CELLS];   /* virtual screen: what programs drew */
static unsigned char vs_at[HBC_CELLS];
static unsigned char ps_ch[HBC_CELLS];   /* physical: what the desktop shows    */
static unsigned char ps_at[HBC_CELLS];
static int ps_valid = 0;                 /* ps matches the desktop's grid      */

static int scr_rows = 0, scr_cols = 0;   /* adopted terminal size              */
static int term_ok = 0;                  /* handshake done                     */

static int cur_row = 0, cur_col = 0;     /* where the screen cursor should be  */
static int cursor_shown = 1;             /* as the desktop believes            */
static int cursor_target = 1;            /* as the program wants               */
static int pen_attr = 0;                 /* SGR state as the desktop believes  */

static WINDOW stdscr_store;
WINDOW *stdscr = &stdscr_store;

static WINDOW curscr_store;
WINDOW *curscr = &curscr_store;    /* only for wrefresh(curscr) (full repaint) */

/* Defined with the screen-model helpers below; adopt_size() re-fits stdscr
 * when a resize arrives. */
static void stdscr_ensure_cells(void);

static int ended = 0;                    /* between endwin() and refresh()     */

static void scr_blank(int rows, int cols) {
  memset(vs_ch, ' ', sizeof vs_ch);
  memset(vs_at, 0, sizeof vs_at);
  (void)rows; (void)cols;
}

static int cell_index(int row, int col) { return row * scr_cols + col; }

/* ---- resize adoption ---- */

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static int adopt_size(int rows, int cols) {
  rows = clamp_int(rows, 1, HBC_MAX_ROWS);
  cols = clamp_int(cols, 1, HBC_MAX_COLS);
  if (rows == scr_rows && cols == scr_cols) return 0;
  scr_rows = rows;
  scr_cols = cols;
  LINES = rows;
  COLS = cols;
  scr_blank(rows, cols);     /* old-pitch content is meaningless now */
  ps_valid = 0;
  cur_row = clamp_int(cur_row, 0, rows - 1);
  cur_col = clamp_int(cur_col, 0, cols - 1);
  stdscr_ensure_cells();     /* re-fit stdscr's cells to the new size */
  return 1;
}

/* ====================================================================== */
/* Input side: byte stream -> key codes                                   */
/* ====================================================================== */

#define HB_NEED_MORE (-1)   /* want more bytes                     */
#define HB_SWALLOWED (-2)   /* consumed a sequence, no key emitted */
#define HB_EOF       (-3)

static unsigned char in_buf[2048];
static int in_len = 0, in_pos = 0;
static int unget_key = -1;
static int res_pending = 0;
static int eof_seen = 0;
static int nodelay_mode = 0;
static int halfdelay_tenths = 0;

static void in_compact(void) {
  if (in_pos > 0) {
    int keep = in_len - in_pos;
    if (keep > 0) memmove(in_buf, in_buf + in_pos, keep);
    in_len = keep;
    in_pos = 0;
  }
}

/* Read whatever is available into the parse buffer (never blocks). */
static void in_fill_now(void) {
  in_compact();
  if (in_len >= (int)sizeof in_buf) return;
  int r = io_read_now((char *)in_buf + in_len, (int)sizeof in_buf - in_len);
  if (r < 0) eof_seen = 1;
  else if (r > 0) in_len += r;
}

/* Blocking read of at least one byte (polls so it stays interruptible). */
static void in_fill_block(void) {
  in_compact();
  for (;;) {
    if (in_len > 0 && in_pos < in_len) return;
    if (eof_seen) return;
    int r = io_read_block((char *)in_buf + in_len,
                          (int)sizeof in_buf - in_len);
    if (r < 0) { eof_seen = 1; return; }
    if (r > 0) { in_len += r; return; }
  }
}

/* Wait (bounded) for the tail of a possibly-split escape sequence. */
static int in_grace_wait(void) {
  for (int i = 0; i < 30; i++) {
    in_fill_now();
    if (in_pos < in_len || eof_seen) return in_pos < in_len;
    io_sleep(1);
  }
  return 0;
}

static int parse_number(const unsigned char *p, int start, int end, int *out) {
  /* Parse digits in [start,end); returns the index just past them. */
  int v = 0, i = start, any = 0;
  while (i < end && p[i] >= '0' && p[i] <= '9') {
    v = v * 10 + (p[i] - '0');
    any = 1;
    i++;
  }
  if (out) *out = any ? v : 0;
  return i;
}

static int key_from_csi_final(int final, const unsigned char *p, int plen) {
  int a = 0, b = 0;
  if (plen > 0) {
    int i = parse_number(p, 0, plen, &a);
    if (i < plen && p[i] == ';') parse_number(p, i + 1, plen, &b);
  }
  switch (final) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'Z': return KEY_BTAB;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'E': return KEY_CLEAR;
    case 'P': case 'G': case 'R': return HB_SWALLOWED;  /* mouse press/drag/release */
    case '~':
      switch (a) {
        case 1: case 7: return KEY_HOME;
        case 2: return KEY_IC;
        case 3: return KEY_DC;
        case 4: case 8: return KEY_END;
        case 5: return KEY_PPAGE;
        case 6: return KEY_NPAGE;
        case 11: return KEY_F(1);
        case 12: return KEY_F(2);
        case 13: return KEY_F(3);
        case 14: return KEY_F(4);
        case 15: return KEY_F(5);
        case 17: return KEY_F(6);
        case 18: return KEY_F(7);
        case 19: return KEY_F(8);
        case 20: return KEY_F(9);
        case 21: return KEY_F(10);
        default: return HB_SWALLOWED;
      }
    default: return HB_SWALLOWED;
  }
}

/* Handle an OSC message from the desktop: "]S <rows>;<cols>". */
static void handle_osc(const unsigned char *p, int len) {
  if (len >= 4 && p[0] == ']' && p[1] == 'S') {
    int i = 2;
    /* skip spaces */
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    int rows = 0, cols = 0;
    i = parse_number(p, i, len, &rows);
    if (i < len && p[i] == ';') {
      i++;
      parse_number(p, i, len, &cols);
    }
    if (rows > 0 && cols > 0) {
      if (adopt_size(rows, cols)) res_pending = 1;
    }
  }
}

/* Parse one key from the input buffer.
 *   > 0  : key code
 *   HB_NEED_MORE / HB_SWALLOWED / HB_EOF
 */
static int parse_key(void) {
  if (in_pos >= in_len) return HB_NEED_MORE;
  int c = in_buf[in_pos];

  if (c != 27) {
    in_pos++;
    /* The desktop sends '\n' for its Enter key; nano (like every curses
     * program on a real terminal, where Enter is CR) wants '\r'. */
    if (c == '\n') return '\r';
    return c;
  }

  /* ESC: need at least one more byte to know what this is. */
  int avail = in_len - in_pos;
  if (avail == 1) {
    if (eof_seen || !in_grace_wait()) { in_pos++; return 27; }
    avail = in_len - in_pos;
  }

  int c1 = in_buf[in_pos + 1];

  if (c1 == '[') {
    /* CSI: params are 0x30..0x3F, final byte 0x40..0x7E. */
    int j = in_pos + 2;
    while (j < in_len && in_buf[j] >= 0x20 && in_buf[j] <= 0x3F) j++;
    if (j >= in_len) {
      if (!eof_seen && in_grace_wait()) {
        j = in_pos + 2;
        while (j < in_len && in_buf[j] >= 0x20 && in_buf[j] <= 0x3F) j++;
      }
      if (j >= in_len) { in_pos++; return 27; }
    }
    int final = in_buf[j];
    int plen = j - (in_pos + 2);
    unsigned char params[32];
    if (plen > 31) plen = 31;
    for (int k = 0; k < plen; k++) params[k] = in_buf[in_pos + 2 + k];
    /* strip a leading '?' (mode set/reset) */
    int pstart = 0;
    if (plen > 0 && params[0] == '?') pstart = 1;
    int key = key_from_csi_final(final, params + pstart, plen - pstart);
    in_pos = j + 1;
    if (final == 'P' || final == 'G' || final == 'R') {
      /* The desktop's mouse protocol ends with '~': ESC [ P <c>;<r>;<b> ~ */
      while (in_pos < in_len && in_buf[in_pos] != '~') in_pos++;
      if (in_pos < in_len) in_pos++;
    }
    if (key == HB_SWALLOWED) return HB_SWALLOWED;
    return key;
  }

  if (c1 == ']') {
    /* OSC: scan for the '~' (or BEL) terminator. */
    int j = in_pos + 2;
    while (j < in_len && in_buf[j] != '~' && in_buf[j] != '\a') j++;
    if (j >= in_len) {
      if (!eof_seen && in_grace_wait()) {
        j = in_pos + 2;
        while (j < in_len && in_buf[j] != '~' && in_buf[j] != '\a') j++;
      }
      if (j >= in_len) {
        if (in_len - in_pos >= 200) { in_pos = in_len; }
        return HB_NEED_MORE;
      }
    }
    handle_osc(in_buf + in_pos + 1, j - in_pos - 1);  /* "]S <rows>;<cols>" */
    in_pos = j + 1;
    return HB_SWALLOWED;
  }

  if (c1 == 'O') {
    /* SS3: ESC O <final> (function keys on some terminals). */
    if (avail < 3) {
      if (!eof_seen && in_grace_wait()) avail = in_len - in_pos;
    }
    if (avail >= 3) {
      int final = in_buf[in_pos + 2];
      in_pos += 3;
      switch (final) {
        case 'P': return KEY_F(1);
        case 'Q': return KEY_F(2);
        case 'R': return KEY_F(3);
        case 'S': return KEY_F(4);
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default: return HB_SWALLOWED;
      }
    }
    in_pos++;
    return 27;
  }

  /* ESC + ordinary byte: emit ESC and let the next byte be read on its
   * own; nano composes its own Meta-<key> handling from that. */
  in_pos++;
  return 27;
}

/* ====================================================================== */
/* Window primitives                                                      */
/* ====================================================================== */

static unsigned char attr_to_bits(int attr) {
  unsigned char bits = 0;
  if (attr & (A_REVERSE | A_STANDOUT)) bits |= 0x01;
  if (attr & A_BOLD) bits |= 0x02;
  if (attr & (A_UNDERLINE | A_ITALIC)) bits |= 0x04;
  return bits;
}

WINDOW *newwin(int rows, int cols, int y, int x) {
  if (rows < 0 || cols < 0) return NULL;
  if (rows == 0 || cols == 0) return NULL;
  if (y < 0 || x < 0) return NULL;
  WINDOW *win = (WINDOW *)malloc(sizeof(WINDOW));
  if (!win) return NULL;
  win->rows = rows;
  win->cols = cols;
  win->y = y;
  win->x = x;
  win->cur_y = 0;
  win->cur_x = 0;
  win->attr = 0;
  win->scroll = 0;
  win->ch = (unsigned char *)malloc((size_t)rows * cols);
  win->at = (unsigned char *)malloc((size_t)rows * cols);
  if (!win->ch || !win->at) {
    free(win->ch);
    free(win->at);
    free(win);
    return NULL;
  }
  memset(win->ch, ' ', (size_t)rows * cols);
  memset(win->at, 0, (size_t)rows * cols);
  return win;
}

int delwin(WINDOW *win) {
  if (!win || win == &stdscr_store) return ERR;
  free(win->ch);
  free(win->at);
  free(win);
  return OK;
}

static void win_scroll_up(WINDOW *win, int n) {
  if (n <= 0 || n >= win->rows) {
    memset(win->ch, ' ', (size_t)win->rows * win->cols);
    memset(win->at, 0, (size_t)win->rows * win->cols);
    return;
  }
  memmove(win->ch, win->ch + (size_t)n * win->cols,
          (size_t)(win->rows - n) * win->cols);
  memmove(win->at, win->at + (size_t)n * win->cols,
          (size_t)(win->rows - n) * win->cols);
  memset(win->ch + (size_t)(win->rows - n) * win->cols, ' ', (size_t)n * win->cols);
  memset(win->at + (size_t)(win->rows - n) * win->cols, 0, (size_t)n * win->cols);
}

int wmove(WINDOW *win, int y, int x) {
  if (!win) return ERR;
  if (y < 0 || y >= win->rows || x < 0 || x >= win->cols) return ERR;
  win->cur_y = y;
  win->cur_x = x;
  return OK;
}

int wscrl(WINDOW *win, int n) {
  if (!win) return ERR;
  if (n > 0 && win->scroll) win_scroll_up(win, n);
  return OK;
}

static void win_put(WINDOW *win, unsigned char ch, unsigned char at) {
  if (!win->ch) return;
  /* '\\n': down (scrolling only when scrollok), '\\r': to column 0. */
  if (ch == '\n') {
    win->cur_x = 0;
    if (win->cur_y + 1 < win->rows) win->cur_y++;
    else if (win->scroll) win_scroll_up(win, 1);
    return;
  }
  if (ch == '\r') { win->cur_x = 0; return; }
  if (ch == '\b') { if (win->cur_x > 0) win->cur_x--; return; }
  if (ch == '\t') {
    int next = (win->cur_x + 8) & ~7;
    while (win->cur_x < next && win->cur_x < win->cols) {
      win->ch[win->cur_y * win->cols + win->cur_x] = ' ';
      win->at[win->cur_y * win->cols + win->cur_x] = at;
      win->cur_x++;
    }
    return;
  }
  win->ch[win->cur_y * win->cols + win->cur_x] = ch;
  win->at[win->cur_y * win->cols + win->cur_x] = at;
  if (win->cur_x + 1 < win->cols) win->cur_x++;
  else if (win->scroll) { win->cur_x = 0; win_scroll_up(win, 1); }
  /* at the last column without scrollok the cursor stays (ncurses would
   * wrap; nano never relies on it) */
}

int waddch(WINDOW *win, const chtype ch) {
  if (!win) return ERR;
  unsigned char c = (unsigned char)(ch & 0xff);
  unsigned char at = attr_to_bits(win->attr);
  /* Attribute bits in ch combine with the window attribute set. */
  at |= attr_to_bits((int)(ch & A_ATTRIBUTES));
  win_put(win, c, at);
  return OK;
}

int mvwaddch(WINDOW *win, int y, int x, const chtype ch) {
  if (wmove(win, y, x) == ERR) return ERR;
  return waddch(win, ch);
}
int mvaddch(int y, int x, const chtype ch) { return mvwaddch(&stdscr_store, y, x, ch); }

int waddnstr(WINDOW *win, const char *str, int n) {
  if (!win || !str) return ERR;
  for (int i = 0; str[i] && (n < 0 || i < n); i++) waddch(win, (chtype)(unsigned char)str[i]);
  return OK;
}

int waddstr(WINDOW *win, const char *str) { return waddnstr(win, str, -1); }

int mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n) {
  if (wmove(win, y, x) == ERR) return ERR;
  return waddnstr(win, str, n);
}

int mvwaddstr(WINDOW *win, int y, int x, const char *str) {
  return mvwaddnstr(win, y, x, str, -1);
}

int mvaddnstr(int y, int x, const char *str, int n) {
  return mvwaddnstr(&stdscr_store, y, x, str, n);
}

int mvaddstr(int y, int x, const char *str) {
  return mvwaddnstr(&stdscr_store, y, x, str, -1);
}

/* Minimal vsnprintf-free printf into a window: only the conversions nano's
 * wprintw/mvwprintw calls use ('%s', '%d', '%c', '%%'). */
#include <stdarg.h>

int wprintw(WINDOW *win, const char *fmt, ...) {
  va_list ap;
  char buf[512];
  int n = 0;
  va_start(ap, fmt);
  for (const char *p = fmt; *p && n < (int)sizeof buf - 1; p++) {
    if (*p != '%') { buf[n++] = *p; continue; }
    p++;
    if (*p == 's') {
      const char *s = va_arg(ap, const char *);
      if (!s) s = "(null)";
      while (*s && n < (int)sizeof buf - 1) buf[n++] = *s++;
    } else if (*p == 'd' || *p == 'i') {
      int v = va_arg(ap, int);
      char tmp[12];
      int m = 0;
      if (v < 0) { if (n < (int)sizeof buf - 1) buf[n++] = '-'; v = -v; }
      if (v == 0) tmp[m++] = '0';
      while (v > 0) { tmp[m++] = (char)('0' + (v % 10)); v /= 10; }
      while (m > 0 && n < (int)sizeof buf - 1) buf[n++] = tmp[--m];
    } else if (*p == 'c') {
      int c = va_arg(ap, int);
      if (n < (int)sizeof buf - 1) buf[n++] = (char)c;
    } else if (*p == '%') {
      if (n < (int)sizeof buf - 1) buf[n++] = '%';
    } else if (*p == 'l') {
      if (p[1] == 'u' || p[1] == 'd') {
        long v = va_arg(ap, long);
        char tmp[24];
        int m = 0;
        p++;
        if (v < 0) { if (n < (int)sizeof buf - 1) buf[n++] = '-'; v = -v; }
        if (v == 0) tmp[m++] = '0';
        while (v > 0) { tmp[m++] = (char)('0' + (v % 10)); v /= 10; }
        while (m > 0 && n < (int)sizeof buf - 1) buf[n++] = tmp[--m];
      } else if (p[1] == 's') {
        const char *s = va_arg(ap, const char *);
        if (!s) s = "(null)";
        p++;
        while (*s && n < (int)sizeof buf - 1) buf[n++] = *s++;
      }
    } else if (*p == 'z' || *p == 'u') {
      unsigned int v = va_arg(ap, unsigned int);
      char tmp[12];
      int m = 0;
      if (v == 0) tmp[m++] = '0';
      while (v > 0) { tmp[m++] = (char)('0' + (v % 10)); v /= 10; }
      while (m > 0 && n < (int)sizeof buf - 1) buf[n++] = tmp[--m];
      if (*p == 'z') p++;
    } else {
      /* unknown conversion: emit it verbatim */
      if (n < (int)sizeof buf - 1) buf[n++] = '%';
      if (n < (int)sizeof buf - 1) buf[n++] = *p;
    }
  }
  va_end(ap);
  buf[n] = '\0';
  return waddnstr(win, buf, n);
}

int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...) {
  /* Rebuild the string with va_list passed through: reuse wprintw's guts by
   * formatting into a local buffer via the same algorithm. */
  va_list ap;
  char buf[512];
  int n = 0;
  va_start(ap, fmt);
  /* duplicate of the wprintw formatter body (kept simple, see above) */
  for (const char *p = fmt; *p && n < (int)sizeof buf - 1; p++) {
    if (*p != '%') { buf[n++] = *p; continue; }
    p++;
    if (*p == 's') {
      const char *s = va_arg(ap, const char *);
      if (!s) s = "(null)";
      while (*s && n < (int)sizeof buf - 1) buf[n++] = *s++;
    } else if (*p == 'd' || *p == 'i') {
      int v = va_arg(ap, int);
      char tmp[12];
      int m = 0;
      if (v < 0) { if (n < (int)sizeof buf - 1) buf[n++] = '-'; v = -v; }
      if (v == 0) tmp[m++] = '0';
      while (v > 0) { tmp[m++] = (char)('0' + (v % 10)); v /= 10; }
      while (m > 0 && n < (int)sizeof buf - 1) buf[n++] = tmp[--m];
    } else if (*p == 'c') {
      int c = va_arg(ap, int);
      if (n < (int)sizeof buf - 1) buf[n++] = (char)c;
    } else if (*p == '%') {
      if (n < (int)sizeof buf - 1) buf[n++] = '%';
    } else {
      if (n < (int)sizeof buf - 1) buf[n++] = '%';
      if (n < (int)sizeof buf - 1) buf[n++] = *p;
    }
  }
  va_end(ap);
  buf[n] = '\0';
  if (wmove(win, y, x) == ERR) return ERR;
  return waddnstr(win, buf, n);
}

int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs,
            chtype tl, chtype tr, chtype bl, chtype br) {
  (void)ls; (void)rs; (void)ts; (void)bs; (void)tl; (void)tr; (void)bl; (void)br;
  if (!win) return ERR;
  return OK;
}

int box(WINDOW *win, chtype verch, chtype horch) {
  return wborder(win, verch, verch, horch, horch, 0, 0, 0, 0);
}

/* ---- clearing ---- */

static void win_touch(WINDOW *win) { (void)win; /* doupdate diffs everything */ }

int werase(WINDOW *win) {
  if (!win) return ERR;
  memset(win->ch, ' ', (size_t)win->rows * win->cols);
  memset(win->at, 0, (size_t)win->rows * win->cols);
  win->cur_y = 0;
  win->cur_x = 0;
  win_touch(win);
  return OK;
}

int wclear(WINDOW *win) { return werase(win); }
int erase(void) { return werase(&stdscr_store); }
int clear(void) { return werase(&stdscr_store); }

int wclrtoeol(WINDOW *win) {
  if (!win) return ERR;
  for (int x = win->cur_x; x < win->cols; x++) {
    win->ch[win->cur_y * win->cols + x] = ' ';
    win->at[win->cur_y * win->cols + x] = 0;
  }
  win_touch(win);
  return OK;
}

int wclrtobot(WINDOW *win) {
  if (!win) return ERR;
  wclrtoeol(win);
  for (int y = win->cur_y + 1; y < win->rows; y++) {
    for (int x = 0; x < win->cols; x++) {
      win->ch[y * win->cols + x] = ' ';
      win->at[y * win->cols + x] = 0;
    }
  }
  return OK;
}

/* ====================================================================== */
/* Attributes                                                             */
/* ====================================================================== */

int wattron(WINDOW *win, int attrs) { if (!win) return ERR; win->attr |= attrs; return OK; }
int wattroff(WINDOW *win, int attrs) { if (!win) return ERR; win->attr &= ~attrs; return OK; }
int wattrset(WINDOW *win, int attrs) { if (!win) return ERR; win->attr = attrs; return OK; }
int wattr_get(WINDOW *win, int *attrs, short *pair, void *opts) {
  (void)opts;
  if (attrs) *attrs = win ? win->attr : 0;
  if (pair) *pair = 0;
  return OK;
}
int wbkgdset(WINDOW *win, chtype ch) { (void)win; (void)ch; return OK; }
int wbkgd(WINDOW *win, chtype ch) { (void)win; (void)ch; return OK; }
int wstandend(WINDOW *win) { return wattrset(win, 0); }
int wstandout(WINDOW *win) { return wattrset(win, A_REVERSE); }

/* ====================================================================== */
/* Refresh / doupdate                                                     */
/* ====================================================================== */

static int refresh_owner_valid = 0;

static void set_cursor_owner(WINDOW *win) {
  if (!win) return;
  cur_row = clamp_int(win->y + win->cur_y, 0, scr_rows > 0 ? scr_rows - 1 : 0);
  cur_col = clamp_int(win->x + win->cur_x, 0, scr_cols > 0 ? scr_cols - 1 : 0);
  refresh_owner_valid = 1;
}

/* stdscr is a static store, not malloc'd, so its cells have to be given to
 * it once the screen size is known (and re-fitted on a resize).  Without
 * this, drawing on stdscr -- mvaddstr(stdscr,...), refresh() -- would
 * silently go nowhere: nano draws into its own windows, but plain curses
 * programs (and the host integration test) use stdscr. */
static void stdscr_ensure_cells(void) {
  size_t need = (size_t)scr_rows * (size_t)scr_cols;
  if (stdscr_store.ch &&
      (size_t)stdscr_store.rows * (size_t)stdscr_store.cols != need) {
    free(stdscr_store.ch);
    free(stdscr_store.at);
    stdscr_store.ch = NULL;
    stdscr_store.at = NULL;
  }
  if (!stdscr_store.ch) {
    stdscr_store.ch = (unsigned char *)malloc(need);
    stdscr_store.at = (unsigned char *)malloc(need);
    if (stdscr_store.ch) memset(stdscr_store.ch, ' ', need);
    if (stdscr_store.at) memset(stdscr_store.at, 0, need);
  }
  stdscr_store.rows = scr_rows;
  stdscr_store.cols = scr_cols;
  stdscr_store.y = 0;
  stdscr_store.x = 0;
  stdscr_store.cur_y = clamp_int(stdscr_store.cur_y, 0, scr_rows - 1);
  stdscr_store.cur_x = clamp_int(stdscr_store.cur_x, 0, scr_cols - 1);
}

int wnoutrefresh(WINDOW *win) {
  if (!win) return ERR;
  if (!ps_valid) { /* nothing to copy into; still keep the cursor */ }
  /* Copy the window's cells into the virtual screen (a window whose cell
   * allocation failed has none to copy). */
  if (win->ch) {
    for (int r = 0; r < win->rows; r++) {
      int sr = win->y + r;
      if (sr < 0 || sr >= scr_rows) continue;
      for (int c = 0; c < win->cols; c++) {
        int sc = win->x + c;
        if (sc < 0 || sc >= scr_cols) continue;
        vs_ch[sr * scr_cols + sc] = win->ch[r * win->cols + c];
        vs_at[sr * scr_cols + sc] = win->at[r * win->cols + c];
      }
    }
  }
  set_cursor_owner(win);
  return OK;
}

static void emit_sgr(int attr) {
  if (attr == pen_attr) return;
  oputs("\033[0m");
  if (attr & 0x01) oputs("\033[7m");   /* reverse */
  if (attr & 0x02) oputs("\033[1m");   /* bold */
  if (attr & 0x04) oputs("\033[4m");   /* underline */
  pen_attr = attr;
}

static unsigned char emit_char(unsigned char c) {
  /* Only ASCII prints on this display; anything else becomes '?'. */
  if (c >= 0x20 && c <= 0x7e) return c;
  return '?';
}

static void doupdate_rows(const unsigned char *new_ch, const unsigned char *new_at,
                          const unsigned char *old_ch, const unsigned char *old_at) {
  for (int r = 0; r < scr_rows; r++) {
    int c = 0;
    while (c < scr_cols) {
      int idx = r * scr_cols + c;
      if (new_ch[idx] == old_ch[idx] && new_at[idx] == old_at[idx]) { c++; continue; }
      int start = c;
      while (c < scr_cols && !(new_ch[r * scr_cols + c] == old_ch[r * scr_cols + c] &&
                               new_at[r * scr_cols + c] == old_at[r * scr_cols + c]))
        c++;
      /* Emit the changed run [start, c): position + attribute runs + text. */
      oputs("\033[");
      oput_int(r + 1);
      oputc(';');
      oput_int(start + 1);
      oputc('H');
      int run = start;
      while (run < c) {
        int a = new_at[r * scr_cols + run];
        emit_sgr(a);
        while (run < c && new_at[r * scr_cols + run] == a) {
          oputc((char)emit_char(new_ch[r * scr_cols + run]));
          run++;
        }
      }
    }
  }
}

int doupdate(void) {
  if (!term_ok) return OK;
  if (!refresh_owner_valid) set_cursor_owner(&stdscr_store);

  if (!ps_valid) {
    /* Repaint the whole screen row by row (keeps the desktop's grid model
     * exact after a resize or a fresh start). */
    for (int r = 0; r < scr_rows; r++) {
      oputs("\033[");
      oput_int(r + 1);
      oputs(";1H");
      int run = 0;
      while (run < scr_cols) {
        int a = vs_at[r * scr_cols + run];
        emit_sgr(a);
        while (run < scr_cols && vs_at[r * scr_cols + run] == a) {
          oputc((char)emit_char(vs_ch[r * scr_cols + run]));
          run++;
        }
      }
    }
    ps_valid = 1;
  } else {
    doupdate_rows(vs_ch, vs_at, ps_ch, ps_at);
  }
  memcpy(ps_ch, vs_ch, sizeof ps_ch);
  memcpy(ps_at, vs_at, sizeof ps_at);

  /* Cursor: visibility + position. */
  if (cursor_shown != cursor_target) {
    oputs(cursor_target ? "\033[?25h" : "\033[?25l");
    cursor_shown = cursor_target;
  }
  if (cursor_target) {
    oputs("\033[");
    oput_int(cur_row + 1);
    oputc(';');
    oput_int(cur_col + 1);
    oputc('H');
  }
  oflush();
  return OK;
}

int wrefresh(WINDOW *win) {
  if (win == &curscr_store) {
    /* nano's full_refresh(): repaint everything, unconditionally. */
    ended = 0;
    ps_valid = 0;
    return doupdate();
  }
  wnoutrefresh(win);
  return doupdate();
}

int refresh(void) {
  if (!term_ok) return OK;
  if (ended) ps_valid = 0;    /* re-entering after endwin(): repaint all */
  ended = 0;
  /* ncurses: refresh() == wnoutrefresh(stdscr) + doupdate(). */
  wnoutrefresh(&stdscr_store);
  return doupdate();
}

int redrawwin(WINDOW *win) { (void)win; ps_valid = 0; return OK; }

int wredrawln(WINDOW *win, int beg_line, int num_lines) {
  if (!win) return ERR;
  /* Force the affected screen rows to differ from the model so the next
   * doupdate repaints them. */
  for (int i = 0; i < num_lines; i++) {
    int sr = win->y + beg_line + i;
    if (sr < 0 || sr >= scr_rows) continue;
    memset(ps_ch + sr * scr_cols, 0xff, (size_t)scr_cols);
    memset(ps_at + sr * scr_cols, 0xff, (size_t)scr_cols);
  }
  return OK;
}

/* ====================================================================== */
/* Input                                                                  */
/* ====================================================================== */

int wgetch(WINDOW *win) {
  (void)win;
  int waited_ms = 0;
  int limit_ms = nodelay_mode ? 0 : (halfdelay_tenths > 0 ? halfdelay_tenths * 100 : -1);

  for (;;) {
    if (unget_key >= 0) {
      int k = unget_key;
      unget_key = -1;
      return k;
    }

    /* Flush pending screen updates before waiting for input, exactly like
     * ncurses' wgetch(): nano defers its repaints (refresh_needed) and
     * relies on the NEXT wgetch to actually push the changed cells to the
     * physical screen.  Without this flush here, per-keystroke edits stay
     * in the virtual screen and the user sees nothing until a full
     * refresh (^L, Enter, save) eventually happens. */
    doupdate();

    int k = parse_key();
    if (k > 0) return k;
    if (k == HB_SWALLOWED) continue;
    if (k == HB_EOF || eof_seen) {
      /* No more input: the window was closed (or a pipe broke). */
      return ERR;
    }
    if (res_pending) {
      res_pending = 0;
      return KEY_RESIZE;
    }
    if (nodelay_mode) return ERR;
    if (limit_ms >= 0 && waited_ms >= limit_ms) return ERR;

    if (limit_ms < 0) {
      /* Blocking: wait for bytes (a resize always arrives as bytes). */
      in_fill_block();
      waited_ms = 0;
    } else {
      in_fill_now();
      if (in_pos >= in_len) {
        io_sleep(3);
        waited_ms += 3;
      }
    }
  }
}

int getch(void) { return wgetch(&stdscr_store); }
int mvwgetch(WINDOW *win, int y, int x) {
  if (wmove(win, y, x) == ERR) return ERR;
  return wgetch(win);
}

int ungetch(int ch) {
  if (unget_key >= 0) return ERR;
  unget_key = ch;
  return OK;
}

/* ====================================================================== */
/* Terminal mode / setup                                                  */
/* ====================================================================== */

/* Ask for terminal mode and wait for the desktop's size reply
 * (ESC ] S <rows>;<cols> ~).  Leftover bytes (keys typed meanwhile) are
 * stashed into the input buffer. */
static int handshake(void) {
  unsigned char tmp[768];
  int len = 0;
  int waited = 0;

  oputs("\033]V 1~");
  oflush();

  while (waited < 3000) {
    if (len < (int)sizeof tmp) {
      int r = io_read_now((char *)tmp + len, (int)sizeof tmp - len);
      if (r < 0) break;                    /* EOF: no desktop */
      if (r > 0) len += r;
    }

    /* Scan for the size message. */
    for (int i = 0; i + 3 < len; i++) {
      if (tmp[i] == 27 && tmp[i + 1] == ']' && tmp[i + 2] == 'S') {
        int j = i + 3;
        int rows = 0, cols = 0, any = 0;
        while (j < len && (tmp[j] == ' ' || tmp[j] == '\t')) j++;   /* "]S 24;80" */
        while (j < len && tmp[j] != '~' && tmp[j] != '\a') {
          if (tmp[j] >= '0' && tmp[j] <= '9') { rows = rows * 10 + (tmp[j] - '0'); any = 1; }
          else break;
          j++;
        }
        if (j < len && tmp[j] == ';') {
          j++;
          while (j < len && tmp[j] >= '0' && tmp[j] <= '9') { cols = cols * 10 + (tmp[j] - '0'); j++; }
        }
        if (j < len && (tmp[j] == '~' || tmp[j] == '\a') && any && cols > 0) {
          /* Stash everything around the message and finish. */
          for (int k = 0; k < i; k++)
            if (in_len < (int)sizeof in_buf) in_buf[in_len++] = tmp[k];
          for (int k = j + 1; k < len; k++)
            if (in_len < (int)sizeof in_buf) in_buf[in_len++] = tmp[k];
          scr_rows = clamp_int(rows, 1, HBC_MAX_ROWS);
          scr_cols = clamp_int(cols, 1, HBC_MAX_COLS);
          LINES = scr_rows;
          COLS = scr_cols;
          term_ok = 1;
          return 1;
        }
        break;   /* a partial message: keep reading */
      }
    }

    if (len >= (int)sizeof tmp - 1) break;
    io_sleep(2);
    waited += 2;
  }

  /* No reply: run headless-ish with a sane default (host tests that drive
   * the transport directly end up here; the real desktop always replies). */
  for (int k = 0; k < len; k++)
    if (in_len < (int)sizeof in_buf) in_buf[in_len++] = tmp[k];
  scr_rows = 24;
  scr_cols = 80;
  LINES = 24;
  COLS = 80;
  term_ok = 0;
  return 0;
}

WINDOW *initscr(void) {
  if (scr_rows == 0) {
    scr_rows = 24;
    scr_cols = 80;
    LINES = 24;
    COLS = 80;
  }
  if (!term_ok && !eof_seen) handshake();
  scr_blank(scr_rows, scr_cols);
  ps_valid = 0;
  cur_row = 0;
  cur_col = 0;
  cursor_shown = 1;      /* the desktop starts with the cursor visible */
  cursor_target = 1;
  pen_attr = 0;
  ended = 0;
  oputs("\033[2J\033[1;1H");
  oflush();
  stdscr_store.attr = 0;
  stdscr_store.scroll = 0;
  stdscr_store.cur_y = 0;
  stdscr_store.cur_x = 0;
  stdscr_ensure_cells();
  return &stdscr_store;
}

int endwin(void) {
  ended = 1;
  cursor_target = 1;
  if (!cursor_shown) { oputs("\033[?25h"); cursor_shown = 1; }
  oflush();
  return OK;
}

int isendwin(void) { return ended; }

int hb_term_is_terminal(void) { return term_ok; }

int curs_set(int visibility) {
  cursor_target = visibility ? 1 : 0;
  if (term_ok && cursor_shown != cursor_target) {
    oputs(cursor_target ? "\033[?25h" : "\033[?25l");
    cursor_shown = cursor_target;
    oflush();
  }
  return 0;
}

/* ---- mode setters: the desktop owns terminal modes; these are bookkeeping
 * so programs that query them keep working. ---- */

static int raw_mode = 0;
int raw(void) { raw_mode = 1; return OK; }
int noraw(void) { raw_mode = 0; return OK; }
int cbreak(void) { return OK; }
int nocbreak(void) { return OK; }
int echo(void) { return OK; }
int noecho(void) { return OK; }
int nonl(void) { return OK; }
int nl(void) { return OK; }

int halfdelay(int tenths) {
  halfdelay_tenths = tenths;
  if (tenths < 0) tenths = 0;
  return OK;
}

int nodelay(WINDOW *win, int flag) { (void)win; nodelay_mode = flag ? 1 : 0; return OK; }
int notimeout(WINDOW *win, int flag) { (void)win; (void)flag; return OK; }
int keypad(WINDOW *win, int flag) { (void)win; (void)flag; return OK; }
int intrflush(WINDOW *win, int flag) { (void)win; (void)flag; return OK; }
int scrollok(WINDOW *win, int flag) { if (!win) return ERR; win->scroll = flag ? 1 : 0; return OK; }
int clearok(WINDOW *win, int flag) { (void)win; if (flag) ps_valid = 0; return OK; }
int idlok(WINDOW *win, int flag) { (void)win; (void)flag; return OK; }
int leaveok(WINDOW *win, int flag) { (void)win; (void)flag; return OK; }

int napms(int ms) { io_sleep(ms); return OK; }
int beep(void) { return OK; }     /* the desktop has no bell */
int flash(void) { return OK; }

int mvcur(int oldrow, int oldcol, int newrow, int newcol) {
  (void)oldrow; (void)oldcol;
  cur_row = clamp_int(newrow, 0, scr_rows - 1);
  cur_col = clamp_int(newcol, 0, scr_cols - 1);
  return OK;
}

static int syx_y = -1, syx_x = -1;
int getsyx(int *y, int *x) { *y = syx_y; *x = syx_x; return OK; }
int setsyx(int y, int x) { syx_y = y; syx_x = x; return OK; }

/* ====================================================================== */
/* Colors: monochrome                                                    */
/* ====================================================================== */

int has_colors(void) { return FALSE; }
int start_color(void) { return ERR; }
int use_default_colors(void) { return OK; }
int assume_default_colors(int fg, int bg) { (void)fg; (void)bg; return OK; }
int init_pair(short pair, short fg, short bg) { (void)pair; (void)fg; (void)bg; return OK; }
int color_content(short color, short *r, short *g, short *b) {
  (void)color; if (r) *r = 0; if (g) *g = 0; if (b) *b = 0; return ERR;
}
int pair_content(short pair, short *fg, short *bg) {
  (void)pair; if (fg) *fg = 0; if (bg) *bg = 0; return ERR;
}
int init_color(short color, short r, short g, short b) {
  (void)color; (void)r; (void)g; (void)b; return ERR;
}
int can_change_color(void) { return FALSE; }

/* ====================================================================== */
/* termcap / misc stubs                                                  */
/* ====================================================================== */

/* nan o only asks for "kb" (the backspace key string) to tell ^H and ^? apart
 * when binding Help; report DEL (0x7F), what a modern terminal reports. */
char *tgetstr(const char *id, char **area) {
  (void)area;
  if (id && id[0] == 'k' && id[1] == 'b') {
    static char kbs[2] = { 0x7f, 0 };
    return kbs;
  }
  return NULL;
}

char *tigetstr(const char *capname) { (void)capname; return NULL; }
int tputs(const char *str, int affcnt, int (*putc_fn)(int)) {
  (void)affcnt;
  if (str) while (*str) putc_fn(*str++);
  return OK;
}

int key_defined(const char *definition) { (void)definition; return 0; }
int define_key(const char *definition, int keycode) {
  (void)definition; (void)keycode; return ERR;
}
int set_escdelay(int ms) { (void)ms; return OK; }
int typeahead(int fd) { (void)fd; return OK; }

/* ====================================================================== */
/* Mouse: no backend (the desktop's own OSC P protocol is separate)      */
/* ====================================================================== */

mmask_t mousemask(mmask_t newmask, mmask_t *oldmask) {
  (void)newmask;
  if (oldmask) *oldmask = 0;
  return 0;
}
int mouseinterval(int wait) { (void)wait; return OK; }
int getmouse(MEVENT *event) { (void)event; return ERR; }
int ungetmouse(MEVENT *event) { (void)event; return ERR; }
int wmouse_trafo(const WINDOW *win, int *pY, int *pX, int to_screen) {
  (void)win; (void)pY; (void)pX; (void)to_screen;
  return FALSE;
}
int wenclose(const WINDOW *win, int y, int x) {
  (void)win; (void)y; (void)x;
  return FALSE;
}
