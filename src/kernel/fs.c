#include "fs.h"
#include "process.h"
#include "pipe.h"
#include "unix.h"
#include "lock.h"
#include "net.h"
#include "nfs.h"
#include "vfs.h"
#include "errno.h"
#include "timer.h"

static struct file global_file_table[MAX_GLOBAL_FILES];
static spinlock_t fs_lock;

extern void uart_puts(const char *s);
extern void print_int(int val);

/* P4: one user-range helper per arch (trap.c); the msghdr/iovec/cmsg blocks
 * are range-checked through it before the kernel touches them. */
extern int sys_user_range_ok(uint64_t ptr, uint64_t len);

/**
 * Initializes the virtual file system layer.
 * Sets up the global file table and initializes the FS spinlock.
 */
void fs_init(void) {
  spinlock_init(&fs_lock);
  for (int i = 0; i < MAX_GLOBAL_FILES; i++) {
    global_file_table[i].type = FILE_TYPE_EMPTY;
    global_file_table[i].ref_count = 0;
    spinlock_init(&global_file_table[i].lock);
  }
}

/**
 * Allocates a free file structure from the global file table.
 *
 * Returns:
 *   Pointer to the allocated struct file, or 0 if no slots are available.
 */
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

/**
 * Calculates the global file descriptor index for a given file structure pointer.
 *
 * Parameters:
 *   f - Pointer to a file structure in the global table.
 *
 * Returns:
 *   Index in the global_file_table, or -1 if the pointer is null.
 */
static int get_global_fd(struct file *f) {
  if (!f) return -1;
  return (int)(f - global_file_table);
}

/**
 * Opens a file by name and assigns a local file descriptor to the current process.
 *
 * Parameters:
 *   filename - Name of the file to open.
 *
 * Returns:
 *   Local file descriptor index on success, or -1 on failure.
 */
/* Open flags, matching the sysroot's fcntl.h values. */
#ifndef O_CREAT
#define O_CREAT 0x40
#define O_EXCL  0x80
#define O_TRUNC 0x200
#endif

int file_open(struct process *cur, const char *filename, int flags) {
  cur = process_group(cur); /* P1 (D7): fd table + cwd are group state */
  if (!cur || cur->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;

  struct file *f = file_alloc();
  if (!f) return -EMFILE;

  /* Paths under an NFS mount are opened by the NFS client; everything
   * else falls through to FAT-16. */
  int routed = vfs_open_routed(filename, f);
  if (routed < 0) {
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
    return -ENOENT;
  }
  if (routed == 0) {
    /* POSIX open(): a missing file only comes into existence with
       O_CREAT; O_CREAT|O_EXCL refuses to touch an existing one.
       The existence probe must honour the process cwd: fat16_open
       below resolves relative paths itself, so comparing result
       codes against a root-relative lookup made every relative
       open of an existing file in a subdirectory fail with
       ENOENT. */
    struct fat16_dir_entry probe_entry;
    char abs[256];
    int exists = 0;
    if (vfs_abs_path(filename, abs, sizeof abs) == 0) {
      exists = (fat16_resolve_path(abs, &probe_entry, 0, 0) == 0);
    }

    if (!exists && !(flags & O_CREAT)) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -ENOENT;
    }
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -EEXIST;
    }
    if (fat16_open(filename, f) != 0) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -ENOENT;
    }
    if (exists && (flags & O_TRUNC)) {
      (void)fat16_truncate(f);
    }
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
    if (f->type == FILE_TYPE_FAT16) fat16_close(f);
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
  }

  return fd;
}

int file_connect(struct process *cur, uint32_t ip, uint16_t port, int protocol) {
  cur = process_group(cur); /* P1 (D7): fd table is group state */
  if (!cur || cur->num_open_fds >= MAX_OPEN_FDS) return -1;

  struct file *f = file_alloc();
  if (!f) return -1;

  struct socket_pcb* pcb = net_socket_create(protocol);
  if (!pcb) {
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
    return -1;
  }

  if (net_socket_connect(pcb, ip, port) != 0) {
    net_socket_close(pcb);
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
    return -1;
  }

  f->type = FILE_TYPE_SOCKET;
  f->socket.pcb = pcb;

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
    net_socket_close(pcb);
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
  }

  return fd;
}

/* --- Phase F1 (browser.md A.1a): socket/select syscall support ---------- */

extern spinlock_t proc_lock;

/* A socket fd's global file slot, or NULL with *errp set: EBADF for an fd
 * outside the table, ENOTSOCK when the fd is open but is not a socket. */
static struct file *f1_socket_file(struct process *p, int fd, int *errp) {
  p = process_group(p); /* P1 (D7): the fd table is group state */
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) {
    *errp = EBADF;
    return 0;
  }
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) {
    *errp = EBADF;
    return 0;
  }
  struct file *f = &global_file_table[g_fd];
  if (f->type != FILE_TYPE_SOCKET || !f->socket.pcb) {
    *errp = ENOTSOCK;
    return 0;
  }
  return f;
}

/* P4: shared AF_UNIX type validation for socket()/socketpair().  Returns the
 * K_UNIX_* type, or -errno.  SOCK_RAW and unknown types collapse to
 * EPROTONOSUPPORT (no ESOCKTNOSUPPORT in the frozen errno set, matching the
 * existing IP branch's documented v1 choice); stray bits are EINVAL. */
static int unix_type_of(int type) {
  if (type & ~(0xF | K_SOCK_CLOEXEC)) return -EINVAL;
  switch (type & 0xF) {
  case K_SOCK_STREAM:
    return K_UNIX_STREAM;
  case K_SOCK_DGRAM:
    return K_UNIX_DGRAM;
  case K_SOCK_SEQPACKET:
    return K_UNIX_SEQPACKET;
  default:
    return -EPROTONOSUPPORT; /* SOCK_RAW etc. */
  }
}

