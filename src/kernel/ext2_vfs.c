/*
 * ext2_vfs.c - mounts the ext2 data disk at /EXT and adapts the ext2 core
 * (ext2.c) to the kernel's file layer.  Contract: src/include/ext2_vfs.h;
 * design and evidence: docs/cp-ext2.md.
 *
 * Kernel constraints: no FP/SIMD, no unaligned wide access (records handed
 * to user space are assembled byte-wise), the single ext2 lock is a ylock
 * because it is held across device I/O.
 */

#include "ext2_vfs.h"

#include "errno.h"
#include "fat16.h"
#include "frame.h"
#include "kmem.h"
#include "fs.h"
#include "kline.h"
#include "lock.h"
#include "virtio_blk.h"
#include "vfs.h"
#include "ext2_obj.h"
#include "process.h"
#include "vm.h"

extern uint64_t hb_clock_realtime_ms(void);

#define K_DEV_EXT2 0x00000707u

#define O_ACCMODE_ 0x3
#define O_WRONLY_ 0x1
#define O_RDWR_ 0x2
#define O_CREAT_ 0x40
#define O_EXCL_ 0x80
#define O_TRUNC_ 0x200
#define O_APPEND_ 0x400
#define O_DIRECTORY_ 0x10000
#define O_NOFOLLOW_ 0x20000

/* The compat class reports umask 022 (l_umask, linux_compat_proc.c); it is a
   single kernel-wide value there too. */
#define EXT2V_UMASK 022

static struct ext2_fs g_fs;
static ylock_t g_lock;
static int g_mounted;

/* ---- host services for the core --------------------------------------- */

static int dd_read(void *ctx, uint64_t sector, void *buf, uint32_t n) {
  (void)ctx;
  return blk_data_read(sector, buf, n);
}

static int dd_write(void *ctx, uint64_t sector, const void *buf, uint32_t n) {
  (void)ctx;
  return blk_data_write(sector, buf, n);
}

/* cp/ext2w: a run of blocks in as few NVMe commands as the device allows. */
static int dd_rwv(void *ctx, uint64_t sector, void *const *bufs, uint32_t nbuf,
                  uint32_t nsec_each, int wr) {
  (void)ctx;
  return blk_data_rw_vec(sector, bufs, nbuf, nsec_each, wr);
}

static void *dd_alloc(uint32_t n) {
  (void)n; /* always 4096 == FRAME_SIZE */
  uint64_t fr = frame_alloc();
  return fr ? phys_to_virt(fr) : 0;
}

static uint32_t dd_now(void) {
  return (uint32_t)(hb_clock_realtime_ms() / 1000ull);
}

/* ---- small helpers ------------------------------------------------------ */

