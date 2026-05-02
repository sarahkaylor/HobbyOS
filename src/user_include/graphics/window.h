#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#define MAX_WINDOWS 4
#define MAX_TEXT 256

struct window {
    int id;
    int x, y, w, h;
    uint32_t bg_color;
    uint32_t border_color;
    char text[MAX_TEXT];
    int text_len;
    int pid;
    int stdout_fd;
    int stdin_fd;
};

void wm_init(void);
int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd);
void wm_draw_windows(int focused_id);
int wm_get_window_at(int x, int y);
void wm_handle_key(int window_id, char c);
void wm_remove_window(int id);
void wm_draw_text(int x, int y, const char* str, uint32_t color);

#endif