int file_socket(struct process *p, int domain, int type, int protocol) {
  p = process_group(p); /* P1 (D7): fd table is group state */
  if (!p) return -EINVAL;

  if (domain == K_AF_UNIX) { /* P4 (D9) */
    if (protocol != 0) return -EPROTONOSUPPORT;
    int utype = unix_type_of(type);
    if (utype < 0) return utype;
    if (p->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;

    struct file *f = file_alloc();
    if (!f) return -EMFILE;
    struct usock *u = usock_alloc_single(utype);
    if (!u) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -EMFILE;
    }
    f->type = FILE_TYPE_UNIXSOCK;
    f->usock.ptr = u;
    f->usock.end = 0;

    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FDS; i++) {
      if (p->open_fds[i] == -1) {
        p->open_fds[i] = get_global_fd(f);
        p->num_open_fds++;
        fd = i;
        break;
      }
    }
    if (fd == -1) {
      usock_close(u, 0);
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -EMFILE;
    }
    if (type & K_SOCK_CLOEXEC) p->fd_cloexec |= 1u << fd;
    return fd;
  }

  if (domain != K_AF_INET) return -EAFNOSUPPORT;

  int proto;
  if (type == K_SOCK_STREAM) {
    if (protocol != 0 && protocol != K_IPPROTO_TCP) return -EPROTONOSUPPORT;
    proto = IP_PROTO_TCP;
  } else if (type == K_SOCK_DGRAM) {
    if (protocol != 0 && protocol != K_IPPROTO_UDP) return -EPROTONOSUPPORT;
    proto = IP_PROTO_UDP;
  } else {
    /* No ESOCKTNOSUPPORT in the frozen errno set; documented v1 choice. */
    return -EPROTONOSUPPORT;
  }

  if (p->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;

  struct file *f = file_alloc();
  if (!f) return -EMFILE;

  struct socket_pcb *pcb = net_socket_create(proto);
  if (!pcb) {
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
    return -ENFILE;
  }

  f->type = FILE_TYPE_SOCKET;
  f->socket.pcb = pcb;

  int fd = -1;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (p->open_fds[i] == -1) {
      p->open_fds[i] = get_global_fd(f);
      p->num_open_fds++;
      fd = i;
      break;
    }
  }

  if (fd == -1) {
    net_socket_close(pcb);
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
    return -EMFILE;
  }
  return fd;
}

/* P4 (D4): connect() on an AF_UNIX endpoint is -EOPNOTSUPP (pairs are born
 * connected; no pathname namespace in v1). */
static int f1_unix_guard(struct process *p, int fd) {
  p = process_group(p);
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) return 0;
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return 0;
  return global_file_table[g_fd].type == FILE_TYPE_UNIXSOCK;
}

int file_socket_connect(struct process *p, int fd, uint32_t ip_be,
                        uint16_t port_be) {
  if (f1_unix_guard(p, fd)) return -EOPNOTSUPP;
  int err = 0;
  struct file *f = f1_socket_file(p, fd, &err);
  if (!f) return -err;

  struct socket_pcb *pcb = f->socket.pcb;
  uint16_t port = ntohs(port_be); /* the stack keeps ports host-order */

  if (pcb->state == SOCKET_ESTABLISHED && pcb->connect_started &&
      pcb->connect_err == 0) {
    /* Already connected.  Linux says EISCONN; that errno is not in the
     * frozen set, so v1 reports EINVAL (documented in the report). */
    return -EINVAL;
  }

  if (pcb->nonblock) {
    if (net_socket_connect_start(pcb, ip_be, port) != 0) return -EIO;
    /* TCP: handshake in flight.  Completion is observed through select()
     * writability (F1.2/F1.3) and getsockopt(SO_ERROR). */
    return (pcb->protocol == IP_PROTO_TCP) ? -EINPROGRESS : 0;
  }

  if (net_socket_connect(pcb, ip_be, port) != 0) {
    return -(pcb->connect_err ? pcb->connect_err : ETIMEDOUT);
  }
  return 0;
}

/* P4 (§6): F_GETFD/F_SETFD are handled on the fd_cloexec mask for every fd
 * type; F_GETFL/F_SETFL keep their socket-only behavior (now also for
 * AF_UNIX).  Non-socket fds keep today's ENOTSOCK for other commands. */
int file_fcntl(struct process *p, int fd, int cmd, int arg) {
  struct process *pg = process_group(p);
  if (!pg) return -EINVAL;
  if (fd < 0 || fd >= MAX_OPEN_FDS) return -EBADF;
  if (cmd == K_F_GETFD) return (pg->fd_cloexec >> fd) & 1u ? K_FD_CLOEXEC : 0;
  if (cmd == K_F_SETFD) {
    /* Replaces the whole flag word (only FD_CLOEXEC exists). */
    if (arg & K_FD_CLOEXEC)
      pg->fd_cloexec |= 1u << fd;
    else
      pg->fd_cloexec &= ~(1u << fd);
    return 0;
  }

  int g_fd = pg->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];

  if (f->type == FILE_TYPE_SOCKET && f->socket.pcb) {
    struct socket_pcb *pcb = f->socket.pcb;
    switch (cmd) {
    case K_F_GETFL:
      return net_socket_get_nonblock(pcb) ? K_O_NONBLOCK : 0;
    case K_F_SETFL:
      net_socket_set_nonblock(pcb, (arg & K_O_NONBLOCK) != 0);
      return 0;
    default:
      return -EINVAL;
    }
  }
  if (f->type == FILE_TYPE_UNIXSOCK && f->usock.ptr) {
    switch (cmd) {
    case K_F_GETFL:
      return usock_get_nonblock(f->usock.ptr, f->usock.end) ? K_O_NONBLOCK : 0;
    case K_F_SETFL:
      usock_set_nonblock(f->usock.ptr, f->usock.end,
                         (arg & K_O_NONBLOCK) != 0);
      return 0;
    default:
      return -EINVAL;
    }
  }
  return -ENOTSOCK; /* historical behavior for open non-socket fds */
}

/* Readiness probe for one fd, setting the r/w/e/hup out-parameters.
 * POLLHUP is its own channel now (poll); select folds hup into its
 * exception set.  Returns 0, or -1 when the fd is not open (select then
 * fails the whole call with EBADF, like Linux; poll reports POLLNVAL). */
static int f1_probe_fd(struct process *p, int fd, int *r, int *w, int *e,
                       int *hup) {
  p = process_group(p); /* P1 (D7): the fd table is group state */
  *r = 0;
  *w = 0;
  *e = 0;
  *hup = 0;
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) return -1;
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;
  struct file *f = &global_file_table[g_fd];

  if (f->type == FILE_TYPE_PIPE) {
    struct pipe *pp = f->pipe.ptr;
    if (!pp) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&pp->lock);
    uint32_t count = pp->count;
    uint32_t readers = pp->reader_count;
    uint32_t writers = pp->writer_count;
    spinlock_release_irqrestore(&pp->lock, flags);

    if (f->pipe.end == 0) {
      /* Read end: data ready; EOF (last writer closed) also reports
       * readable so the reader observes the close, plus HUP (poll) /
       * exception (select, an fd_set has no separate POLLHUP channel). */
      if (count > 0) *r = 1;
      if (writers == 0) {
        *r = 1;
        *hup = 1;
      }
    } else {
      /* Write end: space available; with no readers left a write fails
       * (EPIPE), reported as writable + error, i.e. POLLOUT|POLLERR. */
      if (readers == 0) {
        *w = 1;
        *e = 1;
      } else if (count < PIPE_SIZE) {
        *w = 1;
      }
    }
    return 0;
  }

  if (f->type == FILE_TYPE_UNIXSOCK) { /* P4 */
    if (!f->usock.ptr) return -1;
    usock_probe(f->usock.ptr, f->usock.end, r, w, e, hup);
    return 0;
  }

  if (f->type == FILE_TYPE_SOCKET) {
    struct socket_pcb *pcb = f->socket.pcb;
    if (!pcb) return -1;
    /* These fields are written by the RX IRQ handler; peeking without the
     * net lock is deliberate — a racy peek can only report stale readiness
     * and the caller re-polls.  net_socket_ready() also drives handshake
     * retransmission for a socket parked in connect. */
    if (net_socket_ready(pcb, 0)) *r = 1;
    if (net_socket_ready(pcb, 1)) *w = 1;
    if (pcb->connect_err != 0) *e = 1;
    if (pcb->protocol == IP_PROTO_TCP && pcb->connect_started &&
        pcb->state == SOCKET_CLOSED && pcb->connect_err == 0) {
      *hup = 1; /* cleanly closed: readable EOF + POLLHUP (select: exception) */
      *r = 1;
    }
    return 0;
  }

  if (f->type == FILE_TYPE_FAT16 || f->type == FILE_TYPE_NFS) {
    /* Regular files are always readable (a read may return 0 at EOF). */
    *r = 1;
    return 0;
  }
  return -1;
}

