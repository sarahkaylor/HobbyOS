#include "pipe.h"
#include "process.h"
#include "lock.h"
#include "errno.h"
#include "frame.h"

extern void print_int(int val);
#include "fs.h"

extern void uart_puts(const char* s);
extern void uart_print_hex(uint64_t val);

/* C1-k5 (docs/cp-k5.md §1): up to MAX_GLOBAL_FILES / 2 pipes (8192 on
 * x86_64; one pipe = two open files).  A struct pipe is ~930 B, so the old
 * static pool at that size would be ~7.4 MiB of BSS.  The first PIPE_STATIC
 * stay static; the rest come in frame-pool chunks of PIPE_CHUNK, allocated
 * when every materialised pipe is busy and never freed (file slots keep raw
 * struct pipe pointers).  pipe_slot(i) is the only accessor. */
#define PIPES_MAX (MAX_GLOBAL_FILES / 2)
#define PIPE_STATIC (GFT_STATIC / 2)
#define PIPE_CHUNK 64
#define PIPE_NCHUNK ((PIPES_MAX - PIPE_STATIC + PIPE_CHUNK - 1) / PIPE_CHUNK + 1)
#define PIPE_CHUNK_PAGES ((int)((PIPE_CHUNK * sizeof(struct pipe) + 4095) / 4096))
static struct pipe pipes_static[PIPE_STATIC];
static struct pipe *volatile pipe_chunk[PIPE_NCHUNK];
static int pipes_live; /* materialised pipes (under pipes_lock) */
static int pipes_hint; /* where the next free-pipe scan starts */
static spinlock_t pipes_lock;

static inline struct pipe *pipe_slot(int i) {
  if (i < PIPE_STATIC) return &pipes_static[i];
  int k = i - PIPE_STATIC;
  return pipe_chunk[k / PIPE_CHUNK] + (k % PIPE_CHUNK);
}

static void pipe_slot_init(struct pipe *p) {
  uint8_t *b = (uint8_t *)p;
  for (uint64_t k = 0; k < sizeof *p; k++) b[k] = 0;
  p->reader_count = 0;
  p->writer_count = 0;
  p->cap = PIPE_SIZE;
  spinlock_init(&p->lock);
}
extern spinlock_t proc_lock;

/* C1-k4: byte `i` of the ring (native inline buffer or the frame ring). */
static inline uint8_t *pipe_at(struct pipe *p, uint32_t i) {
  if (p->pg[0])
    return &p->pg[i >> 12][i & 4095];
  return &p->data[i];
}

/**
 * Initializes the pipe subsystem.
 * Sets up the static pool of pipe structures and their associated locks.
 */
void pipes_init(void) {
  spinlock_init(&pipes_lock);
/* PROCESS class: pipe syscalls only; the IRQ side never touches the pool. */
  lock_register(&pipes_lock, "pipes_lock", LOCK_RANK_BACKEND, LOCK_CLASS_PROCESS);
  pipes_live = PIPE_STATIC;
  pipes_hint = 0;
  for (int i = 0; i < PIPE_STATIC; i++)
    pipe_slot_init(&pipes_static[i]);
}

/* C1-k5: add one chunk of pipes (frames taken with no lock held). */
static int pipes_grow(int live) {
  if (live >= PIPES_MAX) return -1;
  uint64_t pa = frame_alloc_contig(PIPE_CHUNK_PAGES);
  if (!pa) return -1;
  struct pipe *c = (struct pipe *)phys_to_virt(pa);
  for (int k = 0; k < PIPE_CHUNK; k++) pipe_slot_init(&c[k]);
  int ci = (live - PIPE_STATIC) / PIPE_CHUNK;
  spinlock_acquire(&pipes_lock);
  int mine = (pipes_live == live && !pipe_chunk[ci]);
  if (mine) {
    pipe_chunk[ci] = c;
    pipes_live = live + PIPE_CHUNK > PIPES_MAX ? PIPES_MAX : live + PIPE_CHUNK;
  }
  spinlock_release(&pipes_lock);
  if (!mine)
    for (int k = 0; k < PIPE_CHUNK_PAGES; k++) frame_free(pa + (uint64_t)k * 4096);
  return 0;
}

