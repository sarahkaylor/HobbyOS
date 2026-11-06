#include "fs.h"
#include "kline.h"
#include "process.h"
#include "pipe.h"
#include "unix.h"
#include "lock.h"
#include "net.h"
#include "nfs.h"
#include "vfs.h"
#include "vfs_bind.h"
#include "errno.h"
#include "file_obj.h" /* so-vm lane A (CROSS-LANE): page-cache invalidation */
#include "timer.h"
#include "vm.h" /* P2.4 (S4): memfd objects */
#include "frame.h" /* M5: fs_read_image contiguous image frames */

static struct file global_file_table[MAX_GLOBAL_FILES];
static spinlock_t fs_lock;

/* GB-108: fds 0-2 are the process's stdio and must never be handed out by an
 * implicit fd allocation (open/pipe/socket/dup/accept/memfd/msg-install).
 * A process launched by the boot wave starts with open_fds[0..2] == -1, and
 * file_write()'s fd-1/2 console fallback relies on that.  Without a floor the
 * first open takes fd 0 and the second takes fd 1 — which silently redirects
 * stdout into whatever was opened.  Measured in-guest (t4_fd_diag):
 *   [DIAG] null_w_fd=0   [DIAG] urandom_fd=1
 * and the golden T4 probe's open("/dev/urandom") -> 0 followed by
 * open("/dev/null", O_WRONLY) -> 1 swallowed 22 probe rows with no syscall
 * error at all.  Linux reserves 0-2 for every process; so does this. */
#define FD_STDIO_FIRST 3

/* P6.1: advisory record-lock table (the engine lives further down with
 * the fcntl commands; declared up here so fs_init initializes its lock). */
#define MAX_FILE_LOCKS 64

struct file_lock_entry {
  int used;
  int pid;          /* owning process-group pid */
  uint64_t file_id; /* FAT16 dir-entry identity */
  int64_t start;
  int64_t len;      /* 0 = to EOF */
  int type;         /* K_F_RDLCK / K_F_WRLCK */
};

static struct file_lock_entry file_locks[MAX_FILE_LOCKS];
static spinlock_t file_locks_lock;

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
  spinlock_init(&file_locks_lock);
  for (int i = 0; i < MAX_GLOBAL_FILES; i++) {
    global_file_table[i].type = FILE_TYPE_EMPTY;
    global_file_table[i].ref_count = 0;
    spinlock_init(&global_file_table[i].lock);
#ifdef KERNEL_MODE_UNIT_TEST
    /* SEC-OBS-B: register the documented order (see lock.h) for the debug
     * tracker.  file_locks_lock stays unregistered: it is a leaf taken only
     * after f->lock drops, never nesting anything above. */
    lockdbg_register(&fs_lock, "fs_lock", LOCK_RANK_FS);
    lockdbg_register(&global_file_table[i].lock, "f->lock", LOCK_RANK_FILE);
#endif
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
      /* SEC-6: UNINIT, not FAT16.  A caller that fails between allocation
         and setting the real type used to leave a slot claiming to be a
         FAT16 file with garbage start_cluster/cursor — any later operation
         on it read/wrote arbitrary disk sectors.  UNINIT is rejected by the
         invariants in file_read/file_write/file_close/stat, so an abandoned
         slot can only ever halt loudly, never corrupt. */
      global_file_table[i].type = FILE_TYPE_UNINIT;
      spinlock_release_irqrestore(&fs_lock, flags);
      return &global_file_table[i];
    }
  }
  spinlock_release_irqrestore(&fs_lock, flags);
  return 0;
}


/* ---- L57: NFS read-path self-check -------------------------------------
 *
 * "Some defs are missing and nothing was logged" is either a transport that
 * returns bytes it never read, or a game-side enumeration/parse loss.  Hash
 * one file the game definitely loads, through the SAME nfs_read_file() the
 * file layer uses, and print (bytes, size, nul, fnv).  A digest equal to the
 * host's FNV over the same bytes proves the transport is byte-exact, so a
 * missing def is not a short/corrupt read.
 */
static void nfs_selfcheck(struct file *f) {
  static int once;
  if (once) return;
  if (!f || f->type != FILE_TYPE_NFS || f->nfs.is_dir) return;
  const char *p = f->path;
  const char *want = "World_Oneshots_ProjectileImpacts.xml";
  int hit = 0, pl = 0, wl = 0;
  while (p[pl]) pl++;
  while (want[wl]) wl++;
  if (pl >= wl) {
    hit = 1;
    for (int i = 0; i < wl; i++)
      if (p[pl - wl + i] != want[i]) { hit = 0; break; }
  }
  if (!hit) return;
  once = 1;
  const struct nfs_mount *m = nfs_mount_at(f->nfs.mount_idx);
  if (!m) return;
  static uint8_t b[16384];
  unsigned long long h = 1469598103934665603ULL;
  long tot = 0, nul = 0;
  uint32_t off = 0;
  while (off < f->nfs.size && tot < (long)sizeof b) {
    uint32_t w = f->nfs.size - off;
    if (w > 4096) w = 4096;
    if ((long)w > (long)sizeof b - tot) w = (uint32_t)((long)sizeof b - tot);
    int got = nfs_read_file(m, &f->nfs.fh, off, b + tot, w);
    if (got <= 0) break;
    for (int i = 0; i < got; i++) {
      if (b[tot + i] == 0) nul++;
      h ^= (unsigned long long)b[tot + i];
      h *= 1099511628211ULL;
    }
    tot += got;
    off += (uint32_t)got;
  }
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[NFSCHK] bytes=");
  kl_i(&kl, tot);
  kl_s(&kl, " size=");
  kl_i(&kl, (long)f->nfs.size);
  kl_s(&kl, " nul=");
  kl_i(&kl, nul);
  kl_s(&kl, " fnv=0x");
  kl_hex(&kl, (unsigned long)h);
  kl_s(&kl, " ");
  kl_s(&kl, p);
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* ---- L57: fd-lifecycle flight recorder ---------------------------------
 *
 * The EBADF on a Keyed XML read arrives with no history: `read()` answers
 * EBADF, Mono names one path, and nothing in the serial log says who closed
 * that fd — or whether the fd was alive at all (fs.c's `return -1` for a
 * failed NFS READ reaches Mono as EBADF too, because trap.c maps -1 to
 * -EBADF).  Both halves of that question are the same row here.
 *
 * Every open/close/dup/dup2/close_range touching a `/NFS/` path is recorded
 * in a small in-kernel ring (fd, global slot, generation, tid, tgid, path
 * tail), and the ring is printed ONLY when a read on a /NFS file fails.  The
 * hot path costs a handful of stores per event and the serial line stays
 * silent until the failure the ring exists to explain, so the recorder does
 * not change the timing it is measuring.
 *
 * Generation is bumped per install, so "the reader's fd now names a different
 * file" (a slot reused under a live reader) is visible as a gen mismatch
 * rather than inferred.
 */
#define FD_EV_MAX 1024
#define FD_EV_TAIL 56
#define FDEV_OPEN 1
#define FDEV_CLOSE 2
#define FDEV_DUP 3
#define FDEV_DUP2 4
#define FDEV_FAIL 5
#define FDEV_RANGE 6

struct fd_ev {
  uint32_t seq;
  uint32_t tid;
  uint32_t tgid;
  uint32_t gen;
  int16_t fd;
  int16_t gfd;
  uint8_t kind;
  uint8_t pad;
  char tail[FD_EV_TAIL];
};

static struct fd_ev fd_ev_ring[FD_EV_MAX];
static uint32_t fd_ev_seq;
static uint32_t fd_slot_gen[MAX_OPEN_FDS];

static int fs_has_nfs(const char *p) {
  if (!p) return 0;
  for (int i = 0; p[i]; i++)
    if (p[i] == '/' && p[i + 1] == 'N' && p[i + 2] == 'F' && p[i + 3] == 'S' &&
        p[i + 4] == '/')
      return 1;
  return 0;
}

/* The informative part of a path is its tail; keep it, mark a truncation. */
static void fs_path_tail(char *dst, const char *src) {
  int n = 0;
  while (src && src[n]) n++;
  int start = 0;
  if (n > FD_EV_TAIL - 2) { start = n - (FD_EV_TAIL - 2); dst[0] = '~'; }
  int w = (start ? 1 : 0);
  for (int i = start; i < n && w < FD_EV_TAIL - 1; i++) dst[w++] = src[i];
  if (w == 0) dst[w++] = '?';
  dst[w] = '\0';
}

static void fd_ev_note(int kind, struct process *pg, int fd, int gfd,
                       const char *path) {
  if (!pg || fd < 0 || fd >= MAX_OPEN_FDS || !fs_has_nfs(path)) return;
  uint32_t s = fd_ev_seq++;
  struct fd_ev *e = &fd_ev_ring[s % FD_EV_MAX];
  struct process *cur = current_process();
  e->seq = s;
  e->tid = cur ? (uint32_t)cur->pid : 0;
  e->tgid = (uint32_t)pg->pid;
  e->gen = fd_slot_gen[fd];
  e->fd = (int16_t)fd;
  e->gfd = (int16_t)(gfd < 32767 ? gfd : 32767);
  e->kind = (uint8_t)kind;
  e->pad = 0;
  fs_path_tail(e->tail, path);
}

static const char *fd_ev_kind_name(int k) {
  switch (k) {
  case FDEV_OPEN: return "open";
  case FDEV_CLOSE: return "close";
  case FDEV_DUP: return "dup";
  case FDEV_DUP2: return "dup2";
  case FDEV_FAIL: return "readfail";
  case FDEV_RANGE: return "close_range";
  default: return "?";
  }
}

/* Print the last 22 events of THIS fd in this group, then the last 6 events
 * of the group overall (an fd number reused for another file shows up there).
 * Bounded to four dumps per boot: a real failure is one event, not a flood. */
static void fd_ev_dump(struct process *pg, int fd, const char *why) {
  static int dumps;
  if (!pg || dumps >= 4) return;
  dumps++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[FDR] fail fd=");
  kl_i(&kl, (long)fd);
  kl_s(&kl, " gen=");
  kl_i(&kl, (long)((fd >= 0 && fd < MAX_OPEN_FDS) ? (int)fd_slot_gen[fd] : -1));
  kl_s(&kl, " tgid=");
  kl_i(&kl, (long)pg->pid);
  kl_s(&kl, " cur=");
  {
    struct process *cur = current_process();
    kl_i(&kl, cur ? (long)cur->pid : -1L);
  }
  kl_s(&kl, " why=");
  kl_s(&kl, why);
  kl_s(&kl, " open_fds=");
  kl_i(&kl, (long)((fd >= 0 && fd < MAX_OPEN_FDS) ? pg->open_fds[fd] : -2));
  kl_s(&kl, " nfd=");
  kl_i(&kl, (long)pg->num_open_fds);
  kl_c(&kl, '\n');
  kl_emit(&kl);

  uint32_t first = fd_ev_seq > FD_EV_MAX ? fd_ev_seq - FD_EV_MAX : 0;
  int shown = 0;
  for (uint32_t s = fd_ev_seq; s > first && shown < 22; s--) {
    struct fd_ev *e = &fd_ev_ring[(s - 1) % FD_EV_MAX];
    if (e->fd != fd || e->tgid != (uint32_t)pg->pid) continue;
    shown++;
    struct kline kr;
    kl_init(&kr);
    kl_s(&kr, "[FDR]  #");
    kl_i(&kr, (long)e->seq);
    kl_s(&kr, " ");
    kl_s(&kr, fd_ev_kind_name(e->kind));
    kl_s(&kr, " fd=");
    kl_i(&kr, (long)e->fd);
    kl_s(&kr, " gfd=");
    kl_i(&kr, (long)e->gfd);
    kl_s(&kr, " gen=");
    kl_i(&kr, (long)e->gen);
    kl_s(&kr, " tid=");
    kl_i(&kr, (long)e->tid);
    kl_s(&kr, " ");
    kl_s(&kr, e->tail);
    kl_c(&kr, '\n');
    kl_emit(&kr);
  }
  shown = 0;
  for (uint32_t s = fd_ev_seq; s > first && shown < 6; s--) {
    struct fd_ev *e = &fd_ev_ring[(s - 1) % FD_EV_MAX];
    if (e->tgid != (uint32_t)pg->pid || e->fd == fd) continue;
    shown++;
    struct kline kx;
    kl_init(&kx);
    kl_s(&kx, "[FDR]  (other) #");
    kl_i(&kx, (long)e->seq);
    kl_s(&kx, " ");
    kl_s(&kx, fd_ev_kind_name(e->kind));
    kl_s(&kx, " fd=");
    kl_i(&kx, (long)e->fd);
    kl_s(&kx, " gfd=");
    kl_i(&kx, (long)e->gfd);
    kl_s(&kx, " tid=");
    kl_i(&kx, (long)e->tid);
    kl_s(&kx, " ");
    kl_s(&kx, e->tail);
    kl_c(&kx, '\n');
    kl_emit(&kx);
  }
}

/* L55: local-fd allocation was a check-then-set on shared group state.  Two
 * threads opening at once (RimWorld's four `DirectXmlLoader` threads each
 * MemoryMappedFile a def) could take the SAME slot: one file object was
 * orphaned, and the winner's fd number silently named the other file — the
 * measured shape was `mmap fd=19 ... -> -95` for a path that had just been
 * opened on /NFS, plus sporadic failed opens during the 1,209-XML load.
 * One leaf lock makes the scan+install atomic; it is never held across any
 * other lock or any I/O. */
static spinlock_t fdtab_lock;

static int get_global_fd(struct file *f);

static int fd_install(struct process *p, struct file *f) {
  if (!p || !f) return -1;
  uint64_t fl = spinlock_acquire_irqsave(&fdtab_lock);
  int fd = -1;
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++) {
    if (p->open_fds[i] == -1) {
      p->open_fds[i] = get_global_fd(f);
      p->num_open_fds++;
      fd_slot_gen[i]++;
      fd = i;
      break;
    }
  }
  spinlock_release_irqrestore(&fdtab_lock, fl);
  /* L57: record outside the lock (the row goes to the UART). */
  if (fd >= 0) fd_ev_note(FDEV_OPEN, p, fd, get_global_fd(f), f->path);
  return fd;
}

