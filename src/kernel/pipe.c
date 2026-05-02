#include "pipe.h"
#include "process.h"
#include "lock.h"

extern void print_int(int val);
#include "fs.h"

extern void uart_puts(const char* s);
extern void uart_print_hex(uint64_t val);

static struct pipe pipes[MAX_GLOBAL_FILES / 2];
static spinlock_t pipes_lock;
extern spinlock_t proc_lock;

/**
 * Initializes the pipe subsystem.
 * Sets up the static pool of pipe structures and their associated locks.
 */
void pipes_init(void) {
    spinlock_init(&pipes_lock);
    for (int i = 0; i < MAX_GLOBAL_FILES / 2; i++) {
        pipes[i].reader_count = 0;
        pipes[i].writer_count = 0;
        spinlock_init(&pipes[i].lock);
    }
}

/**
 * Allocates a free pipe from the pool and initializes it.
 * Associates the pipe with two file structures (one for reading, one for writing).
 * 
 * Parameters:
 *   f0 - Pointer to the read-end file structure pointer.
 *   f1 - Pointer to the write-end file structure pointer.
 * 
 * Returns:
 *   0 on success, -1 if no pipes are available.
 */
int pipe_alloc(struct file **f0, struct file **f1) {
    uint64_t flags = spinlock_acquire_irqsave(&pipes_lock);
    for (int i = 0; i < MAX_GLOBAL_FILES / 2; i++) {
        if (pipes[i].reader_count == 0 && pipes[i].writer_count == 0) {
            pipes[i].reader_count = 1;
            pipes[i].writer_count = 1;
            pipes[i].head = 0;
            pipes[i].tail = 0;
            pipes[i].count = 0;
            pipes[i].reader_pid_mask = 0;
            pipes[i].writer_pid_mask = 0;
            
            (*f0)->type = FILE_TYPE_PIPE;
            (*f0)->pipe.ptr = &pipes[i];
            (*f0)->pipe.end = 0; // Read end

            (*f1)->type = FILE_TYPE_PIPE;
            (*f1)->pipe.ptr = &pipes[i];
            (*f1)->pipe.end = 1; // Write end
            
            spinlock_release_irqrestore(&pipes_lock, flags);
            return 0;
        }
    }
    spinlock_release_irqrestore(&pipes_lock, flags);
    return -1;
}

/**
 * Increments the reader or writer count for a pipe.
 * Used during process fork to share the pipe end with a child process.
 */
void pipe_reopen(struct pipe *p, int end) {
    uint64_t flags = spinlock_acquire_irqsave(&p->lock);
    if (end == 0) p->reader_count++;
    else p->writer_count++;
    spinlock_release_irqrestore(&p->lock, flags);
}

/**
 * Decrements the reader or writer count for a pipe.
 * If a count reaches zero, it wakes up any blocked processes on the other end.
 */
void pipe_close(struct pipe *p, int end) {
    uint64_t flags = spinlock_acquire_irqsave(&p->lock);
    if (end == 0) {
        if (p->reader_count > 0) p->reader_count--;
        if (p->reader_count == 0) {
            // Wake up writers (they will get -1 on write)
            for (int i = 0; i < 32; i++) {
                if (p->writer_pid_mask & (1 << i)) {
                    process_wakeup(i);
                }
            }
        }
    } else {
        if (p->writer_count > 0) p->writer_count--;
        if (p->writer_count == 0) {
            // Wake up readers (they will get 0 on read)
            for (int i = 0; i < 32; i++) {
                if (p->reader_pid_mask & (1 << i)) {
                    process_wakeup(i);
                }
            }
        }
    }
    spinlock_release_irqrestore(&p->lock, flags);
}

/**
 * Reads data from a pipe. Blocks the calling process if the pipe is empty.
 * 
 * Parameters:
 *   p   - Pointer to the pipe structure.
 *   buf - Destination buffer in memory.
 *   n   - Number of bytes to read.
 *   tf  - Trap frame (used for blocking).
 * 
 * Returns:
 *   Number of bytes read, or -2 if the process was blocked (calling for a retry).
 */
