#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#define MAX_WINDOWS 12
#define MAX_TEXT 2048

#define MAX_WINDOW_MENUS 4
#define MAX_MENU_SUBITEMS 8
#define MENU_NAME_LEN 16

/* Height of the desktop taskbar at the bottom of the screen. */
#define TASKBAR_H 26

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
    
    int escape_state;
    char escape_buf[128];
    int escape_len;
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

/* Draw the mouse pointer sprite (hotspot at x,y). Draw this last so it
 * sits on top of everything else. */
void wm_draw_cursor(int x, int y);

#endif