/* Public: close_range is rare, so one row per call is affordable and answers
 * "does anything close a whole range, and over which slots" directly. */
void fs_fd_ev_range(struct process *p, int first, int last) {
  struct process *pg = process_group(p);
  if (!pg) return;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[FDR] close_range first=");
  kl_i(&kl, (long)first);
  kl_s(&kl, " last=");
  kl_i(&kl, (long)last);
  kl_s(&kl, " nfd=");
  kl_i(&kl, (long)pg->num_open_fds);
  kl_s(&kl, " cur=");
  {
    struct process *cur = current_process();
    kl_i(&kl, cur ? (long)cur->pid : -1L);
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* SEC-6: invariant violated — an fd reached a slot whose type is not one of
 * the real file types.  UNINIT slots are unreachable through legitimate fds
 * (the fd only becomes visible after the creator sets the real type), so
 * hitting this means memory corruption or a new caller that publishes too
 * early: halt with a clear message rather than act on garbage fields. */
static void fs_slot_panic(const char *op) {
  uart_puts("KERNEL BUG (SEC-6): file slot used before its type was set (");
  uart_puts(op);
  uart_puts(")\n");
  for (;;)
    ;
}

#define FS_SLOT_CHECK(f)                              \
  do {                                                \
    if ((f)->type == FILE_TYPE_UNINIT)                \
      fs_slot_panic(__func__);                        \
  } while (0)

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
#define O_NOCTTY 0x100
#endif
/* GB-020: Linux open(2) flag for directories.  Gets routed to a directory-fd
 * (FILE_TYPE_FAT16DIR) instead of the regular file path; getdents64 reads it.
 * 0200000 octal = 0x10000, the Linux O_DIRECTORY value the compat sysroot uses. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif

/* ---- /proc/self/maps: a real rendering of this AS's region list ----------
 *
 * Mono opens /proc/self/maps at startup (measured on the host: 3 opens, then
 * 1024-byte reads until EOF, plus one access(F_OK); fstat reports S_IFREG|
 * 0444 with st_size 0 and the reader ignores the size).  The file is
 * therefore a STREAM, not a stored object: every line below is one region
 * this address space genuinely holds, rendered in Linux's
 *   start-end perms offset dev inode pathname
 * shape.  Nothing is invented: a region the AS does not have is not printed,
 * and no synthetic heap/stack/vdso rows are added.  The IMAGE region is
 * labelled with the executable path (the only mapping name the kernel
 * actually knows); DSO and anonymous regions print an empty pathname, which
 * is exactly how Linux renders an unnamed mapping.
 *
 * The read cursor is (region_index << 8) | byte_offset_in_line.  A rendered
 * line is bounded by two 16-digit hex numbers plus a 159-byte path, well
 * under 256, so 8 bits of line offset cannot overflow.
 */

static int fs_streq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) { a++; b++; }
  return *a == '\0' && *b == '\0';
}

static void fs_copy_path(char *dst, const char *src) {
  int i = 0;
  if (src)
    for (; src[i] && i < VFS_PATH_MAX - 1; i++) dst[i] = src[i];
  dst[i] = '\0';
}

static int maps_hex(char *out, uint64_t v) {
  char tmp[16];
  int n = 0;
  if (v == 0) { out[0] = '0'; return 1; }
  while (v && n < 16) { tmp[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  return n;
}

/* One rendered line for region `idx`.  Returns the length (always > 0). */
static int maps_line(struct process *p, struct addr_space *as, int idx,
                     char *out, int cap) {
  const struct vm_region *r = &as->regions[idx];
  int n = 0;
#define MPUT(ch) do { if (n < cap - 1) out[n++] = (ch); } while (0)
  n += maps_hex(out + n, r->base);
  MPUT('-');
  n += maps_hex(out + n, r->base + r->len);
  MPUT(' ');
  MPUT((r->prot & VM_PROT_READ) ? 'r' : '-');
  MPUT((r->prot & VM_PROT_WRITE) ? 'w' : '-');
  MPUT((r->prot & VM_PROT_EXEC) ? 'x' : '-');
  MPUT((r->flags & VM_MAP_SHARED) ? 's' : 'p');
  MPUT(' ');
  /* offset: 8 lowercase hex digits, zero-padded, like the kernel's %08llx.
     A VMK_LIB region carries the interpreter's own file offset (ld.so sets
     it); the kernel-loaded IMAGE is carved from the file start, so its
     segment offset is base - USER_IMG_BASE. */
  {
    uint64_t off = (r->kind == VMK_IMG) ? (r->base - USER_IMG_BASE) : r->obj_off;
    for (int sh = 28; sh >= 0; sh -= 4) MPUT("0123456789abcdef"[(off >> sh) & 0xF]);
  }
  MPUT(' ');
  MPUT('0'); MPUT('0'); MPUT(':'); MPUT('0'); MPUT('0');
  MPUT(' ');
  MPUT('0'); /* inode: this VFS has no inode numbers for mapped objects */
  if (r->kind == VMK_IMG && p && p->exe_path[0]) {
    MPUT(' ');
    for (int i = 0; p->exe_path[i] && n < cap - 2; i++) out[n++] = p->exe_path[i];
  }
  MPUT('\n');
#undef MPUT
  out[n] = '\0';
  return n;
}

static int file_read_maps(struct process *p, struct file *f, char *ubuf,
                          int size) {
  struct addr_space *as = p ? p->as : 0;
  if (!as || size <= 0) return 0;
  int used = 0;
  unsigned idx = f->maps_cursor >> 8;
  unsigned off = f->maps_cursor & 0xFF;
  char line[256];
  while (used < size) {
    if (idx >= (unsigned)as->nr) break;
    int n = maps_line(p, as, (int)idx, line, (int)sizeof line);
    if (off >= (unsigned)n) { idx++; off = 0; continue; }
    int chunk = n - (int)off;
    if (chunk > size - used) chunk = size - used;
    for (int k = 0; k < chunk; k++) ubuf[used + k] = line[off + (unsigned)k];
    used += chunk;
    off += (unsigned)chunk;
    if ((int)off >= n) { idx++; off = 0; }
  }
  f->maps_cursor = (idx << 8) | (off & 0xFF);
  return used;
}

int file_open(struct process *cur, const char *filename, int flags) {
  cur = process_group(cur); /* P1 (D7): fd table + cwd are group state */
  if (!cur || cur->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;

  /* M5c: a VFS bind may move this path onto another filesystem, so a native
   * absolute open() must see the same namespace a compat openat() does (that
   * path canonicalises through vfs_abs_path before calling in here).  Only an
   * actually-translated name is substituted; with no bind registered this is
   * one table-length check and `filename` is untouched. */
  char vpath[VFS_PATH_MAX];
  if (filename && vfs_bind_count() != 0) {
    char vabs[VFS_PATH_MAX];
    if (vfs_abs_path(filename, vabs, sizeof vabs) == 0 &&
        vfs_bind_translate(vabs, vpath, sizeof vpath) == 1)
      filename = vpath;
  }

  struct file *f = file_alloc();
  if (!f) return -EMFILE;
  f->open_flags = (uint32_t)flags;
  /* Remember the name this fd was opened under: /proc/self/fd/N is a link to
     it, and re-opening through that link must go back to the SAME file.  The
     VFS-resolved absolute form is what Linux prints. */
  f->path[0] = '\0';
  if (filename) {
    char absp[VFS_PATH_MAX];
    if (vfs_abs_path(filename, absp, sizeof absp) == 0)
      fs_copy_path(f->path, absp);
    else
      fs_copy_path(f->path, filename);
  }
  f->maps_cursor = 0;

  /* GB-017: /dev/null and /dev/urandom are virtual device nodes served by
   * the file layer (independent of GB-014's syscall-table routing, per the
   * coordinator's 2026-10-18 note).  /dev/null: write-sink, EOF-on-read.
   * /dev/urandom: entropy file backed by the F1.4 pool (net_get_random_bytes),
   * never a silently-short read.  O_WRONLY/O_RDONLY are not enforced on the
   * node itself; Linux tolerates either for these.  The kernel is
   * freestanding, so the equality check is byte-wise (no libc strcmp). */
  if (filename && filename[0] == '/' &&
      filename[1] == 'd' && filename[2] == 'e' && filename[3] == 'v') {
    const char *rest = filename + 4;
    int isnull = (rest[0] == '/' && rest[1] == 'n' && rest[2] == 'u' &&
                  rest[3] == 'l' && rest[4] == 'l' && rest[5] == '\0');
    int isurand = (rest[0] == '/' && rest[1] == 'u' && rest[2] == 'r' &&
                   rest[3] == 'a' && rest[4] == 'n' && rest[5] == 'd' &&
                   rest[6] == 'o' && rest[7] == 'm' && rest[8] == '\0');
    /* GB-025 / M3: /dev/fb0, the framebuffer device node (docs/linux-abi.md
       §9).  Every compat X11 client opens it in XOpenDisplay() and mmaps the
       scanout smem; SDL's fbcon driver does the same.  The node owns no
       data of its own -- the mmap/ioctl halves live in vm_mmap and the
       compat ioctl route. */
    int isfb = (rest[0] == '/' && rest[1] == 'f' && rest[2] == 'b' &&
                rest[3] == '0' && rest[4] == '\0');
    /* M2 stdio: /dev/console, the character device the loader installs on
       stdio.  Same shape as /dev/null on read/write; distinct type so fstat
       can answer S_IFCHR (Mono's FileStream ctor fstat()s each stdio fd). */
    int isconsole = (rest[0] == '/' && rest[1] == 'c' && rest[2] == 'o' &&
                     rest[3] == 'n' && rest[4] == 's' && rest[5] == 'o' &&
                     rest[6] == 'l' && rest[7] == 'e' && rest[8] == '\0');
    if (isnull) {
      f->type = FILE_TYPE_DEVNULL;
      f->open_flags = (uint32_t)flags;
    } else if (isurand) {
      f->type = FILE_TYPE_DEVURANDO;
      f->open_flags = (uint32_t)flags;
    } else if (isfb) {
      f->type = FILE_TYPE_DEVFB;
      f->open_flags = (uint32_t)flags;
    } else if (isconsole) {
      f->type = FILE_TYPE_CONSOLE;
      f->open_flags = (uint32_t)flags;
    } else {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -ENOENT;
    }
    int dfd = -1;
    dfd = fd_install(cur, f);
    if (dfd == -1) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -EMFILE;
    }
    return dfd;
  }

  /* M6 launch plumbing: /proc/self/maps -- a virtual 0444 regular file whose
     read() renders this process's region list (see file_read_maps).  Mono
     opens it at start-up; the host trace shows O_RDONLY|O_CLOEXEC, no seek,
     1024-byte reads to EOF. */
  if (filename && fs_streq(filename, "/proc/self/maps")) {
    if (flags & O_DIRECTORY) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -ENOTDIR;
    }
    f->type = FILE_TYPE_PROCMAPS;
    f->maps_cursor = 0;
    int mpfd = -1;
    mpfd = fd_install(cur, f);
    if (mpfd == -1) { f->type = FILE_TYPE_EMPTY; f->ref_count = 0; return -EMFILE; }
    return mpfd;
  }

  /* GB-020: virtual /proc/self/fd.  `ls /proc/self/fd` / sh readdir over it
     enumerate this process's own open fds.  Requires O_DIRECTORY (Linux
     opendir opens it O_RDONLY|O_DIRECTORY).  /proc/self resolves to the
     current process; named /proc/self (the only form an ls/sh-class binary
     reaches for in-tree). */
  if (filename && filename[0] == '/' && filename[1] == 'p' &&
      filename[2] == 'r' && filename[3] == 'o' && filename[4] == 'c' &&
      filename[5] == '/' && filename[6] == 's' && filename[7] == 'e' &&
      filename[8] == 'l' && filename[9] == 'f' && filename[10] == '/' &&
      filename[11] == 'f' && filename[12] == 'd' && filename[13] == '\0') {
    if (!(flags & O_DIRECTORY)) { f->type = FILE_TYPE_EMPTY; f->ref_count = 0; return -EINVAL; }
    f->type = FILE_TYPE_PROCFD;
    f->fat16.cursor = 0;
    int pdfd = -1;
    pdfd = fd_install(cur, f);
    if (pdfd == -1) { f->type = FILE_TYPE_EMPTY; f->ref_count = 0; return -EMFILE; }
    return pdfd;
  }

  /* GB-020: O_DIRECTORY opens a directory fd (getdents64).  Route it before
   * the regular FAT16 file path so a directory never reaches the file open
   * (which refuses dirs for boot-sector safety).  Missing target => ENOENT.
   *
   * L54: an NFS directory is a directory too.  This used to fall straight
   * into fat16_open_dir(), whose resolve fails for /NFS/... and returns
   * ENOENT — glibc/Mono turn that into DirectoryNotFoundException.  That is
   * exactly what killed Verse.ModLister.RebuildModList() the moment the
   * PLINQ stall was fixed.  Route NFS first, and only fall back to FAT16.
   *
   * The check runs whether or not O_DIRECTORY is set: Linux allows
   * open("/dir", O_RDONLY) and only fails read(2) with EISDIR, so a runtime
   * that opens the directory without the flag must get a directory fd too. */
  if (flags & O_DIRECTORY) {
    {
      int nmidx = -1;
      const char *nrel = 0;
      int routed = 0, lrc = -1, ltype = -1;
      if (f->path[0]) {
        routed = vfs_route(f->path, &nmidx, &nrel);
        if (routed && nrel) {
          const struct nfs_mount *nm = nfs_mount_at(nmidx);
          struct nfs_attr na;
          struct nfs_fh nfh;
          nfh.len = 0;
          if (nm) {
            lrc = nfs_lookup(nm, nrel, &nfh, &na);
            if (lrc == 0) ltype = (int)na.type;
          }
          /* One bounded row per O_DIRECTORY open.  "open() says ENOENT" is
             four different bugs (not routed, lookup failed, not a dir, fd
             table full) and they are indistinguishable from the caller. */
          {
            static int l54_rows;
            if (l54_rows < 8) {
              l54_rows++;
              struct kline kl;
              kl_init(&kl);
              kl_s(&kl, "[LNFSD] path=");
              kl_s(&kl, f->path);
              kl_s(&kl, " routed=");
              kl_i(&kl, routed);
              kl_s(&kl, " midx=");
              kl_i(&kl, nmidx);
              kl_s(&kl, " rel=");
              kl_s(&kl, nrel ? nrel : "(null)");
              kl_s(&kl, " lrc=");
              kl_i(&kl, lrc);
              kl_s(&kl, " type=");
              kl_i(&kl, ltype);
              kl_c(&kl, '\n');
              kl_emit(&kl);
            }
          }
          if (lrc == 0 && ltype == NF3DIR) {
            f->type = FILE_TYPE_NFSDIR;
            f->nfs.fh = nfh;
            f->nfs.size = 0;
            f->nfs.cursor = 0;          /* getdents resume index */
            f->nfs.is_dir = 1;
            f->nfs.mount_idx = nmidx;
            int ndfd = -1;
            ndfd = fd_install(cur, f);
            if (ndfd == -1) {
              f->type = FILE_TYPE_EMPTY;
              f->ref_count = 0;
              return -EMFILE;
            }
            return ndfd;
          }
        }
      }
    }
    if (fat16_open_dir(filename, f) != 0) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -ENOENT;
    }
    int dfd = -1;
    dfd = fd_install(cur, f);
    if (dfd == -1) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -EMFILE;
    }
    return dfd;
  }

  /* Paths under an NFS mount are opened by the NFS client; everything
   * else falls through to FAT-16. */
  int routed = vfs_open_routed(filename, f);
  if (routed < 0) {
    f->type = FILE_TYPE_EMPTY;
    f->ref_count = 0;
    return -ENOENT;
  }
  if (routed == 1 && f->nfs.is_dir) {
    /* A directory opened WITHOUT O_DIRECTORY.  Linux allows that open and
       fails read(2) with EISDIR; keep the fd a directory so getdents64 works
       and file_read refuses the data path.  No extra lookup: vfs_open_routed
       already fetched fattr3. */
    f->type = FILE_TYPE_NFSDIR;
    f->nfs.size = 0;
    f->nfs.cursor = 0;
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
    /* SEC-1: O_CREAT|O_EXCL must be decided atomically against concurrent
       creators, so it is enforced inside fat16_open_ex (one fat_lock
       critical section covers existence check + slot claim), not by this
       probe.  The probe above stays as the cheap ENOENT fast path only. */
    int orc = fat16_open_ex(filename, f,
                            (flags & O_CREAT) && (flags & O_EXCL) ? 1 : 0);
    if (orc == -2) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -EEXIST;
    }
    if (orc != 0) {
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -ENOENT;
    }
    if (exists && (flags & O_TRUNC)) {
      (void)fat16_truncate(f);
    }
  }

  if (routed == 1) nfs_selfcheck(f);
  int fd = -1;
  fd = fd_install(cur, f);

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
  fd = fd_install(cur, f);

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
  FS_SLOT_CHECK(f);
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
  if (type & ~(0xF | K_SOCK_CLOEXEC | K_SOCK_NONBLOCK)) return -EINVAL;
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
    fd = fd_install(p, f);
    if (fd == -1) {
      usock_close(u, 0);
      f->type = FILE_TYPE_EMPTY;
      f->ref_count = 0;
      return -EMFILE;
    }
    if (type & K_SOCK_CLOEXEC) fd_cloexec_set(p->fd_cloexec, fd);
    if (type & K_SOCK_NONBLOCK) usock_set_nonblock(u, 0, 1);
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
  fd = fd_install(p, f);

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