/* select()/poll() wake strategy (F1.2, P4): never busy-spin.  A wait with
 * nothing ready parks the process through the scheduler exactly like
 * sys_sleep() (PROC_STATE_BLOCKED + a short wake_ms slice) and the syscall
 * restarts on wake (-2 convention), re-polling all fds.  The caller's
 * timeout is made exact by remembering the absolute deadline per pid across
 * the restarts; the table is shared by select and poll.  A stale entry (a
 * process that died inside a wait) can cost the slot's next occupant one
 * early wake, never a longer-than-requested wait, and it is cleared by the
 * next wait's completion. */
static struct {
  uint64_t deadline_ms; /* absolute; 0 = wait forever */
  int valid;
} select_wait[MAX_PROCESSES];

/* P1: forget a pid's remembered select deadline (group teardown / thread
 * death).  Callers hold proc_lock; never takes it itself so the exit paths
 * can call this while already holding it. */
void file_select_forget(int pid) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return;
  select_wait[pid].valid = 0;
  select_wait[pid].deadline_ms = 0;
}

#define SELECT_POLL_SLICE_MS 10

int file_select(struct process *p, int nfds, struct fd_set_k *rd,
                struct fd_set_k *wr, struct fd_set_k *ex, int timeout_ms) {
  if (!p) return -EINVAL;
  if (nfds < 0 || nfds > K_FD_SETSIZE) return -EINVAL;

  uint64_t now = timer_get_ms();
  if (!select_wait[p->pid].valid) {
    select_wait[p->pid].valid = 1;
    select_wait[p->pid].deadline_ms =
        (timeout_ms < 0) ? 0 : now + (uint64_t)timeout_ms;
  } else if (timeout_ms >= 0) {
    uint64_t cap = now + (uint64_t)timeout_ms;
    if (select_wait[p->pid].deadline_ms == 0 ||
        select_wait[p->pid].deadline_ms > cap) {
      /* Only reachable through a stale entry (pid reuse): never wait past
       * what this caller asked for. */
      select_wait[p->pid].deadline_ms = cap;
    }
  }

  struct fd_set_k out_rd, out_wr, out_ex;
  for (int i = 0; i < K_FD_SET_WORDS; i++) {
    out_rd.bits[i] = 0;
    out_wr.bits[i] = 0;
    out_ex.bits[i] = 0;
  }

  int ready = 0;
  for (int word = 0; word < K_FD_SET_WORDS; word++) {
    uint32_t bits = 0;
    if (rd) bits |= rd->bits[word];
    if (wr) bits |= wr->bits[word];
    if (ex) bits |= ex->bits[word];
    while (bits) {
      int bit = __builtin_ctz(bits);
      bits &= bits - 1;
      int fd = word * 32 + bit;
      if (fd >= nfds) continue; /* POSIX: only fds < nfds are examined */

      int r = 0, w = 0, e = 0, hup = 0;
      if (f1_probe_fd(p, fd, &r, &w, &e, &hup) != 0) {
        select_wait[p->pid].valid = 0;
        return -EBADF;
      }
      e |= hup || 0; /* select folds POLLHUP into its exception set */
      uint32_t m = 1u << bit;
      if (rd && (rd->bits[word] & m) && r) {
        out_rd.bits[word] |= m;
        ready++;
      }
      if (wr && (wr->bits[word] & m) && w) {
        out_wr.bits[word] |= m;
        ready++;
      }
      if (ex && (ex->bits[word] & m) && e) {
        out_ex.bits[word] |= m;
        ready++;
      }
    }
  }

  if (ready > 0) {
    select_wait[p->pid].valid = 0;
    for (int i = 0; i < K_FD_SET_WORDS; i++) {
      if (rd) rd->bits[i] = out_rd.bits[i];
      if (wr) wr->bits[i] = out_wr.bits[i];
      if (ex) ex->bits[i] = out_ex.bits[i];
    }
    return ready;
  }

  int expired = (timeout_ms == 0) ||
                (select_wait[p->pid].deadline_ms != 0 &&
                 now >= select_wait[p->pid].deadline_ms);
  if (expired) {
    select_wait[p->pid].valid = 0;
    for (int i = 0; i < K_FD_SET_WORDS; i++) {
      if (rd) rd->bits[i] = 0;
      if (wr) wr->bits[i] = 0;
      if (ex) ex->bits[i] = 0;
    }
    return 0;
  }

  uint64_t wake = now + SELECT_POLL_SLICE_MS;
  if (select_wait[p->pid].deadline_ms != 0 &&
      select_wait[p->pid].deadline_ms < wake) {
    wake = select_wait[p->pid].deadline_ms;
  }
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  p->wake_ms = wake;
  p->state = PROC_STATE_BLOCKED;
  spinlock_release_irqrestore(&proc_lock, flags);
  return -2;
}

/* P4 (docs/browser/p4-ipc-design.md §4.3): the SYS_POLL engine.  A pure
 * read-only pass over the caller's array; revents is written back per entry
 * (-EBADF is never a poll error; bad fds are POLLNVAL).  Not-ready parks
 * through the shared deadline table exactly like file_select. */
