#include "libc.h"

void print_num(int val) {
    if (val == 0) { print("0"); return; }
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    while(val > 0 && i >= 0) {
        buf[i--] = (char)('0' + (val % 10));
        val /= 10;
    }
    print(&buf[i+1]);
}

__attribute__((section(".text._start")))
void _start(void) {
    print("--- HobbyOS SMP Verification ---\n");
    print("Spawning 8 worker processes to saturate 4 cores...\n");
    
    for(int i = 0; i < 8; i++) {
        int pid = fork();
        if (pid == 0) {
            // Child process
            for(int j = 0; j < 5; j++) {
                int cpu = get_cpuid();
                print("Process heart-beat from CPU: ");
                print_num(cpu);
                print("\n");
                
                // Busy work to keep the core occupied
                for(volatile int k = 0; k < 5000000; k++);
            }
            exit(0);
        }
    }
    
    // Parent waits a bit
    for(volatile int k = 0; k < 50000000; k++);
    print("SMP Verification Test Complete.\n");
    exit(0);
}