/* ---- P6.1 (browser.md section 6): advisory record locks -----------------
 *
 * A small fixed table of region locks over regular FAT16 files, serving
 * fcntl(F_GETLK/F_SETLK/F_SETLKW) -- the lock surface SQLite's unix-style
 * VFS consumes.  Semantics that matter for SQLite:
 *   - locks belong to the PROCESS: the owner is the process-group pid, so
 *     every thread and fd of one process shares them;
 *   - a process never conflicts with its own locks (an overlapping
 *     F_SETLK from the same owner replaces the owner's earlier entry);
 *   - closing an fd referring to the file drops that process's locks on
 *     it (POSIX), and a sweep drops entries whose owning process has
 *     exited (the fs_close_global teardown path carries no pid);
 *   - len == 0 means "to EOF", as in POSIX.
 *
 * Identity: the directory-entry location, unique per file on FAT16 --
 * stable across separate opens of one file and distinct for empty files
 * (whose start_cluster is 0 for all of them).  NFS/pipe/socket fds have no
 * record locks here (-EINVAL; SQLite runs off FAT16 for P6).
 *
 * The kernel never blocks on a lock: F_SETLKW behaves like F_SETLK (one
 * attempt, -EAGAIN on conflict).  The libc wrapper supplies the bounded
 * blocking loop with real sleeps -- the same shape as SQLite's own
 * non-blocking + busy-retry consumption.  Parking the syscall instead
 * would need the -2 restart plumbing select() uses, which buys nothing
 * here.
 */

static uint64_t file_lock_id(const struct file *f) {
  return ((uint64_t)f->fat16.dir_sector << 16) |
         ((uint64_t)f->fat16.dir_offset & 0xFFFFu);
}

/* User-ABI struct flock (LP64: 2 + 2 + 8 + 8 + 4 bytes + 4 pad = 32,
 * K_FLOCK_SIZE in fs.h).  Parsed byte-wise: never a wide load through a
 * user pointer. */
#define K_FLOCK_OFF_TYPE   0
#define K_FLOCK_OFF_WHENCE 2
#define K_FLOCK_OFF_START  8
#define K_FLOCK_OFF_LEN    16
#define K_FLOCK_OFF_PID    24

static int64_t fs_ld64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
  return (int64_t)v;
}

static void fs_st64(uint8_t *p, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(u >> (8 * i));
}

static void fs_st32(uint8_t *p, int v) {
  uint32_t u = (uint32_t)v;
  p[0] = (uint8_t)u;
  p[1] = (uint8_t)(u >> 8);
  p[2] = (uint8_t)(u >> 16);
  p[3] = (uint8_t)(u >> 24);
}

static int64_t file_lock_end(int64_t start, int64_t len) {
  if (len == 0) return 0x7FFFFFFFFFFFFFFFLL; /* to EOF */
  return start + len;                        /* len > 0 (caller checked) */
}

static int file_lock_overlap(int64_t a_start, int64_t a_len,
                             int64_t b_start, int64_t b_len) {
  int64_t a_end = file_lock_end(a_start, a_len);
  int64_t b_end = file_lock_end(b_start, b_len);
  return a_start < b_end && b_start < a_end;
}

/* First lock conflicting with (pid, range, type), or -1.  Same-pid locks
 * never conflict: a process's own locks merge/replace. */
static int file_lock_find_conflict(int pid, uint64_t id, int64_t start,
                                   int64_t len, int type) {
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    struct file_lock_entry *e = &file_locks[i];
    if (!e->used || e->file_id != id || e->pid == pid) continue;
    if (!file_lock_overlap(e->start, e->len, start, len)) continue;
    if (e->type == K_F_WRLCK || type == K_F_WRLCK) return i;
  }
  return -1;
}

/* Drop the owner's entries overlapping (start, len). */
static void file_lock_drop_own(int pid, uint64_t id, int64_t start,
                               int64_t len) {
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    struct file_lock_entry *e = &file_locks[i];
    if (!e->used || e->file_id != id || e->pid != pid) continue;
    if (!file_lock_overlap(e->start, e->len, start, len)) continue;
    e->used = 0;
  }
}

/* Drop entries whose owning process no longer exists (state read is a
 * benign race: a live owner is never FREE/EXITED at the same instant). */
static void file_locks_sweep_dead(void) {
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (!file_locks[i].used) continue;
    struct process *o = process_get_pcb(file_locks[i].pid);
    if (!o || o->state == PROC_STATE_FREE || o->state == PROC_STATE_EXITED)
      file_locks[i].used = 0;
  }
}

void file_locks_release(int pid, uint64_t file_id) {
  uint64_t flags = spinlock_acquire_irqsave(&file_locks_lock);
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].used && file_locks[i].pid == pid &&
        file_locks[i].file_id == file_id)
      file_locks[i].used = 0;
  }
  spinlock_release_irqrestore(&file_locks_lock, flags);
}

void file_locks_release_pid(int pid) {
  uint64_t flags = spinlock_acquire_irqsave(&file_locks_lock);
  for (int i = 0; i < MAX_FILE_LOCKS; i++) {
    if (file_locks[i].used && file_locks[i].pid == pid)
      file_locks[i].used = 0;
  }
  spinlock_release_irqrestore(&file_locks_lock, flags);
}

/* The F_GETLK/F_SETLK/F_SETLKW engine for one open FAT16 file.  `uarg` is a
 * user pointer whose range + alignment the trap layer has validated. */