int file_poll(struct process *p, struct k_pollfd *fds, int nfds,
              int timeout_ms) {
  if (!p) return -EINVAL;
  if (nfds < 0 || nfds > K_FD_SETSIZE) return -EINVAL;

  uint64_t now = timer_get_ms();
  if (!select_wait[p->pid].valid) {
    select_wait[p->pid].valid = 1;
    select_wait[p->pid].deadline_ms =
        (timeout_ms < 0) ? 0 : now + (uint64_t)timeout_ms;
  } else if (timeout_ms >= 0) {
    uint64_t cap = now + (uint64_t)timeout_ms;
    if (select_wait[p->pid].deadline_ms == 0 ||
        select_wait[p->pid].deadline_ms > cap) {
      select_wait[p->pid].deadline_ms = cap; /* stale pid-reuse entry */
    }
  }

  int ready = 0;
  for (int i = 0; i < nfds; i++) {
    short revents = 0;
    int fd = fds[i].fd;
    if (fd >= 0) {
      int r = 0, w = 0, e = 0, hup = 0;
      if (fd >= MAX_OPEN_FDS ||
          f1_probe_fd(p, fd, &r, &w, &e, &hup) != 0) {
        revents = K_POLLNVAL;
      } else {
        revents = ipc_poll_map(r, w, e, hup, fds[i].events);
      }
    }
    fds[i].revents = revents;
    if (revents) ready++;
  }

  if (ready > 0) {
    select_wait[p->pid].valid = 0;
    return ready;
  }

  int expired = (timeout_ms == 0) ||
                (select_wait[p->pid].deadline_ms != 0 &&
                 now >= select_wait[p->pid].deadline_ms);
  if (expired) {
    select_wait[p->pid].valid = 0;
    return 0;
  }

  uint64_t wake = now + SELECT_POLL_SLICE_MS;
  if (select_wait[p->pid].deadline_ms != 0 &&
      select_wait[p->pid].deadline_ms < wake) {
    wake = select_wait[p->pid].deadline_ms;
  }
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  p->wake_ms = wake;
  p->state = PROC_STATE_BLOCKED;
  spinlock_release_irqrestore(&proc_lock, flags);
  return -2;
}

struct socket_pcb *file_socket_pcb(struct process *p, int fd) {
  int err = 0;
  struct file *f = f1_socket_file(p, fd, &err);
  return f ? f->socket.pcb : 0;
}

int file_socket_getopt(struct process *p, int fd, int level, int optname,
                       void *val, int *len) {
  /* P4: unix endpoints answer SO_ERROR/SO_TYPE/SO_DOMAIN (libc probes). */
  {
    struct process *pg = process_group(p);
    if (pg && fd >= 0 && fd < MAX_OPEN_FDS) {
      int g = pg->open_fds[fd];
      if (g >= 0 && g < MAX_GLOBAL_FILES &&
          global_file_table[g].type == FILE_TYPE_UNIXSOCK) {
        struct file *uf = &global_file_table[g];
        if (level != K_SOL_SOCKET) return -ENOPROTOOPT;
        if (!val || !len) return -EINVAL;
        if (*len < (int)sizeof(int)) return -EINVAL;
        int value;
        switch (optname) {
        case K_SO_ERROR:
          value = 0;
          break;
        case K_SO_TYPE:
          switch (usock_type(uf->usock.ptr)) {
          case K_UNIX_DGRAM: value = K_SOCK_DGRAM; break;
          case K_UNIX_SEQPACKET: value = K_SOCK_SEQPACKET; break;
          default: value = K_SOCK_STREAM; break;
          }
          break;
        case K_SO_DOMAIN:
          value = K_AF_UNIX;
          break;
        default:
          return -ENOPROTOOPT;
        }
        *(int *)val = value;
        *len = (int)sizeof(int);
        return 0;
      }
    }
  }
  int err = 0;
  struct file *f = f1_socket_file(p, fd, &err);
  if (!f) return -err;
  struct socket_pcb *pcb = f->socket.pcb;

  if (level != K_SOL_SOCKET) return -ENOPROTOOPT;
  if (!val || !len) return -EINVAL;
  if (*len < (int)sizeof(int)) return -EINVAL;

  int value;
  switch (optname) {
  case K_SO_ERROR:
    value = net_socket_error(pcb);
    break;
  case K_SO_TYPE:
    value = (pcb->protocol == IP_PROTO_TCP) ? K_SOCK_STREAM : K_SOCK_DGRAM;
    break;
  case K_SO_REUSEADDR:
    value = 0; /* we never "reuse" an address: every socket has its own port */
    break;
  default:
    return -ENOPROTOOPT;
  }
  *(int *)val = value;
  *len = (int)sizeof(int);
  return 0;
}

int file_socket_setopt(struct process *p, int fd, int level, int optname,
                       const void *val, int len) {
  (void)val;
  (void)len;
  int err = 0;
  struct file *f = f1_socket_file(p, fd, &err);
  if (!f) return -err;

  if (level != K_SOL_SOCKET) return -ENOPROTOOPT;
  if (optname == K_SO_REUSEADDR) {
    return 0; /* accepted no-op (each socket owns a unique ephemeral port) */
  }
  return -ENOPROTOOPT;
}

/* --- Phase 3: lseek/stat --------------------------------------------- */

int64_t file_seek(struct process *p, int fd, int64_t offset, int whence,
                  int *errp) {
  p = process_group(p); /* P1 (D7): the fd table is group state */
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) { *errp = EBADF; return -1; }
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) { *errp = EBADF; return -1; }
  struct file *f = &global_file_table[g_fd];

  int64_t base, size;
  if (f->type == FILE_TYPE_FAT16) {
    base = f->fat16.cursor;
    size = f->fat16.entry.file_size;
  } else if (f->type == FILE_TYPE_NFS) {
    base = f->nfs.cursor;
    size = f->nfs.size;
  } else {
    *errp = (f->type == FILE_TYPE_PIPE || f->type == FILE_TYPE_UNIXSOCK)
                ? ESPIPE : EINVAL;
    return -1;
  }

  int64_t newpos;
  switch (whence) {
  case 0: newpos = offset;          break;  /* SEEK_SET */
  case 1: newpos = base + offset;   break;  /* SEEK_CUR */
  case 2: newpos = size + offset;   break;  /* SEEK_END */
  default: *errp = EINVAL; return -1;
  }
  if (newpos < 0) { *errp = EINVAL; return -1; }

  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  if (f->type == FILE_TYPE_FAT16) f->fat16.cursor = (uint32_t)newpos;
  else                            f->nfs.cursor  = (uint32_t)newpos;
  spinlock_release_irqrestore(&f->lock, flags);
  *errp = 0;
  return newpos;
}

/* FAT16 has no on-disk inode numbers: a file's identity is its directory
 * entry.  Like Linux's vfat driver, synthesize a stable number from the
 * location of that entry (the sector holding it plus the entry's index
 * within the 512-byte sector, 16 entries per sector).  Distinct live files
 * always report distinct st_ino values, and repeated opens of one file
 * report the same value, which is what tools such as diffutils' cmp rely
 * on to tell "same file" from "same contents". */
static unsigned long fat16_synth_ino(uint32_t dir_sector, uint32_t dir_offset) {
  return ((unsigned long)dir_sector << 4) | (dir_offset & 0xF);
}

