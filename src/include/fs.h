#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fat16.h"
#include "lock.h"

typedef enum {
    FILE_TYPE_EMPTY,
    FILE_TYPE_FAT16,
    FILE_TYPE_PIPE
} file_type_t;

struct file {
    file_type_t type;
    int ref_count;
    spinlock_t lock;
    union {
        struct {
            struct fat16_dir_entry entry;
            uint32_t dir_sector;
            uint32_t dir_offset;
            uint32_t cursor;
        } fat16;
        struct {
            struct pipe *ptr;
            int end; // 0 for read, 1 for write
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
void fs_reopen(int global_fd);
int fs_duplicate_fd(int global_fd);

#endif // FS_H