static void pipe_free_frames(uint64_t *phys) {
  for (int k = 0; k < PIPE_PAGES; k++)
    if (phys[k])
      frame_free(phys[k]);
}

/**
 * Allocates a free pipe from the pool and initializes it.
 * Associates the pipe with two file structures (one for reading, one for writing).
 *
 * Parameters:
 *   f0 - Pointer to the read-end file structure pointer.
 *   f1 - Pointer to the write-end file structure pointer.
 *   linux - C1-k4: give the pipe Linux's 64 KiB ring (frames).
 *
 * Returns:
 *   0 on success, -1 if no pipes are available.
 */
int pipe_alloc_ex(struct file **f0, struct file **f1, int linux) {
  /* Frames first, with no lock held (frame_alloc takes its own). */
  uint64_t phys[PIPE_PAGES];
  int big = 0;
  for (int k = 0; k < PIPE_PAGES; k++)
    phys[k] = 0;
#ifndef __x86_64__
  linux = 0; /* see pipe.h: no Linux ring off x86_64 */
#endif
  if (linux) {
    big = 1;
    for (int k = 0; k < PIPE_PAGES; k++) {
      phys[k] = frame_alloc();
      if (!phys[k]) {
        big = 0;
        break;
      }
    }
    if (!big) {
      pipe_free_frames(phys);
      for (int k = 0; k < PIPE_PAGES; k++)
        phys[k] = 0;
    }
  }
  for (;;) {
  spinlock_acquire(&pipes_lock);
  int live = pipes_live;
  int span = live - PIPE_STATIC;
  int h = (pipes_hint >= 0 && pipes_hint < span) ? pipes_hint : 0;
  /* C1-k5: static pipes first, lowest first (the old order); then the
     chunks from a rotating hint. */
  for (int n = 0; n < live; n++) {
    int i = n < PIPE_STATIC ? n
                            : PIPE_STATIC + ((h + n - PIPE_STATIC) % (span > 0 ? span : 1));
    struct pipe *pp = pipe_slot(i);
    /* C1-k4: the free test and the (re)initialisation happen under the
       pipe's own lock, the lock pipe_close() detaches the old ring under. */
    spinlock_acquire(&pp->lock);
    if (pp->reader_count == 0 && pp->writer_count == 0) {
      pp->reader_count = 1;
      pp->writer_count = 1;
      pp->head = 0;
      pp->tail = 0;
      pp->count = 0;
      for (int k = 0; k < PIPE_PAGES; k++) {
        pp->pg_phys[k] = big ? phys[k] : 0;
        pp->pg[k] = big ? (uint8_t *)phys_to_virt(phys[k]) : 0;
      }
      pp->cap = big ? PIPE_SIZE_LINUX : PIPE_SIZE;
      pp->reader_pid_mask = (struct pid_mask){0};
      pp->writer_pid_mask = (struct pid_mask){0};
      spinlock_release(&pp->lock);

      (*f0)->type = FILE_TYPE_PIPE;
      (*f0)->pipe.ptr = pp;
      (*f0)->pipe.end = 0; // Read end

      (*f1)->type = FILE_TYPE_PIPE;
      (*f1)->pipe.ptr = pp;
      (*f1)->pipe.end = 1; // Write end

      if (i >= PIPE_STATIC) pipes_hint = i + 1 - PIPE_STATIC;
      spinlock_release(&pipes_lock);
      return 0;
    }
    spinlock_release(&pp->lock);
  }
  spinlock_release(&pipes_lock);
  if (pipes_grow(live) == 0) continue; /* C1-k5: a fresh chunk, rescan */
  pipe_free_frames(phys);
  return -1;
  }
}

int pipe_alloc(struct file **f0, struct file **f1) {
  return pipe_alloc_ex(f0, f1, 0);
}

