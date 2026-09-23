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
        print("Usage: mv oldname newname\n");
        return 1;
    }
    if (rename(argv[0], argv[1]) < 0) {
        print("mv: cannot rename ");
        print(argv[0]);
        print(" to ");
        print(argv[1]);
        print("\n");
        return 1;
    }
    return 0;
}
