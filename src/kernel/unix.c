#include "unix.h"
#include "fs.h"
#include "process.h"
#include "lock.h"
#include "errno.h"
#include "frame.h"

/* P4 (docs/browser/p4-ipc-design.md sections 2-4): the AF_UNIX pair object.
 * Blocking is the pipe park model: a reader/writer with nothing to do sets
 * its pid bit in the direction's mask, flips to PROC_STATE_BLOCKED and
 * returns -2; the trap layer rewinds the syscall and the restarted syscall
 * re-checks.  Lock order stays proc_lock -> f->lock -> usock->lock and wake
 * masks are drained strictly after usock->lock is released (the pipe.c:85
 * deadlock class). */
/* Both ends of a socketpair are full-duplex: end e sends into chan[e] and
 * receives from chan[1-e]. */

static struct usock unix_pool[MAX_UNIX_PAIRS];
static spinlock_t unix_pool_lock;
/* IceWM M1: the name namespace + every listener's accept queue.  Order:
 * proc_lock -> unix_name_lock -> usock->lock; never nested with
 * unix_pool_lock or the frame allocator (ring memory is allocated before
 * this lock is taken and freed after it is released). */
static spinlock_t unix_name_lock;
static uint32_t unix_autobind_seq;

extern spinlock_t proc_lock;

void unix_init(void) {
  spinlock_init(&unix_pool_lock);
  spinlock_init(&unix_name_lock);
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_register(&unix_pool_lock, "unix_pool_lock",
                   LOCK_RANK_BACKEND); /* SEC-OBS-B */
  lockdbg_register(&unix_name_lock, "unix_name_lock", LOCK_RANK_BACKEND);
#endif
}

/* --- ring memory (frame pool) ------------------------------------------- */

#define UNIX_RING_FRAMES ((2 * K_UNIX_RING) / FRAME_SIZE)

static uint8_t *unix_ring_alloc(void) {
  return (uint8_t *)(uintptr_t)frame_alloc_contig(UNIX_RING_FRAMES);
}

static void unix_ring_free(uint8_t *base) {
  if (!base) return;
  for (int i = 0; i < UNIX_RING_FRAMES; i++)
    frame_free((uint64_t)(uintptr_t)base + (uint64_t)i * FRAME_SIZE);
}

static void usock_attach_rings(struct usock *u, uint8_t *base) {
  u->ring_base = base;
  u->chan[0].ring = base;
  u->chan[1].ring = base ? base + K_UNIX_RING : 0;
}

/* Wakes every pid in the mask.  MUST be called with no spinlock held:
 * process_wakeup() takes proc_lock and the global order is
 * proc_lock -> f->lock -> usock->lock. */
static void drain_wakeup_mask(uint64_t mask) {
  while (mask) {
    int pid = __builtin_ctzll(mask);
    mask &= mask - 1;
    process_wakeup(pid);
  }
}

static struct usock *usock_alloc_common(int type, int connected) {
  /* A connected pair needs its rings now; an unconnected endpoint gets
   * them at connect() time (listeners never do). */
  uint8_t *rings = 0;
  if (connected) {
    rings = unix_ring_alloc();
    if (!rings) return 0;
  }
  uint64_t flags = spinlock_acquire_irqsave(&unix_pool_lock);
  for (int i = 0; i < MAX_UNIX_PAIRS; i++) {
    struct usock *u = &unix_pool[i];
    if (u->in_use) continue;
    u->in_use = 1;
    usock_attach_rings(u, rings);
    u->name[0].len = 0;
    u->name[1].len = 0;
    u->bound = 0;
    u->listening = 0;
    u->shut[0] = u->shut[1] = 0;
    u->backlog = 0;
    u->qhead = u->qcount = 0;
    u->acc_wait = u->conn_wait = 0;
    u->type = type;
    u->connected = connected;
    u->refs[0] = 1;
    u->refs[1] = connected ? 1 : 0;
    u->nonblock[0] = 0;
    u->nonblock[1] = 0;
    for (int d = 0; d < 2; d++) {
      struct usock_chan *c = &u->chan[d];
      c->head = c->tail = c->used = 0;
      c->mhead = c->mtail = 0;
      c->mcount = 0;
      c->rwait = c->wwait = 0;
      for (int m = 0; m < K_UNIX_MSGS; m++) c->msg_nfds[m] = 0;
    }
    spinlock_release_irqrestore(&unix_pool_lock, flags);
    return u;
  }
  spinlock_release_irqrestore(&unix_pool_lock, flags);
  unix_ring_free(rings);
  return 0;
}

struct usock *usock_alloc_pair(int type) { return usock_alloc_common(type, 1); }

struct usock *usock_alloc_single(int type) {
  return usock_alloc_common(type, 0);
}

void usock_reopen(struct usock *u, int end) {
  if (!u || end < 0 || end > 1) return;
  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  if (u->in_use) u->refs[end]++;
  spinlock_release_irqrestore(&u->lock, flags);
}

/* Discard one direction's queue, collecting any owned fds for release after
 * the lock is dropped.  Caller holds u->lock. */
static void drop_chan_queue(struct usock_chan *c, int *fds, int *nfds) {
  uint32_t mi = c->mtail;
  while (mi != c->mhead) {
    for (int i = 0; i < c->msg_nfds[mi] && i < K_UNIX_MAX_FDS; i++) {
      int gfd = c->msg_fds[mi][i];
      if (gfd >= 0 && *nfds < K_UNIX_MSGS * 2 * K_UNIX_MAX_FDS)
        fds[(*nfds)++] = gfd;
    }
    c->msg_nfds[mi] = 0;
    mi = (mi + 1) % K_UNIX_MSGS;
  }
  c->mhead = c->mtail;
  c->mcount = 0;
  c->head = c->tail = c->used = 0;
}

