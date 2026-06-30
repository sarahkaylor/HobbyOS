#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

int my_atoi(const char *s) {
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

int read_line(int fd, char *buf, int max_len) {
    int len = 0;
    while (len < max_len - 1) {
        char c;
        int r = read(fd, &c, 1);
        if (r <= 0) {
            if (len == 0) return -1; // EOF
            break;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            buf[len++] = '\n';
            break;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    return len;
}

#define MAX_TAIL_LINES 100
#define LINE_BUF_SIZE 256
char lines_ring[MAX_TAIL_LINES][LINE_BUF_SIZE];

void tail_fd(int fd, int want_lines) {
    if (want_lines > MAX_TAIL_LINES) want_lines = MAX_TAIL_LINES;
    if (want_lines <= 0) return;

    int ring_idx = 0;
    int total_lines_read = 0;

    while (1) {
        int r = read_line(fd, lines_ring[ring_idx], LINE_BUF_SIZE);
        if (r < 0) break;
        total_lines_read++;
        ring_idx = (ring_idx + 1) % want_lines;
    }

    int start_idx = 0;
    int print_count = want_lines;
    if (total_lines_read < want_lines) {
        start_idx = 0;
        print_count = total_lines_read;
    } else {
        start_idx = ring_idx;
    }

    for (int i = 0; i < print_count; i++) {
        print(lines_ring[(start_idx + i) % want_lines]);
    }
}

int main(void) {
    char arg_buf[256];
    read_arg_file("TAIL.ARG", arg_buf, sizeof(arg_buf));

    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);

    int want_lines = 10;
    int file_arg_idx = -1;

    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == 'n') {
                if (i + 1 < argc) {
                    want_lines = my_atoi(argv[i + 1]);
                    i++; // Skip count
                }
            } else {
                // e.g. -5
                want_lines = my_atoi(argv[i] + 1);
            }
        } else {
            file_arg_idx = i;
            break;
        }
    }

    if (file_arg_idx == -1) {
        tail_fd(0, want_lines);
    } else {
        int fd = open(argv[file_arg_idx]);
        if (fd < 0) {
            print("tail: ");
            print(argv[file_arg_idx]);
            print(": No such file or directory\n");
            return 1;
        } else {
            tail_fd(fd, want_lines);
            close(fd);
        }
    }
    return 0;
}
