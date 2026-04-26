#include "libc.h"

__attribute__((section(".text._start")))
void _start(void) {
    print("\n[FILEIO TEST] Starting File I/O Tests...\n");

    const char* filename = "TEST.TXT";
    const char* test_data = "Hello HobbyOS Filesystem!";
    char buffer[64];

    // 1. Open
    int fd = open(filename);
    if (fd < 0) {
        print("[FILEIO TEST] ERROR: Could not open TEST.TXT\n");
        exit();
    }
    print("[FILEIO TEST] Opened TEST.TXT successfully.\n");

    // 2. Write
    int written = write(fd, test_data, 26); // length of test_data
    if (written < 0) {
        print("[FILEIO TEST] ERROR: Write failed.\n");
    } else {
        print("[FILEIO TEST] Wrote data to file.\n");
    }

    // 3. Close
    close(fd);
    print("[FILEIO TEST] Closed file.\n");

    // 4. Open again for reading
    fd = open(filename);
    if (fd < 0) {
        print("[FILEIO TEST] ERROR: Could not reopen TEST.TXT\n");
        exit();
    }
    print("[FILEIO TEST] Reopened TEST.TXT for reading.\n");

    // 5. Read
    for (int i = 0; i < 64; i++) buffer[i] = 0;
    int bytes_read = read(fd, buffer, 64);
    if (bytes_read < 0) {
        print("[FILEIO TEST] ERROR: Read failed.\n");
    } else {
        print("[FILEIO TEST] Read data: ");
        print(buffer);
        print("\n");
    }

    // 6. Close
    close(fd);
    print("[FILEIO TEST] File I/O Tests Complete. Exiting.\n");

    exit();
}