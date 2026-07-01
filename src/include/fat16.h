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

// Opens a file from the filesystem into a generic file structure.
int fat16_open(const char* filename, struct file* f);

// Reads bytes from a FAT16 file.
int fat16_read(struct file* f, void* buf, int size);

// Writes bytes to a FAT16 file.
int fat16_write(struct file* f, const void* buf, int size);

// Closes a FAT16 file (syncs to disk).
int fat16_close(struct file* f);

// Seeks a FAT16 file.
int fat16_seek(struct file* f, int offset);

// Deletes a file.
int fat16_unlink(const char* filename);

// Renames a file.
int fat16_rename(const char* oldname, const char* newname);

// Creates a directory.
int fat16_mkdir(const char* path);

// Reads a directory entry at the specified index.
int fat16_read_dir(const char* path, int index, char* out_name, uint8_t* out_attr, uint32_t* out_size);

// Resolves a path to its directory entry, sector, and offset.
int fat16_resolve_path(const char* path, struct fat16_dir_entry* out_entry, uint32_t* out_sector, uint32_t* out_offset);

// Resolves the parent directory and last component name.
int fat16_resolve_parent(const char* path, struct fat16_dir_entry* out_parent_entry, char* out_last_component);

#endif // FAT16_H