static int file_lock_fcntl(struct process *pg, struct file *f, int cmd,
                           uint64_t uarg) {
  if (f->type != FILE_TYPE_FAT16) return -EINVAL;
  if (!uarg) return -EFAULT;
  const uint8_t *u = (const uint8_t *)uarg;
  int type = (int)(int16_t)((uint16_t)u[K_FLOCK_OFF_TYPE] |
                            ((uint16_t)u[K_FLOCK_OFF_TYPE + 1] << 8));
  int whence = (int)(int16_t)((uint16_t)u[K_FLOCK_OFF_WHENCE] |
                              ((uint16_t)u[K_FLOCK_OFF_WHENCE + 1] << 8));
  int64_t start = fs_ld64(u + K_FLOCK_OFF_START);
  int64_t len = fs_ld64(u + K_FLOCK_OFF_LEN);
  if (type != K_F_RDLCK && type != K_F_WRLCK && type != K_F_UNLCK)
    return -EINVAL;
  if (len < 0) return -EINVAL;

  /* Resolve whence against the open handle's cursor / size (SQLite uses
   * SEEK_SET; CUR/END are supported for completeness). */
  if (whence == 1) /* SEEK_CUR */
    start += (int64_t)f->fat16.cursor;
  else if (whence == 2) /* SEEK_END */
    start += (int64_t)f->fat16.entry.file_size;
  else if (whence != 0) /* SEEK_SET */
    return -EINVAL;
  if (start < 0) return -EINVAL;

  int pid = pg->pid;
  uint64_t id = file_lock_id(f);
  uint64_t flags = spinlock_acquire_irqsave(&file_locks_lock);
  file_locks_sweep_dead();

  if (cmd == K_F_GETLK) {
    int ci = file_lock_find_conflict(pid, id, start, len, type);
    int rtype = K_F_UNLCK;
    int64_t rstart = 0, rlen = 0;
    int rpid = 0;
    if (ci >= 0) {
      rtype = file_locks[ci].type;
      rstart = file_locks[ci].start;
      rlen = file_locks[ci].len;
      rpid = file_locks[ci].pid;
    }
    spinlock_release_irqrestore(&file_locks_lock, flags);
    uint8_t *w = (uint8_t *)uarg;
    w[K_FLOCK_OFF_TYPE] = (uint8_t)rtype;
    w[K_FLOCK_OFF_TYPE + 1] = 0;
    w[K_FLOCK_OFF_WHENCE] = 0;
    w[K_FLOCK_OFF_WHENCE + 1] = 0;
    fs_st64(w + K_FLOCK_OFF_START, rstart);
    fs_st64(w + K_FLOCK_OFF_LEN, rlen);
    fs_st32(w + K_FLOCK_OFF_PID, rpid);
    return 0;
  }

  /* F_SETLK / F_SETLKW: replace the owner's overlapping locks, then add the
   * new one unless this is an unlock.  Kernel-side this is one attempt for
   * both commands (see the block comment above). */
  if (type != K_F_UNLCK &&
      file_lock_find_conflict(pid, id, start, len, type) >= 0) {
    spinlock_release_irqrestore(&file_locks_lock, flags);
    return -EAGAIN;
  }
  file_lock_drop_own(pid, id, start, len);
  if (type != K_F_UNLCK) {
    int slot = -1;
    for (int i = 0; i < MAX_FILE_LOCKS; i++)
      if (!file_locks[i].used) {
        slot = i;
        break;
      }
    if (slot < 0) {
      spinlock_release_irqrestore(&file_locks_lock, flags);
      return -EAGAIN; /* table exhausted: transient contention */
    }
    file_locks[slot].used = 1;
    file_locks[slot].pid = pid;
    file_locks[slot].file_id = id;
    file_locks[slot].start = start;
    file_locks[slot].len = len;
    file_locks[slot].type = type;
  }
  spinlock_release_irqrestore(&file_locks_lock, flags);
  return 0;
}

/* SYS_FTRUNCATE (row 33): resize an open fd.  FAT16 gets cluster-granular
 * shrink + zero-fill extend from fat16_truncate_to; a memfd (S4, design
 * 4.3) sets its object size (SHRINK/GROW seals enforced). */
int file_ftruncate(struct process *p, int fd, int64_t length) {
  p = process_group(p);
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) return -EBADF;
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  if (f->type == FILE_TYPE_MEMFD) {
    if (length < 0 || (uint64_t)length > VM_OBJ_MAX_BYTES) return -EINVAL;
    return vm_object_truncate(f->memfd.obj, (uint64_t)length);
  }
  if (f->type != FILE_TYPE_FAT16) return -EINVAL;
  if (length < 0 || length > 0xFFFFFFFFLL) return -EINVAL;
  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  int r = fat16_truncate_to(f, (uint32_t)length);
  spinlock_release_irqrestore(&f->lock, flags);
  return r == 0 ? 0 : -EIO;
}

/* P4 (§6): F_GETFD/F_SETFD are handled on the fd_cloexec mask for every fd
 * type; F_GETFL/F_SETFL keep their socket-only behavior (now also for
 * AF_UNIX).  Non-socket fds keep today's ENOTSOCK for other commands. */