/* NFSv3 file handles are the server-side identity of a file; fold one into
 * a 64-bit number for st_ino (BKDR hash).  Distinct handles collide only
 * with negligible probability. */
static unsigned long nfs_synth_ino(const struct nfs_fh *fh) {
  unsigned long h = 0;
  uint32_t i;
  for (i = 0; i < fh->len && i < sizeof fh->data; i++) {
    h = (h << 5) - h + fh->data[i];
  }
  return h;
}

static void k_stat_fill(struct k_stat *st, unsigned long ino,
                        unsigned long mode, long size) {
  st->st_dev = 0;
  st->st_ino = ino;
  st->st_mode = mode;
  st->st_nlink = 1;
  st->st_uid = 0;
  st->st_gid = 0;
  st->st_rdev = 0;
  st->st_size = size;
  st->st_blksize = 512;
  st->st_blocks = (size + 511) / 512;
  st->st_atime = st->st_mtime = st->st_ctime = 0;
}

int file_stat_fd(struct process *p, int fd, struct k_stat *st, int *errp) {
  p = process_group(p); /* P1 (D7): the fd table is group state */
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) { *errp = EBADF; return -1; }
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) { *errp = EBADF; return -1; }
  struct file *f = &global_file_table[g_fd];
  switch (f->type) {
  case FILE_TYPE_FAT16:
    k_stat_fill(st,
                fat16_synth_ino(f->fat16.dir_sector, f->fat16.dir_offset),
                (f->fat16.entry.attr & 0x10)
                    ? (K_S_IFDIR | 0755) : (K_S_IFREG | 0644),
                f->fat16.entry.file_size);
    return 0;
  case FILE_TYPE_NFS:
    k_stat_fill(st, nfs_synth_ino(&f->nfs.fh),
                f->nfs.is_dir ? (K_S_IFDIR | 0755)
                              : (K_S_IFREG | 0644),
                f->nfs.size);
    return 0;
  case FILE_TYPE_PIPE:
    k_stat_fill(st, 0, K_S_IFIFO | 0600, 0);
    return 0;
  case FILE_TYPE_SOCKET:
    k_stat_fill(st, 0, K_S_IFSOCK | 0600, 0);
    return 0;
  case FILE_TYPE_UNIXSOCK: /* P4: F_GETFD-probing tools stat caches use this */
    k_stat_fill(st, 0, K_S_IFSOCK | 0600, 0);
    return 0;
  default:
    *errp = EBADF;
    return -1;
  }
}

int file_stat_path(struct process *p, const char *path, struct k_stat *st,
                   int *errp) {
  (void)p;
  if (!path) { *errp = EINVAL; return -1; }
  char abs[256];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) { *errp = EINVAL; return -1; }
  struct fat16_dir_entry entry;
  uint32_t sector = 0, offset = 0;
  if (fat16_resolve_path(abs, &entry, &sector, &offset) != 0) {
    *errp = ENOENT;
    return -1;
  }
  k_stat_fill(st, fat16_synth_ino(sector, offset),
              (entry.attr & 0x10) ? (K_S_IFDIR | 0755) : (K_S_IFREG | 0644),
              entry.file_size);
  return 0;
}

/**
 * Closes a local file descriptor and releases its reference to the global file.
 *
 * Parameters:
 *   fd - Local file descriptor index.
 *
 * Returns:
 *   0 on success, -1 if the descriptor is invalid.
 */
int file_close(struct process *cur, int fd) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

  struct file *f = &global_file_table[g_fd];

  uint64_t flags = spinlock_acquire_irqsave(&f->lock);

  if (f->type == FILE_TYPE_PIPE) {
    pipe_close(f->pipe.ptr, f->pipe.end);
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    usock_close(f->usock.ptr, f->usock.end); /* P4: release this end's ref */
  }

  f->ref_count--;
  if (f->ref_count == 0) {
    if (f->type == FILE_TYPE_FAT16) {
      fat16_close(f);
    } else if (f->type == FILE_TYPE_SOCKET) {
      net_socket_close(f->socket.pcb);
    }
    /* FILE_TYPE_NFS keeps no backend state: nothing to release. */
    f->type = FILE_TYPE_EMPTY;
  }
  spinlock_release_irqrestore(&f->lock, flags);

  cur->open_fds[fd] = -1;
  cur->fd_cloexec &= ~(1u << fd); /* P5 (D3.2): closing discards fd flags */
  cur->num_open_fds--;
  return 0;
}

/* dup(fd): the lowest unused user fd now refers to the same open file. */
int file_dup(struct process *cur, int fd) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -EBADF;
  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];
  if (f->type == FILE_TYPE_EMPTY) return -EBADF;
  if (cur->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;
  int newfd = -1;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (cur->open_fds[i] == -1) {
      newfd = i;
      break;
    }
  }
  if (newfd < 0) return -EMFILE;
  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  f->ref_count++;
  if (f->type == FILE_TYPE_PIPE)
    pipe_reopen(f->pipe.ptr, f->pipe.end); /* keep the pipe end's fd count
        in sync: file_close calls pipe_close per fd, so every dup must too,
        or closing the duplicate makes readers see EOF while refs remain */
  else if (f->type == FILE_TYPE_UNIXSOCK)
    usock_reopen(f->usock.ptr, f->usock.end); /* P4: same ref discipline */
  spinlock_release_irqrestore(&f->lock, flags);
  cur->open_fds[newfd] = g_fd;
  cur->fd_cloexec &= ~(1u << newfd); /* P5 (D3.2): dup clears CLOEXEC */  cur->num_open_fds++;
  return newfd;
}

/* dup2(oldfd, newfd): newfd refers to the same open file (closing whatever
 * is there now, per POSIX).  oldfd == newfd is a no-op success. */
int file_dup2(struct process *cur, int oldfd, int newfd) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || oldfd < 0 || oldfd >= MAX_OPEN_FDS) return -EBADF;
  int g_fd = cur->open_fds[oldfd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];
  if (f->type == FILE_TYPE_EMPTY) return -EBADF;
  if (oldfd == newfd) return newfd;
  if (newfd < 0 || newfd >= MAX_OPEN_FDS) return -EINVAL;
  if (cur->open_fds[newfd] != -1) {
    int r = file_close(cur, newfd);
    if (r != 0) return -EBADF;
  }
  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  f->ref_count++;
  if (f->type == FILE_TYPE_PIPE)
    pipe_reopen(f->pipe.ptr, f->pipe.end); /* see file_dup: keep the pipe
        end's per-fd count aligned with ref_count */
  else if (f->type == FILE_TYPE_UNIXSOCK)
    usock_reopen(f->usock.ptr, f->usock.end); /* P4 */
  spinlock_release_irqrestore(&f->lock, flags);
  cur->open_fds[newfd] = g_fd;
  cur->fd_cloexec &= ~(1u << newfd); /* P5 (D3.2): dup2 clears CLOEXEC */  cur->num_open_fds++;
  return newfd;
}

