#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "lock.h"

#define PIPE_SIZE 512

/**
 * Shared memory structure for a circular pipe buffer.
 */
struct pipe {
    uint8_t data[PIPE_SIZE];    /**< Circular buffer storage */
    uint32_t head;              /**< Write pointer index */
    uint32_t tail;              /**< Read pointer index */
    uint32_t count;             /**< Number of bytes currently in the buffer */
    spinlock_t lock;            /**< Lock for atomic buffer access */
    uint32_t reader_count;      /**< Number of processes with read access */
    uint32_t writer_count;      /**< Number of processes with write access */
    uint32_t reader_pid_mask;   /**< Bitmask of PIDs waiting to read */
    uint32_t writer_pid_mask;   /**< Bitmask of PIDs waiting to write */
};

struct file; // Forward declaration
struct trap_frame;

int pipe_alloc(struct file **f0, struct file **f1);
int pipe_read(struct pipe *p, void *buf, int n, struct trap_frame *tf);
int pipe_write(struct pipe *p, const void *buf, int n, struct trap_frame *tf);
void pipe_reopen(struct pipe *p, int end);
void pipe_close(struct pipe *p, int end);
int pipe_available(struct pipe *p);

#endif // PIPE_H
