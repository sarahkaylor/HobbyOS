#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

int main(void) {
    char buf[32];
    int index = 0;
    while (read_dir(index, buf) == 0) {
        print(buf);
        print("\n");
        index++;
    }
    return 0;
}
