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

void wm_draw_windows(int focused_id) {
    for (int i = 0; i < num_windows; i++) {
        struct window* win = &windows[i];

        int focused = (win->id == focused_id);

        // Frame border (bright when focused)
        uint32_t border = focused ? COLOR(96, 166, 255) : win->border_color;
        graphics_draw_rect(win->x, win->y, win->w, win->h, border);
        graphics_draw_rect_outline(win->x, win->y, win->w, win->h, border);

        // Title bar (subtle vertical gradient, brighter when focused)
        uint32_t t_top = focused ? COLOR(64, 78, 110) : COLOR(52, 54, 66);
        uint32_t t_bot = focused ? COLOR(38, 46, 70) : COLOR(36, 37, 46);
        graphics_fill_gradient_v(win->x + 2, win->y + 2, win->w - 4, 16, t_top, t_bot);

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
        int text_top = win->y + 44;
        int text_bottom = win->y + win->h - 4;
        int max_rows = (text_bottom - text_top) / 10;
        if (max_rows < 1) max_rows = 1;

        int lines = count_lines(win->text);
        const char *start = win->text;
        if (lines > max_rows) {
            start = skip_lines(win->text, lines - max_rows);
        }

        graphics_set_clip(win->x + 2, win->y + 34, win->w - 4, win->h - 36);
        wm_draw_text(win->x + 10, text_top, start, COLOR(255, 255, 255));
        graphics_reset_clip();
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

void wm_handle_key(int window_id, char c) {
    if (window_id < 0 || window_id >= num_windows) return;

    struct window* win = &windows[window_id];

    if (c == '\b') { // Backspace
        if (win->text_len > 0) {
            win->text_len--;
            win->text[win->text_len] = '\0';
        }
    } else if (win->text_len < MAX_TEXT - 1) {
        win->text[win->text_len] = c;
        win->text_len++;
        win->text[win->text_len] = '\0';
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