int file_fcntl(struct process *p, int fd, int cmd, uint64_t arg) {
  /* P2.4 (S4): memfd sealing (F_ADD_SEALS/F_GET_SEALS, Linux numbers). */
  if (cmd == K_F_ADD_SEALS || cmd == K_F_GET_SEALS)
    return file_memfd_seals(p, fd, cmd, (int)arg);
  struct process *pg = process_group(p);
  if (!pg) return -EINVAL;
  if (fd < 0 || fd >= MAX_OPEN_FDS) return -EBADF;
  if (cmd == K_F_GETFD) return fd_cloexec_test(pg->fd_cloexec, fd) ? K_FD_CLOEXEC : 0;
  if (cmd == K_F_SETFD) {
    /* Replaces the whole flag word (only FD_CLOEXEC exists). */
    if (arg & K_FD_CLOEXEC)
      fd_cloexec_set(pg->fd_cloexec, fd);
    else
      fd_cloexec_clear(pg->fd_cloexec, fd);
    return 0;
  }

  int g_fd = pg->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);

  /* P6.1: record-lock commands (FAT16 regular files). */
  if (cmd == K_F_GETLK || cmd == K_F_SETLK || cmd == K_F_SETLKW)
    return file_lock_fcntl(pg, f, cmd, arg);

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
  /* GB-019: a non-socket fd's F_GETFL returns the O_* flags it was opened with
     (Linux semantics; previously fell through to -ENOTSOCK, so a regular file
     or /dev/null answered -88 instead of its flags).  F_SETFL applies the
     status bits (O_NONBLOCK/O_APPEND) onto that stored word.
     GB-148: Linux's F_GETFL does NOT report the creation-only bits — open()
     strips O_CREAT|O_EXCL|O_NOCTTY|O_TRUNC from f_flags before it is stored.
     Returning them raw made a probe opened O_RDWR|O_CREAT answer 0x8042 where
     Linux answers 0x8002 (GB-108's fcntl_getfl rows). */
  switch (cmd) {
  case K_F_GETFL:
    return (int)(f->open_flags &
                 ~(uint32_t)(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC));
  case K_F_SETFL:
    f->open_flags = (f->open_flags & ~(uint32_t)(K_O_NONBLOCK | K_O_APPEND)) |
                    ((uint32_t)arg & (uint32_t)(K_O_NONBLOCK | K_O_APPEND));
    return 0;
  default:
    return -EINVAL;
  }
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
  FS_SLOT_CHECK(f);

  if (f->type == FILE_TYPE_TIMERFD) {
    extern int lt_timerfd_pending(int slot);
    if (lt_timerfd_pending(f->timerfd.slot)) *r = 1;
    return 0;
  }

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
        if (optname == K_SO_PEERCRED) {
          /* IceWM M1 (4.1): struct ucred, zeroed (single-user OS: uid/gid
           * 0; pid 0 = "unknown"). */
          if (*len < K_UCRED_SIZE) return -EINVAL;
          for (int i = 0; i < K_UCRED_SIZE; i++) ((uint8_t *)val)[i] = 0;
          *len = K_UCRED_SIZE;
          return 0;
        }
        if (*len < (int)sizeof(int)) return -EINVAL;
        int value;
        switch (optname) {
        case K_SO_ERROR:
          value = 0;
          break;
        case K_SO_SNDBUF: /* IceWM M1: the per-direction ring */
        case K_SO_RCVBUF:
          value = K_UNIX_RING;
          break;
        case K_SO_ACCEPTCONN:
          value = usock_is_listening(uf->usock.ptr) ? 1 : 0;
          break;
        case K_SO_PASSCRED:
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
  /* IceWM M1: unix endpoints accept the buffer/credential knobs X
   * transports set (Xtrans raises SO_SNDBUF) as no-ops. */
  {
    struct process *pg = process_group(p);
    if (pg && fd >= 0 && fd < MAX_OPEN_FDS) {
      int g = pg->open_fds[fd];
      if (g >= 0 && g < MAX_GLOBAL_FILES &&
          global_file_table[g].type == FILE_TYPE_UNIXSOCK) {
        if (level != K_SOL_SOCKET) return -ENOPROTOOPT;
        switch (optname) {
        case K_SO_SNDBUF:
        case K_SO_RCVBUF:
        case K_SO_PASSCRED:
        case K_SO_REUSEADDR:
        case K_SO_KEEPALIVE:
          return 0;
        default:
          return -ENOPROTOOPT;
        }
      }
    }
  }
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
  FS_SLOT_CHECK(f);

  int64_t base, size;
  if (f->type == FILE_TYPE_FAT16) {
    base = f->fat16.cursor;
    size = f->fat16.entry.file_size;
  } else if (f->type == FILE_TYPE_NFS) {
    base = f->nfs.cursor;
    size = f->nfs.size;
  } else if (f->type == FILE_TYPE_FAT16DIR || f->type == FILE_TYPE_PROCFD) {
    /* L54: Linux allows lseek on a directory fd (SEEK_SET/CUR move the
       getdents resume point, SEEK_END is 0).  .NET's enumerator probes the
       directory with lseek; answering EINVAL made DirectoryInfo.GetDirectories
       throw IOException("Invalid argument") on /NFS/Data. */
    base = f->fat16.cursor;
    size = 0;
  } else if (f->type == FILE_TYPE_NFSDIR) {
    base = f->nfs.cursor;
    size = 0;
  } else {
    *errp = (f->type == FILE_TYPE_PIPE || f->type == FILE_TYPE_UNIXSOCK ||
             f->type == FILE_TYPE_CONSOLE || f->type == FILE_TYPE_DEVNULL ||
             f->type == FILE_TYPE_DEVFB)
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
  if (f->type == FILE_TYPE_FAT16)
    f->fat16.cursor = (uint32_t)newpos;
  else if (f->type == FILE_TYPE_FAT16DIR || f->type == FILE_TYPE_PROCFD)
    f->fat16.cursor = (uint32_t)newpos;
  else
    f->nfs.cursor = (uint32_t)newpos;
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

/* Volume identity.  Linux's (st_dev, st_ino) pair is what a loader keys a
 * "is this object already mapped?" lookup on: glibc's
 * _dl_map_object_from_fd() compares l_ino/l_dev against fstat() and returns
 * the EXISTING link map when both match, so two distinct files that report
 * the same pair become ONE handle.  Reporting st_dev = 0 for every file type
 * (the old behaviour) left the pair keyed on the synthetic inode alone, so a
 * FAT16 file and an NFS file whose hashes collided — or any two files the
 * inode synthesis could not separate — silently shared a handle.  Give each
 * volume its own device number, exactly as Linux does for two mounts. */
#define K_DEV_FAT16 0x00000101u
#define K_DEV_NFS 0x00000202u
#define K_DEV_PIPE 0x00000303u
#define K_DEV_SOCK 0x00000404u
#define K_DEV_PROCFS 0x00000505u
#define K_DEV_DEVFS 0x00000606u

static void k_stat_fill(struct k_stat *st, unsigned long dev,
                        unsigned long ino, unsigned long mode, long size) {
  st->st_dev = dev;
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
  FS_SLOT_CHECK(f);
  switch (f->type) {
  case FILE_TYPE_FAT16: {
    k_stat_fill(st, K_DEV_FAT16,
                fat16_synth_ino(f->fat16.dir_sector, f->fat16.dir_offset),
                (f->fat16.entry.attr & 0x10)
                    ? (K_S_IFDIR | 0755) : (K_S_IFREG | 0644),
                f->fat16.entry.file_size);
    /* w10-fstime: report the dirent's real RTC timestamp (UTC).  FAT has
       no atime; use the modify time for all three (matches sysinfo/time). */
    long ts = (long)fat16_entry_epoch(&f->fat16.entry);
    st->st_atime = st->st_mtime = st->st_ctime = ts;
    return 0;
  }
  case FILE_TYPE_NFS:
  case FILE_TYPE_NFSDIR:
    k_stat_fill(st, K_DEV_NFS, nfs_synth_ino(&f->nfs.fh),
                (f->type == FILE_TYPE_NFSDIR || f->nfs.is_dir)
                    ? (K_S_IFDIR | 0755) : (K_S_IFREG | 0644),
                f->nfs.size);
    return 0;
  /* Anonymous objects: Linux gives each pipe/socket its own inode on its own
     pseudo-filesystem.  Deriving the inode from the global slot keeps two
     distinct pipes from reporting the same (dev, ino) pair — the property a
     caller comparing two fds relies on. */
  case FILE_TYPE_PIPE:
    k_stat_fill(st, K_DEV_PIPE, (unsigned long)g_fd + 1, K_S_IFIFO | 0600, 0);
    return 0;
  case FILE_TYPE_SOCKET:
    k_stat_fill(st, K_DEV_SOCK, (unsigned long)g_fd + 1, K_S_IFSOCK | 0600, 0);
    return 0;
  case FILE_TYPE_UNIXSOCK: /* P4: F_GETFD-probing tools stat caches use this */
    k_stat_fill(st, K_DEV_SOCK, (unsigned long)g_fd + 1, K_S_IFSOCK | 0600, 0);
    return 0;
  case FILE_TYPE_FAT16DIR: /* GB-020: a directory fd stats as a directory */
    k_stat_fill(st, K_DEV_FAT16,
                fat16_synth_ino(f->fat16.dir_sector, f->fat16.dir_offset),
                K_S_IFDIR | 0755, 0);
    return 0;
  case FILE_TYPE_PROCMAPS:
    /* Linux reports the maps file as a 0444 regular file with st_size 0
       (measured host-side); readers must not size-gate it. */
    k_stat_fill(st, K_DEV_PROCFS, (unsigned long)g_fd + 1, K_S_IFREG | 0444, 0);
    return 0;
  case FILE_TYPE_CONSOLE:
    /* M2 stdio: a character device, 0644, size 0.  Mono's FileStream ctor
       fstat()s each stdio handle and refuses anything that is not a real
       file object (ArgumentException "handle"/"Invalid"); a char device is
       exactly what Linux reports for a redirected-console fd. */
    k_stat_fill(st, K_DEV_DEVFS, 1, K_S_IFCHR | 0644, 0);
    return 0;
  case FILE_TYPE_DEVNULL:
    k_stat_fill(st, K_DEV_DEVFS, 2, K_S_IFCHR | 0666, 0);
    return 0;
  case FILE_TYPE_DEVURANDO:
    k_stat_fill(st, K_DEV_DEVFS, 3, K_S_IFCHR | 0666, 0);
    return 0;
  case FILE_TYPE_DEVFB:
    k_stat_fill(st, K_DEV_DEVFS, 4, K_S_IFCHR | 0644, 0);
    return 0;
  default:
    *errp = EBADF;
    return -1;
  }
}

/* M6 launch plumbing: the name an open fd was opened under.  This is what
 * readlink("/proc/self/fd/N") prints and what opening that link re-opens. */
int file_path_of(struct process *p, int fd, char *out, int cap) {
  p = process_group(p);
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS || !out || cap <= 0) return -EBADF;
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  if (f->type == FILE_TYPE_EMPTY) return -EBADF;
  if (!f->path[0]) return -ENOENT;
  int i = 0;
  for (; f->path[i] && i < cap - 1; i++) out[i] = f->path[i];
  out[i] = '\0';
  return 0;
}

/* GB-020: getdents64 over an open directory fd (FILE_TYPE_FAT16DIR or
 * the virtual FILE_TYPE_PROCFD self-fd directory). */
int file_getdents64(struct process *p, int fd, void *ubuf, int bufsize) {
  p = process_group(p); /* P1 (D7): the fd table is group state */
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) return -EBADF;
  int g_fd = p->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  if (f->type != FILE_TYPE_FAT16DIR && f->type != FILE_TYPE_PROCFD &&
      f->type != FILE_TYPE_NFSDIR)
    return -ENOTDIR;
  if (!ubuf || bufsize <= 0) return -EINVAL;

  /* L54: NFS directory enumeration.  nfs_list_dir() owns a one-directory
     cache and READDIRPLUS parsing; this loop only packs its entries into
     Linux dirent64 records.  The resume index lives in f->nfs.cursor, the
     same role f->fat16.cursor plays for a FAT directory. */
  if (f->type == FILE_TYPE_NFSDIR) {
    int midx = -1;
    const char *rel = 0;
    if (!f->path[0] || !vfs_route(f->path, &midx, &rel)) return -EIO;
    const struct nfs_mount *m = nfs_mount_at(midx);
    if (!m) return -EIO;
    /* End of directory must be 0, not -EIO: nfs_list_dir() uses -1 for both
       "past the end" and "RPC error", and .NET's FileSystemEnumerator reads
       once more after the last entry.  Ask for the count explicitly. */
    int total = nfs_dir_entries(m, rel);
    if (total < 0) return -EIO;
    if ((long)f->nfs.cursor >= total) return 0;
    int used = 0;
    uint32_t idx = f->nfs.cursor;
    uint32_t start_idx = idx;
    int first_rc = 7777;
    for (;;) {
      char nm[NFS_NAME_MAX];
      uint8_t attr = 0;
      uint32_t sz = 0;
      int grc = nfs_list_dir(m, rel, (int)idx, nm, sizeof nm, &attr, &sz);
      if (idx == start_idx) first_rc = grc;
      if (grc != 0)
        break;
      int nlen = 0;
      while (nm[nlen]) nlen++;
      int reclen = (19 + nlen + 1 + 7) & ~7;
      if (used + reclen > bufsize) break;
      uint8_t *pb = (uint8_t *)ubuf + used;
      uint64_t ino = 0x4E4600000000ULL + (uint64_t)idx + 1;
      uint64_t off = (uint64_t)idx + 1;
      for (int k = 0; k < 8; k++) pb[k] = (uint8_t)(ino >> (k * 8));
      for (int k = 0; k < 8; k++) pb[8 + k] = (uint8_t)(off >> (k * 8));
      pb[16] = (uint8_t)(reclen & 0xFF);
      pb[17] = (uint8_t)((reclen >> 8) & 0xFF);
      pb[18] = (attr & 0x10) ? 4 : 8;   /* DT_DIR / DT_REG */
      for (int n = 0; n < nlen; n++) pb[19 + n] = (uint8_t)nm[n];
      pb[19 + nlen] = 0;
      used += reclen;
      idx++;
    }
    f->nfs.cursor = idx;
    {
      static int lgd_rows;
      if (lgd_rows < 4) {
        lgd_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[LGD] path=");
        kl_s(&kl, f->path);
        kl_s(&kl, " rel=");
        kl_s(&kl, rel);
        kl_s(&kl, " from=");
        kl_i(&kl, (long)start_idx);
        kl_s(&kl, " first_rc=");
        kl_i(&kl, first_rc);
        kl_s(&kl, " n=");
        kl_i(&kl, (long)(idx - start_idx));
        kl_s(&kl, " used=");
        kl_i(&kl, used);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    }
    return used > 0 ? used : -EIO;
  }

  if (f->type == FILE_TYPE_PROCFD) {
    /* /proc/<pid>/fd: list this process's open user fds (0..n).  Reuses the
       same dirent64 record layout the FAT16 path emits. */
    int used = 0;
    int from = f->fat16.cursor; /* resume index (an fd number) */
    for (int q = from; q < MAX_OPEN_FDS && used < bufsize; q++) {
      if (p->open_fds[q] == -1)
        continue;
      /* entry name = decimal fd */
      char nm[16];
      int nl = 0, v = q;
      if (v == 0) { nm[nl++] = '0'; }
      else { char tmp[16]; int ti = 0; while (v > 0 && ti < 14) { tmp[ti++] = '0' + (v % 10); v /= 10; } while (ti > 0) nm[nl++] = tmp[--ti]; }
      int reclen = (19 + nl + 1 + 7) & ~7;
      if (used + reclen > bufsize) break;
      uint8_t *pb = (uint8_t *)ubuf + used;
      uint64_t ino = 4096u + (uint64_t)(q + 1);
      uint64_t off = (uint64_t)(q + 1);
      for (int k = 0; k < 8; k++) pb[k] = (uint8_t)(ino >> (k * 8));
      for (int k = 0; k < 8; k++) pb[8 + k] = (uint8_t)(off >> (k * 8));
      pb[16] = (uint8_t)(reclen & 0xFF); pb[17] = (uint8_t)((reclen >> 8) & 0xFF);
      pb[18] = 8; /* DT_LNK -> DT_REG-ish; ls renders fine either way */
      for (int n = 0; n < nl; n++) pb[19 + n] = (uint8_t)nm[n];
      pb[19 + nl] = 0;
      used += reclen;
      f->fat16.cursor = q + 1;
    }
    return used;
  }

  int rc = fat16_getdents64(f, f->fat16.cursor, ubuf, bufsize);
  if (rc < 0) return -EIO;
  return rc;
}

int file_stat_path(struct process *p, const char *path, struct k_stat *st,
                   int *errp) {
  if (!path) { *errp = EINVAL; return -1; }
  /* M6 launch plumbing: the virtual maps file exists for access()/stat()
     (the host trace shows access("/proc/self/maps", F_OK) before the open). */
  if (fs_streq(path, "/proc/self/maps")) {
    k_stat_fill(st, K_DEV_PROCFS, 1, K_S_IFREG | 0444, 0);
    return 0;
  }
  /* M2 stdio: the device nodes exist to stat()/access() too, not only to
     open().  Linux reports /dev/console and /dev/null as char devices. */
  if (fs_streq(path, "/dev/console") || fs_streq(path, "/dev/null")) {
    k_stat_fill(st, K_DEV_DEVFS, fs_streq(path, "/dev/null") ? 2 : 1,
                K_S_IFCHR | 0644, 0);
    return 0;
  }
  /* AD-9 (M6): Unity resolves its executable with
       realpath("/proc/self/exe", buf)
     and glibc's realpath lstat()s every component.  Without these entries it
     fails at "/proc" with ENOENT and the player prints
       "Error 2 encountered while resolving executable path".
     /proc/self is presented as a directory (a procfs could; the kernel has no
     per-pid directory to point a self->pid link at yet) and the links below it
     are symlinks whose targets readlink() already answers. */
  if (fs_streq(path, "/proc") || fs_streq(path, "/proc/self") ||
      fs_streq(path, "/proc/self/fd")) {
    k_stat_fill(st, K_DEV_PROCFS, 2, K_S_IFDIR | 0555, 0);
    return 0;
  }
  if (fs_streq(path, "/proc/self/exe") || fs_streq(path, "/proc/self/cwd") ||
      fs_streq(path, "/proc/self/root")) {
    struct process *g = p ? process_group(p) : 0;
    const char *tgt = "/";
    if (fs_streq(path, "/proc/self/exe"))
      tgt = (g && g->exe_path[0]) ? g->exe_path : "/";
    else if (fs_streq(path, "/proc/self/cwd"))
      tgt = (g && g->cwd[0]) ? g->cwd : "/";
    long n = 0;
    while (tgt[n]) n++;
    k_stat_fill(st, K_DEV_PROCFS, 3, K_S_IFLNK | 0777, n);
    return 0;
  }
  char abs[256];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) { *errp = EINVAL; return -1; }
  /* Paths under an NFS mount: answer from the server's fattr3.  Before this,
     stat()/lstat() of anything on /NFS was ENOENT because only FAT16 was
     consulted — realpath() of the launcher's own path needs it (AD-9). */
  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) {
    const struct nfs_mount *m = nfs_mount_at(midx);
    struct nfs_attr a;
    struct nfs_fh fh;
    fh.len = 0;
    if (!m || nfs_lookup(m, rel, &fh, &a) != 0) {
      *errp = ENOENT;
      return -1;
    }
    /* The file handle is the server-side identity, and file_stat_fd() folds
       the SAME handle for the same file — so stat(path) and fstat(fd) agree.
       Leaving this at 0 made every NFS file report (dev=0, ino=0), which is
       exactly the pair glibc's dlopen uses to decide "already loaded": two
       different NFS DSOs answered the same link map. */
    k_stat_fill(st, K_DEV_NFS, nfs_synth_ino(&fh),
                (a.type == NF3DIR) ? (K_S_IFDIR | 0755)
                                   : (K_S_IFREG | 0644),
                (long)a.size);
    /* M6/L31: the engine's JSON probe, as stat() sees it. */
    {
      const char *t = "ScriptingAssemblies.json";
      int tl = 0, pl = 0, hit = 1;
      while (t[tl]) tl++;
      while (abs[pl]) pl++;
      if (pl < tl) hit = 0;
      for (int i = 0; hit && i < tl; i++)
        if (abs[pl - tl + i] != t[i]) hit = 0;
      if (hit) {
        struct kline kl; kl_init(&kl);
        kl_s(&kl, "[XSTAT] size="); kl_i(&kl, (long)a.size);
        kl_s(&kl, " type="); kl_i(&kl, (long)a.type);
        kl_c(&kl, '\n'); kl_emit(&kl);
      }
    }
    st->st_atime = st->st_mtime = st->st_ctime = (long)a.mtime;
    return 0;
  }
  struct fat16_dir_entry entry;
  uint32_t sector = 0, offset = 0;
  if (fat16_resolve_path(abs, &entry, &sector, &offset) != 0) {
    *errp = ENOENT;
    return -1;
  }
  k_stat_fill(st, K_DEV_FAT16, fat16_synth_ino(sector, offset),
              (entry.attr & 0x10) ? (K_S_IFDIR | 0755) : (K_S_IFREG | 0644),
              entry.file_size);
  /* w10-fstime: real RTC timestamp from the dirent (UTC; no FAT atime). */
  long ts = (long)fat16_entry_epoch(&entry);
  st->st_atime = st->st_mtime = st->st_ctime = ts;
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

  /* SEC-9 (security-plan.md #9): claim the fd slot atomically before
   * touching anything.  The old form read open_fds[fd], dropped the ref
   * under f->lock, and only then wrote -1 — so two threads sharing the
   * group (a sibling's close(), a racing teardown in process_kill_group_sig
   * or group_teardown) could both snapshot the same g_fd and both run the
   * whole release: double pipe_close/usock_close, ref_count driven below
   * the true count, slot freed while other fds still referenced it.  The
   * claim IS the "closing" flag the plan suggests: whoever swaps the slot
   * to -1 under proc_lock owns the single release; every later close of
   * that fd is a plain EBADF no-op (idempotent by construction). */
  int g_fd;
  {
    uint64_t cflags = spinlock_acquire_irqsave(&proc_lock);
    g_fd = cur->open_fds[fd];
    if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) {
      spinlock_release_irqrestore(&proc_lock, cflags);
      return -1;
    }
    cur->open_fds[fd] = -1;
    fd_cloexec_clear(cur->fd_cloexec, fd); /* P5 (D3.2): closing discards fd flags */
    if (cur->num_open_fds > 0)
      cur->num_open_fds--;
    spinlock_release_irqrestore(&proc_lock, cflags);
  }

  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  /* L57: who closed which fd, and which file description it named. */
  fd_ev_note(FDEV_CLOSE, cur, fd, g_fd, f->path);

  /* P6.1: capture the record-lock identity before the type may be cleared;
   * closing ANY fd of a FAT16 file drops this process's locks on it. */
  int lock_release = (f->type == FILE_TYPE_FAT16);
  uint64_t lock_id = lock_release ? file_lock_id(f) : 0;

  /* SEC-OBS-B (security-plan.md observation B): pipe/usock close wakes
   * blocked peers, which takes proc_lock — and the documented order is
   * proc -> file, so those calls must NOT run under f->lock.  Capture the
   * backend handles here, release them below once the slot lock is gone.
   * (Both backends are statically pooled with their own locks and per-ref
   * accounting, so dropping a reference just after unlock is safe.) */
  struct pipe *pp = 0;
  struct usock *uu = 0;
  int bend = 0;

  uint64_t flags = spinlock_acquire_irqsave(&f->lock);

  if (f->type == FILE_TYPE_PIPE) {
    pp = f->pipe.ptr;
    bend = f->pipe.end;
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    uu = f->usock.ptr; /* P4: release this end's ref */
    bend = f->usock.end;
  }

  f->ref_count--;
  if (f->type == FILE_TYPE_MEMFD)
    vm_object_unref(f->memfd.obj); /* one instance ref goes away (S4) */
  if (f->ref_count == 0) {
    if (f->type == FILE_TYPE_TIMERFD) {
      extern void lt_timerfd_release(int slot);
      lt_timerfd_release(f->timerfd.slot);
    } else if (f->type == FILE_TYPE_EPOLL) {
      extern void lt_epoll_release(int slot);
      lt_epoll_release(f->epollfd.slot);
    } else if (f->type == FILE_TYPE_FAT16) {
      fat16_close(f);
    } else if (f->type == FILE_TYPE_SOCKET) {
      net_socket_close(f->socket.pcb);
    }
    /* FILE_TYPE_NFS keeps no backend state: nothing to release. */
    f->type = FILE_TYPE_EMPTY;
  }
  spinlock_release_irqrestore(&f->lock, flags);

  if (pp) pipe_close(pp, bend);
  else if (uu) usock_close(uu, bend);

  if (lock_release) file_locks_release(cur->pid, lock_id);

  return 0;
}

