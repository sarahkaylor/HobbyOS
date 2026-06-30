#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

int my_strstr(const char *haystack, const char *needle) {
    if (!*needle) return 1;
    for (int i = 0; haystack[i]; i++) {
        int match = 1;
        for (int j = 0; needle[j]; j++) {
            if (haystack[i + j] != needle[j]) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
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

void grep_fd(int fd, const char *pattern) {
    char line[512];
    while (read_line(fd, line, sizeof(line)) >= 0) {
        if (my_strstr(line, pattern)) {
            print(line);
        }
    }
}

int main(void) {
    char arg_buf[256];
    read_arg_file("GREP.ARG", arg_buf, sizeof(arg_buf));

    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);

    if (argc < 1) {
        print("Usage: grep <pattern> [file...]\n");
        return 1;
    }

    const char *pattern = argv[0];

    if (argc == 1) {
        grep_fd(0, pattern);
    } else {
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i]);
            if (fd < 0) {
                print("grep: ");
                print(argv[i]);
                print(": No such file or directory\n");
            } else {
                grep_fd(fd, pattern);
                close(fd);
            }
        }
    }
    return 0;
}
