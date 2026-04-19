#include "libc.h"

__attribute__((section(".text._start")))
void _start(void) {
    print("\n==============================\n"
          "Hello from isolated EL0 Space!\n"
          "==============================\n");

    // Compute (delay)
    for (volatile int i = 0; i < 20000000; i++) {}

    print("Computation finished. Exiting.\n");

    exit();
}