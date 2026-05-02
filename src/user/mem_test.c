#include "libc.h"
#include <stdint.h>

__attribute__((section(".text._start")))
void _start(void) {
    print("\n==============================\n"
          "HobbyOS User Program - Memory Test\n"
          "==============================\n");

    print("Test 1: Writing to user space (should work):\n");
    volatile uint64_t *user_mem = (uint64_t*)0x44000000;
    *user_mem = 42; // This should work
    print("✓ Write to user space successful\n");

    print("Test 2: Attempting to write to kernel space (should fail):\n");
    volatile uint64_t *kernel_mem = (uint64_t*)0x40000000;
    
    // This will trigger a permission fault
    *kernel_mem = 1337; 
    
    // If we reach here, the protection failed
    print("✗ ERROR: Kernel memory write succeeded (protection broken)\n");
    
    print("Test 3: Testing if we can continue execution after fault...\n");
    
    print("Test complete. Returning to kernel with error code.\n");

    exit(0); // Explicitly call exit if we somehow continue after the fault
}
