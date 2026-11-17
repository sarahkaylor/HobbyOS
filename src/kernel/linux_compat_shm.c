/*
 * src/kernel/linux_compat_shm.c - System V shared memory (lane C4-x3,
 * docs/cyberpunk.md §7.2e; plan in docs/cp-x2.md §6, result in
 * docs/cp-x3.md).
 *
 * Why: MIT-SHM.  Wine's winex11 builds its window surfaces with
 * XShmCreateImage + shmget(IPC_PRIVATE) + shmat + XShmAttach, and the X server
 * (XHOBBY.BIN, a NATIVE HobbyOS process) shmat()s the same id to read the
 * pixels without a copy through the socket.  So the segment table is one
 * kernel table shared by two syscall ABIs:
 *
 *   compat (x86_64 Linux): shmget 29, shmat 30, shmctl 31, shmdt 67
 *                          (l_tab_shm, dispatched by linux_compat.c)
 *   native:                SYS_SHM (op, a1, a2, a3) -- one row, see
 *                          src/include/syscall.h; XHOBBY.BIN's wrappers are in
 *                          third_party/x11/ddx/compat/hbxcompat.c
 *
 * Backing: a segment is one VM_OBJ_MEMFD object (vm_object_create), the same
 * page-cache object memfd_create uses, so every attach in every process maps
 * the same frames (coherent; KUSER_SHARED_DATA and Wine's memfd sections
 * already rely on that).  An attach is a VMK_SHARED region over the object
 * (vm_region_alloc_hole), exactly what mmap(MAP_SHARED) of a memfd installs;
 * shmdt is munmap of that region.  This file only calls vm.c's public API.
 *
 * Lifetime (Linux semantics):
 *   - the table holds ONE object reference per live segment;
 *   - every region record holds its own (vm.c's rule), so fork() and exit()
 *     keep the attach count right with no hook here:
 *         shm_nattch = object refs - 1 (the table's);
 *   - IPC_RMID marks the segment destroyed: its key becomes IPC_PRIVATE (no
 *     shmget can find it again) and it is reclaimed once nattch reaches 0.
 *     Until then shmat on the id still works, as on Linux (winex11 calls
 *     IPC_RMID right after XShmAttach + XSync, before the server's shmat
 *     could be guaranteed in a racier client).  Reclaim is checked at every
 *     shm syscall and at IPC_RMID itself, so memory of a dead segment is
 *     released at the next shm call after its last detach.
 *
 * Not modelled: SHM_LOCK/UNLOCK (accepted, no-op), SHM_HUGETLB (ignored),
 * SHM_REMAP over an existing mapping (EINVAL), IPC_INFO/SHM_INFO/SHM_STAT
 * (EINVAL), per-user limits.  HobbyOS has one identity (uid/gid 0), so the
 * permission check is the owner bits against uid 0, like Linux root.
 */
#include <stdint.h>

#include "arch/trap.h"
#include "errno.h"
#include "kline.h"
#include "linux_compat.h"
#include "linux_syscall.h"
#include "lock.h"
#include "process.h"
#include "vm.h"

/* x86_64 Linux numbers (arch/x86/entry/syscalls/syscall_64.tbl).  Kept here,
   not in linux_syscall.h, so this lane touches no shared header. */
#ifndef L_SYS_shmget
#define L_SYS_shmget 29
#endif
#ifndef L_SYS_shmat
#define L_SYS_shmat 30
#endif
#ifndef L_SYS_shmctl
#define L_SYS_shmctl 31
#endif
#ifndef L_SYS_shmdt
#define L_SYS_shmdt 67
#endif

/* <sys/ipc.h> / <sys/shm.h> values (Linux, all arches). */
#define SHM_IPC_PRIVATE 0
#define SHM_IPC_CREAT 01000
#define SHM_IPC_EXCL 02000
#define SHM_IPC_RMID 0
#define SHM_IPC_SET 1
#define SHM_IPC_STAT 2
#define SHM_IPC_64 0x100 /* glibc may OR it into cmd */
#define SHM_LOCK_CMD 11
#define SHM_UNLOCK_CMD 12
#define SHM_RDONLY 010000
#define SHM_RND 020000
#define SHM_REMAP 040000
#define SHM_EXEC 0100000
#define SHM_LBA 0x1000ULL

/* Table size: winex11 keeps one segment per window surface (plus the
   server's own CheckForShmSyscall probe); a game session has a handful. */
