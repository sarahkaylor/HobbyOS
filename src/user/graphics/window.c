#include "window.h"
#include "graphics.h"
#include "libc.h"

#include "font.h"

struct window windows[MAX_WINDOWS];
int num_windows = 0;

/* ---- Text drawing (8x8 font, 8px cells, 10px line height) ---- */

void wm_draw_char(int x, int y, char c, uint32_t color) {
  graphics_draw_glyph(x, y, c, color, 1);
}

void wm_draw_text(int x, int y, const char* str, uint32_t color) {
  int cur_x = x;
  while (*str) {
    if (*str == '\n') {
      y += 10; // Move down (8 + padding)
      cur_x = x;
    } else {
      wm_draw_char(cur_x, y, *str, color);
      cur_x += 8; // character width
    }
    str++;
  }
}

void wm_init(void) {
  num_windows = 0;
}

static void update_layout(void) {
  if (num_windows == 0) return;

  int cols = 1, rows = 1;
  if (num_windows == 2) { cols = 2; rows = 1; }
  else if (num_windows == 3 || num_windows == 4) { cols = 2; rows = 2; }
  else if (num_windows > 4 && num_windows <= 6) { cols = 3; rows = 2; }
  else if (num_windows > 6 && num_windows <= 9) { cols = 3; rows = 3; }
  else if (num_windows > 9) { cols = 4; rows = 4; }

  /* The taskbar owns the bottom strip of the screen. */
  int area_h = SCREEN_HEIGHT - TASKBAR_H;
  int w = SCREEN_WIDTH / cols;
  int h = area_h / rows;

  for (int i = 0; i < num_windows; i++) {
    windows[i].x = (i % cols) * w;
    windows[i].y = (i / cols) * h;
    windows[i].w = w;
    windows[i].h = h;
  }
}

int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd) {
  if (num_windows >= MAX_WINDOWS) return -1;

  static int next_id = 0;
  int idx = num_windows; // Use num_windows for array index
  int win_id = next_id++;
  windows[idx].id = win_id;
  windows[idx].bg_color = bg_color;
  windows[idx].border_color = COLOR(64, 68, 82); // Inactive border
  windows[idx].title[0] = '\0';
  windows[idx].text_len = 0;
  windows[idx].text[0] = '\0';
  windows[idx].pid = pid;
  windows[idx].stdout_fd = stdout_fd;
  windows[idx].stdin_fd = stdin_fd;
  windows[idx].num_menus = 0;
  windows[idx].mouse_events = 0;
  windows[idx].escape_state = 0;
  windows[idx].escape_len = 0;
  windows[idx].rendered_valid = 0;   /* nothing painted yet */
  windows[idx].rendered_rows = 0;
  windows[idx].rendered_skip = 0;
  windows[idx].rendered_text[0] = '\0';
  windows[idx].chrome_dirty = 0;     /* a new window is painted whole */

  num_windows++;
  update_layout();
  return win_id;
}

void wm_set_window_title(int id, const char *title) {
  for (int i = 0; i < num_windows; i++) {
    if (windows[i].id == id) {
      int k = 0;
      while (title[k] && k < (int)sizeof(windows[i].title) - 1) {
        windows[i].title[k] = title[k];
        k++;
      }
      windows[i].title[k] = '\0';
      /* The title bar and this window's taskbar button must repaint. */
      windows[i].chrome_dirty = 1;
      return;
    }
  }
}

/* Number of text lines in the window buffer. */
static int count_lines(const char *text) {
  int lines = 1;
  for (int i = 0; text[i]; i++) {
    if (text[i] == '\n') lines++;
  }
  return lines;
}

/* Skip forward to the start of line `n` (0-based). */
static const char *skip_lines(const char *text, int n) {
  const char *p = text;
  while (n > 0 && *p) {
    if (*p == '\n') n--;
    p++;
  }
  return p;
}

