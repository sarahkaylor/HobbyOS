#include "libc.h"

#define MAX_FILE_SIZE 2000
char text_buffer[MAX_FILE_SIZE];
int text_len = 0;
int cursor_idx = 0;

int mode = 0; // 0 = NORMAL, 1 = COMMAND
char cmd_buffer[64];
int cmd_len = 0;
char current_file[64] = "TEST.TXT";

// string functions
void my_strcpy(char *dest, const char *src) {
    int i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

void redraw() {
    print("\f"); // Clear screen
    
    char out_buf[MAX_FILE_SIZE + 10];
    int o = 0;
    for (int i = 0; i < cursor_idx; i++) {
        out_buf[o++] = text_buffer[i];
    }
    out_buf[o++] = (mode == 0) ? '_' : '=';
    for (int i = cursor_idx; i < text_len; i++) {
        out_buf[o++] = text_buffer[i];
    }
    out_buf[o] = '\0';
    print(out_buf);
    
    if (mode == 1) {
        print("\n\n--- COMMAND MODE (w=save, r=load, q=quit) ---\n:");
        print(cmd_buffer);
        print("_");
    }
}

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
#else
int main(void) {
#endif
    print("\fHobbyOS Editor\nPress ESC for Command Mode\n");
    for (volatile int i = 0; i < 2000000; i++) {} // brief delay
    
    // Initial read attempt
    int fd = open(current_file);
    if (fd >= 0) {
        text_len = read(fd, text_buffer, MAX_FILE_SIZE - 1);
        if (text_len < 0) text_len = 0;
        text_buffer[text_len] = '\0';
        close(fd);
    } else {
        text_buffer[0] = '\0';
        text_len = 0;
    }
    cursor_idx = text_len;
    
    redraw();
    
    while (1) {
        char c;
        if (read(0, &c, 1) > 0) {
            if (c == 27) { // ESC key
                if (available(0) >= 2) {
                    char seq[2];
                    read(0, seq, 2);
                    if (seq[0] == '[') {
                        if (seq[1] == 'C' && cursor_idx < text_len) cursor_idx++;
                        if (seq[1] == 'D' && cursor_idx > 0) cursor_idx--;
                        // Simple UP/DOWN by moving 10 chars for now
                        if (seq[1] == 'A') {
                            cursor_idx -= 10;
                            if (cursor_idx < 0) cursor_idx = 0;
                        }
                        if (seq[1] == 'B') {
                            cursor_idx += 10;
                            if (cursor_idx > text_len) cursor_idx = text_len;
                        }
                    }
                } else {
                    mode = !mode;
                    cmd_len = 0;
                    cmd_buffer[0] = '\0';
                }
                redraw();
                continue;
            }
            
            if (mode == 0) {
                // NORMAL MODE
                if (c == '\b') {
                    if (cursor_idx > 0) {
                        for (int i = cursor_idx - 1; i < text_len - 1; i++) {
                            text_buffer[i] = text_buffer[i + 1];
                        }
                        text_len--;
                        cursor_idx--;
                        text_buffer[text_len] = '\0';
                        redraw();
                    }
                } else if (c == '\n' || (c >= 32 && c <= 126)) {
                    if (text_len < MAX_FILE_SIZE - 1) {
                        for (int i = text_len; i > cursor_idx; i--) {
                            text_buffer[i] = text_buffer[i - 1];
                        }
                        text_buffer[cursor_idx] = c;
                        cursor_idx++;
                        text_len++;
                        text_buffer[text_len] = '\0';
                        redraw();
                    }
                }
            } else {
                // COMMAND MODE
                if (c == '\b') {
                    if (cmd_len > 0) {
                        cmd_len--;
                        cmd_buffer[cmd_len] = '\0';
                        redraw();
                    }
                } else if (c == '\n') {
                    if (cmd_buffer[0] == 'w') {
                        // write
                        char *fname = current_file;
                        if (cmd_len > 2 && cmd_buffer[1] == ' ') {
                            fname = &cmd_buffer[2];
                            my_strcpy(current_file, fname);
                        }
                        int out_fd = open(current_file);
                        if (out_fd >= 0) {
                            write(out_fd, text_buffer, text_len);
                            close(out_fd);
                        }
                        mode = 0;
                        redraw();
                    } else if (cmd_buffer[0] == 'r') {
                        // read
                        char *fname = current_file;
                        if (cmd_len > 2 && cmd_buffer[1] == ' ') {
                            fname = &cmd_buffer[2];
                            my_strcpy(current_file, fname);
                        }
                        int in_fd = open(current_file);
                        if (in_fd >= 0) {
                            text_len = read(in_fd, text_buffer, MAX_FILE_SIZE - 1);
                            if (text_len < 0) text_len = 0;
                            text_buffer[text_len] = '\0';
                            close(in_fd);
                        } else {
                            // Failed to open, just clear
                            text_len = 0;
                            text_buffer[0] = '\0';
                        }
                        mode = 0;
                        redraw();
                    } else if (cmd_buffer[0] == 'q') {
                        // quit
                        exit(0);
                    } else {
                        // unknown, return to normal
                        mode = 0;
                        redraw();
                    }
                } else if (c >= 32 && c <= 126) {
                    if (cmd_len < 63) {
                        cmd_buffer[cmd_len++] = c;
                        cmd_buffer[cmd_len] = '\0';
                        redraw();
                    }
                }
            }
        }
    }
}