/* Release an end's fd ref.  When the ref drops to zero the direction INTO
 * the closed end can never be delivered: its queued messages are discarded
 * (their fds released) and the peer is woken to observe EOF (read side) /
 * EPIPE (write side).  Data already queued FROM the closing end survives for
 * the peer to drain (Linux).  Both refs zero returns the slot to the pool. */
void usock_close(struct usock *u, int end) {
  if (!u || end < 0 || end > 1) return;
  int close_fds[K_UNIX_MSGS * 2 * K_UNIX_MAX_FDS];
  int nclose = 0;
  uint64_t wake = 0;
  struct usock *pending[K_UNIX_BACKLOG];
  int npending = 0;
  uint8_t *free_rings = 0;

  /* IceWM M1: a bound / listening end 0 owns namespace state that is
   * guarded by unix_name_lock (taken first: name -> usock order).  The
   * flags are only ever set on an end the caller holds an fd to. */
  int named = (end == 0) && (u->bound || u->listening);
  uint64_t nflags = 0;
  if (named) nflags = spinlock_acquire_irqsave(&unix_name_lock);

  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  if (!u->in_use || u->refs[end] == 0) {
    spinlock_release_irqrestore(&u->lock, flags);
    if (named) spinlock_release_irqrestore(&unix_name_lock, nflags);
    return;
  }
  u->refs[end]--;
  if (u->refs[end] == 0) {
    int peer = 1 - end;
    if (named) {
      /* Closing the listener frees the name; pending connections are
       * released below (their clients observe EOF / EPIPE). */
      u->bound = 0;
      if (u->listening) {
        u->listening = 0;
        while (u->qcount > 0) {
          pending[npending++] = u->accq[u->qhead];
          u->qhead = (u->qhead + 1) % K_UNIX_BACKLOG;
          u->qcount--;
        }
        wake |= u->acc_wait | u->conn_wait;
        u->acc_wait = u->conn_wait = 0;
      }
    }
    /* The direction into the closed end is undeliverable. */
    drop_chan_queue(&u->chan[peer], close_fds, &nclose);
    /* Peer readers of chan[end] observe EOF; peer writers into chan[peer]
     * observe EPIPE. */
    wake |= u->chan[end].rwait;
    u->chan[end].rwait = 0;
    wake |= u->chan[peer].wwait;
    u->chan[peer].wwait = 0;
    if (u->refs[peer] == 0) {
      /* Both ends gone: nothing can consume the surviving direction. */
      drop_chan_queue(&u->chan[end], close_fds, &nclose);
      u->chan[end].wwait = 0;
      u->chan[peer].rwait = 0;
      free_rings = u->ring_base;
      usock_attach_rings(u, 0);
      u->in_use = 0;
      u->connected = 0;
    }
  }
  spinlock_release_irqrestore(&u->lock, flags);
  if (named) spinlock_release_irqrestore(&unix_name_lock, nflags);

  drain_wakeup_mask(wake);
  for (int i = 0; i < nclose; i++) fs_close_global(close_fds[i]);
  /* Never-accepted connections: drop the queue's end-1 reference. */
  for (int i = 0; i < npending; i++) usock_close(pending[i], 1);
  unix_ring_free(free_rings);
}

int usock_type(struct usock *u) { return u ? u->type : -1; }

void usock_set_nonblock(struct usock *u, int end, int on) {
  if (!u || end < 0 || end > 1) return;
  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  u->nonblock[end] = on ? 1 : 0;
  spinlock_release_irqrestore(&u->lock, flags);
}

int usock_get_nonblock(struct usock *u, int end) {
  if (!u || end < 0 || end > 1) return 0;
  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  int v = u->nonblock[end];
  spinlock_release_irqrestore(&u->lock, flags);
  return v;
}

/* --- shutdown-aware peer state (caller holds u->lock) ------------------- */

/* Reads on `end` hit EOF once the queue is drained: the peer is gone, the
 * peer shut its write side, or this end shut its read side. */
static int usock_rd_eof(const struct usock *u, int end) {
  int peer = 1 - end;
  return u->refs[peer] == 0 || (u->shut[peer] & K_UNIX_SHUT_WR) ||
         (u->shut[end] & K_UNIX_SHUT_RD);
}

/* Writes from `end` fail EPIPE: the peer is gone, this end shut its write
 * side, or the peer shut its read side. */
static int usock_wr_dead(const struct usock *u, int end) {
  int peer = 1 - end;
  return u->refs[peer] == 0 || (u->shut[end] & K_UNIX_SHUT_WR) ||
         (u->shut[peer] & K_UNIX_SHUT_RD);
}

/* --- probing / accounting ---------------------------------------------- */

/* Queue depth.  The ring cursor pair alone cannot distinguish "8 queued"
 * from "empty" (mhead == mtail is the empty state), so the explicit
 * mcount carries fullness: the note's queue depth is exactly K_UNIX_MSGS
 * queued messages per direction.  Caller holds u->lock. */
static int chan_empty(const struct usock *u, const struct usock_chan *c) {
  if (u->type == K_UNIX_STREAM) return c->used == 0;
  return c->mcount == 0;
}