/**
 * Increments the reader or writer count for a pipe.
 * Used during process fork to share the pipe end with a child process.
 */
void pipe_reopen(struct pipe *p, int end) {
  spinlock_acquire(&p->lock);
  if (end == 0) {
    p->reader_count++;
  } else {
    p->writer_count++;
  }
  spinlock_release(&p->lock);
}

/**
 * Decrements the reader or writer count for a pipe.
 * If a count reaches zero, it wakes up any blocked processes on the other end.
 */
/* Wakes every pid in the mask.  MUST be called with no spinlock held:
 * process_wakeup() takes proc_lock, and the global lock order is
 * proc_lock -> f->lock -> p->lock (fork holds proc_lock while taking
 * file and pipe locks via fs_reopen).  A wakeup performed under p->lock
 * would add the p->lock -> proc_lock edge that closes the deadlock
 * cycle (observed live: 7 CPUs spinning on proc_lock, 1 on a pipe lock). */
static void drain_wakeup_mask(const struct pid_mask *mask) {
  for (int wi = 0; wi < PID_MASK_WORDS; wi++) {
    uint64_t x = mask->w[wi];
    while (x) {
      int pid = (wi << 6) + __builtin_ctzll(x);
      x &= x - 1;
      process_wakeup(pid);
    }
  }
}

void pipe_close(struct pipe *p, int end) {
  spinlock_acquire(&p->lock);
  struct pid_mask wake_mask;
  pid_mask_zero(&wake_mask);
  if (end == 0) {
    if (p->reader_count > 0) p->reader_count--;
    if (p->reader_count == 0) {
      // Wake up writers (they will get -1 on write).  Clear the mask as
      // we collect it: a stale bit left behind would make later closes
      // re-wake (or appear to hold) pids that are no longer waiting.
      wake_mask = p->writer_pid_mask;
      pid_mask_zero(&p->writer_pid_mask);
    }
  } else {
    if (p->writer_count > 0) p->writer_count--;
    if (p->writer_count == 0) {
      // Wake up readers (they will get 0 on read)
      wake_mask = p->reader_pid_mask;
      pid_mask_zero(&p->reader_pid_mask);
    }
  }
  /* C1-k4: the last close of both ends returns the ring's frames. */
  uint64_t phys[PIPE_PAGES];
  int drop = 0;
  if (p->reader_count == 0 && p->writer_count == 0 && p->pg[0]) {
    drop = 1;
    for (int k = 0; k < PIPE_PAGES; k++) {
      phys[k] = p->pg_phys[k];
      p->pg_phys[k] = 0;
      p->pg[k] = 0;
    }
    p->cap = PIPE_SIZE;
    p->count = 0;
    p->head = p->tail = 0;
  }
  spinlock_release(&p->lock);
  if (drop)
    pipe_free_frames(phys);
  drain_wakeup_mask(&wake_mask);
  poll_notify(); /* C1-k4: EOF / EPIPE is a readiness change */
}

/* C1-k4: is a Linux-ABI caller asking?  The blocking-write-in-full and
   PIPE_BUF rules are Linux's; the native track keeps its historic contract
   (short writes, park only when full). */
static int pipe_linux_caller(struct process *cur) {
  return cur && (cur->abi_flags & PROC_FLAG_LINUX_ABI);
}

static int pipe_sig_pending(struct process *cur) {
  extern int linux_compat_sig_deliverable(struct process *)
      __attribute__((weak));
  return pipe_linux_caller(cur) && linux_compat_sig_deliverable &&
         linux_compat_sig_deliverable(cur);
}

/**
 * Reads data from a pipe. Blocks the calling process if the pipe is empty.
 *
 * Parameters:
 *   p   - Pointer to the pipe structure.
 *   buf - Destination buffer in memory.
 *   n   - Number of bytes to read.
 *   nonblock - C1-k4: O_NONBLOCK; an empty pipe answers -EAGAIN.
 *
 * Returns:
 *   Number of bytes read, or -2 if the process was blocked (calling for a retry).
 */
