#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fat16.h"
#include "lock.h"

/**
 * Types of files supported by the VFS layer.
 */
typedef enum {
    FILE_TYPE_EMPTY,    /**< Unallocated file slot */
    FILE_TYPE_FAT16,    /**< Regular file on FAT16 filesystem */
    FILE_TYPE_PIPE      /**< Anonymous pipe for IPC */
} file_type_t;

/**
 * Represents an open file instance in the global file table.
 * Contains type-specific data and synchronization primitives.
 */
struct file {
    file_type_t type;   /**< Type of the file */
    int ref_count;      /**< Number of processes currently using this file */
    spinlock_t lock;    /**< Lock for atomic access to file state */
    union {
        struct {
            struct fat16_dir_entry entry; /**< FAT16 directory entry copy */
            uint32_t dir_sector;         /**< Sector on disk containing the entry */
            uint32_t dir_offset;         /**< Offset within the sector */
            uint32_t cursor;             /**< Current read/write position */
        } fat16;
        struct {
            struct pipe *ptr;            /**< Pointer to the pipe structure */
            int end;                     /**< 0 for read end, 1 for write end */
        } pipe;
    };
};

#define MAX_GLOBAL_FILES 128

void fs_init(void);
struct trap_frame;
int file_open(const char *filename);
int file_close(int fd);
int file_read(int fd, void *buf, int size, struct trap_frame *tf);
int file_write(int fd, const void *buf, int size, struct trap_frame *tf);
int file_pipe(int fds[2]);

// Helpers for process management
/**
 * Increments the reference count of a global file.
 */
void fs_reopen(int global_fd);

/**
 * Duplicates a file descriptor (not currently used in the main logic).
 */
int fs_duplicate_fd(int global_fd);

#endif // FS_H
