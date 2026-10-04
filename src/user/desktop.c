#include "graphics.h"
#include "libc.h"
#include "window.h"
#include "desktop_damage.h"
#include <signal.h>
#ifndef HOST_TEST
#include "syscall.h"
#endif



// Basic key mapping for US keyboard
char keymap[128] = {0,    27,  '1', '2',  '3',  '4',  '5', '6', '7',  '8',
                    '9',  '0', '-', '=',  '\b', '\t', 'q', 'w', 'e',  'r',
                    't',  'y', 'u', 'i',  'o',  'p',  '[', ']', '\n', 0,
                    'a',  's', 'd', 'f',  'g',  'h',  'j', 'k', 'l',  ';',
                    '\'', '`', 0,   '\\', 'z',  'x',  'c', 'v', 'b',  'n',
                    'm',  ',', '.', '/',  0,    '*',  0,   ' ', 0};

char shift_keymap[128] = {0,    27,  '!', '@',  '#',  '$',  '%', '^', '&',  '*',
                          '(',  ')', '_', '+',  '\b', '\t', 'Q', 'W', 'E',  'R',
                          'T',  'Y', 'U', 'I',  'O',  'P',  '{', '}', '\n', 0,
                          'A',  'S', 'D', 'F',  'G',  'H',  'J', 'K', 'L',  ':',
                          '"',  '~', 0,   '|',  'Z',  'X',  'C', 'V', 'B',  'N',
                          'M',  '<', '>', '?',  0,    '*',  0,   ' ', 0};

static int shift_pressed = 0;

#define MAX_MENU_ITEMS 128
char menu_items[MAX_MENU_ITEMS][16];
int num_menu_items = 0;

int menu_open = 0;
int menu_x = 0;
int menu_y = 0;

/* ---- Start menu (taskbar) state ---- */
static int start_menu_open = 0;
static int start_sel = 0;      /* selected item index */
static int start_scroll = 0;   /* first visible item */
#define START_MENU_VISIBLE 16

/* ---- Taskbar geometry ---- */
#define TASKBAR_Y (SCREEN_HEIGHT - TASKBAR_H)
#define APPS_BTN_X 6
#define APPS_BTN_W 64
#define TASKBAR_BTN_X 76
#define TASKBAR_BTN_W_MAX 150
#define CLOCK_W 96

static inline long my_syscall(long sysno, long arg0, long arg1, long arg2, long arg3) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = arg0;
  register long rsi __asm__("rsi") = arg1;
  register long rdx __asm__("rdx") = arg2;
  register long r10 __asm__("r10") = arg3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(sysno), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 asm("x8") = sysno;
  register long x0 asm("x0") = arg0;
  register long x1 asm("x1") = arg1;
  register long x2 asm("x2") = arg2;
  register long x3 asm("x3") = arg3;
  asm volatile("svc #0"
               : "=r"(x0)
               : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3)
               : "memory");
  return x0;
#endif
}

__attribute__((weak)) void print_console(const char *s) {
  int len = 0;
  while (s[len]) len++;
  my_syscall(1, (long)s, len, 0, 0);
}

extern struct window windows[MAX_WINDOWS];
extern int num_windows;

int app_menu_open = 0;
int app_menu_win_id = -1;
int app_menu_idx = -1;
int app_menu_x = 0;
int app_menu_y = 0;

/* Currently focused window id (-1 = none). File scope so launch helpers and
 * the taskbar can use it. */
static int focused_window = -1;

/* Pointer position. File scope: the frame painter and the damage
 * bookkeeping need it too, not just the event loop. */
static int mouse_x = SCREEN_WIDTH / 2;
static int mouse_y = SCREEN_HEIGHT / 2;

/* ---- Pointer drag support (drag & drop) ------------------------------
 * A press in the content area of a window that opted into mouse events
 * (ESC ] P 1 ~) opens a "drag session" for that window. While the button is
 * held, pointer motion is forwarded as ESC [ G <col>;<row>;<btn> ~ and the
 * button release as ESC [ R <x>;<y>;<btn> ~, both clamped to the
 * window's content area (cells for text windows, pixels for pixel-mode
 * windows). Press, drag and release all go through
 * desktop_send_hook so host tests can capture them instead of writing to
 * the app's pipe. Windows that never see a press never get G/R events.
 * (Motion WITHOUT a press -- cursor tracking -- is the separate
 * ESC [ T report built and sent further down; see wm_hover_service.)
 *
 * The helper functions below are non-static so src/host/desktop_drag_test.c
 * can drive them directly. */

static int drag_win_id = -1;   /* window that received the press (-1 = none) */
static int drag_last_x = -1;   /* last cell/pixel reported to that window   */
static int drag_last_y = -1;

static void launch_app_named(const char *bin, const char *args);

static void window_write_default(int win_id, const char *buf, int len) {
  for (int i = 0; i < num_windows; i++) {
    if (windows[i].id == win_id) {
      int wr = write(windows[i].stdin_fd, buf, len);
      (void)wr;
      return;
    }
  }
}

/* Where forwarded mouse events go. Host tests swap this for a capture. */
void (*desktop_send_hook)(int win_id, const char *buf, int len) = window_write_default;

/* ====================================================================== */
/* Keyboard input: modifiers, key payloads, software auto-repeat          */
/*                                                                        */
/* The desktop forwards the focused window's keys as the byte stream a     */
/* terminal would send: printable bytes (Shift picks the legend, Ctrl      */
/* turns a letter into its classic control byte), ESC sequences for the    */
/* special keys.  A held key keeps firing (auto-repeat) after a delay,     */
/* which also lets a held arrow scroll a list without the device having    */
/* to send repeat events.                                                  */
/*                                                                        */
/* src/host/desktop_input_test.c is the spec for this behavior:            */
/* map_key_char() / key_may_repeat() / key_repeat_tick() and the state     */
/* they read (ctrl_pressed, held_key_*) are exercised there directly.      */
/* ====================================================================== */

static struct window *find_window(int id);   /* window lookup, defined below */

static int ctrl_pressed = 0;
static int alt_pressed = 0;

/* Modifier bits reported to pixel-mode windows (ESC [ K <mods> ~, the
 * F1.8 input-v2 addition -- see window.h): 1=Shift 2=Ctrl 4=Alt, OR-ed.
 * Shift and Ctrl also keep their classic byte mapping (map_key_char
 * above); the K message is additive and is what carries Alt, which has
 * no byte of its own. */
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4

static int current_mods(void) {
  return (shift_pressed ? MOD_SHIFT : 0) | (ctrl_pressed ? MOD_CTRL : 0) |
         (alt_pressed ? MOD_ALT : 0);
}

/* Auto-repeat timing: wait KEY_REPEAT_DELAY_MS before the first repeat,
 * then fire every KEY_REPEAT_PERIOD_MS.  held_key_last_ms == 0 means "no
 * repeat fired yet for this press". */
#define KEY_REPEAT_DELAY_MS 400
#define KEY_REPEAT_PERIOD_MS 50
static int held_key_code = -1;
static int held_key_since_ms = 0;
static int held_key_last_ms = 0;

/* Byte for a key press.  Shift selects the shifted legend; Ctrl turns a
 * letter into its control byte (Ctrl+C = 0x03, Ctrl+Q = 0x11) -- digits
 * and punctuation keep their plain legend, which is what nano binds them
 * to anyway (Ctrl+digit has no classic meaning). */