#define SHM_SEGS 256
#define SHM_IDX_BITS 8 /* id = (seq << 8) | idx, always >= 0 */
#define SHM_MAX_BYTES VM_OBJ_MAX_BYTES /* one object's ceiling (1 GiB) */

/* struct shmid64_ds as x86_64 Linux lays it out (112 bytes; glibc's and
   musl's struct shmid_ds are the same bytes).  The native wrappers in
   hbxcompat.c use the same layout on both HobbyOS arches.  Filled byte-wise
   by shm_put*: no struct is ever copied with a compiler-chosen layout. */
#define SHMDS_SIZE 112
#define SHMDS_KEY 0
#define SHMDS_UID 4
#define SHMDS_GID 8
#define SHMDS_CUID 12
#define SHMDS_CGID 16
#define SHMDS_MODE 20
#define SHMDS_SEQ 24
#define SHMDS_SEGSZ 48
#define SHMDS_ATIME 56
#define SHMDS_DTIME 64
#define SHMDS_CTIME 72
#define SHMDS_CPID 80
#define SHMDS_LPID 84
#define SHMDS_NATTCH 88

struct shm_seg {
  int used;
  int rmid;        /* IPC_RMID seen: key gone, reclaim at nattch 0 */
  int32_t key;
  uint16_t seq;    /* bumped per slot reuse: a stale id is EINVAL */
  uint32_t mode;   /* low 9 bits */
  uint32_t uid, gid, cuid, cgid;
  int32_t cpid, lpid;
  uint64_t size;   /* bytes asked for (shm_segsz) */
  int64_t atime, dtime, ctime;
  struct vm_object *obj; /* the table's reference */
};

static struct shm_seg shm_tab[SHM_SEGS];
static spinlock_t shm_lock;

extern uint64_t hb_clock_realtime_ms(void);
static int64_t shm_now(void) { return (int64_t)(hb_clock_realtime_ms() / 1000ULL); }

static int shm_id_of(int idx) {
  return (int)(((uint32_t)shm_tab[idx].seq << SHM_IDX_BITS) | (uint32_t)idx);
}

/* Slot for a live id, or -1.  Caller holds shm_lock. */
static int shm_lookup_locked(int id) {
  if (id < 0)
    return -1;
  int idx = id & ((1 << SHM_IDX_BITS) - 1);
  if (idx >= SHM_SEGS || !shm_tab[idx].used)
    return -1;
  if (shm_id_of(idx) != id)
    return -1;
  return idx;
}

static int shm_nattch_locked(const struct shm_seg *s) {
  int r = *(volatile int *)&s->obj->refs;
  return r > 1 ? r - 1 : 0;
}

/* Release every destroyed segment nobody maps any more.  The references are
   dropped after shm_lock is released (vm_object_unref takes vm_lock and may
   free frames). */
static void shm_reap(void) {
  struct vm_object *drop[8];
  int n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&shm_lock);
  for (int i = 0; i < SHM_SEGS && n < 8; i++) {
    struct shm_seg *s = &shm_tab[i];
    if (s->used && s->rmid && shm_nattch_locked(s) == 0) {
      drop[n++] = s->obj;
      s->obj = 0;
      s->used = 0;
    }
  }
  spinlock_release_irqrestore(&shm_lock, fl);
  for (int i = 0; i < n; i++)
    vm_object_unref(drop[i]);
}

