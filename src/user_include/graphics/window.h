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