static char map_key_char(int code) {
  if (code < 0 || code >= 128) return 0;
  char c = shift_pressed ? shift_keymap[code] : keymap[code];
  if (ctrl_pressed && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
    c = (char)(c & 0x1f);
  return c;
}

/* Keys that may auto-repeat while held: the arrows and ordinary printable
 * keys.  Enter/Tab/Backspace do not repeat, the function keys do not, and
 * nothing repeats while Ctrl is held -- a held Ctrl+C must fire once. */
static int key_may_repeat(int code) {
  if (ctrl_pressed) return 0;
  if (code >= 103 && code <= 108) return 1;          /* arrows */
  char c = map_key_char(code);
  return c >= 0x20 && c != 0x7f;
}

/* Bytes to forward for a key press.  `out` must hold 8 bytes; returns the
 * length, or 0 when there is nothing to forward (unknown key, or one the
 * desktop consumes itself such as F4). */
static int key_payload(int code, char *out) {
  if (code >= 103 && code <= 108) {                  /* arrows */
    out[0] = 27; out[1] = '[';
    if (code == 103) out[2] = 'A';                   /* Up */
    if (code == 108) out[2] = 'B';                   /* Down */
    if (code == 106) out[2] = 'C';                   /* Right */
    if (code == 105) out[2] = 'D';                   /* Left */
    return 3;
  }
  if (code == 102) { out[0] = 27; out[1] = '['; out[2] = 'H'; return 3; }   /* Home */
  if (code == 107) { out[0] = 27; out[1] = '['; out[2] = 'F'; return 3; }   /* End */
  if (code == 104) { out[0]=27; out[1]='['; out[2]='5'; out[3]='~'; return 4; }/* PgUp */
  if (code == 109) { out[0]=27; out[1]='['; out[2]='6'; out[3]='~'; return 4; }/* PgDn */
  if (code == 110) { out[0]=27; out[1]='['; out[2]='2'; out[3]='~'; return 4; }/* Insert */
  if (code == 111) { out[0]=27; out[1]='['; out[2]='3'; out[3]='~'; return 4; }/* Delete */
  if (code == 15 && shift_pressed) {                                            /* Shift+Tab */
    out[0]=27; out[1]='['; out[2]='Z'; return 3;
  }
  if (code >= 59 && code <= 68) {                    /* F1-F10, xterm numbers */
    int n = (code <= 63) ? 11 + (code - 59) : 17 + (code - 64);
    out[0]=27; out[1]='['; out[2]=(char)('0'+n/10); out[3]=(char)('0'+n%10); out[4]='~';
    return 5;
  }
  if (code == 87 || code == 88) {                    /* F11/F12 */
    out[0]=27; out[1]='['; out[2]='2'; out[3]=(code == 87) ? '3' : '4'; out[4]='~';
    return 5;
  }
  char c = map_key_char(code);
  if (c) { out[0] = c; return 1; }
  return 0;
}

/* Build the modifier report for a pixel-mode keypress: ESC [ K <mods> ~.
 * mods is the OR of the MOD_* bits above (0..7, always one digit).
 * Wire bytes carry NO spaces -- the spaces in the protocol comments are
 * notation, exactly like ESC [ P <col>;<row>;<btn> ~ above (the mouse
 * builder writes none either).  Returns the byte count (excluding NUL).
 * Non-static so the host test asserts on it directly. */
int wm_build_mods_seq(char *out, int cap, int mods) {
  if (mods < 0) mods = 0;
  if (mods > 7) mods = 7;
  char body[8];
  int j = 0;
  body[j++] = 27; body[j++] = '['; body[j++] = 'K';
  body[j++] = (char)('0' + mods);
  body[j++] = '~';
  if (j > cap - 1) j = cap - 1;
  for (int i = 0; i < j; i++) out[i] = body[i];
  out[j] = '\0';
  return j;
}

/* Forward one key message stream to a window.  A pixel-mode window is
 * told the live modifier state first (ESC [ K <mods> ~, one write), then
 * the key payload (a second write) -- one write() per message, the wire
 * protocol's atomicity rule (window.h).  Every other window gets exactly
 * the payload it always got, byte for byte. */
static void forward_payload_to_window(int win_id, const char *seq, int n) {
  struct window *w = find_window(win_id);
  if (w && w->pixel_mode) {
    char mods_seq[8];
    int mn = wm_build_mods_seq(mods_seq, sizeof mods_seq, current_mods());
    desktop_send_hook(win_id, mods_seq, mn);
  }
  desktop_send_hook(win_id, seq, n);
}

/* Forward one key press to the focused window and arm auto-repeat (a fresh
 * press always re-arms; a non-repeatable key disarms a previous one). */
static void forward_key_to_focused(int code, int now_ms) {
  if (focused_window < 0) return;
  char seq[8];
  int n = key_payload(code, seq);
  if (n <= 0) return;
  forward_payload_to_window(focused_window, seq, n);
  if (key_may_repeat(code)) {
    held_key_code = code;
    held_key_since_ms = now_ms;
    held_key_last_ms = 0;
  } else {
    held_key_code = -1;
  }
}

/* One auto-repeat turn: a key held past the delay keeps firing the same
 * payload through the focused window.  Quiet while any menu is open or
 * nothing is focused. */
static void key_repeat_tick(int now_ms) {
  if (held_key_code < 0) return;
  if (!key_may_repeat(held_key_code)) return;
  if (start_menu_open || menu_open || app_menu_open) return;
  if (focused_window < 0) return;
  if (now_ms - held_key_since_ms < KEY_REPEAT_DELAY_MS) return;
  if (held_key_last_ms != 0 && now_ms - held_key_last_ms < KEY_REPEAT_PERIOD_MS)
    return;

  char seq[8];
  int n = key_payload(held_key_code, seq);
  if (n <= 0) return;
  forward_payload_to_window(focused_window, seq, n);
  held_key_last_ms = now_ms;
}

static struct window *find_window(int id) {
  for (int i = 0; i < num_windows; i++) {
    if (windows[i].id == id) return &windows[i];
  }
  return 0;
}

/* Append the decimal digits of a non-negative value (no padding) and
 * return the new fill position.  Values are capped at 4 digits: the
 * desktop's coordinates (content cells or pixels) fit comfortably. */
static int seq_append_int(char *out, int j, int v) {
  if (v < 0) v = 0;
  if (v > 9999) v = 9999;
  char digits[8];
  int d = 0;
  if (v == 0) digits[d++] = '0';
  while (v > 0) { digits[d++] = (char)('0' + v % 10); v /= 10; }
  while (d > 0) out[j++] = digits[--d];
  return j;
}

/* Build one mouse escape sequence: ESC [ <kind> <x> ; <y> ; <btn> ~
 * kind: 'P' press, 'G' drag, 'R' release. x/y are content cells for a
 * text window and content pixels for a pixel-mode window (see window.h);
 * all parameters must be >= 0.
 * Returns the number of bytes written (excluding the NUL terminator). */
int wm_build_mouse_seq(char *out, int cap, char kind, int col, int row, int btn) {
  char body[24];
  int j = 0;
  body[j++] = 27; body[j++] = '[';
  body[j++] = kind;
  j = seq_append_int(body, j, col);
  body[j++] = ';';
  j = seq_append_int(body, j, row);
  body[j++] = ';';
  j = seq_append_int(body, j, btn);
  body[j++] = '~';
  if (j > cap - 1) j = cap - 1;
  for (int i = 0; i < j; i++) out[i] = body[i];
  out[j] = '\0';
  return j;
}

/* Build a pointer-tracking report: ESC [ T <x_root>;<y_root> ~
 * x/y are SCREEN coordinates (unlike the content-relative mouse trio
 * above).  Returns the byte count (excluding the NUL), like
 * wm_build_mouse_seq. */
int wm_build_hover_seq(char *out, int cap, int x, int y) {
  char body[24];
  int j = 0;
  body[j++] = 27; body[j++] = '[';
  body[j++] = 'T';
  j = seq_append_int(body, j, x);
  body[j++] = ';';
  j = seq_append_int(body, j, y);
  body[j++] = '~';
  if (j > cap - 1) j = cap - 1;
  for (int i = 0; i < j; i++) out[i] = body[i];
  out[j] = '\0';
  return j;
}

/* ---- Pointer tracking (hover) for pixel windows ----------------------
 * A pixel window that opted into pointer events (ESC ] P 1 ~) receives
 * the pointer's SCREEN position as ESC [ T <x_root>;<y_root> ~ while the
 * pointer moves, button held or not -- the desktop equivalent of a client
 * polling XQueryPointer against the root window, which is what
 * cursor-following apps (the XANTFARM port's "poke the ants") need.  The
 * press/drag/release trio above only fires for the window the button went
 * down in; tracking fires for whoever asked, pointer anywhere on screen.
 *
 * Reports are throttled to one per HOVER_MIN_MS of movement: the app's
 * stdin pipe is small and a blocked pipe write would freeze the desktop,
 * and a cursor-follower needs no more than ~30 Hz anyway.  Because this
 * runs every pass of the main loop, the latest position is delivered as
 * soon as the interval has passed -- a fast flick can be coalesced but
 * never lost.  A window holding a drag session is skipped: its motion
 * already arrives as the content-relative drag 'G' message.
 *
 * Non-static (like the drag helpers above) so desktop_pixel_test.c can
 * drive it directly and capture reports through desktop_send_hook. */

#define HOVER_MIN_MS 30
static int hover_sent_x = -1, hover_sent_y = -1;  /* pos the app has seen */
static int hover_sent_ms = 0;                     /* when sent (0 = never) */
static int hover_paused = 0;   /* a menu was open on the last service pass */

void wm_hover_service(int mx, int my, int now_ms) {
  /* While a menu is open the pointer drives the menu, not the apps: no
   * tracking goes out.  When the menu closes, the live position is
   * reported once (however far the pointer wandered) so the app resumes
   * on the actual pointer instead of a stale one. */
  if (start_menu_open || menu_open || app_menu_open) {
    hover_paused = 1;
    return;
  }
  if (hover_paused) {
    hover_paused = 0;
    hover_sent_x = -1;
    hover_sent_y = -1;
    hover_sent_ms = 0;
  }
  if (mx == hover_sent_x && my == hover_sent_y) return;   /* still */
  if (hover_sent_ms != 0 && now_ms - hover_sent_ms < HOVER_MIN_MS) return;
  hover_sent_ms = now_ms;
  hover_sent_x = mx;
  hover_sent_y = my;
  char seq[24];
  int n = wm_build_hover_seq(seq, sizeof seq, mx, my);
  for (int i = 0; i < num_windows; i++) {
    struct window *w = &windows[i];
    if (!w->pixel_mode || !w->mouse_events) continue;
    if (w->id == drag_win_id) continue;
    desktop_send_hook(w->id, seq, n);
  }
}

/* Map absolute pointer coordinates to the content cell (col,row) of window
 * w, clamped to its content grid. Cell (0,0) is the first print() cell at
 * (w->x + 10, w->y + 44); cells advance 8px right and 10px down. */
void wm_mouse_cell(const struct window *w, int mx, int my, int *col, int *row) {
  int c = (mx - (w->x + 10)) / 8;
  int r = (my - (w->y + 44)) / 10;
  int maxc = (w->w - 12) / 8 - 1;
  int maxr = (w->h - 48) / 10 - 1;
  if (c < 0) c = 0;
  if (r < 0) r = 0;
  if (maxc < 0) maxc = 0;
  if (maxr < 0) maxr = 0;
  if (c > maxc) c = maxc;
  if (r > maxr) r = maxr;
  *col = c; *row = r;
}

/* Map absolute pointer coordinates to the content PIXEL (x,y) of a
 * pixel-mode window w, clamped to its content rectangle.  Pixel (0,0) is
 * the content corner (w->x + 2, w->y + 34); cells differ only in their
 * origin (x+10, y+44) and 8x10 step. */
void wm_mouse_pixel(const struct window *w, int mx, int my, int *px, int *py) {
  int x = mx - (w->x + 2);
  int y = my - (w->y + 34);
  int maxx = w->w - 5;      /* content is w-4 wide: 0 .. w-5 */
  int maxy = w->h - 37;     /* content is h-36 tall: 0 .. h-37 */
  if (maxx < 0) maxx = 0;
  if (maxy < 0) maxy = 0;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > maxx) x = maxx;
  if (y > maxy) y = maxy;
  *px = x; *py = y;
}

/* Pointer position inside a window's content area: a text cell for a text
 * window, a content pixel for a pixel-mode window. */
void wm_mouse_local(const struct window *w, int mx, int my, int *x, int *y) {
  if (w->pixel_mode) wm_mouse_pixel(w, mx, my, x, y);
  else wm_mouse_cell(w, mx, my, x, y);
}

/* Start a drag session after a press was delivered at (x,y) -- a content
 * cell for a text window, content pixels for a pixel-mode window. */
void desktop_drag_begin(int win_id, int x, int y) {
  drag_win_id = win_id;
  drag_last_x = x;
  drag_last_y = y;
}

/* End a drag session without sending anything (window closed, etc). */
void desktop_drag_cancel(void) {
  drag_win_id = -1;
  drag_last_x = -1;
  drag_last_y = -1;
}

/* Window currently holding the drag session (-1 = none). */
int desktop_drag_window(void) { return drag_win_id; }

/* Forward pointer motion while the button is held. Only a change of cell
 * (text window) or pixel (pixel-mode window) is reported. */
void desktop_drag_move(int mx, int my) {
  if (drag_win_id < 0) return;
  struct window *w = find_window(drag_win_id);
  if (!w) { desktop_drag_cancel(); return; }
  int x, y;
  wm_mouse_local(w, mx, my, &x, &y);
  if (x == drag_last_x && y == drag_last_y) return;
  drag_last_x = x;
  drag_last_y = y;
  char seq[24];
  int n = wm_build_mouse_seq(seq, sizeof seq, 'G', x, y, 1);
  desktop_send_hook(drag_win_id, seq, n);
}

/* Deliver the button release at the (clamped) position and end the
 * session. */
void desktop_drag_end(int mx, int my) {
  if (drag_win_id < 0) return;
  struct window *w = find_window(drag_win_id);
  if (w) {
    int x, y;
    wm_mouse_local(w, mx, my, &x, &y);
    char seq[24];
    int n = wm_build_mouse_seq(seq, sizeof seq, 'R', x, y, 1);
    desktop_send_hook(drag_win_id, seq, n);
  }
  desktop_drag_cancel();
}

/* ---- Mouse wheel (pixel windows only, F1.8 input v2) -----------------
 * A wheel tick is forwarded as the existing press/release pair with btn=4
 * (up) / 5 (down) -- the X11 convention the protocol reuses (browser.md
 * A.2).  It goes to the pixel-mode window under the pointer that opted
 * into pointer events, exactly like a button press goes to the window it
 * landed in: the position is localised and clamped like every other mouse
 * message (wm_mouse_pixel) and ticks elsewhere on screen are dropped.
 * Each tick is its own pair of writes (P then R); |delta| ticks are
 * delivered, capped so a rogue axis value cannot flood the app's pipe.
 * QEMU's virtio input reports the wheel as EV_REL/REL_WHEEL (+1 per
 * detent up, -1 down); the ARM kernel passes EV_REL through untouched. */
#ifndef REL_WHEEL
#define REL_WHEEL 0x08
#endif

#define WHEEL_MAX_TICKS 8