void fs_close_global(int g_fd) {
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return;

  struct file *f = &global_file_table[g_fd];

  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  if (f->type == FILE_TYPE_EMPTY) {
    spinlock_release_irqrestore(&f->lock, flags);
    return;
  }

  if (f->type == FILE_TYPE_PIPE) {
    pipe_close(f->pipe.ptr, f->pipe.end);
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    usock_close(f->usock.ptr, f->usock.end); /* P4 */
  }

  f->ref_count--;
  if (f->ref_count == 0) {
    if (f->type == FILE_TYPE_FAT16) {
      fat16_close(f);
    } else if (f->type == FILE_TYPE_SOCKET) {
      net_socket_close(f->socket.pcb);
    }
    f->type = FILE_TYPE_EMPTY;
  }
  spinlock_release_irqrestore(&f->lock, flags);
}

/**
 * Reads data from a file descriptor into a buffer.
 * Supports FAT16 files and Pipes.
 *
 * Parameters:
 *   fd   - Local file descriptor index.
 *   buf  - Destination buffer in memory.
 *   size - Number of bytes to read.
 *   tf   - Trap frame (used for blocking reads in pipes).
 *
 * Returns:
 *   Number of bytes read, or -1 on failure.
 */
int file_read(struct process *cur, int fd, void *buf, int size, struct trap_frame *tf) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

  struct file *f = &global_file_table[g_fd];
  if (f->type == FILE_TYPE_FAT16) {
    return fat16_read(f, buf, size);
  } else if (f->type == FILE_TYPE_NFS) {
    if (f->nfs.is_dir) return -1;               /* use read_dir for dirs */
    if (size <= 0) return 0;
    const struct nfs_mount *m = nfs_mount_at(f->nfs.mount_idx);
    if (!m) return -1;
    uint64_t left = f->nfs.size > f->nfs.cursor
                        ? f->nfs.size - f->nfs.cursor : 0;
    uint32_t want = (uint32_t)size;
    if ((uint64_t)want > left) want = (uint32_t)left;
    if (want == 0) return 0;
    int got = nfs_read_file(m, &f->nfs.fh, f->nfs.cursor, buf, want);
    if (got > 0) f->nfs.cursor += (uint32_t)got;
    return got;
  } else if (f->type == FILE_TYPE_PIPE) {
    if (f->pipe.end != 0) return -1; // Read end only
    return pipe_read(f->pipe.ptr, buf, size, tf);
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    if (!f->usock.ptr) return -1;
    /* Both ends are readable; -2 (park) rides the same restart convention
     * as pipes.  EOF (0) and -ECONNRESET/-ENOTCONN pass through for the
     * trap carve-out (§6.5: ret < -1 is a real errno). */
    return usock_recv(f->usock.ptr, f->usock.end, buf, size, 0);
  } else if (f->type == FILE_TYPE_SOCKET) {
    return net_socket_recv(f->socket.pcb, buf, size);
  }
  return -1;
}

/**
 * Checks how many bytes are available to read from a file descriptor.
 *
 * Returns:
 *   Number of bytes available, or -1 if closed/EOF.
 */
int file_available(struct process *cur, int fd) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

  struct file *f = &global_file_table[g_fd];
  if (f->type == FILE_TYPE_FAT16) {
    // FAT16 files just return size minus cursor
    return f->fat16.entry.file_size - f->fat16.cursor;
  } else if (f->type == FILE_TYPE_NFS) {
    if (f->nfs.is_dir) return -1;
    if (f->nfs.size <= f->nfs.cursor) return 0;
    uint64_t left = f->nfs.size - f->nfs.cursor;
    return left > 0x7FFFFFFF ? 0x7FFFFFFF : (int)left;
  } else if (f->type == FILE_TYPE_PIPE) {
    if (f->pipe.end != 0) return -1; // Read end only
    return pipe_available(f->pipe.ptr);
  } else if (f->type == FILE_TYPE_SOCKET) {
    uint32_t avail = f->socket.pcb->rx_tail - f->socket.pcb->rx_head;
    return avail;
  }
  return -1;
}

/**
 * Writes data from a buffer to a file descriptor.
 * Supports FAT16 files and Pipes.
 *
 * Parameters:
 *   fd   - Local file descriptor index.
 *   buf  - Source buffer in memory.
 *   size - Number of bytes to write.
 *   tf   - Trap frame (used for blocking writes in pipes).
 *
 * Returns:
 *   Number of bytes written, or -1 on failure.
 */
int file_write(struct process *cur, int fd, const void *buf, int size, struct trap_frame *tf) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

  struct file *f = &global_file_table[g_fd];
  if (f->type == FILE_TYPE_FAT16) {
    return fat16_write(f, buf, size);
  } else if (f->type == FILE_TYPE_NFS) {
    return -1;      /* NFS mounts are read-only */
  } else if (f->type == FILE_TYPE_PIPE) {
    if (f->pipe.end != 1) {
      uart_puts("file_write: wrong pipe end: ");
      print_int(f->pipe.end);
      uart_puts("\n");
      return -1;
    }
    return pipe_write(f->pipe.ptr, buf, size, tf);
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    if (!f->usock.ptr) return -1;
    /* §2.2/D3: peer gone -> -EPIPE (no signal in P4); message types frame one
     * write as one message (-EMSGSIZE past the cap).  -2 parks, exactly like
     * the pipe path; other negatives pass through to the trap carve-out. */
    return usock_send(f->usock.ptr, f->usock.end, buf, size, 0);
  } else if (f->type == FILE_TYPE_SOCKET) {
    return net_socket_send(f->socket.pcb, buf, size);
  }
  return -1;
}

/**
 * Returns 1 if the global file slot holds a pipe, 0 otherwise (or if the
 * index is out of range).  Used by fd inheritance: a pipe end must not be
 * handed to a child by *default* (stderr), since a duplicated end keeps
 * the pipe's reader/writer counts alive and can self-deadlock a full
 * pipe whose only remaining drainer is the child's own inherited fd.
 */
int file_gfd_is_pipe(int gfd) {
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES) return 0;
  return global_file_table[gfd].type == FILE_TYPE_PIPE;
}

/**
 * Creates an anonymous pipe and assigns two file descriptors (read and write).
 *
 * Parameters:
 *   fds - Array to store the two local file descriptors (fds[0]=read, fds[1]=write).
 *
 * Returns:
 *   0 on success, -1 on failure.
 */
int file_pipe(struct process *cur, int fds[2]) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
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

/**
 * Increments the reference count of a global file descriptor.
 * Used during process fork to share open file descriptors with the child.
 *
 * Parameters:
 *   global_fd - Index in the global_file_table.
 */
