#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

#define MAX_LESS_BUF 8192
#define MAX_LESS_LINES 512
#define SCREEN_HEIGHT 15

char less_buf[MAX_LESS_BUF];
char *line_starts[MAX_LESS_LINES];
int total_lines = 0;

void draw_page(int current_line) {
    print("\f"); // Clear screen
    
    int end = current_line + SCREEN_HEIGHT;
    if (end > total_lines) end = total_lines;
    
    for (int i = current_line; i < end; i++) {
        // Print line up to newline or null
        char *p = line_starts[i];
        while (*p && *p != '\n') {
            char c = *p++;
            write(1, &c, 1);
        }
        print("\n");
    }
    
    print("\n-- less (q to quit, Space/Down to page, Up to scroll up) --");
}

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));

    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);

    int fd = 0; // default stdin
    if (argc >= 1) {
        fd = open(argv[0]);
        if (fd < 0) {
            print("less: ");
            print(argv[0]);
            print(": No such file or directory\n");
            return 1;
        }
    }

    // Read the file into less_buf
    int bytes_read = 0;
    while (bytes_read < MAX_LESS_BUF - 1) {
        int r = read(fd, less_buf + bytes_read, MAX_LESS_BUF - 1 - bytes_read);
        if (r <= 0) break;
        bytes_read += r;
    }
    less_buf[bytes_read] = '\0';
    if (fd != 0) close(fd);

    // Parse lines
    total_lines = 0;
    if (bytes_read > 0) {
        line_starts[total_lines++] = less_buf;
        for (int i = 0; i < bytes_read; i++) {
            if (less_buf[i] == '\n' && total_lines < MAX_LESS_LINES) {
                line_starts[total_lines++] = less_buf + i + 1;
            }
        }
    }

    int current_line = 0;
    if (total_lines > 0) {
        draw_page(current_line);
    } else {
        print("less: empty file or input\n");
        return 0;
    }

    while (1) {
        char c;
        if (read(0, &c, 1) > 0) {
            if (c == 'q' || c == 'Q') {
                print("\f"); // Clear screen on exit
                break;
            } else if (c == ' ' || c == '\n') {
                // Scroll down
                if (current_line + SCREEN_HEIGHT < total_lines) {
                    current_line += (c == ' ') ? SCREEN_HEIGHT : 1;
                    if (current_line + SCREEN_HEIGHT > total_lines) {
                        current_line = total_lines - SCREEN_HEIGHT;
                    }
                    draw_page(current_line);
                }
            } else if (c == 27) { // Escape sequence
                char seq[2];
                if (read(0, seq, 2) == 2) {
                    if (seq[0] == '[') {
                        if (seq[1] == 'A') { // Up arrow
                            if (current_line > 0) {
                                current_line--;
                                draw_page(current_line);
                            }
                        } else if (seq[1] == 'B') { // Down arrow
                            if (current_line + SCREEN_HEIGHT < total_lines) {
                                current_line++;
                                draw_page(current_line);
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}