void desktop_wheel(int mx, int my, int delta) {
  if (delta == 0) return;
  int ticks = delta > 0 ? delta : -delta;
  if (ticks > WHEEL_MAX_TICKS) ticks = WHEEL_MAX_TICKS;
  int btn = delta > 0 ? 4 : 5;

  int win_id = wm_get_window_at(mx, my);
  if (win_id < 0) return;
  struct window *w = find_window(win_id);
  if (!w || !w->pixel_mode || !w->mouse_events) return;

  /* The pointer must be over the content area, like a content press. */
  int cx, cy, cw, ch;
  wm_pixel_content_rect(w, &cx, &cy, &cw, &ch);
  if (mx < cx || mx >= cx + cw || my < cy || my >= cy + ch) return;

  int x, y;
  wm_mouse_pixel(w, mx, my, &x, &y);
  char pseq[24], rseq[24];
  int pn = wm_build_mouse_seq(pseq, sizeof pseq, 'P', x, y, btn);
  int rn = wm_build_mouse_seq(rseq, sizeof rseq, 'R', x, y, btn);
  for (int i = 0; i < ticks; i++) {
    desktop_send_hook(win_id, pseq, pn);
    desktop_send_hook(win_id, rseq, rn);
  }
}

/* ---- Graceful close for pixel-mode windows (F1.8 input v2) -----------
 * When the desktop closes a pixel-mode window (the F4 key or the
 * title-bar X button) it first asks the app to quit:
 *
 *     ESC [ D ~        one write; the X11 library's WM_DELETE_WINDOW
 *
 * The window then stays alive for up to CLOSE_GRACE_MS of desktop ticks
 * while the app exits on its own (its process exit closes the stdout pipe
 * and the frame loop -- which already treats that as "window gone" --
 * removes the window); if it is still there when the grace expires the
 * desktop falls back to today's behavior: kill(pid) + remove.  A text
 * window has no in-band close message and keeps the immediate kill.
 * Only one close is ever in flight: a second request forces the first
 * one's fallback.  The helpers are non-static so desktop_input_test.c
 * can drive them with a fake clock. */
#define CLOSE_GRACE_MS 250

static int close_pending_win = -1;   /* pixel window asked to quit (-1 none) */
static int close_pending_ms = 0;     /* when ESC [ D was sent */

/* Build the graceful-close message: ESC [ D ~ (four bytes, no spaces --
 * the comments' spacing is notation; this is what the decoder's one-byte
 * lookahead after 'D' expects).  Returns the byte count. */
int wm_build_close_seq(char *out, int cap) {
  static const char msg[] = { 27, '[', 'D', '~' };
  int j = (int)sizeof msg;
  if (j > cap - 1) j = cap - 1;
  for (int i = 0; i < j; i++) out[i] = msg[i];
  out[j] = '\0';
  return j;
}

/* Close a window now -- the pre-F1.8 path: kill the process, drop the
 * window, forget any focus/drag/close bookkeeping for it. */
static void close_window_now(int win_id) {
  struct window *w = find_window(win_id);
  if (!w) return;
  kill(w->pid, 9);
  wm_remove_window(win_id);
  if (focused_window == win_id) focused_window = -1;
  if (drag_win_id == win_id) desktop_drag_cancel();
  if (close_pending_win == win_id) close_pending_win = -1;
}

/* Test-support: report a close transition as ONE console write, so the
 * serial line lands intact (print_dec() writes through fd 1, which does
 * not reach the console from the desktop, and per-fragment writes get an
 * interleaved "[CONSOLE]" prefix from the kernel's console). */
static void close_note(int win_id, const char *what) {
  char line[72];
  int i = 0;
  const char *pre = "[CLOSE] win=";
  for (int j = 0; pre[j] && i < 60; j++) line[i++] = pre[j];
  char digits[12];
  int d = 0;
  if (win_id == 0) {
    digits[d++] = '0';
  } else {
    int v = win_id < 0 ? -win_id : win_id;
    while (v > 0 && d < 10) { digits[d++] = (char)('0' + v % 10); v /= 10; }
  }
  while (d > 0 && i < 60) line[i++] = digits[--d];
  for (int j = 0; what[j] && i < 68; j++) line[i++] = what[j];
  line[i++] = '\n';
  line[i] = '\0';
  print_console(line);
}

/* Ask a window to close.  A pixel-mode window gets ESC [ D ~ and the
 * grace period (returns 1); anything else closes immediately (returns 0).
 * The caller repaints when the window actually goes away. */
int desktop_request_close(int win_id, int now_ms) {
  struct window *w = find_window(win_id);
  if (!w) return 0;
  if (!w->pixel_mode) {
    close_window_now(win_id);
    return 0;
  }
  if (close_pending_win >= 0 && close_pending_win != win_id)
    close_window_now(close_pending_win);   /* one close in flight */
  if (drag_win_id == win_id) desktop_drag_cancel();
  char seq[8];
  int n = wm_build_close_seq(seq, sizeof seq);
  desktop_send_hook(win_id, seq, n);
  close_pending_win = win_id;
  close_pending_ms = now_ms;
  close_note(win_id, ": ESC [ D sent");
  return 1;
}

/* One tick of the close grace timer: fires the kill fallback when the app
 * has not exited in time.  Returns 1 when it closed a window. */
int desktop_close_tick(int now_ms) {
  if (close_pending_win < 0) return 0;
  if (now_ms - close_pending_ms < CLOSE_GRACE_MS) return 0;
  int win_id = close_pending_win;
  close_pending_win = -1;
  close_note(win_id, ": grace expired, killing");
  close_window_now(win_id);
  return 1;
}

/* The app exited while its close was pending -- nothing left to kill. */
void desktop_close_note_gone(int win_id) {
  if (close_pending_win != win_id) return;
  close_pending_win = -1;
  close_note(win_id, ": exited within grace");
}

/* Parse an OSC "run in new window" request: seq[0]==']', seq[1]=='R',
 * then <bin>[;<args>] (the desktop's OSC collector hands the sequence over
 * with the leading ']' included). Fills bin and args and returns 1 when a
 * non-empty program name was found. */
int wm_parse_run_request(const char *seq, char *bin, int bincap, char *args, int argcap) {
  if (!seq || seq[0] != ']' || seq[1] != 'R') return 0;
  int i = 2, j = 0;
  while (seq[i] && seq[i] != ';' && j < bincap - 1) bin[j++] = seq[i++];
  bin[j] = '\0';
  while (seq[i] && seq[i] != ';') i++;     /* skip a truncated bin name */
  if (seq[i] == ';') i++;
  j = 0;
  while (seq[i] && j < argcap - 1) args[j++] = seq[i++];
  args[j] = '\0';
  return bin[0] != '\0';
}

static int csi_param(const char *s, int *i);   /* defined below */

void wm_handle_app_escape(int win_id, char* seq) {
  if (seq[0] == ']' && seq[1] == 'M') {
    int idx = seq[2] - '0';
    if (idx >= 0 && idx < 10) {
      char* ptr = seq + 4;
      struct window* win = 0;
      for(int i=0; i<num_windows; i++) if(windows[i].id == win_id) { win = &windows[i]; break; }
      if(!win) return;

      if (idx >= win->num_menus) win->num_menus = idx + 1;

      int n_len = 0;
      while(*ptr && *ptr != ';') {
        win->menus[idx].name[n_len++] = *ptr++;
      }
      win->menus[idx].name[n_len] = 0;
      if(*ptr == ';') ptr++;

      win->menus[idx].num_items = 0;
      while(*ptr) {
        int i_len = 0;
        while(*ptr && *ptr != ',') {
          win->menus[idx].items[win->menus[idx].num_items][i_len++] = *ptr++;
        }
        win->menus[idx].items[win->menus[idx].num_items][i_len] = 0;
        win->menus[idx].num_items++;
        if(*ptr == ',') ptr++;
      }
      /* The menu bar changed: repaint this window's chrome. */
      win->chrome_dirty = 1;
    }
  } else if (seq[0] == ']' && seq[1] == 'T') {
    /* Window title: ESC ] T <title> ~.  Spaces after the T are separators
     * (gui.c prints "ESC ] T Title~", the X11 library "ESC ] T Title~"
     * with a space); the title itself may contain spaces. */
    const char *p = seq + 2;
    while (*p == ' ') p++;
    wm_set_window_title(win_id, p);
  } else if (seq[0] == ']' && seq[1] == 'P') {
    /* Pointer events opt-in: ESC ] P 1 ~ (1 = enable, 0 = disable).  The
     * space is optional: gui.c prints "]P1~", the X11 library "]P 1~"
     * (window.h documents the spaced form), so skip separators like the
     * ]V and ]X handlers do -- reading seq[2] directly silently dropped
     * the X11 library's opt-in and its mouse clicks never arrived. */
    const char *p = seq + 2;
    while (*p == ' ') p++;
    for (int i = 0; i < num_windows; i++) {
      if (windows[i].id == win_id) {
        windows[i].mouse_events = (*p == '1') ? 1 : 0;
        break;
      }
    }
  } else if (seq[0] == ']' && seq[1] == 'V') {
    /* Terminal mode opt-in: ESC ] V 1 ~ (spaces allowed after the V -- the
     * curses shim prints "ESC ] V 1~").  The window becomes a character
     * grid (see wm_term_* in window.c) and the app is answered with its
     * size, ESC ] S <rows>;<cols> ~.  This is what full-screen "curses"
     * programs -- the ported nano -- use. */
    const char *p = seq + 2;
    while (*p == ' ') p++;
    if (*p == '1') {
      for (int i = 0; i < num_windows; i++) {
        if (windows[i].id == win_id) {
          /* The window's output drove us here, so the frame below will
           * repaint it: wm_term_begin marked the whole grid dirty. */
          wm_term_begin(&windows[i]);
          break;
        }
      }
    }
  } else if (seq[0] == ']' && seq[1] == 'X') {
    /* Pixel-mode opt-in: ESC ] X <w>;<h> ~ -- the app wants to own its
     * content pixels (an X11 client; see window.h).  It is answered with
     * the content rectangle (ESC ] G) and a full repair request; the
     * window output that carried this message already drives the frame
     * that flushes them. */
    const char *p = seq + 2;
    while (*p == ' ') p++;
    int i = 0;
    int pw = csi_param(p, &i);
    if (p[i] == ';') i++;
    int ph = csi_param(p, &i);
    for (int j = 0; j < num_windows; j++) {
      if (windows[j].id == win_id) {
        windows[j].pixel_mode = 1;
        windows[j].pix_pref_w = pw < 0 ? 0 : pw;
        windows[j].pix_pref_h = ph < 0 ? 0 : ph;
        windows[j].rendered_valid = 0;  /* the WM no longer paints it */
        wm_pixel_send_geometry(&windows[j]);
        wm_pixel_queue_expose_all(&windows[j]);
        break;
      }
    }
  } else if (seq[0] == ']' && seq[1] == 'F') {
    /* "Frame flushed": the app just painted framebuffer pixels; the
     * pointer sprite may need re-stamping (see window.h). */
    for (int j = 0; j < num_windows; j++) {
      if (windows[j].id == win_id) {
        windows[j].pix_restamp = 1;
        break;
      }
    }
  } else if (seq[0] == ']' && seq[1] == 'R') {
    /* Run a program in a new window: ESC ] R <bin>[;<args>] ~
     * Used by FILES to open documents in EDITOR.BIN and to launch .BIN
     * programs. */
    char bin[32];
    char rargs[160];
    if (wm_parse_run_request(seq, bin, sizeof bin, rargs, sizeof rargs)) {
      launch_app_named(bin, rargs);
    }
  }
}

/* GUI app binaries surfaced at the top of the Apps menu, then the games.
 * Each tier is a stable partition: pinned entries keep their read_dir order
 * and move ahead of everything else, which keeps its own order. Editing
 * these lists is the only change needed to alter what gets pinned. */
