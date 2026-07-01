#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

static void wc_fd(int fd, int *lines, int *words, int *chars) {
    char buf[1024];
    int n;
    int in_word = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            (*chars)++;
            char c = buf[i];
            if (c == '\n') {
                (*lines)++;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                (*words)++;
            }
        }
    }
}

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));
    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);
    int lines = 0, words = 0, chars = 0;
    if (argc == 0) {
        wc_fd(0, &lines, &words, &chars);
        print(" ");
        print_dec(lines);
        print(" ");
        print_dec(words);
        print(" ");
        print_dec(chars);
        print("\n");
    } else {
        for (int i = 0; i < argc; i++) {
            int fd = open(argv[i]);
            if (fd < 0) {
                print("wc: ");
                print(argv[i]);
                print(": No such file or directory\n");
                continue;
            }
            int l = 0, w = 0, c = 0;
            wc_fd(fd, &l, &w, &c);
            close(fd);
            print(" ");
            print_dec(l);
            print(" ");
            print_dec(w);
            print(" ");
            print_dec(c);
            print(" ");
            print(argv[i]);
            print("\n");
            lines += l;
            words += w;
            chars += c;
        }
        if (argc > 1) {
            print(" ");
            print_dec(lines);
            print(" ");
            print_dec(words);
            print(" ");
            print_dec(chars);
            print(" total\n");
        }
    }
    return 0;
}
