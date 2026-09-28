#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#define MAX_WINDOWS 12
/* Captured output per window.  Sized so a console window can show a whole
 * typical command output at once (grep --help is ~4 KB) plus some
 * scrollback; the capture slides (wm_text_putc) once the buffer is full,
 * so nothing is ever silently dropped mid-word. */
#define MAX_TEXT 8192

#define MAX_WINDOW_MENUS 4
#define MAX_MENU_SUBITEMS 8
#define MENU_NAME_LEN 16

/* Height of the desktop taskbar at the bottom of the screen. */
#define TASKBAR_H 26

/* ---- Terminal mode (ESC ] V 1 ~) --------------------------------------
 * A window is a line-oriented text tail by default.  An app that wants a
 * character-addressed surface -- a full-screen "curses" program such as
 * the ported nano -- opts in by printing the OSC message ESC ] V 1 ~ (see
 * src/user/nano/curses.h for the whole protocol).  The desktop then
 * interprets that window's output as ANSI (cursor addressing, SGR
 * attributes, erases) and paints a character grid instead of the tail,
 * and answers with ESC ] S <rows>;<cols> ~ so the program learns the
 * surface it got; a later reflow sends a fresh ESC ] S.
 *
 * The grid covers what a window can actually show at the desktop's
 * 8x10-pixel cell pitch; the caps below match the curses shim's
 * (HBC_MAX_ROWS/COLS in hb_curses.c) so a size the shim is told is always
 * one it can hold. */
#define TERM_MAX_ROWS 72
#define TERM_MAX_COLS 128
#define TERM_CELLS (TERM_MAX_ROWS * TERM_MAX_COLS)

/* SGR bits kept per cell -- exactly the subset the curses shim emits
 * (emit_sgr in hb_curses.c: 0m, 1m, 4m, 7m). */
#define TERM_AT_REVERSE   0x01
#define TERM_AT_BOLD      0x02
#define TERM_AT_UNDERLINE 0x04

/* ---- Pixel mode (ESC ] X <w>;<h> ~) -----------------------------------
 * An app that wants a pixel-addressed surface -- the first is XCALC.BIN
 * running on the X11 support library (src/user/x11/) -- opts in by
 * printing the OSC message ESC ] X <w>;<h> ~ (a preferred content size in
 * pixels; 0 means "any").  The desktop then stops maintaining the
 * window's content area: the app owns those pixels and paints them
 * straight into the framebuffer through its own mapped view, and the
 * desktop answers with the rectangle the content area got:
 *
 *     ESC ] G <x>;<y>;<w>;<h> ~      (screen pixels, re-sent after reflow)
 *
 * Because the app's pixels live in the shared framebuffer, the WM keeps
 * two contracts with it:
 *
 *   - it never paints over content pixels itself: the wallpaper pass
 *     carves the content rectangles out and the chrome pass skips them;
 *   - whenever something the WM *does* paint may have covered them (a
 *     menu, the pointer, a full repaint) it sends a repair request:
 *
 *         ESC [ E ~                     repaint everything
 *         ESC [ E <x>;<y>;<w>;<h> ~     repaint that content-relative rect
 *
 * The X11 library answers by blitting the affected rectangle back from
 * its client-side shadow (its drawing never reaches the display until a
 * flush, exactly like Xlib's output buffer).
 *
 * Mouse input for a pixel window arrives as ESC [ P/G/R <x>;<y>;<btn> ~
 * with x;y in content-relative pixels (cell coordinates for text
 * windows).  After painting, the app prints ESC ] F ~ ("frame flushed")
 * so the desktop can re-stamp the mouse pointer, which app drawing may
 * have overwritten. */
#define PIX_MAX_EXPOSE 8

/* A rectangle in screen (or content-relative) pixels. */
struct wm_rect { int x, y, w, h; };

struct window_menu {
  char name[MENU_NAME_LEN];
  char items[MAX_MENU_SUBITEMS][MENU_NAME_LEN];
  int num_items;
};

struct window {
  int id;
  int x, y, w, h;
  uint32_t bg_color;
  uint32_t border_color;
  char title[24];              /* shown in the title bar; set via ESC ] T */
  char text[MAX_TEXT];
  int text_len;
  int pid;
  int stdout_fd;
  int stdin_fd;

  struct window_menu menus[MAX_WINDOW_MENUS];
  int num_menus;

  /* Set when the app opts into mouse events (ESC ] P 1 ~). The desktop
   * then forwards content-area clicks as ESC [ P <col>;<row>;<btn> ~ */
  int mouse_events;

  /* ---- Terminal mode (ESC ] V 1 ~); see wm_term_* below ---- */
  int term_mode;
  int term_rows, term_cols;        /* grid size in cells (what the app got) */
  int term_cur_row, term_cur_col;  /* cursor cell, 0-based */
  int term_cursor_visible;         /* DEC cursor visibility (ESC [ ?25h/l) */
  unsigned char term_attr;         /* current SGR attribute bits */
  unsigned char term_dirty[TERM_MAX_ROWS];  /* rows needing a repaint */
  int term_caret_row, term_caret_col;       /* where the caret bar is painted
                                               (-1 = none) */
  char term_ch[TERM_CELLS];        /* cell text */
  unsigned char term_at[TERM_CELLS]; /* cell attributes */