int pipe_read_ex(struct pipe *p, void *buf, int n, int nonblock) {
  if (!p) return -1;
  uint8_t *d = (uint8_t *)buf;
  struct process *cur = current_process();

  while (1) {
    int i = 0;
    struct pid_mask wake_mask;
    pid_mask_zero(&wake_mask);

    spinlock_acquire(&p->lock);

    while (i < n && p->count > 0) {
      d[i] = *pipe_at(p, p->tail);
      p->tail = (p->tail + 1) % p->cap;
      i++;
      p->count--;
    }

    /* C1-k4: any freed space can satisfy a parked writer (it may be waiting
       for room for a whole <= PIPE_BUF record, not just one byte).  The old
       rule woke writers only on the full -> full-1 transition. */
    if (i > 0) {
      wake_mask = p->writer_pid_mask;
      pid_mask_zero(&p->writer_pid_mask);
    }

    if (i == 0 && n > 0) {
      // Empty pipe
      if (p->writer_count == 0) {
        // No writers left, EOF
        spinlock_release(&p->lock);
        return 0;
      }
      if (nonblock) {
        spinlock_release(&p->lock);
        return -EAGAIN;
      }
      // Block until data available.  Lock order is proc_lock BEFORE the
      // pipe lock (fork holds proc_lock while taking file/pipe locks via
      // fs_reopen), so drop p->lock, retake both in order, and re-check.
      if (cur) {
        spinlock_release(&p->lock);
        uint64_t proc_flags = spinlock_acquire_irqsave(&proc_lock);
        spinlock_acquire(&p->lock);
        if (p->count == 0 && p->writer_count > 0) {
          /* M6/L33: Linux's pipe_read checks signal_pending() exactly here --
             after "the pipe is empty" and before registering the waiter -- and
             answers -ERESTARTSYS.  A compat reader that just resumed from a
             signal wake must not silently sleep again (measured: the futex-sig
             probe's read case re-parked forever).  Compat class only: the
             native park/deliver contract is unchanged. */
          if (pipe_sig_pending(cur)) {
            spinlock_release(&p->lock);
            spinlock_release_irqrestore(&proc_lock, proc_flags);
            return -EINTR;
          }
          if (process_park_ok_locked(cur)) { /* C1-k4: never re-park the dead */
            pid_mask_set(&p->reader_pid_mask, cur->pid);
            cur->state = PROC_STATE_BLOCKED;
          }
          spinlock_release(&p->lock);
          spinlock_release_irqrestore(&proc_lock, proc_flags);
          return -2; // EAGAIN, handled by trap.c
        }
        spinlock_release(&p->lock);
        spinlock_release_irqrestore(&proc_lock, proc_flags);
        continue; // state changed while locking; re-evaluate
      }
      spinlock_release(&p->lock);
      return 0;
    }

    spinlock_release(&p->lock);
    drain_wakeup_mask(&wake_mask);
    if (i > 0)
      poll_notify(); /* C1-k4: space freed (POLLOUT) */
    return i;
  }
}

int pipe_read(struct pipe *p, void *buf, int n, struct trap_frame *tf) {
  (void)tf;
  return pipe_read_ex(p, buf, n, 0);
}

/* Copy `len` bytes at flattened offset `off` of the gather list into the
 * ring at head.  Caller holds p->lock and has checked the space. */
static void pipe_put(struct pipe *p, const struct pipe_seg *seg, int nseg,
                     uint32_t off, uint32_t len) {
  int si = 0;
  while (si < nseg && off >= seg[si].len) {
    off -= seg[si].len;
    si++;
  }
  while (len > 0 && si < nseg) {
    uint32_t take = seg[si].len - off;
    if (take > len)
      take = len;
    const uint8_t *src = seg[si].base + off;
    for (uint32_t k = 0; k < take; k++) {
      *pipe_at(p, p->head) = src[k];
      p->head = (p->head + 1) % p->cap;
    }
    p->count += take;
    len -= take;
    off = 0;
    si++;
  }
}