/* Is the write side full?  Caller holds u->lock. */
static int chan_full(const struct usock *u, const struct usock_chan *c) {
  if (u->type == K_UNIX_STREAM) return c->used >= K_UNIX_RING;
  return c->mcount >= K_UNIX_MSGS;
}

void usock_probe(struct usock *u, int end, int *r, int *w, int *e, int *hup) {
  *r = *w = *e = *hup = 0;
  if (!u) {
    *e = 1;
    return;
  }
  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  if (u->listening) {
    /* IceWM M1: a listener is readable (POLLIN) while connections are
     * pending; never writable, never an error (Linux). */
    *r = u->qcount > 0;
    spinlock_release_irqrestore(&u->lock, flags);
    return;
  }
  if (!u->connected) {
    /* No peer ever attached: every operation fails ENOTCONN. */
    if (end == 1) *w = 1;
    *e = 1;
    spinlock_release_irqrestore(&u->lock, flags);
    return;
  }
  /* Full-duplex: fd `end` reads chan[1-end] (EOF once refs[1-end] hits 0)
   * and writes into chan[end] (EPIPE once the peer is gone).  Both
   * directions of every end must be probed — the socketpair ends are
   * peers, not pipe-style read/write halves. */
  int peer = 1 - end;
  struct usock_chan *rc = &u->chan[1 - end];
  struct usock_chan *wc = &u->chan[end];
  if (!chan_empty(u, rc)) {
    *r = 1;
  } else if (usock_rd_eof(u, end)) {
    *r = 1; /* drained and no more data can arrive: EOF is readable */
    /* POLLHUP once both directions are dead (peer closed, or shut both
     * ways); a half-close (peer SHUT_WR only) is POLLIN alone (Linux). */
    if (usock_wr_dead(u, end)) *hup = 1;
  }
  if (u->refs[peer] == 0) {
    *w = 1;
    *e = 1; /* peer closed: writes fail EPIPE (POLLOUT|POLLERR) */
  } else if (usock_wr_dead(u, end)) {
    *w = 1; /* shut down: a write fails EPIPE immediately (never blocks) */
  } else if (!chan_full(u, wc)) {
    *w = 1;
  }
  spinlock_release_irqrestore(&u->lock, flags);
}

int usock_available(struct usock *u, int end) {
  if (!u || end < 0 || end > 1) return -1;
  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  int avail;
  if (u->type == K_UNIX_STREAM) {
    avail = (int)u->chan[1 - end].used;
  } else {
    int total = 0;
    uint32_t mi = u->chan[1 - end].mtail;
    while (mi != u->chan[1 - end].mhead) {
      total += u->chan[1 - end].msg_len[mi];
      mi = (mi + 1) % K_UNIX_MSGS;
    }
    avail = total;
  }
  spinlock_release_irqrestore(&u->lock, flags);
  return avail;
}

/* --- iovec copy helpers (byte-wise; `base` is a user VA the syscall layer
 * has already range-checked) ------------------------------------------- */

/* Copy `n` bytes from `src` into the iovec set at iov-relative offset
 * `off`. */
static void iov_store(const struct k_iovec *io, int niov, int off,
                      const uint8_t *src, int n) {
  int k = 0;
  for (int v = 0; v < niov && k < n; v++) {
    uint64_t len = io[v].len;
    if ((uint64_t)off >= len) {
      off -= (int)len;
      continue;
    }
    uint8_t *dst = (uint8_t *)io[v].base + off;
    int take = (int)(len - (uint64_t)off);
    if (take > n - k) take = n - k;
    for (int i = 0; i < take; i++) dst[i] = src[k + i];
    k += take;
    off = 0;
  }
}

/* Copy `n` bytes out of the iovec set at iov-relative offset `off` into
 * `dst`. */
static void iov_load(const struct k_iovec *io, int niov, int off, uint8_t *dst,
                     int n) {
  int k = 0;
  for (int v = 0; v < niov && k < n; v++) {
    uint64_t len = io[v].len;
    if ((uint64_t)off >= len) {
      off -= (int)len;
      continue;
    }
    const uint8_t *src = (const uint8_t *)io[v].base + off;
    int take = (int)(len - (uint64_t)off);
    if (take > n - k) take = n - k;
    for (int i = 0; i < take; i++) dst[k + i] = src[i];
    k += take;
    off = 0;
  }
}

/* --- parking ------------------------------------------------------------ */

/* Park the caller on its direction's mask when the operation cannot proceed.
 * Self-contained: takes/releases u->lock itself and only sets the mask under
 * proc_lock + u->lock (the pipe.c order).  Returns:
 *   -2         caller was parked (propagate: the trap layer restarts it)
 *    1         the state changed while relocking: retry the outer loop
 *   -errno     terminal (ENOTCONN / EPIPE / EOF 0 / ECONNRESET / EAGAIN) */