static const char *const pinned_apps[] = {
  "browser.bin", "CONSOLE.BIN", "FILES.BIN", "CALC.BIN", "XCALC.BIN", "ANTFARM.BIN", "XEYES.BIN", "CLOCK.BIN", "SYSMON.BIN",
  "HEX.BIN", "TASKS.BIN", "FIND.BIN", "DIFF.BIN", "NOTES.BIN",
  "NANO.BIN", "UNIT.BIN",
};
#define NUM_PINNED_APPS ((int)(sizeof(pinned_apps) / sizeof(pinned_apps[0])))

/* The games, pinned right below the apps so all twelve fit on screen. */
static const char *const pinned_games[] = {
  "PONG.BIN", "MILLIPED.BIN",
};
#define NUM_PINNED_GAMES ((int)(sizeof(pinned_games) / sizeof(pinned_games[0])))

/* Exact string equality (desktop.c has no libc strcmp). */
static int name_is(const char *a, const char *b) {
  int i = 0;
  while (a[i] && b[i] && a[i] == b[i]) i++;
  return a[i] == b[i];
}

static int is_pinned_app(const char *name) {
  for (int i = 0; i < NUM_PINNED_APPS; i++) {
    if (name_is(name, pinned_apps[i])) return 1;
  }
  return 0;
}

static int is_pinned_game(const char *name) {
  for (int i = 0; i < NUM_PINNED_GAMES; i++) {
    if (name_is(name, pinned_games[i])) return 1;
  }
  return 0;
}

static void copy_name(char *dst, const char *src, int max) {
  int k = 0;
  while (src[k] && k < max - 1) { dst[k] = src[k]; k++; }
  dst[k] = '\0';
}

/* Move the pinned GUI apps, then the games, to the front of menu_items
 * (tier 1: apps, tier 2: games, tier 3: everything else; each stable).
 * Non-static: exercised directly by the host test (desktop_menu_test.c). */
void menu_apps_first(void) {
  char tmp[MAX_MENU_ITEMS][16];
  int w = 0;
  for (int i = 0; i < num_menu_items; i++) {
    if (is_pinned_app(menu_items[i])) copy_name(tmp[w++], menu_items[i], 16);
  }
  for (int i = 0; i < num_menu_items; i++) {
    if (is_pinned_game(menu_items[i])) copy_name(tmp[w++], menu_items[i], 16);
  }
  for (int i = 0; i < num_menu_items; i++) {
    if (!is_pinned_app(menu_items[i]) && !is_pinned_game(menu_items[i]))
      copy_name(tmp[w++], menu_items[i], 16);
  }
  for (int i = 0; i < num_menu_items; i++) {
    copy_name(menu_items[i], tmp[i], 16);
  }
}

/* Display label for a menu entry: the file name minus a trailing ".BIN"
 * (launcher entries read "FILES", not "FILES.BIN"); other names unchanged.
 * The strip is case-insensitive so the browser ships as 8.3 lowercase
 * "browser.bin" and reads exactly "browser".  Non-static: exercised directly
 * by the host test. */
void menu_display_name(const char *raw, char *out, int max) {
  copy_name(out, raw, max);
  int i = 0;
  while (out[i] && i < max - 1) i++;
  if (i >= 4) {
    /* Case-insensitive .BIN/.bin: uppercase the four tail bytes and compare. */
    char b0 = out[i - 4], b1 = out[i - 3], b2 = out[i - 2], b3 = out[i - 1];
    if (b0 >= 'a' && b0 <= 'z') b0 -= 32;
    if (b1 >= 'a' && b1 <= 'z') b1 -= 32;
    if (b2 >= 'a' && b2 <= 'z') b2 -= 32;
    if (b3 >= 'a' && b3 <= 'z') b3 -= 32;
    if (b0 == '.' && b1 == 'B' && b2 == 'I' && b3 == 'N') out[i - 4] = '\0';
  }
}

void load_menu(void) {
  num_menu_items = 0;

  /* Pass 1: the pinned launcher entries, wherever they sit in the
   * directory.  A single scan stopped at MAX_MENU_ITEMS entries, so once
   * the disk root grew past that (the 256 MiB W0.3 boot disk) the tail of
   * the listing -- FILES, CALC, XCALC, XEYES, CLOCK and the rest -- was
   * silently cut off and those apps vanished from the Apps menu (the
   * run_xcalc_test gate could no longer reach XCALC.BIN at all).  The
   * pinned set must always surface; the rest fills the remaining slots. */
  for (int idx = 0; num_menu_items < MAX_MENU_ITEMS; idx++) {
    struct sys_dirent ent;
    if (read_dir("/", idx, &ent) < 0) break;
    if (!is_pinned_app(ent.name) && !is_pinned_game(ent.name)) continue;
    copy_name(menu_items[num_menu_items], ent.name, 16);
    num_menu_items++;
  }

  /* Pass 2: everything else, in directory order, until the cap. */
  for (int idx = 0; num_menu_items < MAX_MENU_ITEMS; idx++) {
    struct sys_dirent ent;
    if (read_dir("/", idx, &ent) < 0) break;
    if (is_pinned_app(ent.name) || is_pinned_game(ent.name)) continue;
    copy_name(menu_items[num_menu_items], ent.name, 16);
    num_menu_items++;
  }

  /* Surface the GUI apps at the top of the Apps menu. */
  menu_apps_first();
  /* Test-support: dump the exact menu index -> name mapping once at load. */
  for (int i = 0; i < num_menu_items; i++) {
    print_console("[MENU] ");
    print_dec(i);
    print_console("=");
    print_console(menu_items[i]);
    print_console("\n");
  }
  print_console("[MENU] count=");
  print_dec(num_menu_items);
  print_console("\n");
}

/* Window title for a launched binary: the base name without directory or
 * extension ("EDITOR.BIN" -> "EDITOR", "/SUB/FOO.BIN" -> "FOO"). */
static void app_name_from_bin(const char *bin, char *out, int max) {
  const char *base = bin;
  for (int i = 0; bin[i]; i++) {
    if (bin[i] == '/') base = bin + i + 1;
  }
  int i = 0;
  while (base[i] && base[i] != '.' && i < max - 1) {
    out[i] = base[i];
    i++;
  }
  out[i] = '\0';
}

/* Launch `bin` into a new tiled window (pipe pair + spawn2 + window).
 * `args` (or 0) is handed to the child, readable via get_args(). */
static void launch_app_named(const char *bin, const char *args) {
  if (!bin || !bin[0]) return;

  int in_pipe[2], out_pipe[2];
  pipe(in_pipe);
  pipe(out_pipe);

  print_console("[LAUNCH] ");
  print_console(bin);
  print_console("\n");
  int pid = spawn2(bin, in_pipe[0], out_pipe[1], -1, args);
  if (pid >= 0) {
    int win_id = wm_create_window(COLOR(16, 18, 30), pid, out_pipe[0], in_pipe[1]);
    char name[24];
    app_name_from_bin(bin, name, sizeof(name));
    wm_set_window_title(win_id, name);
    focused_window = win_id;
    close(in_pipe[0]);
    close(out_pipe[1]);
  } else {
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
  }
}

/* Launch menu item `idx` into a new tiled window. */
static void launch_menu_item(int idx) {
  if (idx < 0 || idx >= num_menu_items) return;
  launch_app_named(menu_items[idx], 0);
}

/* ---- Right-click application menu (unchanged geometry; tests depend) ---- */

void draw_menu(void) {
  if (app_menu_open) {
    struct window* win = 0;
    for(int i=0; i<num_windows; i++) if(windows[i].id == app_menu_win_id) { win = &windows[i]; break; }
    if (win && app_menu_idx >= 0 && app_menu_idx < win->num_menus) {
      int n_items = win->menus[app_menu_idx].num_items;
      graphics_draw_rect(app_menu_x, app_menu_y, 100, n_items * 20, COLOR(200, 200, 200));
      for(int i=0; i<n_items; i++) {
        wm_draw_text(app_menu_x + 5, app_menu_y + i * 20 + 5, win->menus[app_menu_idx].items[i], COLOR(0, 0, 0));
      }
    }
  }

  if (!menu_open)
    return;
  int w = 120;
  int h = num_menu_items * 20;
  graphics_draw_rect(menu_x, menu_y, w, h, COLOR(200, 200, 200));
  for (int i = 0; i < num_menu_items; i++) {
    char label[16];
    menu_display_name(menu_items[i], label, sizeof(label));
    wm_draw_text(menu_x + 5, menu_y + i * 20 + 5, label,
                 COLOR(0, 0, 0));
  }
}

/* ---- Taskbar ---- */

static void get_clock_string(char *buf) {
  struct sys_time t;
  if (sysinfo(6, &t, sizeof(t)) == 0) {
    int j = 0;
    buf[j++] = '0' + (t.hour / 10); buf[j++] = '0' + (t.hour % 10);
    buf[j++] = ':';
    buf[j++] = '0' + (t.minute / 10); buf[j++] = '0' + (t.minute % 10);
    buf[j++] = ':';
    buf[j++] = '0' + (t.second / 10); buf[j++] = '0' + (t.second % 10);
    buf[j] = '\0';
    return;
  }
  /* Fallback: uptime */
  int ms = sysinfo(1, 0, 0);
  if (ms < 0) ms = 0;
  int sec = ms / 1000;
  int j = 0;
  buf[j++] = '0' + ((sec / 3600) % 24) / 10; buf[j++] = '0' + ((sec / 3600) % 24) % 10;
  buf[j++] = ':';
  buf[j++] = '0' + ((sec / 60) % 60) / 10; buf[j++] = '0' + ((sec / 60) % 60) % 10;
  buf[j++] = ':';
  buf[j++] = '0' + (sec % 60) / 10; buf[j++] = '0' + (sec % 60) % 10;
  buf[j] = '\0';
}

static void taskbar_button_geometry(int *btn_w) {
  int avail = SCREEN_WIDTH - TASKBAR_BTN_X - CLOCK_W - 8;
  int bw = TASKBAR_BTN_W_MAX;
  if (num_windows > 0 && bw * num_windows > avail) bw = avail / num_windows;
  if (bw < 40) bw = 40;
  *btn_w = bw;
}

static void draw_taskbar(void) {
  /* Background */
  graphics_fill_gradient_v(0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H,
                           COLOR(48, 54, 74), COLOR(30, 33, 46));
  graphics_draw_hline(0, TASKBAR_Y, SCREEN_WIDTH, COLOR(96, 166, 255));

  /* Apps button */
  graphics_draw_rect(APPS_BTN_X, TASKBAR_Y + 3, APPS_BTN_W, TASKBAR_H - 6,
                     start_menu_open ? COLOR(96, 166, 255) : COLOR(64, 74, 100));
  graphics_draw_rect_outline(APPS_BTN_X, TASKBAR_Y + 3, APPS_BTN_W, TASKBAR_H - 6,
                             COLOR(140, 150, 180));
  wm_draw_text(APPS_BTN_X + 10, TASKBAR_Y + 9, "Apps", COLOR(240, 242, 248));

  /* Window buttons */
  int bw;
  taskbar_button_geometry(&bw);
  int bx = TASKBAR_BTN_X;
  for (int i = 0; i < num_windows; i++) {
    graphics_draw_rect(bx, TASKBAR_Y + 3, bw - 2, TASKBAR_H - 6, COLOR(52, 58, 78));
    graphics_draw_rect_outline(bx, TASKBAR_Y + 3, bw - 2, TASKBAR_H - 6,
                               (windows[i].id == focused_window) ? COLOR(96, 166, 255)
                                                                 : COLOR(90, 96, 116));
    graphics_set_clip(bx + 4, TASKBAR_Y + 3, bw - 10, TASKBAR_H - 6);
    wm_draw_text(bx + 6, TASKBAR_Y + 9,
                 windows[i].title[0] ? windows[i].title : "app",
                 COLOR(226, 230, 240));
    graphics_reset_clip();
    bx += bw;
  }

  /* Clock */
  char clk[12];
  get_clock_string(clk);
  wm_draw_text(SCREEN_WIDTH - CLOCK_W + 8, TASKBAR_Y + 9, clk, COLOR(220, 226, 240));
}

