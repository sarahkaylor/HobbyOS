#include "unix.h"
#include "fs.h"
#include "process.h"
#include "lock.h"
#include "errno.h"

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

extern spinlock_t proc_lock;

void unix_init(void) { spinlock_init(&unix_pool_lock); }

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
  uint64_t flags = spinlock_acquire_irqsave(&unix_pool_lock);
  for (int i = 0; i < MAX_UNIX_PAIRS; i++) {
    struct usock *u = &unix_pool[i];
    if (u->in_use) continue;
    u->in_use = 1;
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

  uint64_t flags = spinlock_acquire_irqsave(&u->lock);
  if (!u->in_use || u->refs[end] == 0) {
    spinlock_release_irqrestore(&u->lock, flags);
    return;
  }
  u->refs[end]--;
  if (u->refs[end] == 0) {
    int peer = 1 - end;
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
      u->in_use = 0;
      u->connected = 0;
    }
  }
  spinlock_release_irqrestore(&u->lock, flags);

  drain_wakeup_mask(wake);
  for (int i = 0; i < nclose; i++) fs_close_global(close_fds[i]);
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
  if (!u->connected) {
    /* No peer ever attached: every operation fails ENOTCONN. */
    if (end == 1) *w = 1;
    *e = 1;
    spinlock_release_irqrestore(&u->lock, flags);
    return;
  }
  int peer = 1 - end;
  if (end == 0) {
    /* Read side: drains chan[1]. */
    struct usock_chan *c = &u->chan[1];
    if (!chan_empty(u, c)) {
      *r = 1;
    } else if (u->refs[peer] == 0) {
      *r = 1;
      *hup = 1; /* drained and peer closed: EOF is readable */
    }
  } else {
    /* Write side: fills chan[1]. */
    struct usock_chan *c = &u->chan[1];
    if (u->refs[peer] == 0) {
      *w = 1;
      *e = 1; /* peer closed: writes fail EPIPE (POLLOUT|POLLERR) */
    } else if (!chan_full(u, c)) {
      *w = 1;
    }
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
  if (u->refs[peer] == 0) {
    int dead = read_side ? (u->type == K_UNIX_DGRAM ? -ECONNRESET : 0) : -EPIPE;
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
  } else if (u->refs[peer] == 0) {
    ret = read_side ? (u->type == K_UNIX_DGRAM ? -ECONNRESET : 0) : -EPIPE;
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
    if (u->refs[1 - end] == 0) {
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
    if (u->refs[1 - end] == 0) {
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
      if (cmsg_cloexec) grp->fd_cloexec |= (1u << ufd);
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
