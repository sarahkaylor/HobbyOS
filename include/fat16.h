#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>

// Initializes the FAT16 filesystem by reading the boot sector.
// Returns 0 on success, or -1 on failure.
int fat16_init(void);

// Opens a file from the root directory.
// Returns a file descriptor (fd >= 0) if successful, -1 on failure.
int file_open(const char* filename);

// Closes a previously opened file.
int file_close(int fd);

// Reads bytes from the file into the buffer based on the current cursor.
// Returns the number of bytes read.
int file_read(int fd, void* buf, int size);

// Writes bytes to the file from the buffer based on the current cursor.
// Returns the number of bytes written.
int file_write(int fd, const void* buf, int size);

// Seeks the cursor to the specified offset inside the file.
// Returns 0 on success, -1 on failure.
int file_seek(int fd, int offset);

#endif