static void draw_start_menu(void) {
  if (!start_menu_open)
    return;

  int visible = START_MENU_VISIBLE;
  if (num_menu_items < visible) visible = num_menu_items;
  int w = 220;
  int h = visible * 20 + 8;
  int x = 4;
  int y = TASKBAR_Y - h;

  graphics_draw_rect(x, y, w, h, COLOR(232, 234, 240));
  graphics_draw_rect_outline(x, y, w, h, COLOR(96, 166, 255));

  if (start_scroll > 0) {
    wm_draw_text(x + w - 40, y - 8, "more ^", COLOR(220, 226, 240));
  }
  if (start_scroll + visible < num_menu_items) {
    wm_draw_text(x + w - 40, y + h, "more v", COLOR(220, 226, 240));
  }

  for (int i = 0; i < visible; i++) {
    int idx = start_scroll + i;
    if (idx >= num_menu_items) break;
    int row_y = y + 4 + i * 20;
    char label[16];
    menu_display_name(menu_items[idx], label, sizeof(label));
    if (idx == start_sel) {
      graphics_draw_rect(x + 2, row_y, w - 4, 20, COLOR(96, 166, 255));
      wm_draw_text(x + 8, row_y + 5, label, COLOR(255, 255, 255));
    } else {
      wm_draw_text(x + 8, row_y + 5, label, COLOR(20, 20, 24));
    }
  }
}

static void start_menu_ensure_visible(void) {
  int visible = START_MENU_VISIBLE;
  if (num_menu_items < visible) visible = num_menu_items;
  if (start_sel < start_scroll) start_scroll = start_sel;
  if (start_sel >= start_scroll + visible) start_scroll = start_sel - visible + 1;
  if (start_scroll < 0) start_scroll = 0;
  int max_scroll = num_menu_items - visible;
  if (max_scroll < 0) max_scroll = 0;
  if (start_scroll > max_scroll) start_scroll = max_scroll;
}

/* ====================================================================== */
/* Damage-driven compositing                                              */
/* ====================================================================== */
/* A frame repaints only what changed since the last one:
 *
 *   - windows created or removed -> the tiling moved every window, so the
 *     whole scene repaints (as before);
 *   - a window's captured text changed -> wm_draw_window_rows() repairs
 *     just the content rows that differ (an arrow key in FILES normally
 *     touches two listing rows, not the screen);
 *   - anything else that moved (pointer, menus, focus highlight, taskbar
 *     clock, window titles/menus) -> the scene repaints inside a small
 *     damage rectangle (see desktop_damage()).
 *
 * Contract with window.c: a window whose text changed is repaired by
 * wm_draw_window_rows() BEFORE the rectangle passes run, so its
 * framebuffer bookkeeping stays exact no matter how those passes clip. */

/* The wallpaper gradient, carved around pixel-mode window content: the WM
 * must never paint over app pixels (see window.h).  With no pixel windows
 * this is the single full-span call it always was; otherwise the span is
 * filled band by band with the exact per-row colours the full gradient
 * would show, stepping over the content rectangles. */

/* One wallpaper rectangle, filled with the exact row colours. */
static void paint_wallpaper_rows(int x, int y, int w, int h) {
  int span_h = SCREEN_HEIGHT - TASKBAR_H;
  for (int r = 0; r < h; r++) {
    graphics_draw_hline(x, y + r, w, graphics_wallpaper_row_color(y + r, span_h));
  }
}

static void paint_wallpaper(void) {
  int span_h = SCREEN_HEIGHT - TASKBAR_H;

  /* The rectangles this pass must step around. */
  struct wm_rect cut[MAX_WINDOWS];
  int ncut = 0;
  for (int i = 0; i < num_windows; i++) {
    if (!windows[i].pixel_mode) continue;
    int x, y, w, h;
    wm_pixel_content_rect(&windows[i], &x, &y, &w, &h);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > span_h) h = span_h - y;
    if (w <= 0 || h <= 0) continue;
    cut[ncut].x = x; cut[ncut].y = y; cut[ncut].w = w; cut[ncut].h = h;
    ncut++;
  }
  if (ncut == 0) {
    graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, span_h,
                             COLOR(16, 20, 38), COLOR(44, 56, 96));
    return;
  }

  /* Scan bands between the rectangles' y edges: inside one band every cut
   * rectangle either spans it or not at all, so the free part of a band
   * is a set of x-intervals. */
  int ys[2 * MAX_WINDOWS + 2];
  int ny = 0;
  ys[ny++] = 0;
  ys[ny++] = span_h;
  for (int i = 0; i < ncut; i++) {
    ys[ny++] = cut[i].y;
    ys[ny++] = cut[i].y + cut[i].h;
  }
  for (int a = 1; a < ny; a++) {              /* insertion sort */
    int v = ys[a], b = a - 1;
    while (b >= 0 && ys[b] > v) { ys[b + 1] = ys[b]; b--; }
    ys[b + 1] = v;
  }
  for (int k = 0; k + 1 < ny; k++) {
    int y0 = ys[k], y1 = ys[k + 1];
    if (y1 <= y0) continue;

    struct wm_rect iv[MAX_WINDOWS];
    int ni = 0;
    for (int i = 0; i < ncut; i++) {
      if (cut[i].y <= y0 && cut[i].y + cut[i].h >= y1) iv[ni++] = cut[i];
    }
    for (int a = 1; a < ni; a++) {            /* insertion sort by x */
      struct wm_rect v = iv[a];
      int b = a - 1;
      while (b >= 0 && iv[b].x > v.x) { iv[b + 1] = iv[b]; b--; }
      iv[b + 1] = v;
    }
    int cur = 0;
    for (int i = 0; i < ni; i++) {
      if (iv[i].x > cur) paint_wallpaper_rows(cur, y0, iv[i].x - cur, y1 - y0);
      if (iv[i].x + iv[i].w > cur) cur = iv[i].x + iv[i].w;
    }
    if (cur < SCREEN_WIDTH) paint_wallpaper_rows(cur, y0, SCREEN_WIDTH - cur, y1 - y0);
  }
}

/* One full scene paint into the current clip: wallpaper, every window, the
 * menus, the taskbar and the pointer (which must stay on top). */
static void paint_scene(void) {
  /* Wallpaper: vertical gradient behind the tiled windows. */
  paint_wallpaper();
  wm_draw_windows(focused_window);
  draw_menu();
  draw_start_menu();
  draw_taskbar();
  wm_draw_cursor(mouse_x, mouse_y);
}

/* ---- Open menus vs pixel windows --------------------------------------
 *
 * The menus are painted last: like the pointer sprite they sit above
 * every window.  A pixel-mode app, though, blits its own pixels straight
 * into the framebuffer, so the desktop keeps the overlays above it:
 *
 *   1. no repair request is delivered over an open menu -- otherwise the
 *      app repaints exactly the rectangle the menu was just painted into
 *      and erases it (the XEYES "menu vanishes" bug: the menu-open
 *      frame's own repair expose did exactly that);
 *   2. an app's ESC ] F flush re-stamps any menu over its content, like
 *      the pointer (wm_pixel_service_frame);
 *   3. no pointer tracking is forwarded while a menu is open -- the
 *      pointer is a menu pointer then (wm_hover_service).
 *
 * The rectangles are exactly the ones chrome_capture() diffs, so the
 * close frame's damage repaints (and re-exposes) the region the menu
 * covered.  out[] holds 3 entries -- Apps menu, right-click menu, app
 * dropdown -- zero-size when closed (at most one is ever open). */

static void overlay_rects(struct desktop_rect out[3]) {
  for (int i = 0; i < 3; i++) {
    out[i].x = 0;
    out[i].y = 0;
    out[i].w = 0;
    out[i].h = 0;
  }
  if (start_menu_open) {
    int visible = START_MENU_VISIBLE;
    if (num_menu_items < visible) visible = num_menu_items;
    int h = visible * 20 + 8;
    out[0].x = 4;
    out[0].y = TASKBAR_Y - h - 12;
    out[0].w = 220;
    out[0].h = h + 24;
  }
  if (menu_open) {
    out[1].x = menu_x;
    out[1].y = menu_y;
    out[1].w = 120;
    out[1].h = num_menu_items * 20;
  }
  if (app_menu_open) {
    struct window *w = find_window(app_menu_win_id);
    int items = 0;
    if (w && app_menu_idx >= 0 && app_menu_idx < w->num_menus)
      items = w->menus[app_menu_idx].num_items;
    if (items > 0) {
      out[2].x = app_menu_x;
      out[2].y = app_menu_y;
      out[2].w = 100;
      out[2].h = items * 20;
    }
  }
}

/* Intersect a screen rect with (x,y,w,h); 0 when disjoint. */
static int overlay_intersect(const struct desktop_rect *r, int x, int y, int w,
                             int h, int *ox, int *oy, int *ow, int *oh) {
  int x0 = r->x > x ? r->x : x;
  int y0 = r->y > y ? r->y : y;
  int x1 = (r->x + r->w < x + w) ? r->x + r->w : x + w;
  int y1 = (r->y + r->h < y + h) ? r->y + r->h : y + h;
  if (x1 <= x0 || y1 <= y0) return 0;
  *ox = x0;
  *oy = y0;
  *ow = x1 - x0;
  *oh = y1 - y0;
  return 1;
}

/* Subtract one content-relative `cut` rect from every piece in ps[0..n);
 * the result replaces the list.  Returns the new count (0 when fully
 * covered).  One cut splits a rect 4 ways and only one menu is ever open,
 * so ps[16] has room to spare; pieces that somehow do not fit are simply
 * not delivered (the menu-close damage repairs their region). */
static int rect_subtract(struct wm_rect *ps, int n, int max,
                         const struct wm_rect *cut) {
  struct wm_rect out[16];
  int m = 0;
  for (int i = 0; i < n && m < max; i++) {
    struct wm_rect p = ps[i];
    int x0 = cut->x > p.x ? cut->x : p.x;
    int y0 = cut->y > p.y ? cut->y : p.y;
    int x1 = (cut->x + cut->w < p.x + p.w) ? cut->x + cut->w : p.x + p.w;
    int y1 = (cut->y + cut->h < p.y + p.h) ? cut->y + cut->h : p.y + p.h;
    if (x1 <= x0 || y1 <= y0) {          /* no overlap: keep the whole */
      if (m < max) out[m++] = p;
      continue;
    }
    /* Above, below, left and right of the overlap, keeping the pieces
     * that have area. */
    struct wm_rect cand[4];
    cand[0].x = p.x; cand[0].y = p.y; cand[0].w = p.w; cand[0].h = y0 - p.y;
    cand[1].x = p.x; cand[1].y = y1; cand[1].w = p.w; cand[1].h = p.y + p.h - y1;
    cand[2].x = p.x; cand[2].y = y0; cand[2].w = x0 - p.x; cand[2].h = y1 - y0;
    cand[3].x = x1; cand[3].y = y0; cand[3].w = p.x + p.w - x1; cand[3].h = y1 - y0;
    for (int k = 0; k < 4 && m < max; k++) {
      if (cand[k].w > 0 && cand[k].h > 0) out[m++] = cand[k];
    }
  }
  for (int i = 0; i < m; i++) ps[i] = out[i];
  return m;
}

