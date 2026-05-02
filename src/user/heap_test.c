#include "libc.h"

__attribute__((section(".text._start"))) void _start(void) {
    print("\n--- Heap Test Started ---\n");

    print("Allocating 1MB...\n");
    void *p1 = malloc(1024 * 1024);
    if (p1) {
        print("Successfully allocated 1MB\n");
        // Verify 1MB writes
        char *data = (char *)p1;
        for (int i = 0; i < 1024; i++) data[i * 1024] = (char)(i % 256);
        print("Verified 1MB writes\n");
    } else {
        print("Failed to allocate 1MB!\n");
    }

    print("Allocating 50MB...\n");
    void *p2 = malloc(50 * 1024 * 1024);
    if (p2) {
        print("Successfully allocated 50MB\n");
        // Verify 50MB writes (start and end)
        char *data = (char *)p2;
        data[0] = 'H';
        data[50 * 1024 * 1024 - 1] = 'Y';
        if (data[0] == 'H' && data[50 * 1024 * 1024 - 1] == 'Y') {
            print("Verified 50MB writes (start and end)\n");
        } else {
            print("Verified 50MB writes FAILED!\n");
        }
    } else {
        print("Failed to allocate 50MB!\n");
    }

    print("Freeing 1MB...\n");
    free(p1);
    
    print("Allocating 2MB (should reuse space)...\n");
    void *p3 = malloc(2 * 1024 * 1024);
    if (p3) {
        print("Successfully allocated 2MB\n");
    } else {
        print("Failed to allocate 2MB!\n");
    }

    print("--- Heap Test Completed ---\n");
    exit(0);
}