static int pipe_epipe(struct pipe *p, struct process *cur) {
  static int epipe_reports = 0;
  if (epipe_reports < 5) {
    epipe_reports++;
    /* GB-082: ONE uart_puts for the whole record (see process.c, same fix):
       four calls let a peer CPU fuse a record onto this line. */
    char pb[96];
    int pi = 0;
    const char *ppre = "[PIPE_WRITE_EPIPE] PID ";
    while (*ppre && pi < 30) pb[pi++] = *ppre++;
    {
      char td[12];
      int ti = 0;
      int v = cur ? cur->pid : 0;
      if (v < 0) { pb[pi++] = '-'; v = -v; }
      if (v == 0) td[ti++] = '0';
      while (v > 0 && ti < 11) { td[ti++] = (char)('0' + (v % 10)); v /= 10; }
      while (ti > 0 && pi < 40) pb[pi++] = td[--ti];
    }
    const char *pmid = " pipe 0x";
    while (*pmid && pi < 50) pb[pi++] = *pmid++;
    {
      static const char hx[] = "0123456789ABCDEF";
      uint64_t v = (uint64_t)p;
      for (int sh = 60; sh >= 0 && pi < 66; sh -= 4)
        pb[pi++] = hx[(v >> sh) & 0xF];
    }
    const char *pend = " reader_count is 0\n";
    while (*pend && pi < 94) pb[pi++] = *pend++;
    pb[pi] = 0;
    uart_puts(pb);
  }
  return signal_epipe(cur ? process_group(cur) : 0);
}

/**
 * Writes a gather list to a pipe (write(2) is the one-segment case).
 *
 * C1-k4, Linux pipe_write():
 *  - a write of <= PIPE_BUF bytes is atomic: it waits (or -EAGAIN) until the
 *    whole record fits, never interleaving with another writer;
 *  - a blocking write writes EVERYTHING, parking as often as needed; the
 *    park records how far it got (pw_* in the PCB, keyed by pipe + `key` +
 *    length) and the restarted syscall resumes there;
 *  - a non-blocking write writes what fits (> PIPE_BUF) or answers -EAGAIN;
 *  - a signal ends a partly done blocking write with the byte count.
 * Native callers keep the historic contract: write what fits, park only
 * when nothing fits.
 *
 * Returns bytes written, -2 when parked (restart), or -errno.
 */