/* Send one repair piece with every open overlay cut out of it. */
static void overlay_send_piece(struct window *win, int x, int y, int w, int h,
                               const struct desktop_rect ov[3], int cx, int cy) {
  struct wm_rect ps[16];
  int n = 1;
  ps[0].x = x;
  ps[0].y = y;
  ps[0].w = w;
  ps[0].h = h;
  for (int i = 0; i < 3; i++) {
    if (ov[i].w <= 0 || ov[i].h <= 0) continue;
    struct wm_rect cut;
    cut.x = ov[i].x - cx;
    cut.y = ov[i].y - cy;
    cut.w = ov[i].w;
    cut.h = ov[i].h;
    n = rect_subtract(ps, n, 16, &cut);
    if (n == 0) return;
  }
  for (int i = 0; i < n; i++)
    wm_pixel_send_expose(win, ps[i].x, ps[i].y, ps[i].w, ps[i].h);
}

/* Deliver a pixel window's queued repairs, holding back anything under
 * an open menu (see the overlay note above).  Without an open menu this
 * is exactly wm_pixel_flush_exposes(). */
static void overlay_flush_exposes(struct window *win) {
  struct desktop_rect ov[3];
  overlay_rects(ov);
  if (ov[0].w <= 0 && ov[1].w <= 0 && ov[2].w <= 0) {
    wm_pixel_flush_exposes(win);
    return;
  }
  int cx, cy, cw, ch;
  wm_pixel_content_rect(win, &cx, &cy, &cw, &ch);
  if (win->pix_expose_full) {
    win->pix_expose_full = 0;
    overlay_send_piece(win, 0, 0, cw, ch, ov, cx, cy);
    return;
  }
  for (int i = 0; i < win->pix_expose_n; i++) {
    struct wm_rect r = win->pix_expose[i];
    overlay_send_piece(win, r.x, r.y, r.w, r.h, ov, cx, cy);
  }
  win->pix_expose_n = 0;
}

/* Snapshot the chrome state the next frame diffs against. */
static void chrome_capture(struct desktop_chrome *c) {
  c->win_count = num_windows;
  c->focus = focused_window;
  c->cursor_x = mouse_x;
  c->cursor_y = mouse_y;
  get_clock_string(c->clock);

  /* Menu overlays: one source of truth with the compositor's overlay
   * handling (overlay_rects) and the pixel-window repair filter. */
  {
    struct desktop_rect ov[3];
    overlay_rects(ov);
    c->start_menu = ov[0];
    c->rc_menu = ov[1];
    c->app_menu = ov[2];
  }
  c->start_sel = start_menu_open ? start_sel : 0;
  c->start_scroll = start_menu_open ? start_scroll : 0;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    c->chrome_dirty[i] = (i < num_windows) ? windows[i].chrome_dirty : 0;
  }
}

static int rect_equal(const struct desktop_rect *a, const struct desktop_rect *b) {
  return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

static void rect_add(struct desktop_rect *out, int *n, int max,
                     int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  for (int i = 0; i < *n; i++) {
    /* Already covered (e.g. a menu that did not move): keep the list tight. */
    if (out[i].x == x && out[i].y == y && out[i].w == w && out[i].h == h) return;
  }
  if (*n >= max) return;
  out[*n].x = x; out[*n].y = y; out[*n].w = w; out[*n].h = h;
  (*n)++;
}

/* A whole window including the 1px drop shadow just outside the frame
 * (see wm_draw_windows). */
static void rect_add_window(struct desktop_rect *out, int *n, int max, int win_id) {
  struct window *w = find_window(win_id);
  if (!w) return;
  rect_add(out, n, max, w->x, w->y, w->w + 2, w->h + 2);
}

/* Pointer sprite bounds for the hotspot at (x,y): the 8x12 arrow plus its
 * 1px outline in every direction. */
static void rect_add_cursor(struct desktop_rect *out, int *n, int max, int x, int y) {
  rect_add(out, n, max, x - 1, y - 1, 10, 14);
}

int desktop_damage(const struct desktop_chrome *prev,
                   const struct desktop_chrome *cur,
                   struct desktop_rect *out, int max) {
  int n = 0;

  /* Windows created/removed: the tiling moved every window. */
  if (prev->win_count != cur->win_count) return -1;

  /* Focus changed: both windows' chrome (title/menu colours) repaints,
   * plus the taskbar's focus highlight. */
  if (prev->focus != cur->focus) {
    rect_add_window(out, &n, max, prev->focus);
    rect_add_window(out, &n, max, cur->focus);
    rect_add(out, &n, max, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H);
  }

  /* Pointer moved: restore the old sprite, draw the new one. */
  if (prev->cursor_x != cur->cursor_x || prev->cursor_y != cur->cursor_y) {
    rect_add_cursor(out, &n, max, prev->cursor_x, prev->cursor_y);
    rect_add_cursor(out, &n, max, cur->cursor_x, cur->cursor_y);
  }

  /* Window titles/menus changed (ESC ] T / ESC ] M): the window's chrome,
   * its taskbar button label and - when open - its menu dropdown. */
  for (int i = 0; i < cur->win_count && i < MAX_WINDOWS; i++) {
    if (!cur->chrome_dirty[i]) continue;
    rect_add_window(out, &n, max, windows[i].id);
    rect_add(out, &n, max, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H);
    if (cur->app_menu.w > 0)
      rect_add(out, &n, max, cur->app_menu.x, cur->app_menu.y,
               cur->app_menu.w, cur->app_menu.h);
  }

  /* Menus: opened, closed, moved or re-selected inside. */
  if (!rect_equal(&prev->start_menu, &cur->start_menu) ||
      (cur->start_menu.w > 0 &&
       (prev->start_sel != cur->start_sel ||
        prev->start_scroll != cur->start_scroll))) {
    rect_add(out, &n, max, prev->start_menu.x, prev->start_menu.y,
             prev->start_menu.w, prev->start_menu.h);
    rect_add(out, &n, max, cur->start_menu.x, cur->start_menu.y,
             cur->start_menu.w, cur->start_menu.h);
    /* The Apps button toggles its highlight with the menu. */
    if ((prev->start_menu.w > 0) != (cur->start_menu.w > 0))
      rect_add(out, &n, max, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H);
  }
  if (!rect_equal(&prev->rc_menu, &cur->rc_menu)) {
    rect_add(out, &n, max, prev->rc_menu.x, prev->rc_menu.y,
             prev->rc_menu.w, prev->rc_menu.h);
    rect_add(out, &n, max, cur->rc_menu.x, cur->rc_menu.y,
             cur->rc_menu.w, cur->rc_menu.h);
  }
  if (!rect_equal(&prev->app_menu, &cur->app_menu)) {
    rect_add(out, &n, max, prev->app_menu.x, prev->app_menu.y,
             prev->app_menu.w, prev->app_menu.h);
    rect_add(out, &n, max, cur->app_menu.x, cur->app_menu.y,
             cur->app_menu.w, cur->app_menu.h);
  }

  /* Taskbar clock. */
  if (!name_is(prev->clock, cur->clock))
    rect_add(out, &n, max, SCREEN_WIDTH - CLOCK_W, TASKBAR_Y, CLOCK_W, TASKBAR_H);

  return n;
}

/* Composite one frame and push it to the display.  The full path keeps a
 * full present; a damage frame presents exactly the screen rectangles it
 * painted -- the chrome passes (dmg[]), the window-text repairs and the
 * pointer re-stamp (GX D1: docs/graphics-accel.md; presenting anything
 * less would leave stale pixels, so the union is what goes out).  The
 * present call is made even for an empty set: it is the frame boundary
 * the desktop harnesses tick on, and count == 0 is a driver no-op. */

/* ---- Present list (GX D1: docs/graphics-accel.md) --------------------
 * struct desktop_rect / struct wm_rect / struct fb_rect are all four
 * 32-bit ints (x, y, w, h in screen pixels), so the rectangles move
 * between the three as-is.  Overflow (impossible with the frame's fixed
 * budget: <= DMG_MAX-1 chrome rects + <= MAX_WINDOWS row bands + the
 * pointer cell) folds into a bounding box rather than dropping a rect,
 * which keeps pixel equality a hard guarantee. */
#define PRESENT_MAX 32

static void present_add(struct fb_rect *out, int *n, int max, int x, int y,
                        int w, int h) {
  if (w <= 0 || h <= 0) return;
  for (int i = 0; i < *n; i++) {
    if (out[i].x == x && out[i].y == y && out[i].w == w && out[i].h == h)
      return;                          /* already covered */
  }
  if (*n >= max) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    for (int i = 0; i < *n; i++) {
      if (out[i].x < x0) x0 = out[i].x;
      if (out[i].y < y0) y0 = out[i].y;
      if (out[i].x + out[i].w > x1) x1 = out[i].x + out[i].w;
      if (out[i].y + out[i].h > y1) y1 = out[i].y + out[i].h;
    }
    out[0].x = x0; out[0].y = y0; out[0].w = x1 - x0; out[0].h = y1 - y0;
    *n = 1;
    return;
  }
  out[*n].x = x; out[*n].y = y; out[*n].w = w; out[*n].h = h;
  (*n)++;
}

static void paint_frame(void) {
  static struct desktop_chrome last;
  static int have_last = 0;
  struct desktop_chrome cur;
  struct desktop_rect dmg[DMG_MAX];

  chrome_capture(&cur);

  int count = desktop_damage(&last, &cur, dmg, DMG_MAX);
  int full = !have_last || count < 0 || count >= DMG_MAX;

  if (full) {
    graphics_reset_base_clip();
    paint_scene();
    /* The scene pass repainted every window: pixel-mode content must be
     * blitted back by its app (see window.h). */
    for (int i = 0; i < num_windows; i++) {
      if (windows[i].pixel_mode) wm_pixel_queue_expose_all(&windows[i]);
    }
    flush_fb();
  } else {
    struct fb_rect present[PRESENT_MAX];
    int present_n = 0;
    int rows_painted = 0;

    /* 1. Window text damage first (see the contract above).  The repaired
     * band is not part of dmg[], so it joins the present list here. */
    for (int i = 0; i < num_windows; i++) {
      if (wm_draw_window_rows(&windows[i])) {
        rows_painted = 1;
        present_add(present, &present_n, PRESENT_MAX, windows[i].row_damage.x,
                    windows[i].row_damage.y, windows[i].row_damage.w,
                    windows[i].row_damage.h);
      }
    }
    /* 2. Everything else that moved, one clipped scene pass per rect. */
    for (int i = 0; i < count; i++) {
      present_add(present, &present_n, PRESENT_MAX, dmg[i].x, dmg[i].y,
                  dmg[i].w, dmg[i].h);
      graphics_set_base_clip(dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h);
      paint_scene();
    }
    graphics_reset_base_clip();
    /* 3. Scene passes may have painted over pixel-mode content. */
    for (int i = 0; i < count; i++) {
      for (int w = 0; w < num_windows; w++) {
        if (windows[w].pixel_mode)
          wm_pixel_queue_expose(&windows[w], dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h);
      }
    }
    /* Repainted rows may have covered the pointer: it is stamped outside
     * every clip, so its cell is presented too. */
    if (rows_painted) {
      wm_draw_cursor(mouse_x, mouse_y);
      present_add(present, &present_n, PRESENT_MAX, mouse_x - 1, mouse_y - 1,
                  10, 14);
    }

    /* Present exactly what this frame painted.  A count of 0 is still a
     * call: the harnesses' frame tick hangs off it (empty damage = no
     * repaint, but the frame boundary stays). */
    flush_fb_rects(present, present_n);
  }

  last = cur;
  have_last = 1;
  for (int i = 0; i < num_windows; i++) windows[i].chrome_dirty = 0;
}