void fs_reopen(int global_fd) {
  if (global_fd < 0 || global_fd >= MAX_GLOBAL_FILES) return;
  struct file *f = &global_file_table[global_fd];
  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  if (f->type != FILE_TYPE_EMPTY) {
    f->ref_count++;
    if (f->type == FILE_TYPE_PIPE) {
      pipe_reopen(f->pipe.ptr, f->pipe.end);
    } else if (f->type == FILE_TYPE_UNIXSOCK) {
      usock_reopen(f->usock.ptr, f->usock.end); /* P4: fork's fd copies */
    }
  }
  spinlock_release_irqrestore(&f->lock, flags);
}

int file_mkdir(struct process *cur, const char *path) {
  cur = process_group(cur); /* P1 (D7): cwd is group state */
  if (!cur || !path) return -1;
  return vfs_mkdir(path);
}

/* --- P4 (docs/browser/p4-ipc-design.md §3-§4): AF_UNIX + SCM_RIGHTS ------ */

/* Byte-wise copies over a single user range: the range has already been
 * checked; the loop keeps the kernel free of unaligned wide accesses even
 * if a caller hands us an odd address. */
static int fs_copy_in(void *dst, uint64_t src, uint64_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  if (n == 0) return 1;
  if (!sys_user_range_ok(src, n)) return 0;
  for (uint64_t i = 0; i < n; i++) d[i] = s[i];
  return 1;
}

static int fs_copy_out(uint64_t dst, const void *src, uint64_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  if (n == 0) return 1;
  if (!sys_user_range_ok(dst, n)) return 0;
  for (uint64_t i = 0; i < n; i++) d[i] = s[i];
  return 1;
}

/* SYS_SOCKETPAIR (77): AF_UNIX only; the two ends get the lowest free user
 * fds.  Validated before any allocation (§3). */
int file_socketpair(struct process *p, int domain, int type, int proto,
                    int *ufds) {
  p = process_group(p); /* P1 (D7): fd table is group state */
  if (!p || !ufds) return -EINVAL;
  if (domain != K_AF_UNIX) return -EAFNOSUPPORT;
  if (proto != 0) return -EPROTONOSUPPORT;
  int utype = unix_type_of(type);
  if (utype < 0) return utype;
  if (p->num_open_fds + 2 > MAX_OPEN_FDS) return -EMFILE;

  struct file *f0 = file_alloc();
  struct file *f1 = file_alloc();
  if (!f0 || !f1) {
    if (f0) { f0->type = FILE_TYPE_EMPTY; f0->ref_count = 0; }
    if (f1) { f1->type = FILE_TYPE_EMPTY; f1->ref_count = 0; }
    return -EMFILE;
  }
  struct usock *u = usock_alloc_pair(utype);
  if (!u) {
    f0->type = FILE_TYPE_EMPTY;
    f0->ref_count = 0;
    f1->type = FILE_TYPE_EMPTY;
    f1->ref_count = 0;
    return -EMFILE;
  }
  f0->type = FILE_TYPE_UNIXSOCK;
  f0->usock.ptr = u;
  f0->usock.end = 0;
  f1->type = FILE_TYPE_UNIXSOCK;
  f1->usock.ptr = u;
  f1->usock.end = 1;

  int fd0 = -1, fd1 = -1;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (p->open_fds[i] == -1) {
      if (fd0 == -1) fd0 = i;
      else if (fd1 == -1) { fd1 = i; break; }
    }
  }
  if (fd0 == -1 || fd1 == -1) {
    /* Unreachable behind the num_open_fds check; clean up defensively. */
    usock_close(u, 0);
    usock_close(u, 1);
    f0->type = FILE_TYPE_EMPTY;
    f0->ref_count = 0;
    f1->type = FILE_TYPE_EMPTY;
    f1->ref_count = 0;
    return -EMFILE;
  }
  p->open_fds[fd0] = get_global_fd(f0);
  p->open_fds[fd1] = get_global_fd(f1);
  p->num_open_fds += 2;
  if (type & K_SOCK_CLOEXEC) p->fd_cloexec |= (1u << fd0) | (1u << fd1);
  ufds[0] = fd0;
  ufds[1] = fd1;
  return 0;
}

/* 1 when the global slot holds an fd-table IPC endpoint end (pipe or AF_UNIX
 * socket end): the spawn default-stderr exclusion class (F1 extension of
 * file_gfd_is_pipe). */
int file_gfd_is_ipc_endpoint(int gfd) {
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES) return 0;
  return global_file_table[gfd].type == FILE_TYPE_PIPE ||
         global_file_table[gfd].type == FILE_TYPE_UNIXSOCK;
}

/* §4.2 step 1: validate a sender fd and take a message-owned reference to
 * its global slot (same ref discipline as dup: pipe/unix per-fd counts stay
 * aligned so the later release actually drops the backend ref). */
int fs_msg_ref_gfd(int gfd) {
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[gfd];
  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  if (f->type == FILE_TYPE_EMPTY) {
    spinlock_release_irqrestore(&f->lock, flags);
    return -EBADF;
  }
  f->ref_count++;
  if (f->type == FILE_TYPE_PIPE) {
    pipe_reopen(f->pipe.ptr, f->pipe.end);
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    usock_reopen(f->usock.ptr, f->usock.end);
  }
  spinlock_release_irqrestore(&f->lock, flags);
  return 0;
}

/* §4.2 step 2: install a message-owned reference into the receiver group as
 * the lowest free user fd.  The reference transfers: no ref bump here (the
 * message's ref is the one being handed over).  Capacity is pre-checked by
 * the caller with the message still queued (EMFILE means not consumed). */
int fs_msg_install_gfd(struct process *p, int gfd) {
  p = process_group(p);
  if (!p) return -EBADF;
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES) return -EBADF;
  if (global_file_table[gfd].type == FILE_TYPE_EMPTY) return -EBADF;
  if (p->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;
  int fd = -1;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (p->open_fds[i] == -1) { fd = i; break; }
  }
  if (fd < 0) return -EMFILE;
  p->open_fds[fd] = gfd;
  p->num_open_fds++;
  return fd; /* FD_CLOEXEC is the caller's (MSG_CMSG_CLOEXEC) business */
}

/* Resolve the target fd of sendmsg/recvmsg.  Returns the struct file or a
 * negative errno: EBADF bad fd, ENOTSOCK non-socket, EOPNOTSUPP for AF_INET
 * sockets (UDP append territory; v1 has no sendmsg on them). */
