#include "libc.h"

#define MAX_FILE_SIZE 2000
char text_buffer[MAX_FILE_SIZE];
int text_len = 0;

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
    print(text_buffer);
    if (mode == 1) {
        print("\n\n--- COMMAND MODE (w=save, r=load, q=quit) ---\n:");
        print(cmd_buffer);
    }
}

__attribute__((section(".text._start")))
void _start(void) {
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
    
    redraw();
    
    while (1) {
        char c;
        if (read(0, &c, 1) > 0) {
            if (c == 27) { // ESC key
                mode = !mode;
                cmd_len = 0;
                cmd_buffer[0] = '\0';
                redraw();
                continue;
            }
            
            if (mode == 0) {
                // NORMAL MODE
                if (c == '\b') {
                    if (text_len > 0) {
                        text_len--;
                        text_buffer[text_len] = '\0';
                        redraw();
                    }
                } else if (c == '\n' || (c >= 32 && c <= 126)) {
                    if (text_len < MAX_FILE_SIZE - 1) {
                        text_buffer[text_len++] = c;
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
                        exit();
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
