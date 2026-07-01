#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));
    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);
    if (argc < 1) {
        print("Usage: touch file...\n");
        return 1;
    }
    for (int i = 0; i < argc; i++) {
        int fd = open(argv[i]);
        if (fd < 0) {
            print("touch: cannot touch ");
            print(argv[i]);
            print("\n");
        } else {
            close(fd);
        }
    }
    return 0;
}
