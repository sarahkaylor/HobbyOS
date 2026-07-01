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

#define MAX_LINES 128
#define MAX_LINE_LEN 128

static char lines[MAX_LINES][MAX_LINE_LEN];
static int line_count = 0;

static void read_lines(int fd) {
    char c;
    int col = 0;
    while (read(fd, &c, 1) > 0) {
        if (c == '\n') {
            lines[line_count][col] = '\0';
            line_count++;
            col = 0;
            if (line_count >= MAX_LINES) break;
        } else if (c != '\r') {
            if (col < MAX_LINE_LEN - 1) {
                lines[line_count][col++] = c;
            }
        }
    }
    if (col > 0 && line_count < MAX_LINES) {
        lines[line_count][col] = '\0';
        line_count++;
    }
}

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));
    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);
    if (argc == 0) {
        read_lines(0);
    } else {
        int fd = open(argv[0]);
        if (fd < 0) {
            print("sort: ");
            print(argv[0]);
            print(": No such file or directory\n");
            return 1;
        }
        read_lines(fd);
        close(fd);
    }
    
    for (int i = 0; i < line_count - 1; i++) {
        for (int j = i + 1; j < line_count; j++) {
            if (strcmp(lines[i], lines[j]) > 0) {
                char temp[MAX_LINE_LEN];
                int k = 0;
                while (lines[i][k]) { temp[k] = lines[i][k]; k++; }
                temp[k] = '\0';
                
                k = 0;
                while (lines[j][k]) { lines[i][k] = lines[j][k]; k++; }
                lines[i][k] = '\0';
                
                k = 0;
                while (temp[k]) { lines[j][k] = temp[k]; k++; }
                lines[j][k] = '\0';
            }
        }
    }
    
    for (int i = 0; i < line_count; i++) {
        print(lines[i]);
        print("\n");
    }
    return 0;
}
