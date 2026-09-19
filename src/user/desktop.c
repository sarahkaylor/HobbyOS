#include "graphics.h"
#include "libc.h"
#include "window.h"
#include "desktop_damage.h"



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

#define MAX_MENU_ITEMS 80
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
 * button release as ESC [ R <col>;<row>;<btn> ~, both clamped to the
 * window's content cell grid. Press, drag and release all go through
 * desktop_send_hook so host tests can capture them instead of writing to
 * the app's pipe. Windows that never see a press never get G/R events.
 *
 * The helper functions below are non-static so src/host/desktop_drag_test.c
 * can drive them directly. */

static int drag_win_id = -1;   /* window that received the press (-1 = none) */
static int drag_last_col = -1; /* last cell reported to that window         */
static int drag_last_row = -1;

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

static struct window *find_window(int id) {
    for (int i = 0; i < num_windows; i++) {
        if (windows[i].id == id) return &windows[i];
    }
    return 0;
}

/* Build one mouse escape sequence: ESC [ <kind> <col> ; <row> ; <btn> ~
 * kind: 'P' press, 'G' drag, 'R' release. col/row/btn must be >= 0.
 * Returns the number of bytes written (excluding the NUL terminator). */
int wm_build_mouse_seq(char *out, int cap, char kind, int col, int row, int btn) {
    char body[24];
    int j = 0;
    body[j++] = 27; body[j++] = '[';
    body[j++] = kind;
    if (col >= 100) body[j++] = (char)('0' + (col / 100) % 10);
    if (col >= 10)  body[j++] = (char)('0' + (col / 10) % 10);
    body[j++] = (char)('0' + col % 10);
    body[j++] = ';';
    if (row >= 100) body[j++] = (char)('0' + (row / 100) % 10);
    if (row >= 10)  body[j++] = (char)('0' + (row / 10) % 10);
    body[j++] = (char)('0' + row % 10);
    body[j++] = ';';
    body[j++] = (char)('0' + btn);
    body[j++] = '~';
    if (j > cap - 1) j = cap - 1;
    for (int i = 0; i < j; i++) out[i] = body[i];
    out[j] = '\0';
    return j;
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

/* Start a drag session after a press was delivered at (col,row). */
void desktop_drag_begin(int win_id, int col, int row) {
    drag_win_id = win_id;
    drag_last_col = col;
    drag_last_row = row;
}

/* End a drag session without sending anything (window closed, etc). */
void desktop_drag_cancel(void) {
    drag_win_id = -1;
    drag_last_col = -1;
    drag_last_row = -1;
}

/* Window currently holding the drag session (-1 = none). */
int desktop_drag_window(void) { return drag_win_id; }

/* Forward pointer motion while the button is held. Only a cell change is
 * reported (motion inside one text cell produces no event). */
void desktop_drag_move(int mx, int my) {
    if (drag_win_id < 0) return;
    struct window *w = find_window(drag_win_id);
    if (!w) { desktop_drag_cancel(); return; }
    int col, row;
    wm_mouse_cell(w, mx, my, &col, &row);
    if (col == drag_last_col && row == drag_last_row) return;
    drag_last_col = col;
    drag_last_row = row;
    char seq[24];
    int n = wm_build_mouse_seq(seq, sizeof seq, 'G', col, row, 1);
    desktop_send_hook(drag_win_id, seq, n);
}

/* Deliver the button release at the (clamped) cell and end the session. */
void desktop_drag_end(int mx, int my) {
    if (drag_win_id < 0) return;
    struct window *w = find_window(drag_win_id);
    if (w) {
        int col, row;
        wm_mouse_cell(w, mx, my, &col, &row);
        char seq[24];
        int n = wm_build_mouse_seq(seq, sizeof seq, 'R', col, row, 1);
        desktop_send_hook(drag_win_id, seq, n);
    }
    desktop_drag_cancel();
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
        /* Window title: ESC ] T <title> ~ */
        wm_set_window_title(win_id, seq + 2);
    } else if (seq[0] == ']' && seq[1] == 'P') {
        /* Pointer events opt-in: ESC ] P 1 ~ (1 = enable, 0 = disable) */
        for (int i = 0; i < num_windows; i++) {
            if (windows[i].id == win_id) {
                windows[i].mouse_events = (seq[2] == '1') ? 1 : 0;
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
  "FILES.BIN", "CALC.BIN", "CLOCK.BIN", "SYSMON.BIN", "HEX.BIN",
  "TASKS.BIN", "FIND.BIN",  "DIFF.BIN",  "NOTES.BIN",  "UNIT.BIN",
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
 * Non-static: exercised directly by the host test. */
void menu_display_name(const char *raw, char *out, int max) {
  copy_name(out, raw, max);
  int i = 0;
  while (out[i] && i < max - 1) i++;
  if (i >= 4 && name_is(out + i - 4, ".BIN")) out[i - 4] = '\0';
}

void load_menu(void) {
  num_menu_items = 0;
  while (num_menu_items < MAX_MENU_ITEMS) {
    struct sys_dirent ent;
    if (read_dir("/", num_menu_items, &ent) < 0) {
      break;
    }
    int k = 0;
    while (ent.name[k] && k < 15) {
        menu_items[num_menu_items][k] = ent.name[k];
        k++;
    }
    menu_items[num_menu_items][k] = '\0';
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

/* One full scene paint into the current clip: wallpaper, every window, the
 * menus, the taskbar and the pointer (which must stay on top). */
static void paint_scene(void) {
  /* Wallpaper: vertical gradient behind the tiled windows. */
  graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - TASKBAR_H,
                           COLOR(16, 20, 38), COLOR(44, 56, 96));
  wm_draw_windows(focused_window);
  draw_menu();
  draw_start_menu();
  draw_taskbar();
  wm_draw_cursor(mouse_x, mouse_y);
}

/* Snapshot the chrome state the next frame diffs against. */
static void chrome_capture(struct desktop_chrome *c) {
  c->win_count = num_windows;
  c->focus = focused_window;
  c->cursor_x = mouse_x;
  c->cursor_y = mouse_y;
  get_clock_string(c->clock);

  /* Apps menu: the panel plus the "more ^/v" labels just outside it. */
  c->start_menu.x = 0; c->start_menu.y = 0;
  c->start_menu.w = 0; c->start_menu.h = 0;
  c->start_sel = 0; c->start_scroll = 0;
  if (start_menu_open) {
    int visible = START_MENU_VISIBLE;
    if (num_menu_items < visible) visible = num_menu_items;
    int h = visible * 20 + 8;
    c->start_menu.x = 4;
    c->start_menu.y = TASKBAR_Y - h - 12;
    c->start_menu.w = 220;
    c->start_menu.h = h + 24;
    c->start_sel = start_sel;
    c->start_scroll = start_scroll;
  }

  /* Right-click menu (draw_menu()). */
  c->rc_menu.x = 0; c->rc_menu.y = 0; c->rc_menu.w = 0; c->rc_menu.h = 0;
  if (menu_open) {
    c->rc_menu.x = menu_x;
    c->rc_menu.y = menu_y;
    c->rc_menu.w = 120;
    c->rc_menu.h = num_menu_items * 20;
  }

  /* Per-window menu dropdown (draw_menu()). */
  c->app_menu.x = 0; c->app_menu.y = 0; c->app_menu.w = 0; c->app_menu.h = 0;
  if (app_menu_open) {
    struct window *w = find_window(app_menu_win_id);
    int items = 0;
    if (w && app_menu_idx >= 0 && app_menu_idx < w->num_menus)
      items = w->menus[app_menu_idx].num_items;
    if (items > 0) {
      c->app_menu.x = app_menu_x;
      c->app_menu.y = app_menu_y;
      c->app_menu.w = 100;
      c->app_menu.h = items * 20;
    }
  }

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

/* Composite one frame and push it to the display. */
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
  } else {
    /* 1. Window text damage first (see the contract above). */
    int rows_painted = 0;
    for (int i = 0; i < num_windows; i++) {
      if (wm_draw_window_rows(&windows[i])) rows_painted = 1;
    }
    /* 2. Everything else that moved, one clipped scene pass per rect. */
    for (int i = 0; i < count; i++) {
      graphics_set_base_clip(dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h);
      paint_scene();
    }
    graphics_reset_base_clip();
    /* Repainted rows may have covered the pointer. */
    if (rows_painted) wm_draw_cursor(mouse_x, mouse_y);
  }

  last = cur;
  have_last = 1;
  for (int i = 0; i < num_windows; i++) windows[i].chrome_dirty = 0;
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

int main(void) {
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
                                            kill(windows[w].pid, 9);
                      wm_remove_window(win_id);
                      if (focused_window == win_id)
                        focused_window = -1;
                      if (drag_win_id == win_id)
                        desktop_drag_cancel();
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
                        int col, row;
                        char seq[24];
                        wm_mouse_cell(&windows[w], mouse_x, mouse_y, &col, &row);
                        int n = wm_build_mouse_seq(seq, sizeof seq, 'P', col, row, 1);
                        desktop_send_hook(win_id, seq, n);
                        desktop_drag_begin(win_id, col, row);
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
              for (int w = 0; w < num_windows; w++) {
                if (windows[w].id == focused_window) {
                  kill(windows[w].pid, 9);
                  break;
                }
              }
              wm_remove_window(focused_window);
              if (drag_win_id == focused_window)
                desktop_drag_cancel();
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
              char seq[4];
              int sl = 3;
              seq[0] = 27; seq[1] = '[';
              if (ev->code == 102) seq[2] = 'H';
              if (ev->code == 107) seq[2] = 'F';
              if (ev->code == 104) { seq[2] = '5'; seq[3] = '~'; sl = 4; }
              if (ev->code == 109) { seq[2] = '6'; seq[3] = '~'; sl = 4; }
              for (int w = 0; w < num_windows; w++) {
                if (windows[w].id == focused_window) {
                  int wr = write(windows[w].stdin_fd, seq, sl);
                  (void)wr;
                  break;
                }
              }
              /* No repaint: forwarding does not change the desktop. */
            }
          } else
          // Arrow keys (evdev codes 103-108): forward to the focused window as
          // 3-byte ESC sequences (ESC [ A/B/C/D) so dialogs can navigate.
          if (ev->code >= 103 && ev->code <= 108) {
            char seq[3] = {27, '[', 0};
            if (ev->code == 103) seq[2] = 'A'; // UP
            if (ev->code == 108) seq[2] = 'B'; // DOWN
            if (ev->code == 106) seq[2] = 'C'; // RIGHT
            if (ev->code == 105) seq[2] = 'D'; // LEFT
            if (seq[2] != 0) {
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
                for (int w = 0; w < num_windows; w++) {
                  if (windows[w].id == focused_window) {
                    /* Forward the full 3-byte ESC sequence to the focused
                     * window's stdin. write() delivers all 3 atomically here
                     * (the pipe has ample room for a single keypress). */
                    int wr = write(windows[w].stdin_fd, seq, 3);
                    (void)wr;
                    break;
                  }
                }
                /* No repaint here: forwarding a key does not change
                 * anything on screen.  If the app reacts, its output
                 * triggers the frame that shows the change. */
              }
            }
          } else if (ev->code == 28 && start_menu_open) { // Enter launches selection
            int idx = start_sel;
            start_menu_open = 0;
            launch_menu_item(idx);
            needs_redraw = 1;
          } else if (ev->code < 128) {
            char c = shift_pressed ? shift_keymap[ev->code] : keymap[ev->code];
            if (c) {
              if (focused_window >= 0) {
                // Find window to get its stdin_fd
                for (int w = 0; w < num_windows; w++) {
                  if (windows[w].id == focused_window) {
                    // print_console("Writing key to child window...\n"); // removed to avoid noise if it works
                    int wr = write(windows[w].stdin_fd, &c, 1);
                    (void)wr; // Ignore error for now
                    break;
                  }
                }
              }
            }
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
      }
    }

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
          char c = buf[k];
          if (windows[i].escape_state == 1) {
            windows[i].escape_buf[windows[i].escape_len++] = c;
            if (c == '[') {
              windows[i].escape_state = 2; // CSI sequence
            } else if (c == ']') {
              windows[i].escape_state = 3; // OSC sequence
            } else {
              windows[i].escape_state = 0;
              windows[i].escape_len = 0;
            }
          } else if (windows[i].escape_state == 2) {
            windows[i].escape_buf[windows[i].escape_len++] = c;
            if ((c >= 0x40 && c <= 0x7E) || windows[i].escape_len >= 127) {
              if (c == 'J') {
                windows[i].text_len = 0;
                windows[i].text[0] = '\0';
              }
              windows[i].escape_state = 0;
              windows[i].escape_len = 0;
            }
          } else if (windows[i].escape_state == 3) {
            if (c == '\a' || c == '~' || windows[i].escape_len >= 127) {
              windows[i].escape_buf[windows[i].escape_len] = '\0';
              wm_handle_app_escape(windows[i].id, windows[i].escape_buf);
              windows[i].escape_state = 0;
              windows[i].escape_len = 0;
            } else {
              windows[i].escape_buf[windows[i].escape_len++] = c;
            }
          } else if (c == '\033') {
            windows[i].escape_state = 1;
            windows[i].escape_len = 0;
          } else if (c == '\f') {
            windows[i].text_len = 0;
            windows[i].text[0] = '\0';
          } else if (c == '\b') {
            if (windows[i].text_len > 0) {
              windows[i].text_len--;
              windows[i].text[windows[i].text_len] = '\0';
            }
          } else if (windows[i].text_len < MAX_TEXT - 1) {
            windows[i].text[windows[i].text_len++] = c;
            windows[i].text[windows[i].text_len] = '\0';
          }
        }
        if (drained >= 4096) break; // stay fair with a streaming writer
      }
      if (drained > 0) needs_redraw = 1;
    }

    if (num > 0 || needs_redraw) {
      paint_frame();
      graphics_flush();
      needs_redraw = 0;
    } else {
      yield();
    }
  }

  exit(0);
  return 0;
}
