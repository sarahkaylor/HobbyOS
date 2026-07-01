#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
    struct sys_procinfo procs[64];
    int count = sysinfo(3, procs, sizeof(procs));
    if (count < 0) {
        print("ps: failed to retrieve process list\n");
        return 1;
    }
    print("  PID  PPID  STATE  NAME\n");
    for (int i = 0; i < count; i++) {
        print("   ");
        print_dec(procs[i].pid);
        print("     ");
        print_dec(procs[i].parent_pid);
        print("      ");
        if (procs[i].state == 2) print("READY");
        else if (procs[i].state == 3) print("RUNNING");
        else if (procs[i].state == 5) print("BLOCKED");
        else if (procs[i].state == 6) print("WAIT_SPAWN");
        else print("OTHER");
        print("      ");
        print(procs[i].name);
        print("\n");
    }
    return 0;
}
