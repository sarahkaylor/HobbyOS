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
    if (argc < 2) {
        print("Usage: cp source_file dest_file\n");
        return 1;
    }
    int src_fd = open(argv[0]);
    if (src_fd < 0) {
        print("cp: cannot open ");
        print(argv[0]);
        print("\n");
        return 1;
    }
    int dest_fd = open(argv[1]);
    if (dest_fd < 0) {
        print("cp: cannot open/create ");
        print(argv[1]);
        print("\n");
        close(src_fd);
        return 1;
    }
    char buf[1024];
    int n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        write(dest_fd, buf, n);
    }
    close(src_fd);
    close(dest_fd);
    return 0;
}
