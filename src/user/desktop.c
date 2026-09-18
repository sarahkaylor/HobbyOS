#include "graphics.h"
#include "libc.h"
#include "window.h"



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
    }
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
}

/* Strip a trailing ".BIN" (and any extension) from a program name. */
static void app_name_from_bin(const char *bin, char *out, int max) {
  int i = 0;
  while (bin[i] && bin[i] != '.' && i < max - 1) {
    out[i] = bin[i];
    i++;
  }
  out[i] = '\0';
}

/* Launch menu item `idx` into a new tiled window. */
static void launch_menu_item(int idx) {
  if (idx < 0 || idx >= num_menu_items) return;

  int in_pipe[2], out_pipe[2];
  pipe(in_pipe);
  pipe(out_pipe);

  int pid = spawn2(menu_items[idx], in_pipe[0], out_pipe[1], -1, 0);
  if (pid >= 0) {
    int win_id = wm_create_window(COLOR(16, 18, 30), pid, out_pipe[0], in_pipe[1]);
    char name[24];
    app_name_from_bin(menu_items[idx], name, sizeof(name));
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
    wm_draw_text(menu_x + 5, menu_y + i * 20 + 5, menu_items[i],
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
    if (idx == start_sel) {
      graphics_draw_rect(x + 2, row_y, w - 4, 20, COLOR(96, 166, 255));
      wm_draw_text(x + 8, row_y + 5, menu_items[idx], COLOR(255, 255, 255));
    } else {
      wm_draw_text(x + 8, row_y + 5, menu_items[idx], COLOR(20, 20, 24));
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

  int mouse_x = SCREEN_WIDTH / 2;
  int mouse_y = SCREEN_HEIGHT / 2;
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
                      /* Forward content-area clicks to apps that opted in. */
                      if (windows[w].mouse_events && mouse_y >= windows[w].y + 34) {
                        int col = (mouse_x - (windows[w].x + 10)) / 8;
                        int row = (mouse_y - (windows[w].y + 44)) / 10;
                        if (col < 0) col = 0;
                        if (row < 0) row = 0;
                        char seq[24];
                        int j = 0;
                        seq[j++] = '\033'; seq[j++] = '['; seq[j++] = 'P';
                        if (col >= 100) seq[j++] = '0' + col / 100;
                        if (col >= 10) seq[j++] = '0' + (col / 10) % 10;
                        seq[j++] = '0' + col % 10;
                        seq[j++] = ';';
                        if (row >= 100) seq[j++] = '0' + row / 100;
                        if (row >= 10) seq[j++] = '0' + (row / 10) % 10;
                        seq[j++] = '0' + row % 10;
                        seq[j++] = ';';
                        seq[j++] = '1';
                        seq[j++] = '~';
                        write(windows[w].stdin_fd, seq, j);
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
                needs_redraw = 1;
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
      }
    }

    // Poll windows for stdout
    for (int i = 0; i < num_windows; i++) {
      int fd = windows[i].stdout_fd;
      int avail = available(fd);
      if (avail > 0) {
        char buf[64];
        if (avail > 63)
          avail = 63;
        int r = read(fd, buf, avail);
        if (r > 0) {
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
          needs_redraw = 1;
        }
      } else if (avail < 0) {
        // Process exited
        if (focused_window == windows[i].id) {
          focused_window = -1;
        }
        wm_remove_window(windows[i].id);
        i--; // Adjust index after removal
        needs_redraw = 1;
      }
    }

    if (num > 0 || needs_redraw) {
      /* Wallpaper: vertical gradient behind the tiled windows. */
      graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - TASKBAR_H,
                               COLOR(16, 20, 38), COLOR(44, 56, 96));
      wm_draw_windows(focused_window);
      draw_menu();
      draw_start_menu();
      draw_taskbar();
      wm_draw_cursor(mouse_x, mouse_y);
      graphics_flush();
      needs_redraw = 0;
    } else {
      yield();
    }
  }

  exit(0);
  return 0;
}