/* dup(fd): the lowest unused user fd now refers to the same open file. */
int file_dup(struct process *cur, int fd) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -EBADF;
  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return -EBADF;
  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  if (f->type == FILE_TYPE_EMPTY) return -EBADF;
  if (cur->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;
  int newfd = -1;
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++) {
    if (cur->open_fds[i] == -1) {
      newfd = i;
      break;
    }
  }
  if (newfd < 0) return -EMFILE;
  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  f->ref_count++;
  if (f->type == FILE_TYPE_MEMFD)
    vm_object_ref(f->memfd.obj); /* one more instance ref (S4) */
  if (f->type == FILE_TYPE_PIPE)
    pipe_reopen(f->pipe.ptr, f->pipe.end); /* keep the pipe end's fd count
        in sync: file_close calls pipe_close per fd, so every dup must too,
        or closing the duplicate makes readers see EOF while refs remain */
  else if (f->type == FILE_TYPE_UNIXSOCK)
    usock_reopen(f->usock.ptr, f->usock.end); /* P4: same ref discipline */
  spinlock_release_irqrestore(&f->lock, flags);
  cur->open_fds[newfd] = g_fd;
  fd_cloexec_clear(cur->fd_cloexec, newfd); /* P5 (D3.2): dup clears CLOEXEC */
  cur->num_open_fds++;
  fd_ev_note(FDEV_DUP, cur, newfd, g_fd, f->path);
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
  FS_SLOT_CHECK(f);
  if (f->type == FILE_TYPE_EMPTY) return -EBADF;
  if (oldfd == newfd) return newfd;
  if (newfd < 0 || newfd >= MAX_OPEN_FDS) return -EINVAL;
  if (cur->open_fds[newfd] != -1) {
    int r = file_close(cur, newfd);
    if (r != 0) return -EBADF;
  }
  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  f->ref_count++;
  if (f->type == FILE_TYPE_MEMFD)
    vm_object_ref(f->memfd.obj); /* one more instance ref (S4) */
  if (f->type == FILE_TYPE_PIPE)
    pipe_reopen(f->pipe.ptr, f->pipe.end); /* see file_dup: keep the pipe
        end's per-fd count aligned with ref_count */
  else if (f->type == FILE_TYPE_UNIXSOCK)
    usock_reopen(f->usock.ptr, f->usock.end); /* P4 */
  spinlock_release_irqrestore(&f->lock, flags);
  cur->open_fds[newfd] = g_fd;
  fd_cloexec_clear(cur->fd_cloexec, newfd); /* P5 (D3.2): dup2 clears CLOEXEC */
  fd_ev_note(FDEV_DUP2, cur, newfd, g_fd, f->path);
  cur->num_open_fds++;
  return newfd;
}

void fs_close_global(int g_fd) {
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) return;

  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);

  /* SEC-OBS-B: see file_close — backend close wakes peers (proc_lock), so
   * it runs after f->lock drops, never under it. */
  struct pipe *pp = 0;
  struct usock *uu = 0;
  int bend = 0;

  uint64_t flags = spinlock_acquire_irqsave(&f->lock);
  if (f->type == FILE_TYPE_EMPTY) {
    spinlock_release_irqrestore(&f->lock, flags);
    return;
  }

  if (f->type == FILE_TYPE_PIPE) {
    pp = f->pipe.ptr;
    bend = f->pipe.end;
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    uu = f->usock.ptr; /* P4 */
    bend = f->usock.end;
  }

  f->ref_count--;
  if (f->type == FILE_TYPE_MEMFD)
    vm_object_unref(f->memfd.obj); /* one instance ref goes away (S4) */
  if (f->ref_count == 0) {
    if (f->type == FILE_TYPE_FAT16) {
      fat16_close(f);
    } else if (f->type == FILE_TYPE_SOCKET) {
      net_socket_close(f->socket.pcb);
    }
    f->type = FILE_TYPE_EMPTY;
  }
  spinlock_release_irqrestore(&f->lock, flags);

  if (pp) pipe_close(pp, bend);
  else if (uu) usock_close(uu, bend);
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
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) {
    fd_ev_dump(cur, fd, "nofd");
    return -1;
  }

  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) {
    /* L57: the slot was empty/invalid at read time -- this IS a closed fd. */
    fd_ev_dump(cur, fd, "slot-empty");
    return -1;
  }

  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  if (f->type == FILE_TYPE_EMPTY) {
    fd_ev_dump(cur, fd, "type-empty");
    return -1;
  }
  if (f->type == FILE_TYPE_DEVNULL) {
    return 0; /* /dev/null reads always yield EOF (0 bytes) */
  } else if (f->type == FILE_TYPE_CONSOLE) {
    return 0; /* /dev/console has no input source: EOF, like `< /dev/null` */
  } else if (f->type == FILE_TYPE_TIMERFD) {
    /* L49: an 8-byte u64 of accumulated expirations, drained by the read.
       With none pending the read parks until the timer's next expiry (the
       same timed-block contract pipe reads use) or fails EAGAIN when the fd
       was created/opened non-blocking. */
    extern int lt_timerfd_take(int slot, uint64_t *out);
    extern uint64_t lt_timerfd_next(int slot);
    if (size < 8) return -EINVAL;
    uint64_t n = 0;
    int r = lt_timerfd_take(f->timerfd.slot, &n);
    if (r == -EAGAIN) {
      if (f->open_flags & 0x800 /* O_NONBLOCK */) return -EAGAIN;
      extern uint64_t timer_get_ms(void);
      uint64_t next = lt_timerfd_next(f->timerfd.slot);
      uint64_t now = timer_get_ms();
      uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
      cur->wake_ms = next && next > now ? next : now + 50;
      cur->state = PROC_STATE_BLOCKED;
      spinlock_release_irqrestore(&proc_lock, flags);
      return -2;
    }
    if (r != 0) return r;
    /* buf is the caller's user pointer, already range-checked by sys_read. */
    {
      unsigned char *pb = (unsigned char *)buf;
      for (int k = 0; k < 8; k++) pb[k] = (unsigned char)((n >> (k * 8)) & 0xFF);
    }
    return 8;
  } else if (f->type == FILE_TYPE_PROCMAPS) {
    return file_read_maps(cur, f, (char *)buf, size);
  } else if (f->type == FILE_TYPE_DEVURANDO) {
    if (size <= 0) return 0;
    return net_get_random_bytes(buf, (uint32_t)size); /* never short */
  } else if (f->type == FILE_TYPE_FAT16) {
    return fat16_read(f, buf, size);
  } else if (f->type == FILE_TYPE_NFS) {
    if (f->nfs.is_dir) return -1;               /* use read_dir for dirs */
    if (size <= 0) return 0;
    const struct nfs_mount *m = nfs_mount_at(f->nfs.mount_idx);
    if (!m) {
      fd_ev_dump(cur, fd, "nfs-nomount");
      return -EIO;   /* L57: an I/O failure is not EBADF */
    }
    uint64_t left = f->nfs.size > f->nfs.cursor
                        ? f->nfs.size - f->nfs.cursor : 0;
    uint32_t want = (uint32_t)size;
    if ((uint64_t)want > left) want = (uint32_t)left;
    if (want == 0) return 0;
    int got = nfs_read_file(m, &f->nfs.fh, f->nfs.cursor, buf, want);
    if (got < 0) {
      /* L57: the fd is ALIVE here (we just resolved it, and its type is NFS);
         the failure is the transport's.  Reporting -1 made trap.c turn it
         into EBADF, which Mono renders as "Invalid handle to path" -- a
         transport failure wearing a descriptor fault's clothes. */
      fd_ev_note(FDEV_FAIL, cur, fd, g_fd, f->path);
      fd_ev_dump(cur, fd, "nfs-read");
      return -EIO;
    }
    /* M6/L31: the ScriptingAssemblies.json read, one row per call. */
    {
      const char *t = "ScriptingAssemblies.json";
      int hit = 1, tl = 0, pl = 0;
      while (t[tl]) tl++;
      while (f->path[pl]) pl++;
      if (pl < tl) hit = 0;
      for (int i = 0; hit && i < tl; i++)
        if (f->path[pl - tl + i] != t[i]) hit = 0;
      if (hit) {
        struct kline kl; kl_init(&kl);
        kl_s(&kl, "[XREAD] want="); kl_i(&kl, (long)want);
        kl_s(&kl, " got="); kl_i(&kl, (long)got);
        kl_s(&kl, " cur="); kl_i(&kl, (long)f->nfs.cursor);
        kl_s(&kl, " size="); kl_i(&kl, (long)f->nfs.size);
        kl_c(&kl, '\n'); kl_emit(&kl);
      }
    }
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
  fd_ev_dump(cur, fd, "type-unsupported");
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
  FS_SLOT_CHECK(f);
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
  } else if (f->type == FILE_TYPE_UNIXSOCK) {
    /* IceWM M1: FIONREAD on a unix socket (Xtrans BytesReadable). */
    int a = usock_available(f->usock.ptr, f->usock.end);
    return a < 0 ? 0 : a;
  } else if (f->type == FILE_TYPE_CONSOLE || f->type == FILE_TYPE_DEVNULL) {
    return 0; /* no input source: always at EOF */
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
/* WK-1 M3: length-bounded console write from user space.
 * SYS_WRITE_CONSOLE historically NUL-terminated its output (uart_puts) and
 * returned 0 — unsafe for size-bounded stdio/libc++ writes (an overrun past
 * a short buffer into an unmapped page faults the kernel) and broken for the
 * write-count contract (fwrite stops after one chunk).  This emits exactly
 * `size` bytes and returns the count; -EFAULT when the user range is
 * invalid.  Byte-at-a-time uart_putc (not uart_puts) so no NUL assumption is
 * made about the buffer. */
int console_write_user(struct process *p, const void *buf, int size) {
  extern void uart_putc(char c);
  if (size <= 0)
    return 0;
  /* SEC-2: validate+read atomically per page via vm_user_read.  The old
     code validated the whole range once with vm_range_ok and then read it
     byte-by-byte in kernel mode — a peer unmapping mid-copy turned that
     into a fault on a freed frame (and vm_range_ok rejected every v1
     caller outright). */
  const char *s = (const char *)buf;
  /* GB-082 (2026-10-24): whole-write atomicity.  The previous code read the
     user buffer in fixed 64-byte chunks and took uart_lock PER CHUNK, so a
     single write(1)/printf line longer than 64 bytes was split into several
     lock holds and another program's console bytes interleaved between them —
     the measured glued line `[NLTEST] PASS UNEXPAND.BINmulti-file ...
     CMPTEST.BIN` is two programs' records fused on one serial line because the
     first writer's line was chunked mid-record.  Fix: stage the whole user
     buffer into a kernel buffer FIRST (no lock held across the user copy),
     then emit it under ONE uart_lock hold, splitting oversized writes only at
     a newline so complete lines are never broken by another writer. */
  enum { STAGE_CAP = 8192 };
  char stage[STAGE_CAP];
  int done = 0;
  while (done < size) {
    int n = size - done;
    if (n > STAGE_CAP)
      n = STAGE_CAP;
    if (vm_user_read(p, (uint64_t)(s + done), stage, n) != 0)
      return -EFAULT;
    /* Emit this window under one lock hold.  Avoid splitting a record: if the
       window does not end exactly at the user buffer's end, only emit up to
       the last newline in the window so a partial line is carried to the next
       window (which will then be contiguous under its own single lock hold). */
    int emit = n;
    if (done + n < size) {
      int lastnl = -1;
      for (int i = 0; i < n; i++)
        if (stage[i] == '\n')
          lastnl = i;
      if (lastnl >= 0)
        emit = lastnl + 1;
      else
        emit = n; /* one record spans the whole window (a >8KB line with no
                     newline): emit it whole under the lock rather than drop
                     or re-stage it — a console line longer than STAGE_CAP is
                     pathological, but dropping bytes is never acceptable */
    }
    {
      extern spinlock_t uart_lock;
      extern void uart_putc_raw(char c);
      uint64_t uflags = spinlock_acquire_irqsave(&uart_lock);
      for (int i = 0; i < emit; i++)
        uart_putc_raw(stage[i]);   /* raw: uart_putc would re-take uart_lock */
      spinlock_release_irqrestore(&uart_lock, uflags);
    }
    /* Advance by the completed (emitted) bytes of this window.  A partial
       trailing line (emit stopped at the last newline, before the window's
       end) is left in the user buffer and re-staged by the next iteration,
       where it is emitted contiguously under its own single lock hold. */
    done += emit;
  }
  return size;
}

int file_write(struct process *cur, int fd, const void *buf, int size, struct trap_frame *tf) {
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || fd < 0 || fd >= MAX_OPEN_FDS) return -1;

  int g_fd = cur->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) {
    /* WK-1 M3: a program launched by the boot wave (e.g. the
       statically linked WebKit jsc shell) has no console fd assigned —
       open_fds[1]/[2] are -1 — so write(1)/write(2) returned EBADF and
       printf/fwrite output vanished silently.  Route fd 1/2 to the
       length-bounded console writer, mirroring user/libc.c print()'s
       SYS_WRITE_CONSOLE fallback at the syscall layer so every stdio
       path (printf/puts/fwrite/std::cout) works.  Console writes are
       synchronous; other fds keep the historical EBADF. */
    if (fd == 1 || fd == 2)
      return console_write_user(cur, buf, size);
    return -1;
  }

  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  if (f->type == FILE_TYPE_DEVNULL) {
    return size; /* /dev/null swallows any write, accepts it fully */
  } else if (f->type == FILE_TYPE_CONSOLE) {
    /* The same sink file_write's unassigned-fd 1/2 fallback uses, now behind
       a real file object so fstat/dup/lseek on the fd behave like a device. */
    return console_write_user(cur, buf, size);
  } else if (f->type == FILE_TYPE_DEVURANDO) {
    return -EINVAL; /* writes to /dev/urandom are not supported (Linux EINVAL) */
  } else if (f->type == FILE_TYPE_FAT16) {
    int wr = fat16_write(f, buf, size);
    /* so-vm lane A (CROSS-LANE, additive): an in-place write changes the
       bytes behind (start_cluster, file_size), so the VM_OBJ_FILE page
       cache must drop its frames — otherwise a rebuilt DSO would keep
       serving its old text to every mapping.  Over-invalidation only costs
       a re-read; the object (and any live mapping) stays alive on its own
       references. */
    if (wr > 0)
      file_obj_invalidate_all();
    return wr;
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
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++) {
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

/* ---------------------------------------------------------------------
 * P2.4 (S4, design section 4.3): memfd objects.
 *
 * A memfd's backing object lives in the global file table slot
 * (FILE_TYPE_MEMFD / f->memfd.obj).  Object references mirror the fd's
 * lifetime: every fd instance holds one reference (the ref_count
 * increment/decrement sites below) and every region record holds one
 * more (vm.c), so the object is freed only at the last unref.
 * ------------------------------------------------------------------- */

/* L49: a timerfd.  The fd table, its allocation and the global-file slot are
   private to this file, so the create lives here and the timer state lives in
   linux_compat_timers.c (which returns the slot). */
int file_timerfd_create(struct process *cur, int clockid, int flags) {
  extern int lt_timerfd_new(int clockid, int flags);
  extern void lt_timerfd_release(int slot);
  cur = process_group(cur);
  if (!cur || cur->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;
  int slot = lt_timerfd_new(clockid, flags);
  if (slot < 0) return slot;
  int ufd = -1;
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++)
    if (cur->open_fds[i] == -1) { ufd = i; break; }
  if (ufd < 0) { lt_timerfd_release(slot); return -EMFILE; }
  struct file *f = file_alloc();
  if (!f) { lt_timerfd_release(slot); return -ENFILE; }
  f->type = FILE_TYPE_TIMERFD;
  f->timerfd.slot = slot;
  f->open_flags = (uint32_t)flags;
  cur->open_fds[ufd] = get_global_fd(f);
  cur->num_open_fds++;
  if (flags & 0x80000) fd_cloexec_set(cur->fd_cloexec, ufd);
  return ufd;
}

/* L49: an epoll instance's fd. */
int file_epoll_create(struct process *cur, int flags) {
  extern int lt_epoll_new(void);
  extern void lt_epoll_release(int slot);
  cur = process_group(cur);
  if (!cur || cur->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;
  int slot = lt_epoll_new();
  if (slot < 0) return slot;
  int ufd = -1;
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++)
    if (cur->open_fds[i] == -1) { ufd = i; break; }
  if (ufd < 0) { lt_epoll_release(slot); return -EMFILE; }
  struct file *f = file_alloc();
  if (!f) { lt_epoll_release(slot); return -ENFILE; }
  f->type = FILE_TYPE_EPOLL;
  f->epollfd.slot = slot;
  f->open_flags = (uint32_t)flags;
  cur->open_fds[ufd] = get_global_fd(f);
  cur->num_open_fds++;
  return ufd;
}

int file_epoll_slot(struct process *p, int fd) {
  p = process_group(p);
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) return -1;
  int g = p->open_fds[fd];
  if (g < 0 || g >= MAX_GLOBAL_FILES) return -1;
  struct file *f = &global_file_table[g];
  if (f->type != FILE_TYPE_EPOLL) return -1;
  return f->epollfd.slot;
}