static int usock_park(struct usock *u, int end, int read_side, int nonblock) {
  struct process *cur = current_process();
  int peer = 1 - end;
  struct usock_chan *c = &u->chan[read_side ? 1 - end : end];

  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  if (!u->connected) {
    spinlock_release_irqrestore(&u->lock, flags);
    return -ENOTCONN;
  }
  if (read_side ? usock_rd_eof(u, end) : usock_wr_dead(u, end)) {
    int dead = read_side ? ((u->type == K_UNIX_DGRAM && u->refs[peer] == 0)
                                ? -ECONNRESET : 0)
                         : -EPIPE;
    spinlock_release_irqrestore(&u->lock, flags);
    return dead;
  }
  if (nonblock || u->nonblock[end]) {
    spinlock_release_irqrestore(&u->lock, flags);
    return -EAGAIN;
  }
  if (read_side ? !chan_empty(u, c) : !chan_full(u, c)) {
    spinlock_release_irqrestore(&u->lock, flags);
    return 1;
  }
  spinlock_release_irqrestore(&u->lock, flags);

  if (!cur) return -EAGAIN; /* no process context: never block */

  uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
  uint64_t uf = spinlock_acquire_irqsave(&u->lock);
  int ret;
  if (!u->connected) {
    ret = -ENOTCONN;
  } else if (read_side ? usock_rd_eof(u, end) : usock_wr_dead(u, end)) {
    ret = read_side ? ((u->type == K_UNIX_DGRAM && u->refs[peer] == 0)
                           ? -ECONNRESET : 0)
                    : -EPIPE;
  } else if (u->nonblock[end]) {
    ret = -EAGAIN;
  } else if (read_side ? !chan_empty(u, c) : !chan_full(u, c)) {
    ret = 1;
  } else {
    if (read_side)
      c->rwait |= (1ULL << cur->pid);
    else
      c->wwait |= (1ULL << cur->pid);
    cur->state = PROC_STATE_BLOCKED;
    ret = -2;
  }
  spinlock_release_irqrestore(&u->lock, uf);
  spinlock_release_irqrestore(&proc_lock, pf);
  return ret;
}

/* --- STREAM byte ring --------------------------------------------------- */

static int usock_stream_send(struct usock *u, int end, const struct k_iovec *io,
                             int niov, int total, int nonblock) {
  if (!u || total < 0) return -EINVAL;
  while (1) {
    uint64_t flags = spinlock_acquire_irqsave(&u->lock);
    if (!u->connected) {
      spinlock_release_irqrestore(&u->lock, flags);
      return -ENOTCONN;
    }
    if (usock_wr_dead(u, end)) {
      spinlock_release_irqrestore(&u->lock, flags);
      return -EPIPE;
    }
    if (total == 0) {
      spinlock_release_irqrestore(&u->lock, flags);
      return 0;
    }
    struct usock_chan *c = &u->chan[end];
    int i = 0;
    while (i < total && c->used < K_UNIX_RING) {
      uint32_t free_bytes = K_UNIX_RING - c->used;
      int take = total - i;
      if ((uint32_t)take > free_bytes) take = (int)free_bytes;
      uint32_t head = c->head;
      uint32_t contig = K_UNIX_RING - head;
      if ((uint32_t)take > contig) take = (int)contig;
      iov_load(io, niov, i, &c->ring[head], take);
      c->head = (head + (uint32_t)take) % K_UNIX_RING;
      c->used += (uint32_t)take;
      i += take;
    }
    uint64_t wake = 0;
    if (i > 0) {
      wake = c->rwait;
      c->rwait = 0;
    }
    spinlock_release_irqrestore(&u->lock, flags);
    drain_wakeup_mask(wake);
    if (i > 0) return i; /* partial writes are legal on a stream */

    int r = usock_park(u, end, 0, nonblock);
    if (r == 1) continue;
    return r;
  }
}

static int usock_stream_recv(struct usock *u, int end, const struct k_iovec *io,
                             int niov, int cap, int nonblock) {
  if (!u || cap < 0) return -EINVAL;
  while (1) {
    uint64_t flags = spinlock_acquire_irqsave(&u->lock);
    if (!u->connected) {
      spinlock_release_irqrestore(&u->lock, flags);
      return -ENOTCONN;
    }
    if (cap == 0) {
      spinlock_release_irqrestore(&u->lock, flags);
      return 0;
    }
    struct usock_chan *c = &u->chan[1 - end];
    int i = 0;
    while (i < cap && c->used > 0) {
      int take = cap - i;
      if ((uint32_t)take > c->used) take = (int)c->used;
      uint32_t tail = c->tail;
      uint32_t contig = K_UNIX_RING - tail;
      if ((uint32_t)take > contig) take = (int)contig;
      iov_store(io, niov, i, &c->ring[tail], take);
      c->tail = (tail + (uint32_t)take) % K_UNIX_RING;
      c->used -= (uint32_t)take;
      i += take;
    }
    uint64_t wake = 0;
    if (i > 0 && c->used == 0) {
      wake = c->wwait; /* space freed */
      c->wwait = 0;
    }
    spinlock_release_irqrestore(&u->lock, flags);
    drain_wakeup_mask(wake);
    if (i > 0) return i;

    /* Empty: usock_park re-checks under the lock and reports EOF (0) or
     * -ECONNRESET (DGRAM) when the sending peer is gone. */
    int r = usock_park(u, end, 1, nonblock);
    if (r == 1) continue;
    return r;
  }
}

/* --- SEQPACKET / DGRAM message queue ------------------------------------ */