int pipe_writev_ex(struct pipe *p, const struct pipe_seg *seg, int nseg,
                   int nonblock, uint64_t key, struct trap_frame *tf) {
  if (!p) return -1;
  struct process *cur = current_process();
  int lx = pipe_linux_caller(cur);
  uint64_t total64 = 0;
  for (int k = 0; k < nseg; k++)
    total64 += seg[k].len;
  if (total64 > 0x7FFFF000ULL)
    total64 = 0x7FFFF000ULL;
  uint32_t total = (uint32_t)total64;
  uint32_t done = 0;
  if (cur && cur->pw_pipe == (void *)p && cur->pw_key == key &&
      cur->pw_total == total)
    done = cur->pw_done; /* the restart of a write that parked part-way */
  if (cur)
    cur->pw_pipe = 0;
  if (total == 0)
    return 0;
  if (done && pipe_sig_pending(cur))
    return (int)done; /* Linux: a signal ends a partial write with its count */

  while (1) {
    struct pid_mask wake_mask;
    pid_mask_zero(&wake_mask);

    spinlock_acquire(&p->lock);
    if (p->reader_count == 0) {
      // No readers left.  P5 (D10): the write fails -EPIPE and SIGPIPE is
      // raised against the writer's group -- SIG_IGN survives (the WebKit
      // child's disposition), a handler records pending, and the default
      // action kills the writing group with WTERMSIG 13.
      spinlock_release(&p->lock);
      if (pipe_epipe(p, cur)) {
        /* Default disposition: the writer's group was just killed and its
           memory released.  Never return to a released image -- yield the
           CPU now; the process does not come back. */
        if (tf)
          schedule(tf, 0);
        return -EPIPE;
      }
      return done ? (int)done : -EPIPE;
    }

    uint32_t left = total - done;
    uint32_t space = p->cap - p->count;
    uint32_t w = 0;
    if (lx && left <= PIPE_BUF_ATOMIC && left <= p->cap) {
      if (space >= left)
        w = left; /* atomic record */
    } else {
      w = space < left ? space : left;
    }
    if (w > 0) {
      pipe_put(p, seg, nseg, done, w);
      done += w;
      wake_mask = p->reader_pid_mask;
      pid_mask_zero(&p->reader_pid_mask);
    }
    int finished = (done == total) || (!lx && done > 0) || (nonblock && done > 0);
    if (finished || nonblock || !cur) {
      spinlock_release(&p->lock);
      drain_wakeup_mask(&wake_mask);
      if (w > 0)
        poll_notify(); /* C1-k4: data arrived (POLLIN) */
      if (done > 0)
        return (int)done;
      return nonblock ? -EAGAIN : 0;
    }
    /* Not finished and blocking: hand the wakes out (we may have written a
       part), then park until a reader frees room.  Lock order: proc_lock
       BEFORE the pipe lock (see pipe_read). */
    spinlock_release(&p->lock);
    drain_wakeup_mask(&wake_mask);
    if (w > 0)
      poll_notify();
    uint64_t proc_flags = spinlock_acquire_irqsave(&proc_lock);
    spinlock_acquire(&p->lock);
    left = total - done;
    space = p->cap - p->count;
    int fits = (lx && left <= PIPE_BUF_ATOMIC && left <= p->cap) ? space >= left
                                                                 : space > 0;
    if (!fits && p->reader_count > 0) {
      /* M6/L33: same Linux check on the write side: a compat writer with
         a deliverable pending signal returns -EINTR (or its partial count)
         instead of parking. */
      if (pipe_sig_pending(cur)) {
        spinlock_release(&p->lock);
        spinlock_release_irqrestore(&proc_lock, proc_flags);
        return done ? (int)done : -EINTR;
      }
      if (done) {
        cur->pw_pipe = (void *)p;
        cur->pw_key = key;
        cur->pw_total = total;
        cur->pw_done = done;
      }
      if (process_park_ok_locked(cur)) { /* C1-k4: never re-park the dead */
        pid_mask_set(&p->writer_pid_mask, cur->pid);
        cur->state = PROC_STATE_BLOCKED;
      }
      spinlock_release(&p->lock);
      spinlock_release_irqrestore(&proc_lock, proc_flags);
      return -2; // EAGAIN, handled by trap.c
    }
    spinlock_release(&p->lock);
    spinlock_release_irqrestore(&proc_lock, proc_flags);
    /* state changed while locking; re-evaluate */
  }
}

/**
 * Writes data to a pipe. Blocks the calling process if the pipe is full.
 *
 * Returns:
 *   Number of bytes written, or -2 if the process was blocked (calling for a retry).
 */
int pipe_write(struct pipe *p, const void *buf, int n, struct trap_frame *tf) {
  struct pipe_seg sg = {(const uint8_t *)buf, n > 0 ? (uint32_t)n : 0};
  return pipe_writev_ex(p, &sg, 1, 0, (uint64_t)(uintptr_t)buf, tf);
}

int pipe_writable(struct pipe *p) {
  if (!p) return 0;
  uint32_t space = p->cap - p->count;
  return p->cap > PIPE_BUF_ATOMIC ? space >= PIPE_BUF_ATOMIC : space > 0;
}

/**
 * Checks how many bytes are available to read in the pipe.
 * Returns -1 if the pipe is closed by all writers and empty.
 */
int pipe_available(struct pipe *p) {
  if (!p) return -1;
  spinlock_acquire(&p->lock);
  int avail = p->count;
  if (avail == 0 && p->writer_count == 0) {
    avail = -1; // EOF
  }
  spinlock_release(&p->lock);
  return avail;
}