static int v_len(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void v_copy(char *dst, const char *src, int cap) {
  int i = 0;
  for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
  dst[i] = '\0';
}

/* cp/ext2w (C8 stop 6): who holds the ext2 lock, from where, since when.
 * A waiter that has spun for EXT2W_HOLD_REPORT_MS prints one row naming the
 * holder's pid and its lock() call site (resolve with addr2line on
 * hobbyos.elf), so a wedge says where the holder entered. */
#define EXT2W_HOLD_REPORT_MS 2000u
static volatile uint64_t g_hold_site, g_hold_ms;
static volatile int g_hold_pid;
extern uint64_t timer_get_ms(void);

static void lock_note(uint64_t site) {
  struct process *c = current_process();
  g_hold_site = site;
  g_hold_pid = c ? c->pid : -1;
  g_hold_ms = timer_get_ms();
}

static __attribute__((noinline)) void lock(void) {
  uint64_t site = (uint64_t)(uintptr_t)__builtin_return_address(0);
  static volatile int reported;
  if (!reported && __atomic_load_n(&g_lock.locked, __ATOMIC_ACQUIRE)) {
    /* held: watch (read-only) before joining ylock_acquire's spin */
    uint64_t t0 = timer_get_ms();
    while (__atomic_load_n(&g_lock.locked, __ATOMIC_ACQUIRE) && !reported) {
      if (timer_get_ms() - t0 >= EXT2W_HOLD_REPORT_MS) {
        reported = 1;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[E2W] ext2_lock wait ");
        kl_i(&kl, (long)(timer_get_ms() - t0));
        kl_s(&kl, " ms: holder pid=");
        kl_i(&kl, (long)g_hold_pid);
        kl_s(&kl, " site=");
        kl_hex(&kl, (unsigned long)g_hold_site);
        kl_s(&kl, " held_ms=");
        kl_i(&kl, (long)(timer_get_ms() - g_hold_ms));
        kl_s(&kl, " waiter site=");
        kl_hex(&kl, (unsigned long)site);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
      __asm__ volatile("" ::: "memory");
    }
  }
  ylock_acquire(&g_lock);
  lock_note(site);
}

/* ---- cp/ext2w: when the write-back cache is committed (docs/cp-ext2w.md) --
 *
 * A mutating call no longer writes through.  ext2_flush() (the ordered
 * commit) runs at:
 *   - fsync / fdatasync / syncfs / sync              (ext2v_sync)
 *   - the close of a writable file description writes back the file data
 *     (the early-safe class only, no metadata: ext2v_close)
 *   - the flusher, at most EXT2W_TIMER_MS after the cache first got dirty
 *   - cache pressure: more than half the cache dirty at a call boundary
 *   - inside an operation only when no clean or early-writable buffer is
 *     left (ext2.c free_slot), or an allocation needs this batch's frees
 *   - shutdown: the flusher keeps the machine alive until the cache is
 *     clean, so the wave's halt comes after the last commit.
 * Every commit above except the in-operation one runs under the ext2 lock at
 * a syscall boundary, so it writes a state some syscall sequence produced. */
#define EXT2W_TIMER_MS 1000u  /* oldest dirty data waits at most ~1 s */
#define EXT2W_LINGER_MS 200u  /* a clean flusher exits after this (it is a
                                 process: process censuses see it, and a
                                 halt waits for it) */

static volatile int g_flusher;          /* 1: a flusher task exists */
static volatile uint64_t g_dirty_since; /* ms the cache first went dirty, 0 = clean */
static uint64_t g_ncommit_timer, g_ncommit_sync, g_ncommit_close, g_ncommit_press;

extern int process_create_kernel_nowait(void (*entry)(void *), void *arg);
extern void kernel_exit(void);
extern void safe_wfi(void);

/* cp/ext2w profile (cycles): where a write(2) spends its time. */
static inline uint64_t e2w_tsc(void) {
#if defined(__x86_64__)
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
#else
  return 0;
#endif
}
static uint64_t g_pw_n, g_pw_pf, g_pw_lock, g_pw_core, g_pw_note, g_pw_unl;
static uint64_t g_sy_n, g_sy_obj, g_sy_commit, g_sy_cmds; /* ext2v_sync profile */

static void e2w_row(const char *why) {
  static uint32_t nrows;
  if (nrows++ >= 16 && (nrows & 63) != 0) return; /* bounded console cost */
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[E2W] ");
  kl_s(&kl, why);
  kl_s(&kl, " commits=");
  kl_i(&kl, (long)g_fs.st_commits);
  kl_s(&kl, " timer=");
  kl_i(&kl, (long)g_ncommit_timer);
  kl_s(&kl, " sync=");
  kl_i(&kl, (long)g_ncommit_sync);
  kl_s(&kl, " closewb=");
  kl_i(&kl, (long)g_ncommit_close);
  kl_s(&kl, " press=");
  kl_i(&kl, (long)g_ncommit_press);
  kl_s(&kl, " midop=");
  kl_i(&kl, (long)g_fs.st_midop);
  kl_s(&kl, " evictwb=");
  kl_i(&kl, (long)g_fs.st_evict_wb);
  kl_s(&kl, " blk_w=");
  kl_i(&kl, (long)g_fs.st_dev_writes);
  kl_s(&kl, " blk_r=");
  kl_i(&kl, (long)g_fs.st_dev_reads);
  kl_s(&kl, " cmds=");
  kl_i(&kl, (long)g_fs.st_cmds);
  kl_s(&kl, " ra=");
  kl_i(&kl, (long)g_fs.st_ra);
  kl_s(&kl, " nopre=");
  kl_i(&kl, (long)g_fs.st_nopre);
  if (g_sy_n) {
    kl_s(&kl, " sync_n=");
    kl_i(&kl, (long)g_sy_n);
    kl_s(&kl, " kcyc/sync obj=");
    kl_i(&kl, (long)(g_sy_obj / g_sy_n / 1000));
    kl_s(&kl, " commit=");
    kl_i(&kl, (long)(g_sy_commit / g_sy_n / 1000));
    kl_s(&kl, " cmds/sync*10=");
    kl_i(&kl, (long)(g_sy_cmds * 10 / g_sy_n));
  }
  if (g_pw_n) {
    kl_s(&kl, " write_n=");
    kl_i(&kl, (long)g_pw_n);
    kl_s(&kl, " kcyc/write pf=");
    kl_i(&kl, (long)(g_pw_pf / g_pw_n / 1000));
    kl_s(&kl, " lock=");
    kl_i(&kl, (long)(g_pw_lock / g_pw_n / 1000));
    kl_s(&kl, " core=");
    kl_i(&kl, (long)(g_pw_core / g_pw_n / 1000));
    kl_s(&kl, " note=");
    kl_i(&kl, (long)(g_pw_note / g_pw_n / 1000));
    kl_s(&kl, " unlock=");
    kl_i(&kl, (long)(g_pw_unl / g_pw_n / 1000));
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* Commit under the lock (the caller holds it). */
static int commit_locked(uint64_t *counter) {
  if (!ext2_dirty(&g_fs)) return 0;
  int rc = ext2_flush(&g_fs);
  if (counter) (*counter)++;
  if (rc == 0 && !ext2_dirty(&g_fs)) g_dirty_since = 0;
  return rc;
}

static void flusher_task(void *arg) {
  (void)arg;
  uint64_t clean_since = timer_get_ms();
  for (;;) {
    uint64_t now = timer_get_ms();
    uint64_t ds = g_dirty_since;
    if (ds && now - ds >= EXT2W_TIMER_MS) {
      lock();
      (void)commit_locked(&g_ncommit_timer);
      ylock_release(&g_lock);
      clean_since = timer_get_ms();
    } else if (!ds && now - clean_since >= EXT2W_LINGER_MS) {
      /* Exit only while holding the lock, so a writer that dirtied the cache
         after the check above sees g_flusher == 0 and starts a new one. */
      lock();
      if (!ext2_dirty(&g_fs)) {
        g_dirty_since = 0;
        __atomic_store_n(&g_flusher, 0, __ATOMIC_SEQ_CST);
        ylock_release(&g_lock);
        e2w_row("flusher idle exit");
        kernel_exit();
        return;
      }
      if (!g_dirty_since) g_dirty_since = timer_get_ms();
      ylock_release(&g_lock);
    } else if (ds) {
      clean_since = now;
    }
    uint64_t t0 = timer_get_ms();
    while (timer_get_ms() - t0 < 50) safe_wfi();
  }
}

/* Release the lock after a call that may have dirtied the cache.  `mutated`
 * is advisory now: the cache's own dirty count decides. */
static void unlock_flush(int mutated) {
  (void)mutated;
  int spawn = 0;
  if (ext2_dirty(&g_fs)) {
    /* pressure: keep at least half the cache clean or early-writable */
    if (g_fs.ndirty > g_fs.cache_cap / 2) (void)commit_locked(&g_ncommit_press);
    if (ext2_dirty(&g_fs)) {
      if (!g_dirty_since) g_dirty_since = timer_get_ms();
      if (!g_flusher) {
        g_flusher = 1;
        spawn = 1;
      }
    }
  }
  ylock_release(&g_lock);
  int fpid = spawn ? process_create_kernel_nowait(flusher_task, 0) : 0;
  if (spawn && fpid > 0) {
    struct process *fp = process_get_pcb(fpid);
    if (fp) {
      const char *nm = "ext2flush";
      int i = 0;
      for (; nm[i] && i < 31; i++) fp->name[i] = nm[i];
      fp->name[i] = 0;
    }
  }
  if (spawn && fpid < 0) {
    /* No task slot: commit now instead of leaving data unscheduled. */
    lock();
    (void)commit_locked(&g_ncommit_press);
    g_flusher = 0;
    ylock_release(&g_lock);
  }
}

static void unlock(void) { ylock_release(&g_lock); }

/* cp/ext2b: the VM-object glue (ext2_obj.c) runs under the same lock. */
void ext2v_lock(void) { lock(); }
void ext2v_unlock(int flush) { unlock_flush(flush); }
struct ext2_fs *ext2v_fs(void) { return &g_fs; }

/* cp/ext2b: a user buffer must be resident BEFORE the ext2 lock is taken.  A
   fault on it under the lock would re-enter the lock through an ext2 mmap
   page fill (ext2_obj_page) -- a self-deadlock.  The trap-layer read/write
   already pre-touch; pread64/pwrite64 and the kernel's own callers do not. */
static int user_prefault(const void *buf, int size, int write) {
  uint64_t a = (uint64_t)(uintptr_t)buf;
  if (size <= 0) return 0;
  struct process *cur = current_process();
  if (!cur) return 0;
  struct process *g = process_group(cur);
  if (!g || !g->as) return 0;
  /* P1: a user buffer is any user VA of the caller's address space (compat
     processes live below USER_VA_BASE); a kernel buffer needs no prefault. */
  if (!vm_va_is_user(g->as, a)) return 0;
  return vm_touch(g, a, (uint64_t)size, write) == 0 ? 0 : -EFAULT;
}

/* Resolve under the lock.  EXT2_EXTERNAL rewrites `abs`. */
static int resolve(char *abs, int follow, struct ext2_res *r) {
  int rc = ext2_resolve(&g_fs, EXT2V_MNT, abs, follow ? EXT2_RES_FOLLOW : 0, r);
  if (rc == EXT2_EXTERNAL) v_copy(abs, r->path, VFS_PATH_MAX);
  return rc;
}

static void stat_fill(struct k_stat *st, const struct ext2_stat *es) {
  st->st_dev = K_DEV_EXT2;
  st->st_ino = es->ino;
  st->st_mode = es->mode;
  st->st_nlink = es->nlink ? es->nlink : 1;
  st->st_uid = es->uid;
  st->st_gid = es->gid;
  st->st_rdev = 0;
  st->st_size = (long)es->size;
  st->st_blksize = es->blksize;
  st->st_blocks = (long)es->blocks;
  /* ext2 times are signed 32-bit seconds (cp/ext2b: utimensat can store a
     pre-1970 time, which must not read back as 2106). */
  st->st_atime = (long)(int32_t)es->atime;
  st->st_mtime = (long)(int32_t)es->mtime;
  st->st_ctime = (long)(int32_t)es->ctime;
}

/* ---- mount -------------------------------------------------------------- */

void ext2v_init(void) {
  if (g_mounted || !blk_data_present()) return;
  ylock_init(&g_lock);
  lock_register(&g_lock, "ext2_lock", LOCK_RANK_FAT, LOCK_CLASS_PROCESS);
  struct ext2_dev dev;
  dev.ctx = 0;
  dev.read = dd_read;
  dev.write = dd_write;
  dev.rwv = dd_rwv;
  dev.nsectors = blk_data_sectors();
  g_fs.now = dd_now;
  g_fs.alloc_buf = dd_alloc;
  g_fs.cache_cap = 4096; /* 16 MiB of frames, allocated on demand (cp/ext2w) */
  lock();
  int rc = ext2_mount(&g_fs, &dev, 0);
  char desc[160];
  desc[0] = '\0';
  if (rc == 0) ext2_describe(&g_fs, desc, sizeof desc);
  unlock();
  struct kline kl;
  kl_init(&kl);
  if (rc != 0) {
    kl_s(&kl, "[EXT2] data disk present but NOT mounted: rc=");
    kl_i(&kl, rc);
    kl_s(&kl, rc == -ENOTSUP ? " (unsupported INCOMPAT feature)"
                             : " (not an acceptable ext2 superblock)");
    kl_c(&kl, '\n');
    kl_emit(&kl);
    return;
  }
  g_mounted = 1;
  kl_s(&kl, "[EXT2] mounted " EXT2V_MNT " ");
  kl_s(&kl, desc);
  kl_c(&kl, '\n');
  kl_emit(&kl);
  /* Make the mount point visible in FAT listings of "/" (as NFS mounts are).
     Outside the ext2 lock: fat_lock and ext2_lock share a rank. */
  (void)fat16_mkdir(EXT2V_MNT);
}

int ext2v_mounted(void) { return g_mounted; }
int ext2v_readonly(void) { return g_mounted ? g_fs.ro : 1; }

int ext2v_owns(const char *abs) {
  if (!g_mounted || !abs) return 0;
  const char *m = EXT2V_MNT;
  int i = 0;
  while (m[i]) {
    if (abs[i] != m[i]) return 0;
    i++;
  }
  return abs[i] == '\0' || abs[i] == '/';
}

/* ---- open files --------------------------------------------------------- */

int ext2v_open(char *abs, int flags, int mode, struct file *f) {
  int acc = flags & O_ACCMODE_;
  int wants_write = (acc == O_WRONLY_ || acc == O_RDWR_);
  struct ext2_res r;
  lock();
  int mutated = ext2_obj_reap_locked(); /* cp/ext2b */
  int rc = resolve(abs, !(flags & O_NOFOLLOW_), &r);
  if (rc == EXT2_EXTERNAL) {
    if (mutated) (void)ext2_flush(&g_fs);
    unlock();
    return EXT2_EXTERNAL;
  }
  uint32_t ino = 0;
  uint16_t imode = 0;
  if (rc == -ENOENT && r.parent && (flags & O_CREAT_)) {
    if (g_fs.ro) {
      unlock();
      return -EROFS;
    }
    rc = ext2_create(&g_fs, r.parent, r.name, r.name_len,
                     (uint16_t)(mode & ~EXT2V_UMASK & 07777), &ino);
    mutated = 1;
    imode = EXT2_S_IFREG;
    if (rc == 0) {
      /* the resolved physical path of the new name */
      int pl = v_len(r.path);
      if (pl > 0) v_copy(abs, r.path, VFS_PATH_MAX);
    }
  } else if (rc == 0) {
    if ((flags & O_CREAT_) && (flags & O_EXCL_)) rc = -EEXIST;
    ino = r.ino;
    imode = r.mode;
    v_copy(abs, r.path, VFS_PATH_MAX);
  }
  if (rc == 0) {
    uint16_t fmt = imode & EXT2_S_IFMT;
    if (fmt == EXT2_S_IFLNK) {
      rc = -ELOOP; /* O_NOFOLLOW on a symlink */
    } else if (fmt == EXT2_S_IFDIR) {
      if (wants_write) rc = -EISDIR;
    } else if (flags & O_DIRECTORY_) {
      rc = -ENOTDIR;
    } else if (fmt != EXT2_S_IFREG) {
      rc = -ENXIO; /* device nodes / fifos on ext2: not modelled */
    } else if (wants_write && g_fs.ro) {
      rc = -EROFS;
    }
  }
  if (rc == 0) rc = ext2_iget(&g_fs, ino);
  if (rc == 0 && (imode & EXT2_S_IFMT) == EXT2_S_IFREG && wants_write &&
      (flags & O_TRUNC_)) {
    uint64_t osz = 0;
    (void)ext2_file_size(&g_fs, ino, &osz);
    rc = ext2_truncate(&g_fs, ino, 0);
    mutated = 1;
    if (rc == 0) ext2_obj_note_truncate_locked(ino, osz, 0); /* cp/ext2b */
    if (rc) (void)ext2_iput(&g_fs, ino);
  }
  unlock_flush(mutated);
  if (rc) return rc;
  f->type = ((imode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? FILE_TYPE_EXT2DIR
                                                    : FILE_TYPE_EXT2;
  f->ext2.ino = ino;
  f->ext2.cursor = 0;
  f->open_flags = (uint32_t)flags;
  return 0; /* NOT 1: that value is EXT2_EXTERNAL */
}

void ext2v_close(struct file *f) {
  if (!g_mounted) return;
  lock();
  /* cp/ext2b: a close writes back this file's shared-writable mappings and
     reaps cache entries no mapping uses any more. */
  (void)ext2_obj_writeback_locked(f->ext2.ino);
  (void)ext2_iput(&g_fs, f->ext2.ino);
  (void)ext2_obj_reap_locked();
  /* cp/ext2w: the last close of a WRITABLE description writes the cached
     file data (and every block allocated since the last commit) to the disk
     -- the class of writes that can never leave the on-disk image worse than
     the last commit did.  The metadata that points at it (inode, bitmaps,
     directory entry) follows with the next commit (timer, sync, fsync).  A
     full commit here would keep the disk mid-commit most of the time under a
     program that churns small files (measured: the pull-the-plug gate's
     writer, 600 commits in 2 s, e2fsck found its half-written commit), and
     docs/cp-ext2w.md section 2 promises e2fsck-clean after a crash at any
     point between commits.  A read-only close writes nothing. */
  int acc = f->open_flags & O_ACCMODE_;
  if (acc == O_WRONLY_ || acc == O_RDWR_) {
    if (ext2_writeback_early(&g_fs) > 0) g_ncommit_close++;
  }
  unlock_flush(1);
}

/* read/write must never return -2 (the VFS's "would block" marker, which is
   also -ENOENT) or -1 (mapped to EBADF): fold both into -EIO. */
static int io_ret(int64_t v) {
  if (v == -1 || v == -2) return -EIO;
  if (v > 0x7FFFFFFF) return 0x7FFFFFFF;
  return (int)v;
}

int ext2v_read(struct file *f, void *buf, int size) {
  if ((f->open_flags & O_ACCMODE_) == O_WRONLY_) return -EBADF;
  if (f->type == FILE_TYPE_EXT2DIR) return -EISDIR;
  if (size <= 0) return 0;
  if (user_prefault(buf, size, 1)) return -EFAULT;
  lock();
  /* cp/ext2b: read() sees stores made through a MAP_SHARED mapping. */
  int wb = ext2_obj_writeback_locked(f->ext2.ino);
  int64_t r = ext2_read(&g_fs, f->ext2.ino, f->ext2.cursor, buf, (uint64_t)size);
  if (r > 0) f->ext2.cursor += (uint64_t)r;
  unlock_flush(wb > 0);
  return io_ret(r);
}

int ext2v_write(struct file *f, const void *buf, int size) {
  int acc = f->open_flags & O_ACCMODE_;
  if (acc != O_WRONLY_ && acc != O_RDWR_) return -EBADF;
  if (f->type == FILE_TYPE_EXT2DIR) return -EISDIR;
  if (size <= 0) return 0;
  uint64_t t0 = e2w_tsc();
  if (user_prefault(buf, size, 0)) return -EFAULT;
  uint64_t t1 = e2w_tsc();
  lock();
  uint64_t t2 = e2w_tsc();
  uint64_t off = f->ext2.cursor;
  uint64_t sz = 0;
  (void)ext2_file_size(&g_fs, f->ext2.ino, &sz);
  if (f->open_flags & O_APPEND_) off = sz;
  int64_t w = ext2_write(&g_fs, f->ext2.ino, off, buf, (uint64_t)size);
  uint64_t t3 = e2w_tsc();
  if (w > 0) {
    f->ext2.cursor = off + (uint64_t)w;
    ext2_obj_note_write_locked(f->ext2.ino, off, (uint64_t)w, sz); /* cp/ext2b */
  }
  uint64_t t4 = e2w_tsc();
  unlock_flush(1);
  uint64_t t5 = e2w_tsc();
  g_pw_n++;
  g_pw_pf += t1 - t0;
  g_pw_lock += t2 - t1;
  g_pw_core += t3 - t2;
  g_pw_note += t4 - t3;
  g_pw_unl += t5 - t4;
  return io_ret(w);
}

int64_t ext2v_size(struct file *f) {
  if (f->type == FILE_TYPE_EXT2DIR) return 0;
  uint64_t sz = 0;
  lock();
  int rc = ext2_file_size(&g_fs, f->ext2.ino, &sz);
  unlock();
  return rc ? rc : (int64_t)sz;
}

int ext2v_ftruncate(struct file *f, int64_t len) {
  if (f->type == FILE_TYPE_EXT2DIR) return -EISDIR;
  int acc = f->open_flags & O_ACCMODE_;
  if (acc != O_WRONLY_ && acc != O_RDWR_) return -EINVAL; /* Linux: EINVAL/EBADF */
  if (len < 0) return -EINVAL;
  lock();
  uint64_t osz = 0;
  (void)ext2_file_size(&g_fs, f->ext2.ino, &osz);
  int rc = ext2_truncate(&g_fs, f->ext2.ino, (uint64_t)len);
  if (rc == 0) ext2_obj_note_truncate_locked(f->ext2.ino, osz, (uint64_t)len);
  unlock_flush(1);
  return rc;
}

/* fsync/fdatasync: the whole cache is committed (a superset of the file),
   but the superblock's free counts wait for the next full commit. */
int ext2v_fsync(struct file *f) {
  (void)f;
  if (!g_mounted) return 0;
  lock();
  (void)ext2_obj_writeback_locked(0);
  (void)ext2_obj_reap_locked();
  int rc = ext2_dirty(&g_fs) ? ext2_flush_file(&g_fs) : 0;
  g_ncommit_sync++;
  if (!ext2_dirty(&g_fs)) g_dirty_since = 0;
  unlock_flush(1); /* a still-dirty superblock: the flusher writes it */
  return rc;
}

int ext2v_sync(void) {
  if (!g_mounted) return 0;
  lock();
  uint64_t t0 = e2w_tsc();
  (void)ext2_obj_writeback_locked(0); /* cp/ext2b: mapped stores too */
  (void)ext2_obj_reap_locked();
  uint64_t t1 = e2w_tsc(), c0 = g_fs.st_cmds;
  int rc = commit_locked(&g_ncommit_sync);
  g_sy_n++;
  g_sy_obj += t1 - t0;
  g_sy_commit += e2w_tsc() - t1;
  g_sy_cmds += g_fs.st_cmds - c0;
  if (!ext2_dirty(&g_fs)) g_dirty_since = 0;
  unlock();
  e2w_row("sync");
  return rc;
}

int ext2v_fstat(struct file *f, struct k_stat *st) {
  struct ext2_stat es;
  lock();
  int rc = ext2_stat(&g_fs, f->ext2.ino, &es);
  unlock();
  if (rc) return rc;
  stat_fill(st, &es);
  return 0;
}

static uint8_t ft_to_dt(uint8_t ft) {
  switch (ft) {
  case EXT2_FT_REG: return 8;
  case EXT2_FT_DIR: return 4;
  case EXT2_FT_SYMLINK: return 10;
  case EXT2_FT_CHR: return 2;
  case EXT2_FT_BLK: return 6;
  case EXT2_FT_FIFO: return 1;
  case EXT2_FT_SOCK: return 12;
  default: return 0;
  }
}

int ext2v_getdents64(struct file *f, void *ubuf, int bufsize) {
  if (f->type != FILE_TYPE_EXT2DIR) return -ENOTDIR;
  uint8_t *out = (uint8_t *)ubuf;
  int used = 0;
  char name[EXT2_NAME_MAX + 1];
  /* cp/ext2w (C8 stop 6): the records are stored into user memory under the
     ext2 lock, so make it resident first, like read/write do -- a first touch
     under the lock is a page fault inside it (an ext2-backed buffer would
     re-enter the lock from the fault path). */
  if (user_prefault(ubuf, bufsize, 1)) return -EFAULT;
  lock();
  for (;;) {
    uint64_t pos = f->ext2.cursor;
    uint32_t ino;
    uint8_t ft;
    int nl;
    int rc = ext2_readdir(&g_fs, f->ext2.ino, &pos, &ino, &ft, name, &nl);
    if (rc < 0) {
      unlock();
      return used ? used : rc;
    }
    if (rc == 0) break;
    int reclen = (19 + nl + 1 + 7) & ~7;
    if (used + reclen > bufsize) {
      if (used == 0) {
        unlock();
        return -EINVAL;
      }
      break; /* f->ext2.cursor not advanced: the entry is returned next call */
    }
    uint8_t *pb = out + used;
    uint64_t d_ino = ino, d_off = pos;
    for (int k = 0; k < 8; k++) pb[k] = (uint8_t)(d_ino >> (k * 8));
    for (int k = 0; k < 8; k++) pb[8 + k] = (uint8_t)(d_off >> (k * 8));
    pb[16] = (uint8_t)(reclen & 0xFF);
    pb[17] = (uint8_t)((reclen >> 8) & 0xFF);
    pb[18] = ft_to_dt(ft);
    for (int k = 0; k < nl; k++) pb[19 + k] = (uint8_t)name[k];
    for (int k = 19 + nl; k < reclen; k++) pb[k] = 0;
    used += reclen;
    f->ext2.cursor = pos;
  }
  unlock();
  return used;
}

/* ---- path operations ---------------------------------------------------- */

int ext2v_stat(char *abs, int follow, struct k_stat *st) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, follow, &r);
  struct ext2_stat es;
  if (rc == 0) rc = ext2_stat(&g_fs, r.ino, &es);
  unlock();
  if (rc) return rc;
  stat_fill(st, &es);
  return 0;
}

int ext2v_mkdir(char *abs, int mode) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    rc = -EEXIST;
  } else if (rc == -ENOENT && r.parent) {
    uint32_t ino;
    rc = ext2_mkdir(&g_fs, r.parent, r.name, r.name_len,
                    (uint16_t)(mode & ~EXT2V_UMASK & 07777), &ino);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_rmdir(char *abs) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    if (r.name_len == 0)
      rc = -EBUSY; /* the mount root */
    else {
      rc = ext2_rmdir(&g_fs, r.parent, r.name, r.name_len);
      mutated = 1;
    }
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_unlink(char *abs) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    if (r.name_len == 0)
      rc = -EISDIR;
    else {
      rc = ext2_unlink(&g_fs, r.parent, r.name, r.name_len);
      (void)ext2_obj_reap_locked(); /* cp/ext2b: an unmapped orphan goes now */
      mutated = 1;
    }
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_rename(char *from, char *to, int flags) {
  struct ext2_res a, b;
  lock();
  int rc = resolve(from, 0, &a);
  if (rc == 0) {
    int rb = ext2_resolve(&g_fs, EXT2V_MNT, to, 0, &b);
    if (rb == EXT2_EXTERNAL)
      rc = -EXDEV;
    else if (rb != 0 && !(rb == -ENOENT && b.parent))
      rc = rb;
    else if (rb == 0 && (flags & 1))
      rc = -EEXIST; /* RENAME_NOREPLACE */
    else if (a.name_len == 0 || b.name_len == 0)
      rc = -EBUSY;
  } else if (rc == EXT2_EXTERNAL) {
    rc = -EXDEV;
  }
  int mutated = 0;
  if (rc == 0) {
    rc = ext2_rename(&g_fs, a.parent, a.name, a.name_len, b.parent, b.name,
                     b.name_len);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_link(char *from, char *to) {
  struct ext2_res a, b;
  lock();
  int rc = resolve(from, 0, &a);
  if (rc == EXT2_EXTERNAL) rc = -EXDEV;
  if (rc == 0) {
    int rb = ext2_resolve(&g_fs, EXT2V_MNT, to, 0, &b);
    if (rb == EXT2_EXTERNAL)
      rc = -EXDEV;
    else if (rb == 0)
      rc = -EEXIST;
    else if (!(rb == -ENOENT && b.parent))
      rc = rb;
  }
  int mutated = 0;
  if (rc == 0) {
    rc = ext2_link(&g_fs, a.ino, b.parent, b.name, b.name_len);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_symlink(const char *target, char *linkpath) {
  int tl = v_len(target);
  struct ext2_res r;
  lock();
  int rc = resolve(linkpath, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    rc = -EEXIST;
  } else if (rc == -ENOENT && r.parent) {
    uint32_t ino;
    rc = ext2_symlink(&g_fs, r.parent, r.name, r.name_len, target, tl, &ino);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_readlink(char *abs, char *buf, int cap) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  if (rc == 0) rc = ext2_readlink(&g_fs, r.ino, buf, cap);
  else if (rc == EXT2_EXTERNAL) rc = -EINVAL; /* FAT16/NFS hold no symlinks */
  unlock();
  return rc; /* a length: 1 is a real answer ("/"), never EXT2_EXTERNAL */
}

int ext2v_truncate(char *abs, int64_t len) {
  if (len < 0) return -EINVAL;
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 1, &r);
  int mutated = 0;
  if (rc == 0) {
    uint64_t osz = 0;
    (void)ext2_file_size(&g_fs, r.ino, &osz);
    rc = ext2_truncate(&g_fs, r.ino, (uint64_t)len);
    if (rc == 0) ext2_obj_note_truncate_locked(r.ino, osz, (uint64_t)len);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_chdir(char *abs) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 1, &r);
  unlock();
  if (rc) return rc;
  if ((r.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;
  v_copy(abs, r.path, VFS_PATH_MAX);
  return 0;
}

int ext2v_read_dir(char *abs, int index, char *name, int ncap, uint8_t *attr,
                   uint32_t *size) {
  struct ext2_res r;
  char nm[EXT2_NAME_MAX + 1];
  lock();
  int rc = resolve(abs, 1, &r);
  if (rc) {
    unlock();
    return rc;
  }
  uint64_t pos = 0;
  int seen = 0;
  for (;;) {
    uint32_t ino;
    uint8_t ft;
    int nl;
    rc = ext2_readdir(&g_fs, r.ino, &pos, &ino, &ft, nm, &nl);
    if (rc <= 0) {
      unlock();
      return -1;
    }
    if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.'))
      continue;
    if (seen++ < index) continue;
    struct ext2_stat es;
    es.size = 0;
    (void)ext2_stat(&g_fs, ino, &es);
    unlock();
    v_copy(name, nm, ncap);
    if (attr) *attr = (ft == EXT2_FT_DIR) ? 0x10 : 0x20;
    if (size) *size = es.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)es.size;
    return 0;
  }
}

int ext2v_statfs(struct ext2_statfs *out) {
  if (!g_mounted) return -ENODEV;
  lock();
  int rc = ext2_statfs(&g_fs, out);
  unlock();
  return rc;
}

/* ---- cp/ext2b: setattr / access ------------------------------------------ */

static void sa_to_attr(const struct vfs_setattr *sa, struct ext2_attr *a) {
  a->valid = 0;
  a->mode = sa->mode;
  a->uid = sa->uid;
  a->gid = sa->gid;
  a->atime = sa->atime_sec; /* nanoseconds dropped: ext2 stores seconds */
  a->mtime = sa->mtime_sec;
  if (sa->valid & VFS_SA_MODE) a->valid |= EXT2_ATTR_MODE;
  if (sa->valid & VFS_SA_UID) a->valid |= EXT2_ATTR_UID;
  if (sa->valid & VFS_SA_GID) a->valid |= EXT2_ATTR_GID;
  if (sa->valid & VFS_SA_ATIME) a->valid |= EXT2_ATTR_ATIME;
  if (sa->valid & VFS_SA_MTIME) a->valid |= EXT2_ATTR_MTIME;
  if (sa->valid & VFS_SA_ATIME_NOW) a->valid |= EXT2_ATTR_ATIME_NOW;
  if (sa->valid & VFS_SA_MTIME_NOW) a->valid |= EXT2_ATTR_MTIME_NOW;
}

int ext2v_setattr(char *abs, int follow, const struct vfs_setattr *sa) {
  struct ext2_attr a;
  sa_to_attr(sa, &a);
  struct ext2_res r;
  lock();
  int rc = resolve(abs, follow, &r);
  int mutated = 0;
  if (rc == 0) {
    rc = ext2_setattr(&g_fs, r.ino, &a);
    mutated = (a.valid != 0);
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_fsetattr(struct file *f, const struct vfs_setattr *sa) {
  struct ext2_attr a;
  sa_to_attr(sa, &a);
  lock();
  int rc = ext2_setattr(&g_fs, f->ext2.ino, &a);
  unlock_flush(a.valid != 0);
  return rc;
}

int ext2v_access(char *abs, int mode, int follow) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, follow, &r);
  unlock();
  if (rc) return rc;
  if ((mode & 2) && g_fs.ro) return -EROFS; /* W_OK */
  if ((mode & 1) && (r.mode & EXT2_S_IFMT) != EXT2_S_IFDIR &&
      !(r.mode & 0111))
    return -EACCES; /* X_OK: root still needs one x bit on a file */
  return 0;
}

/* ---- cp/ext2b: exec images ------------------------------------------------ */

int ext2v_read_image(char *abs, uint8_t **buf_out, uint32_t *size_out,
                     uint32_t *pages_out, uint32_t max) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 1, &r);
  if (rc) {
    unlock();
    return rc; /* EXT2_EXTERNAL: abs now names the target off the volume */
  }
  if ((r.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) rc = -EISDIR;
  else if ((r.mode & EXT2_S_IFMT) != EXT2_S_IFREG) rc = -EACCES;
  else if (!(r.mode & 0111)) rc = -EACCES; /* execve: root needs an x bit */
  uint64_t size = 0;
  if (rc == 0) rc = ext2_file_size(&g_fs, r.ino, &size);
  if (rc == 0 && (size == 0 || size > max)) rc = -ENOEXEC;
  uint64_t buf = 0;
  uint32_t pages = 0;
  if (rc == 0) {
    pages = (uint32_t)((size + FRAME_SIZE - 1) / FRAME_SIZE);
    buf = frame_alloc_contig((int)pages);
    if (!buf) rc = -ENOMEM;
  }
  if (rc == 0) {
    int64_t n = ext2_read(&g_fs, r.ino, 0, phys_to_virt(buf), size);
    if (n != (int64_t)size) rc = -EIO;
  }
  unlock();
  if (rc) {
    for (uint32_t i = 0; buf && i < pages; i++)
      frame_free(buf + (uint64_t)i * FRAME_SIZE);
    return rc;
  }
  *buf_out = (uint8_t *)phys_to_virt(buf);
  *size_out = (uint32_t)size;
  *pages_out = pages;
  return 0;
}
