#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
    int ms = sysinfo(1, 0, 0);
    if (ms < 0) {
        print("uptime: failed to retrieve uptime\n");
        return 1;
    }
    int seconds = ms / 1000;
    int minutes = seconds / 60;
    int hours = minutes / 60;
    
    print("up ");
    print_dec(hours);
    print(" hours, ");
    print_dec(minutes % 60);
    print(" minutes, ");
    print_dec(seconds % 60);
    print(" seconds\n");
    return 0;
}