static int usock_msg_send(struct usock *u, int end, const struct k_iovec *io,
                          int niov, int total, const int *gfds, int nfds,
                          int nonblock) {
  if (!u || total < 0 || nfds < 0 || nfds > K_UNIX_MAX_FDS) return -EINVAL;
  if (total > K_UNIX_MSG_MAX) return -EMSGSIZE;
  while (1) {
    uint64_t flags = spinlock_acquire_irqsave(&u->lock);
    if (!u->connected) {
      spinlock_release_irqrestore(&u->lock, flags);
      return -ENOTCONN;
    }
    if (usock_wr_dead(u, end)) {
      spinlock_release_irqrestore(&u->lock, flags);
      return -EPIPE;
    }
    struct usock_chan *c = &u->chan[end];
    if (chan_full(u, c) || c->used + (uint32_t)total > K_UNIX_RING) {
      spinlock_release_irqrestore(&u->lock, flags);
      int r = usock_park(u, end, 0, nonblock);
      if (r == 1) continue;
      return r;
    }
    uint32_t mi = c->mhead;
    int i = 0;
    while (i < total) {
      int take = total - i;
      uint32_t head = c->head;
      uint32_t contig = K_UNIX_RING - head;
      if ((uint32_t)take > contig) take = (int)contig;
      iov_load(io, niov, i, &c->ring[head], take);
      c->head = (head + (uint32_t)take) % K_UNIX_RING;
      i += take;
    }
    c->used += (uint32_t)total;
    c->msg_len[mi] = (uint16_t)total;
    c->msg_nfds[mi] = (uint8_t)nfds;
    for (int f = 0; f < nfds; f++) c->msg_fds[mi][f] = (int16_t)gfds[f];
    c->mhead = (mi + 1) % K_UNIX_MSGS;
    c->mcount++;
    uint64_t wake = c->rwait;
    c->rwait = 0;
    spinlock_release_irqrestore(&u->lock, flags);
    drain_wakeup_mask(wake);
    return total;
  }
}

static int usock_msg_recv(struct usock *u, int end, const struct k_iovec *io,
                          int niov, int bufcap, int *got, int *trunc,
                          int *gfds_out, int maxfds, int *nfds_out,
                          int *ctrl_trunc, int nonblock, int cmsg_cloexec) {
  struct process *grp = process_group(current_process());
  while (1) {
    uint64_t flags = spinlock_acquire_irqsave(&u->lock);
    if (!u->connected) {
      spinlock_release_irqrestore(&u->lock, flags);
      return -ENOTCONN;
    }
    struct usock_chan *c = &u->chan[1 - end];
    if (c->mcount == 0) {
      spinlock_release_irqrestore(&u->lock, flags);
      int r = usock_park(u, end, 1, nonblock);
      if (r == 1) continue;
      return r;
    }
    uint32_t mi = c->mtail;
    int mlen = c->msg_len[mi];
    int nfds = c->msg_nfds[mi];
    int deliver = nfds > maxfds ? maxfds : nfds;
    if (deliver > 0 && !grp) {
      spinlock_release_irqrestore(&u->lock, flags);
      return -EINVAL; /* no receiver fd table to install into */
    }
    if (grp && grp->num_open_fds + deliver > MAX_OPEN_FDS) {
      /* §4.2: message NOT consumed, refs stay with the queued message. */
      spinlock_release_irqrestore(&u->lock, flags);
      return -EMFILE;
    }
    int copy = mlen < bufcap ? mlen : bufcap;
    if (copy > 0) {
      int done = 0;
      while (done < copy) {
        int take = copy - done;
        uint32_t tail = c->tail;
        uint32_t contig = K_UNIX_RING - tail;
        if ((uint32_t)take > contig) take = (int)contig;
        iov_store(io, niov, done, &c->ring[tail], take);
        c->tail = (tail + (uint32_t)take) % K_UNIX_RING;
        c->used -= (uint32_t)take;
        done += take;
      }
    } else {
      c->tail = (c->tail + (uint32_t)mlen) % K_UNIX_RING;
      c->used -= (uint32_t)mlen;
    }
    int gdeliver[K_UNIX_MAX_FDS];
    int gexcess[K_UNIX_MAX_FDS];
    int nexcess = 0;
    for (int f = 0; f < deliver; f++) gdeliver[f] = c->msg_fds[mi][f];
    for (int f = deliver; f < nfds; f++) gexcess[nexcess++] = c->msg_fds[mi][f];
    c->msg_nfds[mi] = 0;
    c->mtail = (mi + 1) % K_UNIX_MSGS;
    c->mcount--;
    uint64_t wake = c->wwait;
    c->wwait = 0;
    spinlock_release_irqrestore(&u->lock, flags);
    drain_wakeup_mask(wake);

    int installed = 0;
    for (int f = 0; f < deliver; f++) {
      int ufd = fs_msg_install_gfd(grp, gdeliver[f]);
      if (ufd < 0) {
        fs_close_global(gdeliver[f]); /* capacity pre-checked; safety net */
        continue;
      }
      if (cmsg_cloexec) grp->fd_cloexec |= (1ull << ufd);
      gfds_out[installed++] = ufd;
    }
    *nfds_out = installed;
    if (nexcess > 0) {
      *ctrl_trunc = 1;
      for (int f = 0; f < nexcess; f++) fs_close_global(gexcess[f]);
    }
    if (mlen > copy) *trunc = 1;
    *got = copy;
    return copy;
  }
}

/* --- public wrappers ---------------------------------------------------- */

/* write(): STREAM appends to the byte ring; SEQPACKET/DGRAM frame exactly one
 * message per call (§2.3: whole-or-nothing, > K_UNIX_MSG_MAX -> -EMSGSIZE). */
int usock_send(struct usock *u, int end, const void *buf, int n, int nonblock) {
  if (!u) return -EINVAL;
  struct k_iovec io;
  io.base = (uint64_t)buf;
  io.len = (uint64_t)(n > 0 ? n : 0);
  if (u->type == K_UNIX_STREAM)
    return usock_stream_send(u, end, &io, 1, n, nonblock);
  return usock_msg_send(u, end, &io, 1, n, 0, 0, nonblock);
}

