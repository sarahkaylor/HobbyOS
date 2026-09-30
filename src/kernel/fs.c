#include "fs.h"
#include "process.h"
#include "pipe.h"
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

int file_socket(struct process *p, int domain, int type, int protocol) {
  if (!p) return -EINVAL;
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

int file_socket_connect(struct process *p, int fd, uint32_t ip_be,
                        uint16_t port_be) {
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

int file_fcntl(struct process *p, int fd, int cmd, int arg) {
  int err = 0;
  struct file *f = f1_socket_file(p, fd, &err);
  if (!f) return -err;
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

/* Readiness probe for one fd, setting the r/w/e out-parameters.  Returns 0,
 * or -1 when the fd is not open (select then fails the whole call with
 * EBADF, like Linux). */
static int f1_probe_fd(struct process *p, int fd, int *r, int *w, int *e) {
  *r = 0;
  *w = 0;
  *e = 0;
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
       * readable so the reader observes the close, plus the exception
       * flag (an fd_set has no separate POLLHUP channel). */
      if (count > 0) *r = 1;
      if (writers == 0) {
        *r = 1;
        *e = 1;
      }
    } else {
      /* Write end: space available; with no readers left a write fails
       * (EPIPE), reported as writable + exception, i.e. POLLOUT|POLLERR. */
      if (readers == 0) {
        *w = 1;
        *e = 1;
      } else if (count < PIPE_SIZE) {
        *w = 1;
      }
    }
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
      *e = 1; /* HUP: a connected socket closed cleanly (also readable) */
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

/* select() wake strategy (F1.2): never busy-spin.  A select with nothing
 * ready parks the process through the scheduler exactly like sys_sleep()
 * (PROC_STATE_BLOCKED + a short wake_ms slice) and the syscall restarts on
 * wake (-2 convention), re-polling all fds.  The caller's timeout is made
 * exact by remembering the absolute deadline per pid across the restarts.
 * A stale entry (a process that died inside select) can cost the slot's
 * next occupant one early wake, never a longer-than-requested wait, and it
 * is cleared by the next select's completion. */
static struct {
  uint64_t deadline_ms; /* absolute; 0 = wait forever */
  int valid;
} select_wait[MAX_PROCESSES];

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

      int r = 0, w = 0, e = 0;
      if (f1_probe_fd(p, fd, &r, &w, &e) != 0) {
        select_wait[p->pid].valid = 0;
        return -EBADF;
      }
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

struct socket_pcb *file_socket_pcb(struct process *p, int fd) {
  int err = 0;
  struct file *f = f1_socket_file(p, fd, &err);
  return f ? f->socket.pcb : 0;
}

int file_socket_getopt(struct process *p, int fd, int level, int optname,
                       void *val, int *len) {
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
    *errp = (f->type == FILE_TYPE_PIPE) ? ESPIPE : EINVAL;
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
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -1;

  struct file *f = &global_file_table[g_fd];

  uint64_t flags = spinlock_acquire_irqsave(&f->lock);

  if (f->type == FILE_TYPE_PIPE) {
    pipe_close(f->pipe.ptr, f->pipe.end);
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
  cur->num_open_fds--;
  return 0;
}

/* dup(fd): the lowest unused user fd now refers to the same open file. */
int file_dup(struct process *cur, int fd) {
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
  spinlock_release_irqrestore(&f->lock, flags);
  cur->open_fds[newfd] = g_fd;
  cur->num_open_fds++;
  return newfd;
}

/* dup2(oldfd, newfd): newfd refers to the same open file (closing whatever
 * is there now, per POSIX).  oldfd == newfd is a no-op success. */
int file_dup2(struct process *cur, int oldfd, int newfd) {
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
  spinlock_release_irqrestore(&f->lock, flags);
  cur->open_fds[newfd] = g_fd;
  cur->num_open_fds++;
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
    }
  }
  spinlock_release_irqrestore(&f->lock, flags);
}

int file_mkdir(struct process *cur, const char *path) {
  if (!cur || !path) return -1;
  return vfs_mkdir(path);
}