/* L49: the timer slot behind a timerfd, for the timer syscalls. */
int file_timerfd_slot(struct process *p, int fd) {
  p = process_group(p);
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) return -1;
  int g = p->open_fds[fd];
  if (g < 0 || g >= MAX_GLOBAL_FILES) return -1;
  struct file *f = &global_file_table[g];
  if (f->type != FILE_TYPE_TIMERFD) return -1;
  return f->timerfd.slot;
}

/* Resolve an fd to its FILE_TYPE_MEMFD global file; *err carries the
 * failure (-EBADF for a closed/absent fd, -EINVAL for another type). */
static struct file *f1_memfd(struct process *p, int fd, int *err) {
  p = process_group(p); /* P1 (D7): the fd table is group state */
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS) {
    *err = EBADF;
    return 0;
  }
  int gfd = p->open_fds[fd];
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES) {
    *err = EBADF;
    return 0;
  }
  struct file *f = &global_file_table[gfd];
  if (f->type != FILE_TYPE_MEMFD) {
    *err = EINVAL;
    return 0;
  }
  *err = 0;
  return f;
}

int file_memfd_create(struct process *cur, const char *name, int flags) {
  (void)name; /* accepted for compat; not visible in the FAT namespace and
                 not retained (design 4.3) */
  if (flags & ~(K_MFD_CLOEXEC | K_MFD_ALLOW_SEALING))
    return -EINVAL;
  cur = process_group(cur); /* P1 (D7): the fd table is group state */
  if (!cur || cur->num_open_fds >= MAX_OPEN_FDS)
    return -EMFILE;
  int ufd = -1;
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++) {
    if (cur->open_fds[i] == -1) {
      ufd = i;
      break;
    }
  }
  if (ufd < 0)
    return -EMFILE;

  struct vm_object *o = vm_object_create(VM_OBJ_MEMFD, 0);
  if (!o)
    return -ENOMEM;
  struct file *f = file_alloc();
  if (!f) {
    vm_object_unref(o);
    return -ENFILE;
  }
  f->type = FILE_TYPE_MEMFD;
  f->memfd.obj = o; /* this fd instance's object reference */
  f->memfd.mfd_flags = flags & (K_MFD_CLOEXEC | K_MFD_ALLOW_SEALING);

  cur->open_fds[ufd] = get_global_fd(f);
  cur->num_open_fds++;
  if (flags & K_MFD_CLOEXEC)
    fd_cloexec_set(cur->fd_cloexec, ufd); /* recorded; P5's exec sweep consumes */
  return ufd;
}

/* Read a whole program image into a contiguous frame range, choosing the
 * filesystem the same way open() does (M5 exec-from-NFS).  The exec path has
 * always gone through fat16_open(), which knows only FAT16 and CREATES a
 * zero-length root entry on a miss — so an image under an NFS mount was
 * reported as "Failed to open file" and left a ghost behind.
 *
 * Returns
 *   1  the path routed to NFS; buf/size/pages are filled and the caller
 *      owns the frames (frame_free each page when done),
 *   0  the path is a FAT16 path; the caller keeps the legacy cluster loader,
 *  <0  a real failure (empty/oversized image, NFS read error, no memory).
 */
int fs_read_image(const char *path, uint8_t **buf_out, uint32_t *size_out,
                  uint32_t *pages_out) {
  if (!path || !buf_out || !size_out || !pages_out) return -1;

  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;
  int midx = -1;
  const char *rel = 0;
  if (!vfs_route(abs, &midx, &rel)) return 0;    /* FAT16 handles it */

  struct file f;
  if (vfs_open_routed(abs, &f) != 1) return -1;
  if (f.nfs.is_dir) return -1;

  uint32_t size = f.nfs.size;
  if (size == 0 || (uint64_t)size > USER_IMG_SIZE) return -1;
  const struct nfs_mount *m = nfs_mount_at(midx);
  if (!m) return -1;

  uint32_t pages = (size + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t buf = frame_alloc_contig((int)pages);
  if (!buf) return -1;

  uint32_t off = 0;
  while (off < size) {
    /* L35: hand the rest of the image to nfs_read_file in one call — it
     * splits the request into rsize-sized READs and pipelines them.  The old
     * 1 KiB clamp here serialized every exec image read at one RPC per
     * millisecond-plus. */
    int got = nfs_read_file(m, &f.nfs.fh, off,
                            (void *)(uintptr_t)(buf + off), size - off);
    if (got <= 0) {
      for (uint32_t i = 0; i < pages; i++)
        frame_free(buf + (uint64_t)i * FRAME_SIZE);
      return -1;
    }
    off += (uint32_t)got;
  }

  *buf_out = (uint8_t *)(uintptr_t)buf;
  *size_out = size;
  *pages_out = pages;
  return 1;
}

struct vm_object *file_memfd_obj(struct process *p, int fd) {
  int err = 0;
  struct file *f = f1_memfd(p, fd, &err);
  if (!f || !f->memfd.obj)
    return 0;
  return f->memfd.obj;
}

/* so-vm lane A (CROSS-LANE, additive; flagged in the lane report):
 * resolve an fd to a REGULAR FAT16 file's on-disk identity so SYS_MMAP can
 * key the VM_OBJ_FILE page cache by (start_cluster, file_size) — the same
 * identity the lazy v2 IMAGE loader uses.  Declared in file_obj.h because
 * fs.h is frozen for this lane.  Returns 0 and fills the out-params (either
 * may be NULL), -EBADF for an absent fd, -ENOTSUP for every other fd type
 * (memfd/pipe/socket/NFS/dir), which is what keeps their mmap answers
 * unchanged. */
int file_fat16_ident(struct process *p, int fd, uint16_t *cluster,
                     uint32_t *size) {
  p = process_group(p); /* P1 (D7): the fd table is group state */
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS)
    return -EBADF;
  int gfd = p->open_fds[fd];
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES)
    return -EBADF;
  struct file *f = &global_file_table[gfd];
  if (f->type != FILE_TYPE_FAT16)
    return -ENOTSUP;
  if (cluster)
    *cluster = f->fat16.entry.start_cluster;
  if (size)
    *size = f->fat16.entry.file_size;
  return 0;
}

/* L54: the NFS sibling of file_fat16_ident.  A regular file on a mounted
 * export is identified by (mount index, NFSv3 file handle); nfs_obj_get()
 * keys the page cache on that pair, so every mapping of one file shares its
 * frames.  Directories are refused (they have their own getdents path). */