/* Post-frame service for pixel-mode windows: re-stamp the pointer -- and
 * any open menu -- for apps that just painted the framebuffer (ESC ] F),
 * then deliver queued repair requests (clipped around open menus).  Runs
 * right after the frame's present, so a repair the frame queued reaches
 * the app in the same iteration.  Only the stamps are the desktop's
 * pixels: the app presented its own content, so the re-stamp rects are
 * presented here with a rect flush (nothing stamped = no flush). */
static void wm_pixel_service_frame(void) {
  struct desktop_rect ov[3];
  overlay_rects(ov);
  for (int i = 0; i < num_windows; i++) {
    struct window *win = &windows[i];
    if (!win->pixel_mode) continue;
    if (win->pix_restamp) {
      win->pix_restamp = 0;
      int cx, cy, cw, ch;
      wm_pixel_content_rect(win, &cx, &cy, &cw, &ch);
      struct fb_rect stamped[12];
      int sn = 0;
      /* Menus sit above the app's pixels: re-stamp the part of each open
       * menu that overlaps the content this flush may have painted. */
      for (int k = 0; k < 3; k++) {
        int ox, oy, ow, oh;
        if (ov[k].w <= 0 || ov[k].h <= 0) continue;
        if (!overlay_intersect(&ov[k], cx, cy, cw, ch, &ox, &oy, &ow, &oh)) continue;
        graphics_set_clip(ox, oy, ow, oh);
        draw_menu();
        draw_start_menu();
        graphics_reset_clip();
        present_add(stamped, &sn, 12, ox, oy, ow, oh);
      }
      if (mouse_x >= cx && mouse_x < cx + cw &&
          mouse_y >= cy && mouse_y < cy + ch) {
        wm_draw_cursor(mouse_x, mouse_y);
        present_add(stamped, &sn, 12, mouse_x - 1, mouse_y - 1, 10, 14);
      }
      if (sn > 0) flush_fb_rects(stamped, sn);
    }
    overlay_flush_exposes(win);
  }
}

/* ====================================================================== */
/* Window output: escape state machine and terminal-mode dispatch         */
/*                                                                        */
/* Every captured output byte of a window goes through                    */
/* desktop_process_output_byte(): escape sequences are collected (CSI /   */
/* OSC) and handed to the handlers, plain bytes go to the text tail ---   */
/* or, for a terminal-mode window, to the character grid.  Non-static so  */
/* host tests can feed a window's byte stream directly                    */
/* (src/host/desktop_term_test.c).                                        */
/* ====================================================================== */

/* Plain byte for a terminal-mode window. */
static void term_put_byte(struct window *win, char c) {
  if (c >= 0x20 && c != 0x7f) wm_term_putc(win, c);
  else if (c == '\n') wm_term_newline(win);
  else if (c == '\r') wm_term_cr(win);
  else if (c == '\b') wm_term_bs(win);
  else if (c == '\t') wm_term_tab(win);
  else if (c == '\f') wm_term_erase_display(win, 2);   /* form feed = clear */
  /* other control bytes (BEL and friends) are dropped */
}

/* Read one decimal parameter; advances *i; -1 when there is none. */
static int csi_param(const char *s, int *i) {
  int v = -1;
  while (s[*i] >= '0' && s[*i] <= '9') {
    if (v < 0) v = 0;
    v = v * 10 + (s[*i] - '0');
    (*i)++;
  }
  return v;
}

/* CSI sequence for a terminal-mode window.  The collected buffer starts at
 * '[' and ends with the final byte.  The curses shim emits exactly:
 *   ESC [ <row> ; <col> H    cursor position
 *   ESC [ 2J                 clear
 *   ESC [ <n>m               SGR (0, 1, 4, 7)
 *   ESC [ ?25h / ?25l        caret on / off
 * Anything else is parsed leniently and ignored, so a future app's extra
 * sequences cannot corrupt the grid. */
static void term_handle_csi(struct window *win, const char *seq) {
  const char *p = seq;
  if (*p != '[') return;
  p++;
  int len = 0;
  while (p[len]) len++;
  if (len == 0) return;
  char final = p[len - 1];
  int i = 0;
  int priv = 0;
  if (p[0] == '?') { priv = 1; i = 1; }

  switch (final) {
  case 'H': case 'f': {                    /* cursor position (1-based) */
      int row = csi_param(p, &i);
      if (p[i] == ';') i++;
      int col = csi_param(p, &i);
      wm_term_cup(win, row < 0 ? 1 : row, col < 0 ? 1 : col);
      break;
    }
  case 'A': case 'B': case 'C': case 'D': { /* relative cursor moves */
      int n = csi_param(p, &i);
      if (n < 1) n = 1;
      int row = win->term_cur_row, col = win->term_cur_col;
      if (final == 'A') row -= n;
      if (final == 'B') row += n;
      if (final == 'C') col += n;
      if (final == 'D') col -= n;
      wm_term_cup(win, row + 1, col + 1);
      break;
    }
  case 'J': {
      int mode = csi_param(p, &i);
      wm_term_erase_display(win, mode < 0 ? 0 : mode);
      break;
    }
  case 'K': {
      int mode = csi_param(p, &i);
      wm_term_erase_line(win, mode < 0 ? 0 : mode);
      break;
    }
  case 'h': case 'l': {                    /* DEC private modes */
      if (priv) {
        int m = csi_param(p, &i);
        if (m == 25) wm_term_set_cursor_visible(win, final == 'h');
      }
      break;
    }
  case 'm': {                              /* SGR: apply each parameter */
      for (;;) {
        int v = csi_param(p, &i);
        if (v >= 0) wm_term_sgr(win, v);
        if (p[i] != ';') break;
        i++;
      }
      break;
    }
  default: break;
  }
}

/* Feed one output byte of a window: escape state machine + dispatch.
 * Non-static (host tests drive a window's byte stream through this). */
void desktop_process_output_byte(struct window *win, char c) {
  if (win->escape_state == 1) {
    if (win->escape_len < (int)sizeof(win->escape_buf) - 1)
      win->escape_buf[win->escape_len++] = c;
    if (c == '[') win->escape_state = 2;       // CSI sequence
    else if (c == ']') win->escape_state = 3;  // OSC sequence
    else {
      win->escape_state = 0;
      win->escape_len = 0;
    }
    return;
  }

  if (win->escape_state == 2) {
    if (win->escape_len < (int)sizeof(win->escape_buf) - 1)
      win->escape_buf[win->escape_len++] = c;
    if ((c >= 0x40 && c <= 0x7E) ||
        win->escape_len >= (int)sizeof(win->escape_buf) - 1) {
      win->escape_buf[win->escape_len] = '\0';
      if (win->term_mode) term_handle_csi(win, win->escape_buf);
      else if (c == 'J') wm_text_clear(win);
      win->escape_state = 0;
      win->escape_len = 0;
    }
    return;
  }

  if (win->escape_state == 3) {
    if (c == '\a' || c == '~' ||
        win->escape_len >= (int)sizeof(win->escape_buf) - 1) {
      win->escape_buf[win->escape_len] = '\0';
      wm_handle_app_escape(win->id, win->escape_buf);
      win->escape_state = 0;
      win->escape_len = 0;
    } else {
      win->escape_buf[win->escape_len++] = c;
    }
    return;
  }

  if (c == '\033') {
    win->escape_state = 1;
    win->escape_len = 0;
    return;
  }

  if (win->term_mode) {
    term_put_byte(win, c);
    return;
  }

  if (c == '\f') wm_text_clear(win);
  else if (c == '\b') wm_text_backspace(win);
  else wm_text_putc(win, c);
}

int main(void);

#ifndef HOST_TEST
#ifndef DESKTOP_TEST_WRAPPER
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif
#endif

/* ---- SIGPIPE ---------------------------------------------------------
 * The desktop must outlive its children.  A pixel app can exit between a
 * desktop write (a repair request, a close request or a pointer report)
 * and the drain that notices the exit; the write then hits a pipe whose
 * reader is gone.  The kernel raises SIGPIPE on such a write (P5/D10,
 * kernel pipe.c) and the default disposition kills the writing process
 * GROUP -- i.e. the whole desktop, halting the machine.  Reproduced on
 * the pristine freeze build (2/9 xcalc E2E runs), so it is a pre-existing
 * race, not this lane's damage-rect change.  Ignore SIGPIPE: the failed
 * write just returns -EPIPE (every writer below already ignores the
 * result) and the next drain pass removes the dead window.  The desktop's
 * bespoke link set ($(DESKTOP_BIN)) has no libc_signal.o, so the device
 * build issues SYS_SIGACTION directly with the same 40-byte user struct
 * libc's sigaction() marshals (see <signal.h>); the host build uses the
 * host libc's signal(). */
static void desktop_ignore_sigpipe(void) {
#ifdef HOST_TEST
  signal(SIGPIPE, SIG_IGN);
#else
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_mask.__bits[0] = 0;
  sa.sa_mask.__bits[1] = 0;
  sa.sa_flags = 0;
  sa.sa_restorer = 0;
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = SIGPIPE;
  register long rsi __asm__("rsi") = (long)&sa;
  register long rdx __asm__("rdx") = 0;
  register long r10 __asm__("r10") = 0;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(SYS_SIGACTION), "r"(rdi), "r"(rsi), "r"(rdx),
                     "r"(r10)
                   : "rcx", "r11", "memory");
  (void)ret;
#else
  register long x8 __asm__("x8") = SYS_SIGACTION;
  register long x0 __asm__("x0") = SIGPIPE;
  register long x1 __asm__("x1") = (long)&sa;
  register long x2 __asm__("x2") = 0;
  register long x3 __asm__("x3") = 0;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  (void)x0;
#endif
#endif
}

