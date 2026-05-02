#include "window.h"
#include "graphics.h"
#include "libc.h"
#include "font.h"

struct window windows[MAX_WINDOWS];
int num_windows = 0;

// Draw characters using the 8x8 font
void wm_draw_char(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 127) c = '?'; // Fallback for unprintable characters
    
    int font_idx = c - 32;
    for (int row = 0; row < 8; row++) {
        uint8_t row_data = font8x8[font_idx][row];
        for (int col = 0; col < 8; col++) {
            // Check if the bit is set from MSB to LSB
            if (row_data & (1 << (7 - col))) {
                graphics_draw_rect(x + col, y + row, 1, 1, color);
            }
        }
    }
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
    
    int w = SCREEN_WIDTH / cols;
    int h = SCREEN_HEIGHT / rows;
    
    for (int i = 0; i < num_windows; i++) {
        windows[i].x = (i % cols) * w;
        windows[i].y = (i / cols) * h;
        windows[i].w = w;
        windows[i].h = h;
    }
}

int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd) {
    if (num_windows >= MAX_WINDOWS) return -1;
    
    int id = num_windows;
    windows[id].id = id;
    windows[id].bg_color = bg_color;
    windows[id].border_color = COLOR(50, 50, 50); // Inactive border
    windows[id].text_len = 0;
    windows[id].text[0] = '\0';
    windows[id].pid = pid;
    windows[id].stdout_fd = stdout_fd;
    windows[id].stdin_fd = stdin_fd;
    
    num_windows++;
    update_layout();
    return id;
}

void wm_draw_windows(int focused_id) {
    for (int i = 0; i < num_windows; i++) {
        struct window* win = &windows[i];
        
        // Determine border color based on focus
        uint32_t border = (win->id == focused_id) ? COLOR(0, 255, 0) : win->border_color;
        
        // Draw border
        graphics_draw_rect(win->x, win->y, win->w, win->h, border);
        
        // Draw title bar
        graphics_draw_rect(win->x + 2, win->y + 2, win->w - 4, 16, COLOR(100, 100, 100));
        
        // Draw 'X' button
        graphics_draw_rect(win->x + win->w - 18, win->y + 2, 16, 16, COLOR(200, 50, 50));
        wm_draw_char(win->x + win->w - 14, win->y + 6, 'X', COLOR(255, 255, 255));
        
        // Draw background
        graphics_draw_rect(win->x + 2, win->y + 18, win->w - 4, win->h - 20, win->bg_color);
        
        // Draw text
        wm_draw_text(win->x + 10, win->y + 28, win->text, COLOR(255, 255, 255));
    }
}

int wm_get_window_at(int x, int y) {
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