/* ====================================================================== */
/* Damage helpers: track what a window's framebuffer region shows so a     */
/* repaint can be limited to the rows that actually changed.               */
/* ====================================================================== */

/* Top y of the first text row and how many rows fit in the window. */
static void window_content_geom(const struct window *win, int *top, int *rows) {
  int text_top = win->y + 44;
  int text_bottom = win->y + win->h - 4;
  int max_rows = (text_bottom - text_top) / 10;
  if (max_rows < 1) max_rows = 1;
  *top = text_top;
  *rows = max_rows;
}

/* First visible line (the newest lines win when the text does not fit). */
static int window_visible_skip(const char *text, int rows) {
  int lines = count_lines(text);
  return lines > rows ? lines - rows : 0;
}

/* Remember what this window's framebuffer region shows now. */
static void window_snapshot_save(struct window *win, int rows, int skip) {
  win->rendered_valid = 1;
  win->rendered_rows = rows;
  win->rendered_skip = skip;
  int i = 0;
  while (win->text[i] && i < MAX_TEXT - 1) {
    win->rendered_text[i] = win->text[i];
    i++;
  }
  win->rendered_text[i] = '\0';
}

void wm_window_invalidate(struct window *win) {
  win->rendered_valid = 0;
}

void wm_windows_invalidate_all(void) {
  for (int i = 0; i < num_windows; i++) windows[i].rendered_valid = 0;
}

/* Do the lines starting at a and b differ (comparing up to '\n' / NUL)? */
static int line_differs(const char *a, const char *b) {
  int i = 0;
  while (a[i] && a[i] != '\n' && b[i] && b[i] != '\n') {
    if (a[i] != b[i]) return 1;
    i++;
  }
  int a_end = (a[i] == '\0' || a[i] == '\n');
  int b_end = (b[i] == '\0' || b[i] == '\n');
  return !(a_end && b_end);
}

/* Advance both pointers past their current line. */
static void line_advance(const char **a, const char **b) {
  while (**a && **a != '\n') (*a)++;
  if (**a == '\n') (*a)++;
  while (**b && **b != '\n') (*b)++;
  if (**b == '\n') (*b)++;
}

/* Repaint content rows [r0..r1] (visible rows, 0-based) from the window's
 * current text, starting at visible line `skip`.  One row = the 10px band
 * the full painter would use: background fill + the line's glyphs. */
static void window_paint_rows(struct window *win, int top, int skip, int r0, int r1) {
  graphics_set_clip(win->x + 2, win->y + 34, win->w - 4, win->h - 36);
  for (int r = r0; r <= r1; r++) {
    int y = top + r * 10;
    graphics_draw_rect(win->x + 2, y, win->w - 4, 10, win->bg_color);
    const char *line = skip_lines(win->text, skip + r);
    int cx = win->x + 10;
    for (int i = 0; line[i] && line[i] != '\n'; i++) {
      wm_draw_char(cx, y, line[i], COLOR(255, 255, 255));
      cx += 8;
    }
  }
  graphics_reset_clip();
}

/* Repair a window's captured text at line granularity: repaint only the
 * content rows whose text changed since the last paint.  Returns 1 when
 * anything was painted, 0 when the screen already matches the text.
 *
 * This is what makes the full-screen "\f + print()" convention cheap: the
 * app still hands the WM a whole screen, but the WM only touches the rows
 * that differ.  Falls back to a full content repaint when the visible
 * window moved (scrolling) or the bookkeeping is invalid. */
int wm_draw_window_rows(struct window *win) {
  int top, rows;
  window_content_geom(win, &top, &rows);
  int skip = window_visible_skip(win->text, rows);

  if (!win->rendered_valid || rows != win->rendered_rows ||
      skip != win->rendered_skip) {
    window_paint_rows(win, top, skip, 0, rows - 1);
    window_snapshot_save(win, rows, skip);
    return 1;
  }

  const char *a = skip_lines(win->rendered_text, win->rendered_skip);
  const char *b = skip_lines(win->text, skip);
  int first = -1, last = -1;
  for (int r = 0; r < rows; r++) {
    if (!a[0] && !b[0]) break;          /* both texts ended */
    if (line_differs(a, b)) {
      if (first < 0) first = r;
      last = r;
    }
    line_advance(&a, &b);
  }
  if (first < 0) return 0;                /* nothing visible changed */

  window_paint_rows(win, top, skip, first, last);
  window_snapshot_save(win, rows, skip);
  return 1;
}

