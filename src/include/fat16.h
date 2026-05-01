#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>

/**
 * Standard 32-byte FAT16 directory entry structure.
 */
struct fat16_dir_entry {
    char name[11];              /**< 8.3 filename format */
    uint8_t attr;               /**< File attributes */
    uint8_t reserved[10];       /**< Reserved for future use */
    uint16_t time;              /**< Creation/Modification time */
    uint16_t date;              /**< Creation/Modification date */
    uint16_t start_cluster;     /**< First cluster of the file's data */
    uint32_t file_size;         /**< File size in bytes */
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
