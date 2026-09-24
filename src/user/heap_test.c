#include "libc.h"
#include <sys/mman.h>

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

    print("--- Realloc Section ---\n");
    {
        int ok = 1;
        char *ra = (char *)malloc(100);
        if (!ra) { print("REALLOC: malloc(100) failed!\n"); ok = 0; }
        else {
            int i;
            for (i = 0; i < 100; i++) ra[i] = (char)(i % 128);
            char *rb = (char *)realloc(ra, 5000);
            if (!rb) { print("REALLOC: grow failed!\n"); ok = 0; }
            else {
                for (i = 0; i < 100; i++) {
                    if (rb[i] != (char)(i % 128)) {
                        print("REALLOC: grow lost prefix!\n");
                        ok = 0;
                        break;
                    }
                }
                char *rc = (char *)realloc(rb, 20);
                if (!rc) { print("REALLOC: shrink failed!\n"); ok = 0; }
                else {
                    if (rc != rb) { print("REALLOC: shrink moved pointer!\n"); ok = 0; }
                    for (i = 0; i < 20; i++) {
                        if (rc[i] != (char)(i % 128)) {
                            print("REALLOC: shrink lost prefix!\n");
                            ok = 0;
                            break;
                        }
                    }
                    if (realloc(rc, 0) != NULL) {
                        print("REALLOC: realloc(p,0) != NULL!\n");
                        ok = 0;
                    }
                }
            }
        }
        if (ok) print("REALLOC: all checks PASSED\n");
        else print("REALLOC: FAILED\n");
    }

    free(p2);
    free(p3);

    /* ============ Phase 4: brk/sbrk + anonymous mmap/munmap ============ */
    print("--- Phase 4: memory syscalls ---\n");
    {
        int ph = 1;
        void *brk0 = sbrk(0);
        if ((intptr_t)brk0 == -1) { print("sbrk(0) FAILED!\n"); ph = 0; }
        else {
            void *b1 = sbrk(4096);
            void *brk1 = sbrk(0);
            if (b1 == (void *)-1 || (char *)brk1 != (char *)brk0 + 4096) {
                print("sbrk(4096) FAILED!\n");
                ph = 0;
            } else {
                print("sbrk growth verified\n");
                sbrk(-4096); /* restore the break */
            }
        }

        void *m1 = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m1 == MAP_FAILED) {
            print("mmap 1MB FAILED!\n");
            ph = 0;
        } else {
            volatile char *m = (volatile char *)m1;
            m[0] = 'A';
            m[1024 * 1024 - 1] = 'Z';
            if (m[0] == 'A' && m[1024 * 1024 - 1] == 'Z') {
                print("mmap 1MB write/readback verified\n");
            } else {
                print("mmap 1MB readback FAILED!\n");
                ph = 0;
            }
        }

        void *m2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m2 == MAP_FAILED) {
            print("second mmap FAILED!\n");
            ph = 0;
        } else if (m1 != MAP_FAILED && m2 == m1) {
            /* must not overlap the 1MB region */
            print("mmap overlap FAILED!\n");
            ph = 0;
        } else {
            print("mmap non-overlap verified\n");
        }

        if (m1 != MAP_FAILED) {
            if (munmap(m1, 1024 * 1024) != 0) {
                print("munmap FAILED!\n");
                ph = 0;
            } else {
                print("munmap OK\n");
            }
        }

        if (ph) print("PHASE4 MEM: all checks PASSED\n");
        else print("PHASE4 MEM: FAILED\n");
    }

    print("--- Heap Test Completed ---\n");
    exit(0);
}