int pipe_read(struct pipe *p, void *buf, int n, struct trap_frame *tf) {
    if (!p) return -1;
    uint8_t *d = (uint8_t *)buf;
    int i = 0;
    struct process *cur = current_process();

    while (i < n) {
        uint64_t flags = spinlock_acquire_irqsave(&p->lock);
        if (p->count > 0) {
            while (i < n && p->count > 0) {
                d[i] = p->data[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
                i++;
                p->count--;
            }
            
            // If we were full and now have space, wake up writers
            if (p->count == PIPE_SIZE - 1) {
                for (int pid = 0; pid < 32; pid++) {
                    if (p->writer_pid_mask & (1 << pid)) {
                        process_wakeup(pid);
                        p->writer_pid_mask &= ~(1 << pid);
                    }
                }
            }
            spinlock_release_irqrestore(&p->lock, flags);
            return i; // Return after reading available data
        } else {
            // Empty pipe
            if (p->writer_count == 0) {
                // No writers left, EOF
                spinlock_release_irqrestore(&p->lock, flags);
                return i;
            }
            // Block until data available
            if (cur) {
                p->reader_pid_mask |= (1 << cur->pid);
                uint64_t proc_flags = spinlock_acquire_irqsave(&proc_lock);
                cur->state = PROC_STATE_BLOCKED;
                spinlock_release_irqrestore(&proc_lock, proc_flags);
                spinlock_release_irqrestore(&p->lock, flags);
                return -2; // EAGAIN, handled by trap.c
            } else {
                spinlock_release_irqrestore(&p->lock, flags);
                return i;
            }
        }
    }
    return i;
}

/**
 * Writes data to a pipe. Blocks the calling process if the pipe is full.
 * 
 * Parameters:
 *   p   - Pointer to the pipe structure.
 *   buf - Source buffer in memory.
 *   n   - Number of bytes to write.
 *   tf  - Trap frame (used for blocking).
 * 
 * Returns:
 *   Number of bytes written, or -2 if the process was blocked (calling for a retry).
 */
int pipe_write(struct pipe *p, const void *buf, int n, struct trap_frame *tf) {
    if (!p) return -1;
    const uint8_t *d = (const uint8_t *)buf;
    int i = 0;
    struct process *cur = current_process();

    while (i < n) {
        uint64_t flags = spinlock_acquire_irqsave(&p->lock);
        if (p->reader_count == 0) {
            // No readers left
            spinlock_release_irqrestore(&p->lock, flags);
            return -1;
        }
        if (p->count < PIPE_SIZE) {
            p->data[p->head] = d[i];
            p->head = (p->head + 1) % PIPE_SIZE;
            i++;
            p->count++;
            
            // If we were empty and now have data, wake up readers
            if (p->count == 1) {
                for (int pid = 0; pid < 32; pid++) {
                    if (p->reader_pid_mask & (1 << pid)) {
                        process_wakeup(pid);
                        p->reader_pid_mask &= ~(1 << pid);
                    }
                }
            }
            spinlock_release_irqrestore(&p->lock, flags);
        } else {
            // Full pipe, block until space available
            if (cur) {
                p->writer_pid_mask |= (1 << cur->pid);
                uint64_t proc_flags = spinlock_acquire_irqsave(&proc_lock);
                cur->state = PROC_STATE_BLOCKED;
                spinlock_release_irqrestore(&proc_lock, proc_flags);
                spinlock_release_irqrestore(&p->lock, flags);
                return -2; // EAGAIN, handled by trap.c
            } else {
                spinlock_release_irqrestore(&p->lock, flags);
                return i;
            }
        }
    }
    return i;
}

/**
 * Checks how many bytes are available to read in the pipe.
 * Returns -1 if the pipe is closed by all writers and empty.
 */
int pipe_available(struct pipe *p) {
    if (!p) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&p->lock);
    int avail = p->count;
    if (avail == 0 && p->writer_count == 0) {
        avail = -1; // EOF
    }
    spinlock_release_irqrestore(&p->lock, flags);
    return avail;
}