void wm_draw_windows(int focused_id) {
  for (int i = 0; i < num_windows; i++) {
    struct window* win = &windows[i];

    int focused = (win->id == focused_id);

    // Frame border (bright when focused)
    uint32_t border = focused ? COLOR(96, 166, 255) : win->border_color;
    graphics_draw_rect(win->x, win->y, win->w, win->h, border);
    graphics_draw_rect_outline(win->x, win->y, win->w, win->h, border);

    /* Drop shadow: a 1px dark band hugging the right and bottom edges of
     * the frame (the sides away from the "light"), plus the same band
     * just outside the frame so the shadow also shows in the 1px gaps
     * the tiling leaves between windows. The left/top edges stay bright,
     * which gives every window a raised look. */
    uint32_t shadow = COLOR(8, 10, 16);
    graphics_draw_vline(win->x + win->w - 1, win->y, win->h, shadow);
    graphics_draw_hline(win->x, win->y + win->h - 1, win->w, shadow);
    graphics_draw_vline(win->x + win->w, win->y, win->h, shadow);
    graphics_draw_hline(win->x, win->y + win->h, win->w, shadow);

    /* Title bar: bright blue for the focused window, muted gray-blue for
     * the others, with a darker fold along the bottom edge for depth. */
    uint32_t t_face = focused ? COLOR(96, 166, 255) : COLOR(72, 86, 120);
    uint32_t t_fold = focused ? COLOR(58, 108, 188) : COLOR(44, 54, 80);
    graphics_draw_rect(win->x + 2, win->y + 2, win->w - 4, 14, t_face);
    graphics_draw_rect(win->x + 2, win->y + 16, win->w - 4, 2, t_fold);

    // Title text (clipped so it never runs under the close button)
    if (win->title[0]) {
      graphics_set_clip(win->x + 6, win->y + 2, win->w - 28, 16);
      wm_draw_text(win->x + 8, win->y + 6, win->title, COLOR(235, 238, 245));
      graphics_reset_clip();
    }

    // Close button
    graphics_draw_rect(win->x + win->w - 18, win->y + 2, 16, 16, COLOR(178, 54, 54));
    graphics_draw_rect_outline(win->x + win->w - 18, win->y + 2, 16, 16, COLOR(230, 120, 120));
    wm_draw_char(win->x + win->w - 14, win->y + 6, 'x', COLOR(255, 240, 240));

    // Menu bar
    uint32_t mb = focused ? COLOR(206, 208, 214) : COLOR(168, 170, 176);
    graphics_draw_rect(win->x + 2, win->y + 18, win->w - 4, 16, mb);
    int menu_x = win->x + 10;
    for (int m = 0; m < win->num_menus; m++) {
      wm_draw_text(menu_x, win->y + 22, win->menus[m].name, COLOR(20, 20, 24));
      // rough width estimation: length * 8 + 16
      int len = 0;
      while(win->menus[m].name[len]) len++;
      menu_x += len * 8 + 16;
    }

    // Content background
    graphics_draw_rect(win->x + 2, win->y + 34, win->w - 4, win->h - 36, win->bg_color);

    // Content text with clipping and auto-scroll to the newest lines.
    int text_top, max_rows;
    window_content_geom(win, &text_top, &max_rows);
    int skip = window_visible_skip(win->text, max_rows);
    const char *start = skip_lines(win->text, skip);

    graphics_set_clip(win->x + 2, win->y + 34, win->w - 4, win->h - 36);
    wm_draw_text(win->x + 10, text_top, start, COLOR(255, 255, 255));
    graphics_reset_clip();

    /* Painted in full: the framebuffer now matches the captured text
     * (see wm_draw_window_rows - it relies on this bookkeeping). */
    window_snapshot_save(win, max_rows, skip);
  }
}

