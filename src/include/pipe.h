#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "lock.h"
#include "pidmask.h"

#define PIPE_SIZE 512 /* native pipes: the historic HobbyOS capacity */
/* C1-k4: Linux-ABI pipes get Linux's default capacity (16 pages, 64 KiB) and
 * PIPE_BUF atomicity.  Wine's request/reply protocol writes a 64-byte header
 * plus up to several KiB of data with one writev() and treats a short count
 * as a protocol error; 512 bytes split every reply above 448 bytes of data. */
#define PIPE_BUF_ATOMIC 4096
#ifdef __x86_64__
#define PIPE_SIZE_LINUX 65536
#else
/* The Linux-ABI class is x86_64-only; the ARM kernel keeps its .bss inside
 * the limine placement window (linker.ld) and never builds a Linux ring. */
#define PIPE_SIZE_LINUX 4096
#endif
#define PIPE_PAGES (PIPE_SIZE_LINUX / 4096)

/**
 * Shared memory structure for a circular pipe buffer.
 */
struct pipe {
  uint8_t data[PIPE_SIZE];    /**< Native ring (cap == PIPE_SIZE) */
  uint8_t *pg[PIPE_PAGES];    /**< Linux-ABI ring: PIPE_PAGES frames, or NULL */
  uint64_t pg_phys[PIPE_PAGES];
  uint32_t cap;               /**< Ring capacity in bytes */
  uint32_t head;              /**< Write pointer index */
  uint32_t tail;              /**< Read pointer index */
  uint32_t count;             /**< Number of bytes currently in the buffer */
  spinlock_t lock;            /**< Lock for atomic buffer access */
  uint32_t reader_count;      /**< Number of processes with read access */
  uint32_t writer_count;      /**< Number of processes with write access */
  struct pid_mask reader_pid_mask; /**< PIDs waiting to read (L12: >64 pids) */
  struct pid_mask writer_pid_mask; /**< PIDs waiting to write */
};

/* One gather segment of a pipe write (a user range already validated). */
struct pipe_seg {
  const uint8_t *base;
  uint32_t len;
};

struct file; // Forward declaration
struct trap_frame;

int pipe_alloc(struct file **f0, struct file **f1);
/* C1-k4: `linux` selects the 64 KiB Linux ring (falls back to the native
 * ring when frames are short). */
int pipe_alloc_ex(struct file **f0, struct file **f1, int linux);
int pipe_read(struct pipe *p, void *buf, int n, struct trap_frame *tf);
int pipe_write(struct pipe *p, const void *buf, int n, struct trap_frame *tf);
/* C1-k4: O_NONBLOCK-aware forms (-EAGAIN instead of a park) and the gather
 * write writev() needs on a pipe (one atomic write for <= PIPE_BUF). */
int pipe_read_ex(struct pipe *p, void *buf, int n, int nonblock);
int pipe_writev_ex(struct pipe *p, const struct pipe_seg *seg, int nseg,
                   int nonblock, uint64_t key, struct trap_frame *tf);
/* POLLOUT: Linux reports a pipe writable when a whole page slot is free. */
int pipe_writable(struct pipe *p);
void pipe_reopen(struct pipe *p, int end);
void pipe_close(struct pipe *p, int end);
int pipe_available(struct pipe *p);

#endif // PIPE_H