/* read(): STREAM coalesces from the ring; SEQPACKET/DGRAM return exactly one
 * message, discarding any tail beyond the buffer (§2.3).  A message carrying
 * fds is drained with the calls closed (read() has no cmsg out-parameter). */
int usock_recv(struct usock *u, int end, void *buf, int n, int nonblock) {
  if (!u) return -EINVAL;
  struct k_iovec io;
  io.base = (uint64_t)buf;
  io.len = (uint64_t)(n > 0 ? n : 0);
  if (u->type == K_UNIX_STREAM)
    return usock_stream_recv(u, end, &io, 1, n, nonblock);
  int got = 0, trunc = 0, nfds = 0, ctrunc = 0;
  int r = usock_msg_recv(u, end, &io, 1, n, &got, &trunc, 0, 0, &nfds,
                         &ctrunc, nonblock, 0);
  return r < 0 ? r : got;
}

int usock_send_msg(struct usock *u, int end, const struct k_iovec *io,
                   int niov, int total, const int *gfds, int nfds,
                   int nonblock) {
  if (!u) return -EINVAL;
  if (u->type == K_UNIX_STREAM)
    return usock_stream_send(u, end, io, niov, total, nonblock);
  return usock_msg_send(u, end, io, niov, total, gfds, nfds, nonblock);
}

int usock_recv_msg(struct usock *u, int end, const struct k_iovec *io,
                   int niov, int bufcap, int *got, int *trunc,
                   int *gfds_out, int maxfds, int *nfds_out, int *ctrl_trunc,
                   int nonblock, int cmsg_cloexec) {
  *got = 0;
  *trunc = 0;
  *nfds_out = 0;
  *ctrl_trunc = 0;
  if (!u) return -EINVAL;
  if (u->type == K_UNIX_STREAM)
    return usock_stream_recv(u, end, io, niov, bufcap, nonblock);
  return usock_msg_recv(u, end, io, niov, bufcap, got, trunc, gfds_out,
                        maxfds, nfds_out, ctrl_trunc, nonblock, cmsg_cloexec);
}

/* --- IceWM M1 (icewm.md section 4.1): named AF_UNIX stream sockets ------
 *
 * The namespace is the pool itself: an end-0 endpoint with `bound` set owns
 * name[0].  Path names ("/tmp/.X11-unix/X0") and abstract names
 * ("\0/tmp/.X11-unix/X0") are distinct keys; nothing touches the
 * filesystem (FAT16 has no socket inodes), so unlink() of a socket path is
 * the caller's harmless no-op and closing the bound end frees the name.
 *
 * connect() completes immediately when the listener's queue has room
 * (Linux AF_UNIX): the connecting endpoint becomes end 0 of a connected
 * pair whose end-1 reference is held by the accept queue until accept()
 * hands it out as a fresh fd.  Blocking accept/connect park on the
 * listener's acc_wait/conn_wait pid masks with the pipe discipline: the
 * mask bit and PROC_STATE_BLOCKED are set under proc_lock + the lock that
 * guards the condition; wakes are drained after every lock is dropped. */

static int usock_name_eq(const struct usock_name *a,
                         const struct usock_name *b) {
  if (a->len != b->len) return 0;
  for (int i = 0; i < a->len; i++)
    if (a->bytes[i] != b->bytes[i]) return 0;
  return 1;
}

static void usock_name_copy(struct usock_name *dst,
                            const struct usock_name *src) {
  dst->len = src->len;
  for (int i = 0; i < src->len; i++) dst->bytes[i] = src->bytes[i];
}

int usock_name_parse(const uint8_t *sa, int salen, struct usock_name *out) {
  out->len = 0;
  if (!sa || salen < 2 || salen > K_UNIX_SUN_LEN) return -EINVAL;
  /* sun_family is a host-order u16; byte-wise so no unaligned load. */
  uint16_t fam = (uint16_t)(sa[0] | ((uint16_t)sa[1] << 8));
  if (fam != K_AF_UNIX) return -EAFNOSUPPORT;
  int plen = salen - 2;
  if (plen == 0) return 0; /* unnamed: autobind (bind) / EINVAL (connect) */
  const uint8_t *path = sa + 2;
  if (path[0] == 0) {
    /* Abstract: every byte is significant, leading NUL included. */
    for (int i = 0; i < plen; i++) out->bytes[i] = path[i];
    out->len = (uint8_t)plen;
    return 0;
  }
  int n = 0;
  while (n < plen && path[n] != 0) n++;
  for (int i = 0; i < n; i++) out->bytes[i] = path[i];
  out->len = (uint8_t)n;
  return 0;
}

int usock_name_format(const struct usock_name *n, uint8_t *sa) {
  sa[0] = (uint8_t)(K_AF_UNIX & 0xFF);
  sa[1] = (uint8_t)(K_AF_UNIX >> 8);
  for (int i = 0; i < n->len; i++) sa[2 + i] = n->bytes[i];
  if (n->len == 0) return 2;
  if (n->bytes[0] == 0) return 2 + n->len; /* abstract: exact length */
  if (n->len < K_UNIX_PATH_MAX) {
    sa[2 + n->len] = 0;
    return 2 + n->len + 1; /* path: Linux counts the terminating NUL */
  }
  return 2 + n->len;
}

