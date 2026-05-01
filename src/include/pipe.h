#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "lock.h"

#define PIPE_SIZE 512

struct pipe {
    uint8_t data[PIPE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    spinlock_t lock;
    uint32_t reader_count;
    uint32_t writer_count;
    uint32_t reader_pid_mask;
    uint32_t writer_pid_mask;
};

struct file; // Forward declaration
struct trap_frame;

int pipe_alloc(struct file **f0, struct file **f1);
int pipe_read(struct pipe *p, void *buf, int n, struct trap_frame *tf);
int pipe_write(struct pipe *p, const void *buf, int n, struct trap_frame *tf);
void pipe_reopen(struct pipe *p, int end);
void pipe_close(struct pipe *p, int end);

#endif // PIPE_H
