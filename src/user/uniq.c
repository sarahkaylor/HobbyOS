#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

#define MAX_LINE_LEN 256

static void uniq_fd(int fd) {
    char last[MAX_LINE_LEN];
    char current[MAX_LINE_LEN];
    int first = 1;
    
    char c;
    int col = 0;
    while (read(fd, &c, 1) > 0) {
        if (c == '\n') {
            current[col] = '\0';
            if (first || strcmp(last, current) != 0) {
                print(current);
                print("\n");
                int k = 0;
                while (current[k]) { last[k] = current[k]; k++; }
                last[k] = '\0';
                first = 0;
            }
            col = 0;
        } else if (c != '\r') {
            if (col < MAX_LINE_LEN - 1) {
                current[col++] = c;
            }
        }
    }
    if (col > 0) {
        current[col] = '\0';
        if (first || strcmp(last, current) != 0) {
            print(current);
            print("\n");
        }
    }
}

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));
    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);
    if (argc == 0) {
        uniq_fd(0);
    } else {
        int fd = open(argv[0]);
        if (fd < 0) {
            print("uniq: ");
            print(argv[0]);
            print(": No such file or directory\n");
            return 1;
        }
        uniq_fd(fd);
        close(fd);
    }
    return 0;
}
