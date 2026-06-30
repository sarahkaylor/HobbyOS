#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

void cat_fd(int fd) {
    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
    }
}

int main(void) {
    char arg_buf[256];
    read_arg_file("CAT.ARG", arg_buf, sizeof(arg_buf));

    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);

    if (argc == 0) {
        cat_fd(0);
    } else {
        for (int i = 0; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1] == '\0') {
                cat_fd(0);
            } else {
                int fd = open(argv[i]);
                if (fd < 0) {
                    print("cat: ");
                    print(argv[i]);
                    print(": No such file or directory\n");
                } else {
                    cat_fd(fd);
                    close(fd);
                }
            }
        }
    }
    return 0;
}