static struct file *f1_msg_socket(struct process *pg, int fd, int *errp) {
  if (!pg || fd < 0 || fd >= MAX_OPEN_FDS) { *errp = -EBADF; return 0; }
  int g_fd = pg->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) { *errp = -EBADF; return 0; }
  struct file *f = &global_file_table[g_fd];
  if (f->type == FILE_TYPE_SOCKET) { *errp = -EOPNOTSUPP; return 0; }
  if (f->type != FILE_TYPE_UNIXSOCK || !f->usock.ptr) {
    *errp = -ENOTSOCK;
    return 0;
  }
  return f;
}

/* Copy the caller's iovec array in and range-check every member. */
static int f1_copy_iovs(const struct k_msghdr *msg, struct k_iovec *iov,
                        int *totalp) {
  if (msg->iovlen < 1 || msg->iovlen > K_IPC_MAX_IOV) return -EINVAL;
  if (!fs_copy_in(iov, msg->iov, msg->iovlen * sizeof(struct k_iovec)))
    return -EFAULT;
  uint64_t total = 0;
  for (int i = 0; i < (int)msg->iovlen; i++) {
    if (iov[i].len > 0 && !sys_user_range_ok(iov[i].base, iov[i].len))
      return -EFAULT;
    total += iov[i].len;
    if (total > 0x7FFFFFFF) return -EMSGSIZE;
  }
  *totalp = (int)total;
  return 0;
}

/* SYS_SENDMSG (78).  One-shot: nothing is enqueued unless every fd validates
 * and the queue accepts the whole message (streams may write partially). */
int file_sendmsg(struct process *p, int fd, uint64_t umsg, int flags) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *f = f1_msg_socket(pg, fd, &err);
  if (!f) return err;

  int ferr = ipc_msg_flags_ok(flags, 0);
  if (ferr != 0) return ferr;

  struct k_msghdr msg;
  if (!fs_copy_in(&msg, umsg, sizeof msg)) return -EFAULT;
  if (msg.name != 0 || msg.namelen != 0) return -EINVAL;

  struct k_iovec iov[K_IPC_MAX_IOV];
  int total = 0;
  int ir = f1_copy_iovs(&msg, iov, &total);
  if (ir != 0) return ir;

  int is_stream = usock_type(f->usock.ptr) == K_UNIX_STREAM;
  int nfds = 0;
  int fds_user[K_IPC_MAX_FDS];
  if (msg.controllen > 0) {
    if (msg.control == 0) return -EINVAL;
    if (msg.controllen > K_IPC_CTRL_MAX) return -EINVAL;
    if (is_stream) return -EOPNOTSUPP; /* no ancillary data on STREAM */
    uint8_t ctrl[K_IPC_CTRL_MAX];
    if (!fs_copy_in(ctrl, msg.control, msg.controllen)) return -EFAULT;
    struct ipc_cmsg_fds parsed;
    int cr = ipc_cmsg_parse(ctrl, (uint32_t)msg.controllen, &parsed);
    if (cr != 0) return cr;
    nfds = parsed.nfds;
    for (int i = 0; i < nfds; i++) fds_user[i] = parsed.fds[i];
  }
  if (!is_stream && total > K_UNIX_MSG_MAX) return -EMSGSIZE;

  /* §4.2 step 1: validate every sender fd, then take message-owned refs.
   * Any failure releases what was already taken and enqueues nothing. */
  int gfds[K_IPC_MAX_FDS];
  int taken = 0;
  for (int i = 0; i < nfds; i++) {
    int ufd = fds_user[i];
    int g2 = -1;
    if (ufd >= 0 && ufd < MAX_OPEN_FDS) g2 = pg->open_fds[ufd];
    if (g2 < 0 || g2 >= MAX_GLOBAL_FILES ||
        fs_msg_ref_gfd(g2) != 0) {
      err = -EBADF;
      goto release;
    }
    gfds[taken++] = g2;
  }

  {
    int nonblock = (flags & K_MSG_DONTWAIT) != 0;
    int r = usock_send_msg(f->usock.ptr, f->usock.end, iov,
                           (int)msg.iovlen, total, gfds, nfds, nonblock);
    if (r < 0) {
      err = r;
      goto release; /* not enqueued: refs never transferred */
    }
    return r; /* success: the queued message owns the refs */
  }

release:
  for (int i = 0; i < taken; i++) fs_close_global(gfds[i]);
  return err;
}

/* SYS_RECVMSG (79). */
int file_recvmsg(struct process *p, int fd, uint64_t umsg, int flags) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *f = f1_msg_socket(pg, fd, &err);
  if (!f) return err;

  int ferr = ipc_msg_flags_ok(flags, 1);
  if (ferr != 0) return ferr;

  struct k_msghdr msg;
  if (!fs_copy_in(&msg, umsg, sizeof msg)) return -EFAULT;
  if (msg.name != 0 || msg.namelen != 0) return -EINVAL;

  struct k_iovec iov[K_IPC_MAX_IOV];
  int cap = 0;
  int ir = f1_copy_iovs(&msg, iov, &cap);
  if (ir != 0) return ir;

  /* Control buffer: how many whole fds fit?  A NULL/absent buffer means
   * arrived fds are truncated (MSG_CTRUNC) and closed (§4.2 step 3). */
  uint32_t maxfds = 0;
  if (msg.control != 0 && msg.controllen > 0) {
    if (msg.controllen > K_IPC_CTRL_MAX) return -EINVAL;
    maxfds = ipc_cmsg_fit((uint32_t)msg.controllen, K_IPC_MAX_FDS);
  }

  int nonblock = (flags & K_MSG_DONTWAIT) != 0;
  int cmsg_cloexec = (flags & K_MSG_CMSG_CLOEXEC) != 0;
  int got = 0, trunc = 0, nfds = 0, ctrunc = 0;
  int fds_user[K_IPC_MAX_FDS];
  int r = usock_recv_msg(f->usock.ptr, f->usock.end, iov,
                         (int)msg.iovlen, cap, &got, &trunc, fds_user,
                         (int)maxfds, &nfds, &ctrunc, nonblock, cmsg_cloexec);
  if (r < 0) return r;

  uint32_t out_controllen = 0;
  if (nfds > 0) {
    uint8_t cbuf[K_IPC_CTRL_MAX];
    out_controllen = ipc_cmsg_emit(cbuf, fds_user, nfds);
    if (!fs_copy_out(msg.control, cbuf, out_controllen)) return -EFAULT;
  }

  int32_t mflags = (int32_t)((trunc ? K_MSG_TRUNC : 0) |
                             (ctrunc ? K_MSG_CTRUNC : 0));
  if (!fs_copy_out(umsg + offsetof(struct k_msghdr, flags), &mflags,
                   sizeof mflags))
    return -EFAULT;
  uint64_t ctl64 = (uint64_t)out_controllen;
  if (!fs_copy_out(umsg + offsetof(struct k_msghdr, controllen), &ctl64,
                   sizeof ctl64))
    return -EFAULT;
  return r; /* bytes copied (Linux AF_UNIX returns the copied count) */
}