  /* ---- Pixel mode (ESC ] X); see the protocol block above ---- */
  int pixel_mode;
  int pix_pref_w, pix_pref_h;      /* preferred content size from ]X */
  int pix_expose_full;             /* queued repair: repaint everything */
  int pix_expose_n;                /* queued partial repairs */
  struct wm_rect pix_expose[PIX_MAX_EXPOSE];
  int pix_restamp;                 /* app painted: re-stamp the pointer */

  int escape_state;
  char escape_buf[128];
  int escape_len;

  /* ---- WM damage bookkeeping (see window.c) ----
   * What this window's framebuffer region currently shows: the text that
   * was painted last, the visible line it started at and how many rows
   * were drawn.  wm_draw_window_rows() compares the live text against
   * these to repaint only the rows that changed.  rendered_valid == 0
   * forces a full content repaint (fresh window, geometry change). */
  int  rendered_valid;
  int  rendered_rows;
  int  rendered_skip;
  char rendered_text[MAX_TEXT];

  /* Title or menu bar changed (ESC ] T / ESC ] M): the desktop must
   * repaint this window's chrome (and its taskbar button). */
  int  chrome_dirty;
};

void wm_init(void);
int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd);
void wm_draw_windows(int focused_id);
int wm_get_window_at(int x, int y);
void wm_handle_key(int window_id, char c);
void wm_remove_window(int id);
void wm_draw_text(int x, int y, const char* str, uint32_t color);
void wm_draw_char(int x, int y, char c, uint32_t color);
void wm_set_window_title(int id, const char *title);

/* ---- Captured-text mutators (window.c) ----
 * The desktop feeds every captured output byte through wm_text_putc, clears
 * on \f / CSI J and backspaces on \b.  A window is a terminal tail: when
 * the buffer is full the oldest lines slide off to make room, so the newest
 * output always lands and a long output never freezes mid-word (the plain
 * "drop when full" this replaces cut `grep --help` off at "  -I ... equi"). */
void wm_text_putc(struct window *win, char c);
void wm_text_backspace(struct window *win);
void wm_text_clear(struct window *win);

/* ---- Terminal-mode surface (window.c) ----
 * The desktop feeds a terminal-mode window's output through these: plain
 * bytes via wm_term_putc/wm_term_newline/wm_term_cr/wm_term_bs/
 * wm_term_tab, parsed ANSI via wm_term_cup/wm_term_erase_display/
 * wm_term_erase_line/wm_term_sgr/wm_term_set_cursor_visible.  Row/column
 * arguments are 1-based ANSI coordinates (0 and omitted mean 1).
 *
 * wm_term_begin() switches a window into terminal mode: it sizes the grid
 * to the window, clears it, and sends the app its size (ESC ] S <r>;<c> ~).
 * wm_term_resize() re-sizes + re-notifies after a reflow.  Both return 1
 * when the size changed. */
int  wm_term_begin(struct window *win);
int  wm_term_resize(struct window *win);
void wm_term_send_size(struct window *win);
void wm_term_putc(struct window *win, char c);
void wm_term_newline(struct window *win);
void wm_term_cr(struct window *win);
void wm_term_bs(struct window *win);
void wm_term_tab(struct window *win);
void wm_term_cup(struct window *win, int row1, int col1);
void wm_term_erase_display(struct window *win, int mode);
void wm_term_erase_line(struct window *win, int mode);
void wm_term_sgr(struct window *win, int which);
void wm_term_set_cursor_visible(struct window *win, int on);

/* ---- Pixel-mode surface (window.c) ----
 * wm_pixel_content_rect() reports the content area (the pixels the app
 * owns) for a window.  wm_pixel_resize() re-notifies a pixel window after
 * a reflow.  Repair requests are queued by the desktop while it paints
 * (wm_pixel_queue_expose* take SCREEN coordinates and clamp) and written
 * to the app after the frame's flush (wm_pixel_flush_exposes). */
void wm_pixel_content_rect(const struct window *win, int *x, int *y, int *w, int *h);
int  wm_pixel_resize(struct window *win);
void wm_pixel_send_geometry(struct window *win);
void wm_pixel_queue_expose(struct window *win, int x, int y, int w, int h);
void wm_pixel_queue_expose_all(struct window *win);
void wm_pixel_flush_exposes(struct window *win);

/* ---- Damage helpers (window.c) ----
 * wm_draw_window_rows() repairs a window's captured text at line
 * granularity: it repaints only the content rows whose text changed since
 * the window was last painted (a full-screen \f + print() rewrite from an
 * app normally costs a couple of rows).  Returns 1 when it painted
 * anything.  wm_draw_windows() paints a window in full and updates the
 * bookkeeping, so a full-frame pass keeps the diff exact. */
int  wm_draw_window_rows(struct window *win);
void wm_window_invalidate(struct window *win);
void wm_windows_invalidate_all(void);

/* Draw the mouse pointer sprite (hotspot at x,y). Draw this last so it
 * sits on top of everything else. */
void wm_draw_cursor(int x, int y);

#endif