int file_nfs_ident(struct process *p, int fd, int *mount_idx,
                   struct nfs_fh *fh, uint32_t *size) {
  p = process_group(p);
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS)
    return -EBADF;
  int gfd = p->open_fds[fd];
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES)
    return -EBADF;
  struct file *f = &global_file_table[gfd];
  if (f->type != FILE_TYPE_NFS || f->nfs.is_dir)
    return -ENOTSUP;
  if (mount_idx)
    *mount_idx = f->nfs.mount_idx;
  if (fh)
    *fh = f->nfs.fh;
  if (size)
    *size = (uint32_t)f->nfs.size;
  return 0;
}

/* GB-025 / M3: is `fd` the /dev/fb0 node?  The mmap half (vm_mmap) needs to
 * recognise the fd before it reaches the memfd/FAT16 object paths, and the
 * compat ioctl route needs the same test.  One predicate, so the two halves
 * cannot disagree about what /dev/fb0 is. */
int file_is_devfb(struct process *p, int fd) {
  p = process_group(p);
  if (!p || fd < 0 || fd >= MAX_OPEN_FDS)
    return 0;
  int gfd = p->open_fds[fd];
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES)
    return 0;
  return global_file_table[gfd].type == FILE_TYPE_DEVFB;
}

/* fcntl(F_ADD_SEALS/F_GET_SEALS) for memfd (design 4.3, OQ7 minimal set:
 * SHRINK/GROW enforced at ftruncate, WRITE recorded advisory). */
int file_memfd_seals(struct process *p, int fd, int cmd, int arg) {
  int err = 0;
  struct file *f = f1_memfd(p, fd, &err);
  if (!f)
    return -err;
  if (cmd == K_F_GET_SEALS)
    return (int)vm_object_get_seals(f->memfd.obj);
  if (arg & ~(K_F_SEAL_SEAL | K_F_SEAL_SHRINK | K_F_SEAL_GROW | K_F_SEAL_WRITE))
    return -EINVAL;
  return vm_object_add_seals(f->memfd.obj, (uint64_t)arg);
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
    if (f->type == FILE_TYPE_MEMFD)
      vm_object_ref(f->memfd.obj); /* one more instance ref (S4) */
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
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++) {
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
  if (type & K_SOCK_CLOEXEC) { fd_cloexec_set(p->fd_cloexec, fd0); fd_cloexec_set(p->fd_cloexec, fd1); }
  if (type & K_SOCK_NONBLOCK) {
    usock_set_nonblock(u, 0, 1);
    usock_set_nonblock(u, 1, 1);
  }
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
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++) {
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
  FS_SLOT_CHECK(f);
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

/* --- IceWM M1 (icewm.md section 4.1): named AF_UNIX stream sockets ------
 * bind/listen/accept4/connect/getsockname/getpeername/shutdown over the
 * unix.c name table.  Kernel-buffer forms first (unit-testable), then the
 * user-pointer wrappers the trap layer calls. */

/* Resolve a socket fd: the unix file, or NULL with *errp = -EBADF (not
 * open), -EOPNOTSUPP (INET socket: these ops are unix-only in 4.1), or
 * -ENOTSOCK (anything else). */
static struct file *f1_unix_file(struct process *pg, int fd, int *errp) {
  if (!pg || fd < 0 || fd >= MAX_OPEN_FDS) { *errp = -EBADF; return 0; }
  int g_fd = pg->open_fds[fd];
  if (g_fd < 0 || g_fd >= MAX_GLOBAL_FILES) { *errp = -EBADF; return 0; }
  struct file *f = &global_file_table[g_fd];
  FS_SLOT_CHECK(f);
  if (f->type == FILE_TYPE_SOCKET) { *errp = -EOPNOTSUPP; return 0; }
  if (f->type != FILE_TYPE_UNIXSOCK || !f->usock.ptr) {
    *errp = -ENOTSOCK;
    return 0;
  }
  return f;
}

int file_bind(struct process *p, int fd, const uint8_t *sa, int salen) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *f = f1_unix_file(pg, fd, &err);
  if (!f) return err;
  struct usock_name n;
  int r = usock_name_parse(sa, salen, &n);
  if (r == -EAFNOSUPPORT) return -EINVAL; /* Linux unix_mkname */
  if (r != 0) return r;
  return usock_bind(f->usock.ptr, f->usock.end, &n);
}

int file_listen(struct process *p, int fd, int backlog) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *f = f1_unix_file(pg, fd, &err);
  if (!f) return err;
  return usock_listen(f->usock.ptr, f->usock.end, backlog);
}

int file_accept(struct process *p, int fd, int flags, uint8_t *sa_out,
                int *salen_out) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *lf = f1_unix_file(pg, fd, &err);
  if (!lf) return err;
  if (flags & ~(K_SOCK_CLOEXEC | K_SOCK_NONBLOCK)) return -EINVAL;
  struct usock *l = lf->usock.ptr;
  if (!usock_is_listening(l)) return -EINVAL;
  if (pg->num_open_fds >= MAX_OPEN_FDS) return -EMFILE;

  /* Reserve the file slot BEFORE dequeuing so a popped connection can
   * always be installed (nothing is lost on ENFILE). */
  struct file *nf = file_alloc();
  if (!nf) return -ENFILE;
  struct usock *c = 0;
  int r = usock_accept(l, lf->usock.end, 0, &c);
  if (r < 0) {
    nf->type = FILE_TYPE_EMPTY;
    nf->ref_count = 0;
    return r; /* -EAGAIN / -EINVAL / -2 (parked) */
  }
  int nfd = -1;
  for (int i = FD_STDIO_FIRST; i < MAX_OPEN_FDS; i++) {
    if (pg->open_fds[i] == -1) { nfd = i; break; }
  }
  if (nfd < 0) { /* raced with a sibling thread filling the table */
    usock_close(c, 1);
    nf->type = FILE_TYPE_EMPTY;
    nf->ref_count = 0;
    return -EMFILE;
  }
  nf->usock.ptr = c;
  nf->usock.end = 1; /* the queue's end-1 reference transfers to this fd */
  nf->type = FILE_TYPE_UNIXSOCK;
  if (flags & K_SOCK_NONBLOCK) usock_set_nonblock(c, 1, 1);
  pg->open_fds[nfd] = get_global_fd(nf);
  pg->num_open_fds++;
  if (flags & K_SOCK_CLOEXEC) fd_cloexec_set(pg->fd_cloexec, nfd);
  else fd_cloexec_clear(pg->fd_cloexec, nfd);

  if (sa_out && salen_out) {
    struct usock_name pn;
    usock_getname(c, 1, 1, &pn); /* the client's bound name, if any */
    *salen_out = usock_name_format(&pn, sa_out);
  }
  return nfd;
}

int file_connect_sa(struct process *p, int fd, const uint8_t *sa, int salen) {
  struct process *pg = process_group(p);
  if (!sa || salen < 2) return -EINVAL;
  uint16_t fam = (uint16_t)(sa[0] | ((uint16_t)sa[1] << 8));
  int err = 0;
  struct file *f = f1_unix_file(pg, fd, &err);
  if (!f) {
    if (err != -EOPNOTSUPP) return err;
    /* INET fd: the POSIX shape over the frozen connect_fd path.  The
     * sockaddr_in fields are already in network order in memory. */
    if (fam != K_AF_INET) return -EAFNOSUPPORT;
    if (salen < 8) return -EINVAL;
    uint16_t port_be = (uint16_t)(sa[2] | ((uint16_t)sa[3] << 8));
    uint32_t ip_be = (uint32_t)sa[4] | ((uint32_t)sa[5] << 8) |
                     ((uint32_t)sa[6] << 16) | ((uint32_t)sa[7] << 24);
    return file_socket_connect(p, fd, ip_be, port_be);
  }
  struct usock_name n;
  int r = usock_name_parse(sa, salen, &n);
  if (r == -EAFNOSUPPORT) return -EINVAL;
  if (r != 0) return r;
  if (n.len == 0) return -EINVAL;
  return usock_connect(f->usock.ptr, f->usock.end, &n, 0);
}

int file_sockname(struct process *p, int fd, int peer, uint8_t *sa_out,
                  int *salen_out) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *f = f1_unix_file(pg, fd, &err);
  if (!f) return err;
  struct usock_name n;
  int r = usock_getname(f->usock.ptr, f->usock.end, peer ? 1 : 0, &n);
  if (r != 0) return r;
  *salen_out = usock_name_format(&n, sa_out);
  return 0;
}

int file_shutdown(struct process *p, int fd, int how) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *f = f1_unix_file(pg, fd, &err);
  if (!f) return err;
  return usock_shutdown(f->usock.ptr, f->usock.end, how);
}

/* --- user-pointer wrappers ------------------------------------------- */

/* Copy a user sockaddr of salen bytes into a K_UNIX_SUN_LEN scratch. */
static int f1_sa_copy_in(uint8_t *dst, uint64_t usa, int salen) {
  if (salen < 0 || salen > K_UNIX_SUN_LEN) return -EINVAL;
  if (salen > 0 && (!usa || !fs_copy_in(dst, usa, (uint64_t)salen)))
    return -EFAULT;
  return 0;
}

int file_bind_user(struct process *p, int fd, uint64_t usa, int salen) {
  uint8_t sa[K_UNIX_SUN_LEN];
  int r = f1_sa_copy_in(sa, usa, salen);
  if (r != 0) return r;
  return file_bind(p, fd, sa, salen);
}

int file_connect_user(struct process *p, int fd, uint64_t usa, int salen) {
  uint8_t sa[K_UNIX_SUN_LEN];
  /* sockaddr_in callers may pass sizeof(struct sockaddr_storage) (128). */
  if (salen > K_UNIX_SUN_LEN && salen <= 128) {
    uint8_t fam[2];
    if (!usa || !fs_copy_in(fam, usa, 2)) return -EFAULT;
    if (fam[0] == K_AF_INET && fam[1] == 0) salen = 16;
  }
  int r = f1_sa_copy_in(sa, usa, salen);
  if (r != 0) return r;
  return file_connect_sa(p, fd, sa, salen);
}

/* Value-result address out: write min(actual, *ulenp) bytes and the
 * actual length (Linux truncation semantics). */
static int f1_sa_copy_out(uint64_t usa, uint64_t ulenp, const uint8_t *sa,
                          int len) {
  uint32_t cap;
  if (!fs_copy_in(&cap, ulenp, sizeof cap)) return -EFAULT;
  uint32_t n = (uint32_t)len < cap ? (uint32_t)len : cap;
  if (n > 0 && !fs_copy_out(usa, sa, n)) return -EFAULT;
  uint32_t out = (uint32_t)len;
  if (!fs_copy_out(ulenp, &out, sizeof out)) return -EFAULT;
  return 0;
}

/* Validate the value-result pair up front (before any state change). */
static int f1_sa_out_ok(uint64_t usa, uint64_t ulenp) {
  if (!usa) return 0;
  uint32_t cap;
  if (!ulenp || !fs_copy_in(&cap, ulenp, sizeof cap)) return -EFAULT;
  if ((int32_t)cap < 0) return -EINVAL;
  if (cap > 0 && !sys_user_range_ok(usa, cap > K_UNIX_SUN_LEN
                                              ? K_UNIX_SUN_LEN : cap))
    return -EFAULT;
  return 0;
}

int file_accept_user(struct process *p, int fd, uint64_t usa, uint64_t ulenp,
                     int flags) {
  int v = f1_sa_out_ok(usa, ulenp);
  if (v != 0) return v;
  uint8_t sa[K_UNIX_SUN_LEN];
  int salen = 0;
  int r = file_accept(p, fd, flags, usa ? sa : 0, usa ? &salen : 0);
  if (r >= 0 && usa) f1_sa_copy_out(usa, ulenp, sa, salen); /* pre-checked */
  return r;
}

int file_sockname_user(struct process *p, int fd, int peer, uint64_t usa,
                       uint64_t ulenp) {
  if (!usa || !ulenp) return -EFAULT;
  int v = f1_sa_out_ok(usa, ulenp);
  if (v != 0) return v;
  uint8_t sa[K_UNIX_SUN_LEN];
  int salen = 0;
  int r = file_sockname(p, fd, peer, sa, &salen);
  if (r != 0) return r;
  return f1_sa_copy_out(usa, ulenp, sa, salen);
}

#ifdef KERNEL_MODE_UNIT_TEST
/* SEC-6 regression hook: expose a slot's type so tests can assert that no
 * allocation path ever strands a slot in UNINIT (or, before this fix, as a
 * fake FAT16 with garbage fields).  Test builds only. */
/* IceWM M1 test hook: the usock behind a unix fd (NULL otherwise). */
struct usock *fs_test_usock(struct process *p, int fd) {
  struct process *pg = process_group(p);
  int err = 0;
  struct file *f = f1_unix_file(pg, fd, &err);
  return f ? f->usock.ptr : 0;
}

int fs_test_slot_type(int gfd) {
  if (gfd < 0 || gfd >= MAX_GLOBAL_FILES) return -1;
  return (int)global_file_table[gfd].type;
}
#endif