int wm_get_window_at(int x, int y) {
  /* Topmost first? Windows are tiled (no overlap), so order is fine. */
  for (int i = 0; i < num_windows; i++) {
    struct window* win = &windows[i];
    if (x >= win->x && x < win->x + win->w &&
        y >= win->y && y < win->y + win->h) {
      return win->id;
    }
  }
  return -1;
}

/* ---- Captured-text mutators ---- */

/* Drop the older half of the buffer, cutting at a line boundary when one
 * exists (a lone over-long line falls back to a plain byte cut). */
static void wm_text_drop_oldest_half(struct window *win) {
  int cut = win->text_len / 2;
  int at = cut;
  while (at < win->text_len && win->text[at] != '\n') at++;
  if (at < win->text_len) cut = at + 1;
  if (cut < 1) cut = 1;
  for (int k = 0; k + cut <= win->text_len; k++) {
    win->text[k] = win->text[k + cut];
  }
  win->text_len -= cut;
  win->text[win->text_len] = '\0';
}

/* Append one captured byte.  The window is a terminal tail: when the
 * buffer is full the oldest lines slide off to make room, so the newest
 * output always lands (dropping bytes once full froze a long output
 * mid-word, e.g. `grep --help` in the console window). */
void wm_text_putc(struct window *win, char c) {
  if (win->text_len >= MAX_TEXT - 1) {
    wm_text_drop_oldest_half(win);
  }
  win->text[win->text_len++] = c;
  win->text[win->text_len] = '\0';
}

void wm_text_backspace(struct window *win) {
  if (win->text_len > 0) {
    win->text_len--;
    win->text[win->text_len] = '\0';
  }
}

void wm_text_clear(struct window *win) {
  win->text_len = 0;
  win->text[0] = '\0';
}

void wm_handle_key(int window_id, char c) {
  if (window_id < 0 || window_id >= num_windows) return;

  struct window* win = &windows[window_id];

  if (c == '\b') { // Backspace
    wm_text_backspace(win);
  } else {
    wm_text_putc(win, c);
  }
}

void wm_remove_window(int id) {
  int idx = -1;
  for (int i = 0; i < num_windows; i++) {
    if (windows[i].id == id) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return;

  // Close FDs
  if (windows[idx].stdout_fd >= 0) close(windows[idx].stdout_fd);
  if (windows[idx].stdin_fd >= 0) close(windows[idx].stdin_fd);

  // Shift remaining
  for (int i = idx; i < num_windows - 1; i++) {
    char *dst = (char *)&windows[i];
    char *src = (char *)&windows[i + 1];
    for (unsigned long j = 0; j < sizeof(struct window); j++) {
      dst[j] = src[j];
    }
  }
  num_windows--;
  update_layout();
}

/* ---- Mouse cursor sprite ----
 * A classic arrow, 8x12 pixels. Drawn white with a 1px black outline
 * (the outline is produced by stamping the same shape offset in 8
 * directions first). */
static const uint8_t cursor_shape[12] = {
  0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF,
  0xF8, 0xD8, 0x0C, 0x0C
};

static void draw_cursor_shape(int x, int y, uint32_t color) {
  for (int row = 0; row < 12; row++) {
    uint8_t bits = cursor_shape[row];
    for (int col = 0; col < 8; col++) {
      if (bits & (1 << (7 - col))) {
        graphics_draw_pixel(x + col, y + row, color);
      }
    }
  }
}

void wm_draw_cursor(int x, int y) {
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) continue;
      draw_cursor_shape(x + dx, y + dy, COLOR(10, 10, 12));
    }
  }
  draw_cursor_shape(x, y, COLOR(250, 250, 252));
}
