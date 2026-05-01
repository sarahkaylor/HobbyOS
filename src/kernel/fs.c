#include "fs.h"
#include "process.h"
#include "pipe.h"
#include "lock.h"

static struct file global_file_table[MAX_GLOBAL_FILES];
static spinlock_t fs_lock;

extern void uart_puts(const char *s);
extern void print_int(int val);

void fs_init(void) {
    spinlock_init(&fs_lock);
    for (int i = 0; i < MAX_GLOBAL_FILES; i++) {
        global_file_table[i].type = FILE_TYPE_EMPTY;
        global_file_table[i].ref_count = 0;
        spinlock_init(&global_file_table[i].lock);
    }
}

static struct file *file_alloc(void) {
    uint64_t flags = spinlock_acquire_irqsave(&fs_lock);
    for (int i = 0; i < MAX_GLOBAL_FILES; i++) {
        if (global_file_table[i].type == FILE_TYPE_EMPTY) {
            global_file_table[i].ref_count = 1;
            global_file_table[i].type = FILE_TYPE_FAT16; // Default to something non-empty
            spinlock_release_irqrestore(&fs_lock, flags);
            return &global_file_table[i];
        }
    }
    spinlock_release_irqrestore(&fs_lock, flags);
    return 0;
}

static int get_global_fd(struct file *f) {
    if (!f) return -1;
    return (int)(f - global_file_table);
}

int file_open(const char *filename) {
    struct process *cur = current_process();
    if (!cur || cur->num_open_fds >= MAX_OPEN_FDS) return -1;

    struct file *f = file_alloc();
    if (!f) return -1;

    if (fat16_open(filename, f) != 0) {
        f->type = FILE_TYPE_EMPTY;
        f->ref_count = 0;
        return -1;
    }

    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FDS; i++) {
        if (cur->open_fds[i] == -1) {
            cur->open_fds[i] = get_global_fd(f);
            cur->num_open_fds++;
            fd = i;
            break;
        }
    }

    if (fd == -1) {
        fat16_close(f);
        f->type = FILE_TYPE_EMPTY;
        f->ref_count = 0;
    }

    return fd;
}

int file_close(int fd) {
    struct process *cur = current_process();
    if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

    int g_fd = cur->open_fds[fd];
    if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

    struct file *f = &global_file_table[g_fd];
    
    uint64_t flags = spinlock_acquire_irqsave(&f->lock);
    f->ref_count--;
    if (f->ref_count == 0) {
        if (f->type == FILE_TYPE_FAT16) {
            fat16_close(f);
        } else if (f->type == FILE_TYPE_PIPE) {
            pipe_close(f->pipe.ptr, f->pipe.end);
        }
        f->type = FILE_TYPE_EMPTY;
    }
    spinlock_release_irqrestore(&f->lock, flags);

    cur->open_fds[fd] = -1;
    cur->num_open_fds--;
    return 0;
}

int file_read(int fd, void *buf, int size, struct trap_frame *tf) {
    struct process *cur = current_process();
    if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

    int g_fd = cur->open_fds[fd];
    if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

    struct file *f = &global_file_table[g_fd];
    if (f->type == FILE_TYPE_FAT16) {
        return fat16_read(f, buf, size);
    } else if (f->type == FILE_TYPE_PIPE) {
        if (f->pipe.end != 0) return -1; // Read end only
        return pipe_read(f->pipe.ptr, buf, size, tf);
    }
    return -1;
}

int file_write(int fd, const void *buf, int size, struct trap_frame *tf) {
    struct process *cur = current_process();
    if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

    int g_fd = cur->open_fds[fd];
    if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

    struct file *f = &global_file_table[g_fd];
    if (f->type == FILE_TYPE_FAT16) {
        return fat16_write(f, buf, size);
    } else if (f->type == FILE_TYPE_PIPE) {
        if (f->pipe.end != 1) {
            uart_puts("file_write: wrong pipe end: ");
            print_int(f->pipe.end);
            uart_puts("\n");
            return -1;
        }
        return pipe_write(f->pipe.ptr, buf, size, tf);
    }
    return -1;
}

int file_pipe(int fds[2]) {
    struct process *cur = current_process();
    if (!cur || cur->num_open_fds + 2 > MAX_OPEN_FDS) return -1;

    struct file *f0 = file_alloc();
    struct file *f1 = file_alloc();
    if (!f0 || !f1) {
        if (f0) f0->type = FILE_TYPE_EMPTY;
        if (f1) f1->type = FILE_TYPE_EMPTY;
        return -1;
    }

    if (pipe_alloc(&f0, &f1) != 0) {
        f0->type = FILE_TYPE_EMPTY;
        f1->type = FILE_TYPE_EMPTY;
        return -1;
    }

    int user_fd0 = -1, user_fd1 = -1;
    for (int i = 0; i < MAX_OPEN_FDS; i++) {
        if (cur->open_fds[i] == -1) {
            if (user_fd0 == -1) user_fd0 = i;
            else if (user_fd1 == -1) {
                user_fd1 = i;
                break;
            }
        }
    }

    if (user_fd0 == -1 || user_fd1 == -1) {
        // This should have been caught by the num_open_fds check, but just in case
        return -1;
    }

    cur->open_fds[user_fd0] = get_global_fd(f0);
    cur->open_fds[user_fd1] = get_global_fd(f1);
    cur->num_open_fds += 2;

    fds[0] = user_fd0;
    fds[1] = user_fd1;
    return 0;
}

void fs_reopen(int global_fd) {
    if (global_fd < 0 || global_fd >= MAX_GLOBAL_FILES) return;
    struct file *f = &global_file_table[global_fd];
    uint64_t flags = spinlock_acquire_irqsave(&f->lock);
    if (f->type != FILE_TYPE_EMPTY) {
        f->ref_count++;
        if (f->type == FILE_TYPE_PIPE) {
            pipe_reopen(f->pipe.ptr, f->pipe.end);
        }
    }
    spinlock_release_irqrestore(&f->lock, flags);
}