int main(void) {
  desktop_ignore_sigpipe();
  print("Desktop starting...\n");
  if (graphics_init() < 0) {
    print("Failed to initialize graphics.\n");
    exit(0);
  }
  wm_init();
  load_menu();

  mouse_x = SCREEN_WIDTH / 2;
  mouse_y = SCREEN_HEIGHT / 2;
  focused_window = -1;
  struct virtio_input_event events[16];

#ifdef DESKTOP_TEST_AUTO_LAUNCH
#endif
  int needs_redraw = 1;
  int last_clock_ms = -1000;

  while (1) {
    /* Periodic redraw so the taskbar clock ticks (>= 1s). */
    int now_ms = sysinfo(1, 0, 0);
    if (now_ms - last_clock_ms >= 1000) {
      last_clock_ms = now_ms;
      needs_redraw = 1;
    }

    int num = get_events(events, 16);

    for (int i = 0; i < num; i++) {
      struct virtio_input_event *ev = &events[i];

      if (ev->type == EV_KEY) {
        if (ev->code == 42 || ev->code == 54) {
          shift_pressed = ev->value;
          continue;
        }
        if (ev->code == 29 || ev->code == 97) {   /* Ctrl (left / right) */
          ctrl_pressed = ev->value;
          continue;
        }
        if (ev->code == 56 || ev->code == 100) {  /* Alt (left / right) */
          alt_pressed = ev->value;
          continue;
        }
        if (ev->value == 0 && ev->code != 0x110) {
          /* Key release: a released key must never auto-repeat.  (The
           * mouse button release is handled by the BTN_LEFT block.) */
          if (ev->code == held_key_code) held_key_code = -1;
          continue;
        }
        if (ev->code == 0x110) { // BTN_LEFT (mouse click)

          if (ev->value == 0) {  // release: close any drag session
            desktop_drag_end(mouse_x, mouse_y);
            needs_redraw = 1;
            continue;
          }

          if (ev->value == 1) {  // press



            if (app_menu_open) {
              struct window* win = 0;
              for(int w=0; w<num_windows; w++) if (windows[w].id == app_menu_win_id) { win = &windows[w]; break; }
              if (win && app_menu_idx >= 0 && app_menu_idx < win->num_menus) {
                int num_items = win->menus[app_menu_idx].num_items;
                if (mouse_x >= app_menu_x && mouse_x < app_menu_x + 100 &&
                    mouse_y >= app_menu_y && mouse_y < app_menu_y + num_items * 20) {
                  int selected = (mouse_y - app_menu_y) / 20;
                  char seq[16] = "\033[M0;0~";
                  seq[3] = '0' + app_menu_idx;
                  seq[5] = '0' + selected;
                  write(win->stdin_fd, seq, 7);
                }
              }
              app_menu_open = 0;
              needs_redraw = 1;
            } else if (start_menu_open) {
              /* Start menu click: item, Apps button toggle, or dismiss. */
              int visible = START_MENU_VISIBLE;
              if (num_menu_items < visible) visible = num_menu_items;
              int w = 220;
              int h = visible * 20 + 8;
              int sx = 4;
              int sy = TASKBAR_Y - h;
              if (mouse_x >= APPS_BTN_X && mouse_x < APPS_BTN_X + APPS_BTN_W &&
                  mouse_y >= TASKBAR_Y) {
                start_menu_open = 0;
              } else if (mouse_x >= sx && mouse_x < sx + w &&
                         mouse_y >= sy + 4 && mouse_y < sy + 4 + visible * 20) {
                int idx = start_scroll + (mouse_y - (sy + 4)) / 20;
                if (idx >= 0 && idx < num_menu_items) {
                  start_menu_open = 0;
                  launch_menu_item(idx);
                }
              } else {
                start_menu_open = 0;
              }
              needs_redraw = 1;
            } else if (menu_open) {
              if (mouse_x >= menu_x && mouse_x < menu_x + 120 &&
                  mouse_y >= menu_y && mouse_y < menu_y + num_menu_items * 20) {
                int selected = (mouse_y - menu_y) / 20;
                launch_menu_item(selected);
              }
              menu_open = 0;
              needs_redraw = 1;
            } else if (mouse_y >= TASKBAR_Y) {
              /* Taskbar clicks */
              if (mouse_x >= APPS_BTN_X && mouse_x < APPS_BTN_X + APPS_BTN_W) {
                start_menu_open = !start_menu_open;
                if (start_menu_open) {
                  start_sel = 0;
                  start_scroll = 0;
                }
              } else {
                int bw;
                taskbar_button_geometry(&bw);
                int bx = TASKBAR_BTN_X;
                for (int w = 0; w < num_windows; w++) {
                  if (mouse_x >= bx && mouse_x < bx + bw - 2) {
                    focused_window = windows[w].id;
                    break;
                  }
                  bx += bw;
                }
              }
              needs_redraw = 1;
            } else {
              int win_id = wm_get_window_at(mouse_x, mouse_y);
              if (win_id >= 0) {
                for (int w = 0; w < num_windows; w++) {
                  if (windows[w].id == win_id) {
                    if (mouse_y >= windows[w].y + 2 &&
                        mouse_y <= windows[w].y + 18 &&
                        mouse_x >= windows[w].x + windows[w].w - 18 &&
                        mouse_x <= windows[w].x + windows[w].w - 2) {
                      desktop_request_close(win_id, now_ms);
                      if (focused_window == win_id)
                        focused_window = -1;
                    } else if (mouse_y >= windows[w].y + 18 && mouse_y <= windows[w].y + 34) {
                      int m_x = windows[w].x + 10;
                      for (int m = 0; m < windows[w].num_menus; m++) {
                        int len = 0;
                        while(windows[w].menus[m].name[len]) len++;
                        int width = len * 8 + 16;
                        if (mouse_x >= m_x && mouse_x < m_x + width) {

                          app_menu_open = 1;


                          app_menu_win_id = win_id;
                          app_menu_idx = m;
                          app_menu_x = m_x;
                          app_menu_y = windows[w].y + 34;
                          needs_redraw = 1;
                          break;
                        }
                        m_x += width;
                      }
                    } else {
                      focused_window = win_id;
                      /* Forward content-area presses to apps that opted in
                       * and open a drag session for the window. */
                      if (windows[w].mouse_events && mouse_y >= windows[w].y + 34) {
                        int x, y;
                        char seq[24];
                        wm_mouse_local(&windows[w], mouse_x, mouse_y, &x, &y);
                        int n = wm_build_mouse_seq(seq, sizeof seq, 'P', x, y, 1);
                        desktop_send_hook(win_id, seq, n);
                        desktop_drag_begin(win_id, x, y);
                      }
                    }
                    needs_redraw = 1;
                    break;
                  }
                }
              }
            }
          }
        } else if (ev->code == 0x111) { // BTN_RIGHT (right click)
          if (ev->value == 1) {         // press
            menu_open = 1;
            menu_x = mouse_x;
            menu_y = mouse_y;
            needs_redraw = 1;
          }
        } else if (ev->value == 1) { // Key press
          if (ev->code == 62) { // F4: close the focused window (keyboard X button)
            if (focused_window >= 0) {
              int closing = focused_window;
              desktop_request_close(closing, now_ms);
              if (focused_window == closing)
                focused_window = -1;
              needs_redraw = 1;
            }
          } else if (ev->code == 102 || ev->code == 104 ||
                     ev->code == 107 || ev->code == 109) {
            // Home / PgUp / End / PgDn
            if (start_menu_open) {
              int visible = START_MENU_VISIBLE;
              if (num_menu_items < visible) visible = num_menu_items;
              if (ev->code == 102) start_sel = 0;                  // Home
              if (ev->code == 107) start_sel = num_menu_items - 1; // End
              if (ev->code == 104) start_sel -= visible;           // PgUp
              if (ev->code == 109) start_sel += visible;           // PgDn
              if (start_sel < 0) start_sel = 0;
              if (start_sel > num_menu_items - 1) start_sel = num_menu_items - 1;
              start_menu_ensure_visible();
              needs_redraw = 1;
            } else if (focused_window >= 0) {
              // Forward as standard terminal escape sequences.
              forward_key_to_focused(ev->code, now_ms);
              /* No repaint: forwarding does not change the desktop. */
            }
          } else
            // Arrow keys (evdev codes 103-108): forward to the focused window as
            // 3-byte ESC sequences (ESC [ A/B/C/D) so dialogs can navigate.
            if (ev->code >= 103 && ev->code <= 108) {
              if (start_menu_open) {
                /* Navigate the start menu. */
                if (ev->code == 103) start_sel--;
                if (ev->code == 108) start_sel++;
                if (start_sel < 0) start_sel = 0;
                if (start_sel > num_menu_items - 1) start_sel = num_menu_items - 1;
                if (start_sel < 0) start_sel = 0;
                start_menu_ensure_visible();
                needs_redraw = 1;
              } else if (focused_window >= 0) {
                /* Forward the ESC sequence (write() delivers it atomically
                 * here: the pipe has ample room for a single keypress) and
                 * arm auto-repeat, so a held arrow keeps moving. */
                forward_key_to_focused(ev->code, now_ms);
                /* No repaint here: forwarding a key does not change
                 * anything on screen.  If the app reacts, its output
                 * triggers the frame that shows the change. */
              }
            } else if (ev->code == 28 && start_menu_open) { // Enter launches selection
              int idx = start_sel;
              start_menu_open = 0;
              launch_menu_item(idx);
              needs_redraw = 1;
            } else if (ev->code != 62) {
              /* Everything else: the mapped byte (Shift/Ctrl applied) or a
               * special-key sequence, straight to the focused window. */
              forward_key_to_focused(ev->code, now_ms);
            }
        }
      } else if (ev->type == EV_ABS) {
        if (ev->code == ABS_X) {
          mouse_x = (ev->value * SCREEN_WIDTH) / 0x7FFF;
          needs_redraw = 1;
        } else if (ev->code == ABS_Y) {
          mouse_y = (ev->value * SCREEN_HEIGHT) / 0x7FFF;
          needs_redraw = 1;
        }
        /* Pointer motion during a drag session is forwarded to the window. */
        if (drag_win_id >= 0) desktop_drag_move(mouse_x, mouse_y);
      } else if (ev->type == EV_REL) {
        /* Mouse wheel: EV_REL/REL_WHEEL, +1 per detent up / -1 down (what
         * QEMU's virtio input emits for wheel-button events; see
         * desktop_wheel above).  Forwarded to the pixel window under the
         * pointer as btn 4/5 press+release pairs. */
        if (ev->code == REL_WHEEL && ev->value != 0)
          desktop_wheel(mouse_x, mouse_y, (int)ev->value);
      }
    }

    /* Pointer tracking: hand the (possibly just moved) position to every
     * pixel window that opted into pointer events.  Runs every pass (not
     * only on motion) so a throttled position is delivered once its
     * interval has passed; see wm_hover_service. */
    wm_hover_service(mouse_x, mouse_y, now_ms);

    /* A key held on the keyboard keeps firing through the focused window
     * (software auto-repeat; see key_repeat_tick). */
    key_repeat_tick(now_ms);

    /* A close asked of a pixel window (ESC [ D ~) falls back to the kill
     * once the grace has passed; a window the app already exited clears
     * the timer in the drain loop below. */
    if (desktop_close_tick(now_ms)) needs_redraw = 1;

    // Poll windows for stdout.  Drain a window's whole pending output in a
    // single iteration (bounded), instead of one 63-byte chunk per frame:
    // a full-screen app update then costs one frame with one repaint
    // instead of one full repaint per chunk.
    for (int i = 0; i < num_windows; i++) {
      int fd = windows[i].stdout_fd;
      int drained = 0;
      int idle_rounds = 0;
      for (;;) {
        int avail = available(fd);
        if (avail < 0) {
          // Process exited (every writer closed and the pipe is empty).
          // When this iteration already read output, let the frame below
          // paint it and close the window on the next one instead.
          if (drained > 0) break;
          if (focused_window == windows[i].id) {
            focused_window = -1;
          }
          if (drag_win_id == windows[i].id) {
            desktop_drag_cancel();
          }
          /* The app exited during a pending close grace: note it so the
           * fallback timer does not fire a kill at a dead pid. */
          desktop_close_note_gone(windows[i].id);
          wm_remove_window(windows[i].id);
          i--; // Adjust index after removal
          needs_redraw = 1;
          drained = -1; // nothing left to paint for this window
          break;
        }
        if (avail == 0) {
          // Pipe empty right now.  A writer between two writes (e.g. one
          // print("\f") followed by the screen) refills as soon as it is
          // scheduled again, so wait a bounded number of rounds before
          // painting: that is what makes one "\f + screen" update land in
          // one frame instead of one frame per pipe-full.
          if (drained <= 0 || drained >= 4096) break;
          if (idle_rounds++ >= 6) break;
          yield();
          continue;
        }
        char buf[512];
        int want = avail > (int)sizeof buf ? (int)sizeof buf : avail;
        int r = read(fd, buf, want);
        if (r <= 0) break;
        drained += r;
        idle_rounds = 0;
        for (int k = 0; k < r; k++) {
          desktop_process_output_byte(&windows[i], buf[k]);
        }
        if (drained >= 4096) break; // stay fair with a streaming writer
      }
      if (drained > 0) needs_redraw = 1;
    }

    if (num > 0 || needs_redraw) {
      /* paint_frame presents exactly the rects it painted (full present on
       * a whole-scene repaint); wm_pixel_service_frame presents only the
       * pixels it re-stamps over an app's own frame. */
      paint_frame();
      wm_pixel_service_frame();
      needs_redraw = 0;
    } else {
      yield();
    }
  }

  exit(0);
  return 0;
}