/* Caller holds unix_name_lock. */
static struct usock *usock_name_lookup(const struct usock_name *n) {
  for (int i = 0; i < MAX_UNIX_PAIRS; i++) {
    struct usock *c = &unix_pool[i];
    if (c->in_use && c->bound && usock_name_eq(&c->name[0], n)) return c;
  }
  return 0;
}

static const char unix_hex[] = "0123456789abcdef";

int usock_bind(struct usock *u, int end, const struct usock_name *n) {
  if (!u || !n) return -EINVAL;
  uint64_t nf = spinlock_acquire_irqsave(&unix_name_lock);
  if (end != 0 || u->connected || u->bound || u->listening) {
    spinlock_release_irqrestore(&unix_name_lock, nf);
    return -EINVAL; /* Linux: already bound / not a fresh endpoint */
  }
  struct usock_name nm;
  if (n->len == 0) {
    /* Autobind (Linux): "\0" + 5 hex digits, unique in the namespace. */
    int ok = 0;
    for (int tries = 0; tries < 0x100000 && !ok; tries++) {
      uint32_t v = (unix_autobind_seq++) & 0xFFFFF;
      nm.len = 6;
      nm.bytes[0] = 0;
      for (int d = 0; d < 5; d++)
        nm.bytes[1 + d] = (uint8_t)unix_hex[(v >> (4 * (4 - d))) & 0xF];
      ok = usock_name_lookup(&nm) == 0;
    }
    if (!ok) {
      spinlock_release_irqrestore(&unix_name_lock, nf);
      return -EADDRINUSE;
    }
  } else {
    usock_name_copy(&nm, n);
    if (usock_name_lookup(&nm)) {
      spinlock_release_irqrestore(&unix_name_lock, nf);
      return -EADDRINUSE;
    }
  }
  usock_name_copy(&u->name[0], &nm);
  u->bound = 1;
  spinlock_release_irqrestore(&unix_name_lock, nf);
  return 0;
}

int usock_listen(struct usock *u, int end, int backlog) {
  if (!u) return -EINVAL;
  if (u->type == K_UNIX_DGRAM) return -EOPNOTSUPP;
  if (end != 0 || u->connected) return -EINVAL;
  if (!u->bound) {
    struct usock_name none;
    none.len = 0;
    int r = usock_bind(u, end, &none); /* Linux autobinds a listener */
    if (r != 0 && r != -EINVAL) return r;
  }
  uint64_t nf = spinlock_acquire_irqsave(&unix_name_lock);
  if (u->connected || !u->bound) {
    spinlock_release_irqrestore(&unix_name_lock, nf);
    return -EINVAL;
  }
  /* Linux admits backlog + 1 pending connections. */
  if (backlog < 0) backlog = 0;
  if (backlog > K_UNIX_BACKLOG - 1) backlog = K_UNIX_BACKLOG - 1;
  uint64_t uf = spinlock_acquire_irqsave(&u->lock);
  u->backlog = backlog + 1;
  u->listening = 1;
  spinlock_release_irqrestore(&u->lock, uf);
  uint64_t wake = u->conn_wait; /* a grown queue may admit parked peers */
  u->conn_wait = 0;
  spinlock_release_irqrestore(&unix_name_lock, nf);
  drain_wakeup_mask(wake);
  return 0;
}

int usock_is_listening(struct usock *u) { return u ? u->listening : 0; }

/* Park the current process on *mask under proc_lock + unix_name_lock
 * after `recheck` (evaluated with both held) still says "wait".  Returns
 * -2 parked, 1 retry (state changed), -EAGAIN without a process. */
static int usock_name_park(uint64_t *mask,
                           int (*still_wait)(struct usock *, void *),
                           struct usock *l, void *arg) {
  struct process *cur = current_process();
  if (!cur) return -EAGAIN;
  uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
  uint64_t nf = spinlock_acquire_irqsave(&unix_name_lock);
  int ret;
  if (still_wait(l, arg)) {
    *mask |= (1ULL << cur->pid);
    cur->state = PROC_STATE_BLOCKED;
    ret = -2;
  } else {
    ret = 1;
  }
  spinlock_release_irqrestore(&unix_name_lock, nf);
  spinlock_release_irqrestore(&proc_lock, pf);
  return ret;
}

/* connect() still has to wait: the named listener exists, is listening,
 * and its queue is full.  Caller holds unix_name_lock. */
static int connect_still_full(struct usock *unused, void *arg) {
  (void)unused;
  const struct usock_name *n = (const struct usock_name *)arg;
  struct usock *l = usock_name_lookup(n);
  return l && l->listening && (int)l->qcount >= l->backlog;
}

static int accept_still_empty(struct usock *l, void *arg) {
  (void)arg;
  return l->in_use && l->listening && l->qcount == 0;
}

