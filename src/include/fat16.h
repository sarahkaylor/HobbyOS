#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>

struct fat16_dir_entry {
    char name[11];
    uint8_t attr;
    uint8_t reserved[10];
    uint16_t time;
    uint16_t date;
    uint16_t start_cluster;
    uint32_t file_size;
} __attribute__((packed));

struct file; // Forward declaration

// Initializes the FAT16 filesystem by reading the boot sector.
// Returns 0 on success, or -1 on failure.
int fat16_init(void);

// Opens a file from the root directory into a generic file structure.
int fat16_open(const char* filename, struct file* f);

// Reads bytes from a FAT16 file.
int fat16_read(struct file* f, void* buf, int size);

// Writes bytes to a FAT16 file.
int fat16_write(struct file* f, const void* buf, int size);

// Closes a FAT16 file (syncs to disk).
int fat16_close(struct file* f);

// Seeks a FAT16 file.
int fat16_seek(struct file* f, int offset);

#endif // FAT16_H
