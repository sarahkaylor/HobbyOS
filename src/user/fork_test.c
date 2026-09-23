#include "libc.h"

// Simple integer-to-string for printing tick counts
static void print_num(int n) {
    char buf[16];
    int idx = 0;
    if (n == 0) {
        print("0");
        return;
    }
    while (n > 0) {
        buf[idx++] = (char)('0' + (n % 10));
        n /= 10;
    }
    // Print in reverse
    char out[16];
    for (int i = 0; i < idx; i++) {
        out[i] = buf[idx - 1 - i];
    }
    out[idx] = '\0';
    print(out);
}

__attribute__((section(".text._start")))
void _start(void) {
    print("\n==============================\n"
          "HobbyOS Fork Test\n"
          "==============================\n");

    print("[FORK TEST] Calling fork()...\n");
    int pid = fork();

    if (pid > 0) {
        // Parent process
        print("[FORK TEST] Parent: child PID = ");
        print_num(pid);
        print("\n");

        for (int i = 0; i < 5; i++) {
            print("[FORK TEST] Parent tick ");
            print_num(i);
            print("\n");
            // Delay loop — will be preempted by timer
            for (volatile int j = 0; j < 2000000; j++) {}
        }

        print("[FORK TEST] Parent exiting.\n");
    } else if (pid == 0) {
        // Child process
        print("[FORK TEST] Child: I am the forked child!\n");

        for (int i = 0; i < 5; i++) {
            print("[FORK TEST] Child tick ");
            print_num(i);
            print("\n");
            // Delay loop — will be preempted by timer
            for (volatile int j = 0; j < 2000000; j++) {}
        }

        print("[FORK TEST] Child exiting.\n");
    } else {
        print("[FORK TEST] ERROR: fork() returned negative!\n");
    }

    exit(0);
}