int usock_connect(struct usock *u, int end, const struct usock_name *n,
                  int nonblock) {
  if (!u || !n) return -EINVAL;
  if (n->len == 0) return -EINVAL;
  if (u->type == K_UNIX_DGRAM) return -EOPNOTSUPP; /* not in scope (4.1) */
  if (u->listening) return -EINVAL;
  if (end != 0 || u->connected) return -EISCONN;

  /* Ring memory first, outside every lock (frame allocator). */
  if (!u->ring_base) {
    uint8_t *rings = unix_ring_alloc();
    if (!rings) return -ENOBUFS;
    uint64_t uf = spinlock_acquire_irqsave(&u->lock);
    int raced = u->ring_base != 0;
    if (!raced) usock_attach_rings(u, rings);
    spinlock_release_irqrestore(&u->lock, uf);
    if (raced) unix_ring_free(rings);
  }

  for (;;) {
    uint64_t nf = spinlock_acquire_irqsave(&unix_name_lock);
    if (u->connected) {
      spinlock_release_irqrestore(&unix_name_lock, nf);
      return -EISCONN; /* another thread won the race */
    }
    struct usock *l = usock_name_lookup(n);
    if (!l) {
      spinlock_release_irqrestore(&unix_name_lock, nf);
      /* Linux: a missing path is ENOENT; a missing abstract name is
       * ECONNREFUSED. */
      return n->bytes[0] == 0 ? -ECONNREFUSED : -ENOENT;
    }
    if (l == u || !l->listening) {
      spinlock_release_irqrestore(&unix_name_lock, nf);
      return -ECONNREFUSED;
    }
    if (l->type != u->type) {
      spinlock_release_irqrestore(&unix_name_lock, nf);
      return -EPROTOTYPE;
    }
    if ((int)l->qcount >= l->backlog) {
      int nb = nonblock || u->nonblock[end];
      spinlock_release_irqrestore(&unix_name_lock, nf);
      if (nb) return -EAGAIN;
      int r = usock_name_park(&l->conn_wait, connect_still_full, 0,
                              (void *)n);
      if (r == 1) continue;
      return r;
    }
    /* Admit: the pair is connected now; the queue holds end 1's ref. */
    uint64_t uf = spinlock_acquire_irqsave(&u->lock);
    u->connected = 1;
    u->refs[1] = 1;
    usock_name_copy(&u->name[1], &l->name[0]);
    u->shut[0] = u->shut[1] = 0;
    spinlock_release_irqrestore(&u->lock, uf);
    uint32_t slot = (l->qhead + l->qcount) % K_UNIX_BACKLOG;
    l->accq[slot] = u;
    l->qcount++;
    uint64_t wake = l->acc_wait;
    l->acc_wait = 0;
    spinlock_release_irqrestore(&unix_name_lock, nf);
    drain_wakeup_mask(wake);
    return 0;
  }
}

int usock_accept(struct usock *l, int end, int nonblock, struct usock **out) {
  if (!l || !out) return -EINVAL;
  *out = 0;
  if (end != 0) return -EINVAL;
  for (;;) {
    uint64_t nf = spinlock_acquire_irqsave(&unix_name_lock);
    if (!l->listening) {
      spinlock_release_irqrestore(&unix_name_lock, nf);
      return -EINVAL;
    }
    if (l->qcount > 0) {
      struct usock *c = l->accq[l->qhead];
      l->accq[l->qhead] = 0;
      l->qhead = (l->qhead + 1) % K_UNIX_BACKLOG;
      l->qcount--;
      uint64_t wake = l->conn_wait; /* room for a parked connect() */
      l->conn_wait = 0;
      spinlock_release_irqrestore(&unix_name_lock, nf);
      drain_wakeup_mask(wake);
      *out = c;
      return 0;
    }
    int nb = nonblock || l->nonblock[end];
    spinlock_release_irqrestore(&unix_name_lock, nf);
    if (nb) return -EAGAIN;
    int r = usock_name_park(&l->acc_wait, accept_still_empty, l, 0);
    if (r == 1) continue;
    return r;
  }
}

int usock_getname(struct usock *u, int end, int peer, struct usock_name *out) {
  out->len = 0;
  if (!u || end < 0 || end > 1) return -EINVAL;
  uint64_t nf = spinlock_acquire_irqsave(&unix_name_lock);
  uint64_t uf = spinlock_acquire_irqsave(&u->lock);
  int r = 0;
  if (!peer) {
    usock_name_copy(out, &u->name[end]);
  } else if (!u->connected) {
    r = -ENOTCONN;
  } else {
    usock_name_copy(out, &u->name[1 - end]);
  }
  spinlock_release_irqrestore(&u->lock, uf);
  spinlock_release_irqrestore(&unix_name_lock, nf);
  return r;
}

int usock_shutdown(struct usock *u, int end, int how) {
  if (!u || end < 0 || end > 1) return -EINVAL;
  if (how < 0 || how > 2) return -EINVAL;
  uint8_t bits = (how == 0)   ? K_UNIX_SHUT_RD
                 : (how == 1) ? K_UNIX_SHUT_WR
                              : (K_UNIX_SHUT_RD | K_UNIX_SHUT_WR);
  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  if (!u->connected) {
    spinlock_release_irqrestore(&u->lock, flags);
    return -ENOTCONN;
  }
  u->shut[end] |= bits;
  /* Everyone parked on either direction re-checks (EOF / EPIPE now). */
  uint64_t wake = 0;
  for (int d = 0; d < 2; d++) {
    wake |= u->chan[d].rwait | u->chan[d].wwait;
    u->chan[d].rwait = u->chan[d].wwait = 0;
  }
  spinlock_release_irqrestore(&u->lock, flags);
  drain_wakeup_mask(wake);
  return 0;
}

#ifdef KERNEL_MODE_UNIT_TEST
/* Test hook: live pool entries (leak checks in unix_test.c). */
int usock_test_live(void) {
  int n = 0;
  for (int i = 0; i < MAX_UNIX_PAIRS; i++) n += unix_pool[i].in_use ? 1 : 0;
  return n;
}
#endif
