#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
    struct sys_meminfo mem;
    int res = sysinfo(2, &mem, sizeof(mem));
    if (res < 0) {
        print("free: failed to retrieve memory info\n");
        return 1;
    }
    print("              total        used        free\n");
    print("Mem:     ");
    print_dec(mem.total_bytes);
    print("    ");
    print_dec(mem.total_bytes - mem.free_bytes);
    print("    ");
    print_dec(mem.free_bytes);
    print("\n");
    return 0;
}
