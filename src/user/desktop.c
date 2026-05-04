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

#define MAX_MENU_ITEMS 32
char menu_items[MAX_MENU_ITEMS][16];
int num_menu_items = 0;

int menu_open = 0;
int menu_x = 0;
int menu_y = 0;


extern struct window windows[MAX_WINDOWS];
extern int num_windows;

int app_menu_open = 0;
int app_menu_win_id = -1;
int app_menu_idx = -1;
int app_menu_x = 0;
int app_menu_y = 0;

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
    }
}

void load_menu(void) {
  num_menu_items = 0;
  while (num_menu_items < MAX_MENU_ITEMS) {
    if (read_dir(num_menu_items, menu_items[num_menu_items]) < 0) {
      break;
    }
    num_menu_items++;
  }
}

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


// Actually I need an accessor. Wait, we can modify window.h to expose a window
// getter, or just add wm_poll_io() to window.c. Let's implement reading output
// inside desktop.c if we expose the array, or just use wm_get_window_at to get
// focused. Better: Add wm_poll_io() to window.c. Wait, desktop.c handles
// syscalls. I will just declare externs for windows and num_windows for
// simplicity.

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
  int focused_window = -1;
  struct virtio_input_event events[16];

#ifdef DESKTOP_TEST_AUTO_LAUNCH
#endif

  while (1) {
    int num = get_events(events, 16);
    int needs_redraw = 0;

    for (int i = 0; i < num; i++) {
      struct virtio_input_event *ev = &events[i];

      if (ev->type == EV_KEY) {
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
            } else if (menu_open) {
              if (mouse_x >= menu_x && mouse_x < menu_x + 120 &&
                  mouse_y >= menu_y && mouse_y < menu_y + num_menu_items * 20) {
                int selected = (mouse_y - menu_y) / 20;

                int in_pipe[2], out_pipe[2];
                pipe(in_pipe);
                pipe(out_pipe);

                int pid = spawn2(menu_items[selected], in_pipe[0], out_pipe[1]);
                if (pid >= 0) {
                  focused_window = wm_create_window(COLOR(20, 20, 50), pid,
                                                    out_pipe[0], in_pipe[1]);
                  close(in_pipe[0]);
                  close(out_pipe[1]);
                } else {
                  close(in_pipe[0]);
                  close(in_pipe[1]);
                  close(out_pipe[0]);
                  close(out_pipe[1]);
                }
              }
              menu_open = 0;
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
          if (ev->code < 128) {
            char c = keymap[ev->code];
            if (c) {
              if (focused_window >= 0) {
                // Find window to get its stdin_fd
                for (int w = 0; w < num_windows; w++) {
                  if (windows[w].id == focused_window) {
                    int wr = write(windows[w].stdin_fd, &c, 1);
                    (void)wr; // Ignore error for now
                    break;
                  }
                }
              }
              needs_redraw = 1;
            }
          } else if (ev->code >= 103 && ev->code <= 108) {
            char seq[3] = {27, '[', 0};
            if (ev->code == 103) seq[2] = 'A'; // UP
            if (ev->code == 108) seq[2] = 'B'; // DOWN
            if (ev->code == 106) seq[2] = 'C'; // RIGHT
            if (ev->code == 105) seq[2] = 'D'; // LEFT
            if (seq[2] != 0 && focused_window >= 0) {
                for (int w = 0; w < num_windows; w++) {
                  if (windows[w].id == focused_window) {
                    int wr = write(windows[w].stdin_fd, seq, 3);
                    (void)wr;
                    break;
                  }
                }
                needs_redraw = 1;
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
                        if (windows[i].escape_state > 0) {
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
      graphics_clear(COLOR(0, 0, 0));
      wm_draw_windows(focused_window);
      draw_menu();
      graphics_draw_rect(mouse_x, mouse_y, 4, 4, COLOR(255, 255, 255));
      graphics_flush();
    } else {
      yield();
    }
  }

  exit(0);
  return 0;
}