static void shm_row(const char *what, struct process *p, int64_t a, int64_t b,
                    int64_t r) {
  static int rows;
  if (rows >= 24)
    return;
  rows++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[SHM] ");
  kl_s(&kl, what);
  kl_s(&kl, " pid=");
  kl_i(&kl, p ? p->pid : -1);
  kl_s(&kl, " a=");
  kl_i(&kl, (long)a);
  kl_s(&kl, " b=");
  kl_hex(&kl, (unsigned long)b);
  kl_s(&kl, " -> ");
  kl_i(&kl, (long)r);
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* ---- the four operations (ABI-neutral; return value or -errno) -------- */

static int64_t shm_get(struct process *p, int32_t key, uint64_t size, int flg) {
  shm_reap();
  if (size > SHM_MAX_BYTES)
    return -EINVAL;
  uint64_t fl = spinlock_acquire_irqsave(&shm_lock);
  if (key != SHM_IPC_PRIVATE) {
    for (int i = 0; i < SHM_SEGS; i++) {
      struct shm_seg *s = &shm_tab[i];
      if (!s->used || s->rmid || s->key != key)
        continue;
      int64_t r;
      if ((flg & SHM_IPC_CREAT) && (flg & SHM_IPC_EXCL))
        r = -EEXIST;
      else if (size > s->size)
        r = -EINVAL;
      else
        r = shm_id_of(i);
      spinlock_release_irqrestore(&shm_lock, fl);
      return r;
    }
    if (!(flg & SHM_IPC_CREAT)) {
      spinlock_release_irqrestore(&shm_lock, fl);
      return -ENOENT;
    }
  }
  spinlock_release_irqrestore(&shm_lock, fl);
  if (size == 0)
    return -EINVAL; /* SHMMIN is 1 */

  /* The object is created outside shm_lock (it allocates and takes vm_lock).
     Two racing shmget(key, IPC_CREAT) calls are re-checked below. */
  struct vm_object *obj = vm_object_create(VM_OBJ_MEMFD, size);
  if (!obj)
    return -ENOMEM;
  struct process *g = p ? process_group(p) : 0;
  fl = spinlock_acquire_irqsave(&shm_lock);
  if (key != SHM_IPC_PRIVATE) {
    for (int i = 0; i < SHM_SEGS; i++) {
      struct shm_seg *s = &shm_tab[i];
      if (s->used && !s->rmid && s->key == key) {
        int64_t r = (flg & SHM_IPC_EXCL) ? -EEXIST
                    : (size > s->size)   ? -EINVAL
                                         : shm_id_of(i);
        spinlock_release_irqrestore(&shm_lock, fl);
        vm_object_unref(obj);
        return r;
      }
    }
  }
  int idx = -1;
  for (int i = 0; i < SHM_SEGS; i++) {
    if (!shm_tab[i].used) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    spinlock_release_irqrestore(&shm_lock, fl);
    vm_object_unref(obj);
    return -ENOSPC;
  }
  struct shm_seg *s = &shm_tab[idx];
  uint16_t seq = (uint16_t)(s->seq + 1);
  if (seq == 0 || seq > 0x7FFF)
    seq = 1; /* id stays a positive int */
  s->used = 1;
  s->rmid = 0;
  s->key = key;
  s->seq = seq;
  s->mode = (uint32_t)flg & 0777u;
  s->uid = s->gid = s->cuid = s->cgid = 0;
  s->cpid = g ? g->pid : 0;
  s->lpid = 0;
  s->size = size;
  s->atime = s->dtime = 0;
  s->ctime = shm_now();
  s->obj = obj;
  int id = shm_id_of(idx);
  spinlock_release_irqrestore(&shm_lock, fl);
  shm_row("get", p, (int64_t)key, (int64_t)size, id);
  return id;
}

static int64_t shm_at(struct process *p, int id, uint64_t addr, int flg) {
  shm_reap();
  struct process *g = p ? process_group(p) : 0;
  struct addr_space *as = g ? g->as : 0;
  if (!as)
    return -EINVAL; /* a v1 (legacy) address space has no region list */
  if (addr) {
    if (flg & SHM_RND)
      addr &= ~(SHM_LBA - 1);
    if (addr & (SHM_LBA - 1))
      return -EINVAL;
  }
  uint64_t fl = spinlock_acquire_irqsave(&shm_lock);
  int idx = shm_lookup_locked(id);
  if (idx < 0) {
    spinlock_release_irqrestore(&shm_lock, fl);
    return -EINVAL;
  }
  struct shm_seg *s = &shm_tab[idx];
  /* uid 0 is the only identity, and Linux lets root attach whatever the
     mode bits say, so there is no permission check here. */
  struct vm_object *obj = s->obj;
  vm_object_ref(obj); /* held across the insert: a racing RMID cannot free it */
  uint64_t len = (s->size + 0xFFFULL) & ~0xFFFULL;
  spinlock_release_irqrestore(&shm_lock, fl);

  uint16_t prot = VM_PROT_READ;
  if (!(flg & SHM_RDONLY))
    prot |= VM_PROT_WRITE;
  if (flg & SHM_EXEC)
    prot |= VM_PROT_EXEC;
  uint64_t base = 0;
  int rc;
  if (addr) {
    if (!vm_range_is_user(as, addr, len))
      rc = -EINVAL;
    else
      rc = vm_region_alloc_hole(as, addr, addr + len, len, prot, VMK_SHARED,
                                VM_MAP_SHARED, obj, 0, &base);
    if (rc == 0 && base != addr) {
      vm_munmap_range(g, base, len); /* cannot happen: the window is exact */
      rc = -EINVAL;
    }
    if (rc == -ENOMEM)
      rc = -EINVAL; /* occupied (SHM_REMAP is not modelled) */
  } else {
    rc = vm_region_alloc_hole(as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2, len,
                              prot, VMK_SHARED, VM_MAP_SHARED, obj, 0, &base);
  }
  vm_object_unref(obj); /* the region record holds its own reference */
  if (rc != 0) {
    shm_row("at-fail", p, id, (int64_t)len, rc);
    return rc;
  }
  fl = spinlock_acquire_irqsave(&shm_lock);
  if (shm_lookup_locked(id) == idx) {
    s->atime = shm_now();
    s->lpid = g->pid;
  }
  spinlock_release_irqrestore(&shm_lock, fl);
  shm_row("at", p, id, (int64_t)base, (int64_t)len);
  return (int64_t)base;
}

static int64_t shm_dt(struct process *p, uint64_t addr) {
  struct process *g = p ? process_group(p) : 0;
  struct addr_space *as = g ? g->as : 0;
  if (!as || (addr & 0xFFF))
    return -EINVAL;
  struct vm_region *r = vm_region_find(as, addr);
  if (!r || r->base != addr || !r->obj)
    return -EINVAL;
  struct vm_object *obj = r->obj;
  /* Must be one of ours (live or destroyed-but-mapped). */
  uint64_t fl = spinlock_acquire_irqsave(&shm_lock);
  int idx = -1;
  for (int i = 0; i < SHM_SEGS; i++) {
    if (shm_tab[i].used && shm_tab[i].obj == obj) {
      idx = i;
      break;
    }
  }
  uint64_t seglen = 0;
  if (idx >= 0)
    seglen = (shm_tab[idx].size + 0xFFFULL) & ~0xFFFULL;
  spinlock_release_irqrestore(&shm_lock, fl);
  if (idx < 0)
    return -EINVAL;
  /* The attach is the run of regions over this object starting at addr (an
     mprotect may have split it), at most the segment's length. */
  uint64_t end = addr;
  while (end < addr + seglen) {
    struct vm_region *q = vm_region_find(as, end);
    if (!q || q->obj != obj)
      break;
    end = q->base + q->len;
  }
  if (end > addr + seglen)
    end = addr + seglen;
  int rc = vm_munmap_range(g, addr, end - addr);
  if (rc != 0)
    return rc < 0 ? rc : -EINVAL;
  fl = spinlock_acquire_irqsave(&shm_lock);
  if (shm_tab[idx].used && shm_tab[idx].obj == obj) {
    shm_tab[idx].dtime = shm_now();
    shm_tab[idx].lpid = g->pid;
  }
  spinlock_release_irqrestore(&shm_lock, fl);
  shm_reap();
  return 0;
}

static void shm_put32(uint8_t *b, int off, uint32_t v) {
  for (int i = 0; i < 4; i++)
    b[off + i] = (uint8_t)(v >> (8 * i));
}
static void shm_put64(uint8_t *b, int off, uint64_t v) {
  for (int i = 0; i < 8; i++)
    b[off + i] = (uint8_t)(v >> (8 * i));
}
static uint32_t shm_get32(const uint8_t *b, int off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= (uint32_t)b[off + i] << (8 * i);
  return v;
}

static int64_t shm_ctl(struct process *p, int id, int cmd, uint64_t ubuf) {
  shm_reap();
  cmd &= ~SHM_IPC_64;
  uint8_t ds[SHMDS_SIZE];
  if (cmd == SHM_IPC_SET) {
    if (!ubuf)
      return -EFAULT;
    if (l_copy_from_user(p, ds, ubuf, SHMDS_SIZE) != 0)
      return -EFAULT;
  } else if (cmd != SHM_IPC_STAT && cmd != SHM_IPC_RMID &&
             cmd != SHM_LOCK_CMD && cmd != SHM_UNLOCK_CMD) {
    return -EINVAL;
  }
  uint64_t fl = spinlock_acquire_irqsave(&shm_lock);
  int idx = shm_lookup_locked(id);
  if (idx < 0) {
    spinlock_release_irqrestore(&shm_lock, fl);
    return -EINVAL;
  }
  struct shm_seg *s = &shm_tab[idx];
  int64_t r = 0;
  if (cmd == SHM_IPC_STAT) {
    for (int i = 0; i < SHMDS_SIZE; i++)
      ds[i] = 0;
    shm_put32(ds, SHMDS_KEY, (uint32_t)s->key);
    shm_put32(ds, SHMDS_UID, s->uid);
    shm_put32(ds, SHMDS_GID, s->gid);
    shm_put32(ds, SHMDS_CUID, s->cuid);
    shm_put32(ds, SHMDS_CGID, s->cgid);
    /* Linux reports SHM_DEST (01000) once removed. */
    shm_put32(ds, SHMDS_MODE, s->mode | (s->rmid ? 01000u : 0u));
    ds[SHMDS_SEQ] = (uint8_t)s->seq;
    ds[SHMDS_SEQ + 1] = (uint8_t)(s->seq >> 8);
    shm_put64(ds, SHMDS_SEGSZ, s->size);
    shm_put64(ds, SHMDS_ATIME, (uint64_t)s->atime);
    shm_put64(ds, SHMDS_DTIME, (uint64_t)s->dtime);
    shm_put64(ds, SHMDS_CTIME, (uint64_t)s->ctime);
    shm_put32(ds, SHMDS_CPID, (uint32_t)s->cpid);
    shm_put32(ds, SHMDS_LPID, (uint32_t)s->lpid);
    shm_put64(ds, SHMDS_NATTCH, (uint64_t)shm_nattch_locked(s));
  } else if (cmd == SHM_IPC_SET) {
    s->uid = shm_get32(ds, SHMDS_UID);
    s->gid = shm_get32(ds, SHMDS_GID);
    s->mode = shm_get32(ds, SHMDS_MODE) & 0777u;
    s->ctime = shm_now();
  } else if (cmd == SHM_IPC_RMID) {
    s->rmid = 1;
    s->key = SHM_IPC_PRIVATE;
    s->ctime = shm_now();
  }
  spinlock_release_irqrestore(&shm_lock, fl);
  if (cmd == SHM_IPC_STAT) {
    if (!ubuf || l_copy_to_user(p, ubuf, ds, SHMDS_SIZE) != 0)
      return -EFAULT;
  }
  if (cmd == SHM_IPC_RMID) {
    shm_row("rmid", p, id, 0, 0);
    shm_reap();
  }
  return r;
}

/* ---- native ABI: SYS_SHM (op, a1, a2, a3) ------------------------------ */

/* op: 0 shmget(key, size, flg)  1 shmat(id, addr, flg)  2 shmdt(addr)
       3 shmctl(id, cmd, buf*).  Called from both arches' trap.c. */
int64_t hb_shm_native(struct process *p, int op, uint64_t a1, uint64_t a2,
                      uint64_t a3) {
  switch (op) {
  case 0:
    return shm_get(p, (int32_t)a1, a2, (int)a3);
  case 1:
    return shm_at(p, (int)a1, a2, (int)a3);
  case 2:
    return shm_dt(p, a1);
  case 3:
    return shm_ctl(p, (int)a1, (int)a2, a3);
  default:
    return -EINVAL;
  }
}

/* ---- compat table ------------------------------------------------------- */

static int l_shm_ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

static int l_shm_shmget(struct process *p, struct trap_frame *tf) {
  return l_shm_ret(tf, shm_get(p, (int32_t)l_arg(tf, 0), l_arg(tf, 1),
                               (int)l_arg(tf, 2)));
}
static int l_shm_shmat(struct process *p, struct trap_frame *tf) {
  return l_shm_ret(tf, shm_at(p, (int)l_arg(tf, 0), l_arg(tf, 1),
                              (int)l_arg(tf, 2)));
}
static int l_shm_shmdt(struct process *p, struct trap_frame *tf) {
  return l_shm_ret(tf, shm_dt(p, l_arg(tf, 0)));
}
static int l_shm_shmctl(struct process *p, struct trap_frame *tf) {
  return l_shm_ret(tf, shm_ctl(p, (int)l_arg(tf, 0), (int)l_arg(tf, 1),
                               l_arg(tf, 2)));
}

const struct l_sysent l_tab_shm[] = {
  {L_SYS_shmget, l_shm_shmget, "shmget"},
  {L_SYS_shmat, l_shm_shmat, "shmat"},
  {L_SYS_shmctl, l_shm_shmctl, "shmctl"},
  {L_SYS_shmdt, l_shm_shmdt, "shmdt"},
  L_SYSENT_END
};
