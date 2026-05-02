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

#define MAX_MENU_ITEMS 16
char menu_items[MAX_MENU_ITEMS][16];
int num_menu_items = 0;

int menu_open = 0;
int menu_x = 0;
int menu_y = 0;

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

extern struct window windows[MAX_WINDOWS];
extern int num_windows;
// Actually I need an accessor. Wait, we can modify window.h to expose a window
// getter, or just add wm_poll_io() to window.c. Let's implement reading output
// inside desktop.c if we expose the array, or just use wm_get_window_at to get
// focused. Better: Add wm_poll_io() to window.c. Wait, desktop.c handles
// syscalls. I will just declare externs for windows and num_windows for
// simplicity.

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
  main();
  exit();
}

int main(void) {
  print("Desktop starting...\n");
  if (graphics_init() < 0) {
    print("Failed to initialize graphics.\n");
    exit();
  }
  wm_init();
  load_menu();

  int mouse_x = SCREEN_WIDTH / 2;
  int mouse_y = SCREEN_HEIGHT / 2;
  int focused_window = -1;
  struct virtio_input_event events[16];

#ifdef DESKTOP_TEST_AUTO_LAUNCH
  int in_pipe[2], out_pipe[2];
  pipe(in_pipe);
  pipe(out_pipe);
  int pid = spawn2("CONSOLE.BIN", in_pipe[0], out_pipe[1]);
  if (pid >= 0) {
    focused_window = wm_create_window(COLOR(20, 20, 50), pid, out_pipe[0], in_pipe[1]);
    close(in_pipe[0]);
    close(out_pipe[1]);
  } else {
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
  }
#endif

  while (1) {
    int num = get_events(events, 16);
    int needs_redraw = 0;

    for (int i = 0; i < num; i++) {
      struct virtio_input_event *ev = &events[i];

      if (ev->type == EV_KEY) {
        if (ev->code == 0x110) { // BTN_LEFT (mouse click)
          if (ev->value == 1) {  // press
            if (menu_open) {
              if (mouse_x >= menu_x && mouse_x < menu_x + 120 &&
                  mouse_y >= menu_y && mouse_y < menu_y + num_menu_items * 20) {
                int selected = (mouse_y - menu_y) / 20;

                int in_pipe[2], out_pipe[2];
                pipe(in_pipe);
                pipe(out_pipe);

                int pid = spawn2(menu_items[selected], in_pipe[0], out_pipe[1]);
                if (pid >= 0) {
                  // Desktop uses in_pipe[1] to write to child's stdin
                  // Desktop uses out_pipe[0] to read child's stdout
                  focused_window = wm_create_window(COLOR(20, 20, 50), pid,
                                                    out_pipe[0], in_pipe[1]);
                  close(in_pipe[0]); // Desktop doesn't need read end of child's
                                     // stdin
                  close(out_pipe[1]); // Desktop doesn't need write end of
                                      // child's stdout
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
                      kill(windows[w].pid);
                      wm_remove_window(win_id);
                      if (focused_window == win_id)
                        focused_window = -1;
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
                    write(windows[w].stdin_fd, &c, 1);
                    break;
                  }
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
            if (windows[i].text_len < MAX_TEXT - 1) {
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
    }
  }

  exit();
  return 0;
}
