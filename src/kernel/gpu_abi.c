/* /dev/gpu ioctl surface (M1, docs/gpu-abi.md).
 *
 * This TU is the only place that translates user pointers/structs into the
 * kernel 3D transport.  It owns the client table that binds a context and
 * its resources to one (process-group pid, fd) pair, so a client can never
 * submit against another client's context and a closed fd releases exactly
 * its own objects.
 *
 * Everything device-facing lives in virtio_gpu_3d.c (x64).  On ARM, or on
 * an x64 host whose GPU offered no VIRTIO_GPU_F_VIRGL, every 3D request
 * answers -ENODEV and the node stays harmless.
 *
 * The pure helpers at the top (request sizes, geometry validation, page
 * count) are unit-tested in virtio_gpu_test.c.
 */
#include "gpu_abi.h"
#include "virtio_gpu.h"
#include "process.h"
#include "vm.h"
#include "errno.h"
#include "lock.h"

/* M1 limits, mirrored in docs/gpu-abi.md. */
#define GPU_ABI_MAX_CLIENTS 8
#define GPU_ABI_MAX_RES_PER_CLIENT 32
#define GPU_ABI_MAX_PAGES 16384          /* == the transport's page pool */
#define GPU_ABI_MAX_BLOB 16384
#define GPU_ABI_MAX_SUBMIT (128 * 1024)
#define GPU_ABI_MAX_DIM 16384
#define GPU_ABI_MAX_LAYERS 2048
#define GPU_ABI_MAX_BYTES (64ull * 1024 * 1024)

struct gpu_abi_client {
  int used;
  int pid; /* process-group pid (threads share ownership) */
  int fd;
  uint32_t ctx_id;
  uint32_t res_ids[GPU_ABI_MAX_RES_PER_CLIENT];
  int nres;
};

static struct gpu_abi_client gpu_clients[GPU_ABI_MAX_CLIENTS];
static spinlock_t gpu_abi_lock; /* zero-initialised == unlocked */
static uint64_t gpu_pages[GPU_ABI_MAX_PAGES] __attribute__((aligned(4096)));
static uint8_t gpu_submit_buf[GPU_ABI_MAX_SUBMIT] __attribute__((aligned(4096)));
static uint8_t gpu_blob_buf[GPU_ABI_MAX_BLOB] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ */
/* arch gate: the 3D transport exists on x64 only (M7 is x64-only).     */
/* ARM keeps the node and the ABI surface, answering -ENODEV.          */
/* ------------------------------------------------------------------ */
#ifdef __x86_64__
int gpu_abi_present(void) { return virtio_gpu_3d_available(); }
static int g3d_capset_cached(const void** b, uint32_t* n, uint32_t* id, uint32_t* v) {
  return virtio_gpu_3d_capset_cached(b, n, id, v);
}
static int g3d_capset_info(uint32_t* id, uint32_t* mv, uint32_t* ms) {
  return virtio_gpu_3d_capset_info(id, mv, ms);
}
static int g3d_ctx_create(uint32_t id, uint32_t ver, const char* name, uint32_t* out) {
  return virtio_gpu_3d_ctx_create(id, ver, name, out);
}
static int g3d_ctx_destroy(uint32_t ctx) { return virtio_gpu_3d_ctx_destroy(ctx); }
static int g3d_res_create(uint32_t ctx, const struct virtio_gpu_3d_res* d, uint32_t* out) {
  return virtio_gpu_3d_res_create(ctx, d, out);
}
static int g3d_res_attach(uint32_t res, const uint64_t* phys, uint32_t n, uint64_t len) {
  return virtio_gpu_3d_res_attach(res, phys, n, len);
}
static int g3d_ctx_attach(uint32_t ctx, uint32_t res) {
  return virtio_gpu_3d_ctx_attach(ctx, res);
}
static int g3d_res_unref(uint32_t res) { return virtio_gpu_3d_res_unref(res); }
static int g3d_submit(uint32_t ctx, const void* cmds, uint32_t len, uint32_t* fence) {
  return virtio_gpu_3d_submit(ctx, cmds, len, fence);
}
static int g3d_submit_unfenced(uint32_t ctx, const void* cmds, uint32_t len) {
  return virtio_gpu_3d_submit_unfenced(ctx, cmds, len);
}
static int g3d_wait(uint32_t fence) { return virtio_gpu_3d_wait(fence); }
static int g3d_transfer(uint32_t res, int from_host, const struct virtio_gpu_box* b,
                        uint32_t level, uint32_t stride, uint32_t lstride, uint64_t off) {
  return virtio_gpu_3d_transfer(res, from_host, b, level, stride, lstride, off);
}
#else
int gpu_abi_present(void) { return 0; }
static int g3d_capset_cached(const void** b, uint32_t* n, uint32_t* id, uint32_t* v) {
  (void)b; (void)n; (void)id; (void)v;
  return -ENODEV;
}
static int g3d_capset_info(uint32_t* id, uint32_t* mv, uint32_t* ms) {
  (void)id; (void)mv; (void)ms;
  return -ENODEV;
}
static int g3d_ctx_create(uint32_t id, uint32_t ver, const char* name, uint32_t* out) {
  (void)id; (void)ver; (void)name; (void)out;
  return -ENODEV;
}
static int g3d_ctx_destroy(uint32_t ctx) { (void)ctx; return -ENODEV; }
static int g3d_res_create(uint32_t ctx, const struct virtio_gpu_3d_res* d, uint32_t* out) {
  (void)ctx; (void)d; (void)out;
  return -ENODEV;
}
static int g3d_res_attach(uint32_t res, const uint64_t* phys, uint32_t n, uint64_t len) {
  (void)res; (void)phys; (void)n; (void)len;
  return -ENODEV;
}
static int g3d_res_unref(uint32_t res) { (void)res; return -ENODEV; }
static int g3d_submit(uint32_t ctx, const void* cmds, uint32_t len, uint32_t* fence) {
  (void)ctx; (void)cmds; (void)len; (void)fence;
  return -ENODEV;
}
static int g3d_submit_unfenced(uint32_t ctx, const void* cmds, uint32_t len) {
  (void)ctx; (void)cmds; (void)len;
  return -ENODEV;
}
static int g3d_wait(uint32_t fence) { (void)fence; return -ENODEV; }
static int g3d_transfer(uint32_t res, int from_host, const struct virtio_gpu_box* b,
                        uint32_t level, uint32_t stride, uint32_t lstride, uint64_t off) {
  (void)res; (void)from_host; (void)b; (void)level; (void)stride; (void)lstride; (void)off;
  return -ENODEV;
}
#endif

/* ------------------------------------------------------------------ */
/* pure helpers (unit-tested)                                         */
/* ------------------------------------------------------------------ */

/* Expected argument size for a request, or -1 for an unknown request. */
int gpu_abi_req_size(unsigned long req) {
  switch (req) {
    case GPU_IOC_GET_CAPSET_INFO: return (int)sizeof(struct gpu_capset_info);
    case GPU_IOC_GET_CAPSET: return (int)sizeof(struct gpu_capset);
    case GPU_IOC_CTX_CREATE: return (int)sizeof(struct gpu_ctx_create);
    case GPU_IOC_CTX_DESTROY: return (int)sizeof(struct gpu_ctx_destroy);
    case GPU_IOC_RES_CREATE: return (int)sizeof(struct gpu_res_create);
    case GPU_IOC_RES_ATTACH: return (int)sizeof(struct gpu_res_attach);
    case GPU_IOC_RES_UNREF: return (int)sizeof(struct gpu_res_unref);
    case GPU_IOC_SUBMIT: return (int)sizeof(struct gpu_submit);
    case GPU_IOC_FENCE_WAIT: return (int)sizeof(struct gpu_fence_wait);
    case GPU_IOC_TRANSFER: return (int)sizeof(struct gpu_transfer);
    default: return -1;
  }
}

uint32_t gpu_abi_pages_for(uint64_t len) {
  if (len == 0) return 0;
  if (len > GPU_ABI_MAX_BYTES) return 0;
  return (uint32_t)((len + 4095) / 4096);
}

/* Resource geometry a virgl host can actually create.  `target`, `format`
 * and `bind` are protocol enums; only the shapes that would make the host
 * allocate absurd memory or divide by zero are rejected here. */
int gpu_abi_res_ok(const struct gpu_res_create* r) {
  if (!r) return 0;
  if (r->ctx_id == 0) return 0;
  if (r->width == 0 || r->height == 0 || r->depth == 0 || r->array_size == 0)
    return 0;
  /* target 0 is legal: it is PIPE_BUFFER == VIRGL_TEXTURE_TARGET_BUFFER, the
   * target every vertex/constant/index buffer uses (M2 measured this as the
   * first thing a real GL client creates).  1D/2D/3D/CUBE are 1..4. */
  if (r->target > 4) return 0;
  int is_buffer = (r->target == 0);
  /* Gallium's width0 for a buffer is its size in bytes, so the texture
   * dimension limit must not be applied to it -- a 1 MiB vertex buffer has
   * width0 = 1048576.  Its ceiling is the backing cap instead. */
  if (is_buffer) {
    if ((uint64_t)r->width > GPU_ABI_MAX_BYTES) return 0;
  } else {
    if (r->width > GPU_ABI_MAX_DIM || r->height > GPU_ABI_MAX_DIM) return 0;
  }
  if (r->depth > GPU_ABI_MAX_LAYERS || r->array_size > GPU_ABI_MAX_LAYERS) return 0;
  if (r->last_level > 31) return 0;
  if (r->nr_samples > 16) return 0;
  if (r->format == 0 || r->bind == 0) return 0;
  /* 4 bytes/texel is the floor for every format the game uses; the cap is
   * the transport's per-resource backing limit, so an oversized request
   * fails at the ioctl instead of at ATTACH_BACKING. */
  uint64_t bytes = is_buffer ? (uint64_t)r->width
                             : (uint64_t)r->width * r->height * r->depth *
                                  r->array_size * 4ull;
  if (bytes == 0 || bytes > GPU_ABI_MAX_BYTES) return 0;
  return 1;
}

int gpu_abi_submit_flags_ok(uint32_t flags) {
  return (flags & ~(uint32_t)GPU_SUBMIT_F_UNFENCED) ? 0 : 1;
}

int gpu_abi_transfer_ok(const struct gpu_transfer* t) {
  if (!t) return 0;
  if (t->res_id == 0) return 0;
  if (t->to_host > 1) return 0;
  if (t->level > 31) return 0;
  if (t->box.w == 0 || t->box.h == 0 || t->box.d == 0) return 0;
  if (t->box.w > GPU_ABI_MAX_DIM || t->box.h > GPU_ABI_MAX_DIM) return 0;
  if (t->offset > GPU_ABI_MAX_BYTES) return 0;
  return 1;
}

/* ------------------------------------------------------------------ */
/* client table                                                       */
/* ------------------------------------------------------------------ */

static struct gpu_abi_client* gpu_client_find(int pid, int fd) {
  for (int i = 0; i < GPU_ABI_MAX_CLIENTS; i++)
    if (gpu_clients[i].used && gpu_clients[i].pid == pid && gpu_clients[i].fd == fd)
      return &gpu_clients[i];
  return 0;
}

static int gpu_client_owns_res(struct gpu_abi_client* c, uint32_t res_id) {
  for (int i = 0; i < c->nres; i++)
    if (c->res_ids[i] == res_id) return 1;
  return 0;
}

static void gpu_client_forget_res(struct gpu_abi_client* c, uint32_t res_id) {
  for (int i = 0; i < c->nres; i++) {
    if (c->res_ids[i] == res_id) {
      c->res_ids[i] = c->res_ids[c->nres - 1];
      c->res_ids[c->nres - 1] = 0;
      c->nres--;
      return;
    }
  }
}

/* Destroy every object this client owns.  MUST be called without the client
 * lock: it sends control commands (its own gpu_lock) and must never nest
 * inside a table lock.  Safe on an already-cleared client. */
static void gpu_client_teardown_unlocked(struct gpu_abi_client* c) {
  c->nres = 0; /* the 3D ctx_destroy reclaims every resource of the ctx */
  if (c->ctx_id) {
    g3d_ctx_destroy(c->ctx_id);
    c->ctx_id = 0;
  }
}

/* fd close / process teardown hook (fs.c).  On ARM this compiles to a
 * lookup that never matches, because no client can ever be created. */
void gpu_abi_release_fd(struct process* p, int fd) {
  if (!p) return;
  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  struct gpu_abi_client snapshot;
  int have = 0;

  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  if (c) {
    snapshot = *c;
    have = 1;
    c->used = 0;
    c->pid = 0;
    c->fd = -1;
    c->ctx_id = 0;
    c->nres = 0;
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (have) gpu_client_teardown_unlocked(&snapshot);
}

/* ------------------------------------------------------------------ */
/* page resolution                                                    */
/* ------------------------------------------------------------------ */

/* Resolve [va, va+len) of `p` into one entry per page.  vm_touch first
 * materialises/validates the range (demand pages become resident), then each
 * page's leaf PTE is read.  v2 address spaces only: v1 processes have no
 * page table to walk, and the 3D clients are all v2. */
static int gpu_resolve_pages(struct process* p, uint64_t va, uint64_t len,
                             uint64_t* phys, uint32_t max_pages, uint32_t* n_out) {
  if (!p || !p->as || !phys || !n_out || len == 0) return -EINVAL;
  if (va < USER_VA_BASE) return -EINVAL;
  if (len > USER_VA_TOP || va > USER_VA_TOP - len) return -EINVAL;
  uint32_t n = gpu_abi_pages_for(len);
  if (n == 0 || n > max_pages) return -EINVAL;

  /* READ|WRITE: the host reads during upload and writes during readback. */
  if (vm_touch(p, va, len, 1) != 0) return -EFAULT;

  uint64_t a = va;
  for (uint32_t i = 0; i < n; i++) {
    uint64_t leaf = 0;
    if (vm_arch_walk(p->as, a & ~(uint64_t)4095, &leaf) != 0) return -EFAULT;
    uint64_t pa = vm_arch_leaf_phys(leaf);
    if (pa == 0) return -EFAULT;
    phys[i] = pa;
    a += 4096;
    if (a < va) return -EINVAL; /* overflow */
  }
  *n_out = n;
  return 0;
}

/* ------------------------------------------------------------------ */
/* ioctl dispatch                                                     */
/* ------------------------------------------------------------------ */

static int gpu_do_capset_info(struct process* p, uint64_t arg) {
  struct gpu_capset_info out;
  const void* blob = 0;
  uint32_t blen = 0, id = 0, ver = 0;
  int rc = g3d_capset_cached(&blob, &blen, &id, &ver);
  if (rc != 0) return -ENODEV;
  out.capset_id = id;
  out.max_version = ver;
  out.max_size = blen;
  out.blob_size = blen;
  return vm_kwrite(p, arg, &out, sizeof(out)) == 0 ? 0 : -EFAULT;
}

static int gpu_do_capset(struct process* p, uint64_t arg) {
  struct gpu_capset cs;
  if (vm_user_read(p, arg, &cs, sizeof(cs)) != 0) return -EFAULT;
  const void* blob = 0;
  uint32_t blen = 0, id = 0, ver = 0;
  int rc = g3d_capset_cached(&blob, &blen, &id, &ver);
  if (rc != 0) return -ENODEV;
  if (!cs.blob || cs.capset_id != id || cs.version != ver) return -EINVAL;
  if (cs.size < blen) return -EINVAL;
  for (uint32_t i = 0; i < blen; i++) gpu_blob_buf[i] = ((const uint8_t*)blob)[i];
  if (vm_kwrite(p, cs.blob, gpu_blob_buf, (int)blen) != 0) return -EFAULT;
  cs.size = blen;
  return vm_kwrite(p, arg, &cs, sizeof(cs)) == 0 ? 0 : -EFAULT;
}

/* Client-table discipline (SEC-OBS-B / lock.h): gpu_abi_lock is a leaf.  It
 * is held ONLY around table reads/writes -- never across vm_touch/vm_user_read
 * (vm_lock sits above it) and never across a control command (gpu_lock).  A
 * concurrent close during an ioctl can therefore destroy the context under
 * us; the 3D layer re-validates every id under its own lock and answers
 * -EINVAL, which is the same answer a late ioctl on a closed fd deserves. */

#define GPU_CTX_RESERVED 0xFFFFFFFFu

static int gpu_do_ctx_create(struct process* p, int fd, uint64_t arg) {
  struct gpu_ctx_create cc;
  if (vm_user_read(p, arg, &cc, sizeof(cc)) != 0) return -EFAULT;
  uint32_t id = 0, maxver = 0, maxsize = 0;
  if (gpu_abi_present() != 1) return -ENODEV;
  if (g3d_capset_info(&id, &maxver, &maxsize) != 0) return -ENODEV;
  if (cc.capset_id != id || cc.capset_version == 0 || cc.capset_version > maxver)
    return -EINVAL;
  cc.name[sizeof(cc.name) - 1] = '\0';

  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  if (c && c->ctx_id != 0) {
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    return -EBUSY;
  }
  if (!c) {
    for (int i = 0; i < GPU_ABI_MAX_CLIENTS; i++)
      if (!gpu_clients[i].used) {
        c = &gpu_clients[i];
        break;
      }
  }
  if (!c) {
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    return -ENOSPC;
  }
  c->used = 1;
  c->pid = pid;
  c->fd = fd;
  c->ctx_id = GPU_CTX_RESERVED; /* creation in flight: a second call is EBUSY */
  c->nres = 0;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);

  int rc = g3d_ctx_create(cc.capset_id, cc.capset_version, cc.name, &id);

  fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  c = gpu_client_find(pid, fd);
  int still_ours = (c && c->ctx_id == GPU_CTX_RESERVED);
  if (rc == 0 && still_ours) {
    c->ctx_id = id;
    cc.ctx_id = id;
  } else {
    if (still_ours) {
      c->used = 0;
      c->pid = 0;
      c->fd = -1;
      c->ctx_id = 0;
    }
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (rc != 0) return -EIO;
  if (!still_ours) {
    /* The fd closed while the context was being created. */
    g3d_ctx_destroy(id);
    return -EBADF;
  }
  return vm_kwrite(p, arg, &cc, sizeof(cc)) == 0 ? 0 : -EFAULT;
}

static int gpu_do_ctx_destroy(struct process* p, int fd, uint64_t arg) {
  struct gpu_ctx_destroy cd;
  if (vm_user_read(p, arg, &cd, sizeof(cd)) != 0) return -EFAULT;
  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  uint32_t ctx_id = 0;

  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  if (!c || c->ctx_id == 0 || c->ctx_id == GPU_CTX_RESERVED ||
      c->ctx_id != cd.ctx_id) {
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    return -EINVAL;
  }
  ctx_id = c->ctx_id;
  c->ctx_id = 0;
  c->nres = 0;
  c->used = 0;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);

  return g3d_ctx_destroy(ctx_id) == 0 ? 0 : -EIO;
}

static int gpu_do_res_create(struct process* p, int fd, uint64_t arg) {
  struct gpu_res_create rc_struct;
  if (vm_user_read(p, arg, &rc_struct, sizeof(rc_struct)) != 0) return -EFAULT;
  if (!gpu_abi_res_ok(&rc_struct)) return -EINVAL;
  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;

  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  if (!c || c->ctx_id == 0 || c->ctx_id == GPU_CTX_RESERVED ||
      c->ctx_id != rc_struct.ctx_id || c->nres >= GPU_ABI_MAX_RES_PER_CLIENT) {
    int full = (c && c->nres >= GPU_ABI_MAX_RES_PER_CLIENT);
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    return full ? -ENOSPC : -EINVAL;
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);

  struct virtio_gpu_3d_res d;
  d.target = rc_struct.target;
  d.format = rc_struct.format;
  d.bind = rc_struct.bind;
  d.width = rc_struct.width;
  d.height = rc_struct.height;
  d.depth = rc_struct.depth;
  d.array_size = rc_struct.array_size;
  d.last_level = rc_struct.last_level;
  d.nr_samples = rc_struct.nr_samples;
  d.flags = rc_struct.flags;
  uint32_t id = 0;
  int rc = g3d_res_create(rc_struct.ctx_id, &d, &id);
  if (rc != 0) return rc == -ENOSPC ? -ENOSPC : -EIO;

  fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  int stored = 0;
  c = gpu_client_find(pid, fd);
  if (c && c->ctx_id == rc_struct.ctx_id && c->nres < GPU_ABI_MAX_RES_PER_CLIENT) {
    c->res_ids[c->nres++] = id;
    stored = 1;
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!stored) {
    g3d_res_unref(id);
    return -EBADF;
  }
  rc_struct.res_id = id;
  return vm_kwrite(p, arg, &rc_struct, sizeof(rc_struct)) == 0 ? 0 : -EFAULT;
}

static int gpu_do_res_attach(struct process* p, int fd, uint64_t arg) {
  struct gpu_res_attach ra;
  if (vm_user_read(p, arg, &ra, sizeof(ra)) != 0) return -EFAULT;
  if (ra.res_id == 0 || ra.len == 0) return -EINVAL;
  uint32_t n = gpu_abi_pages_for(ra.len);
  if (n == 0 || n > GPU_ABI_MAX_PAGES) return -EINVAL;

  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  int owns = c && gpu_client_owns_res(c, ra.res_id);
  uint32_t ctx_id = owns ? c->ctx_id : 0;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!owns) return -EINVAL;

  uint32_t got = 0;
  int rc = gpu_resolve_pages(p, ra.va, ra.len, gpu_pages, GPU_ABI_MAX_PAGES, &got);
  if (rc == 0) rc = g3d_res_attach(ra.res_id, gpu_pages, got, ra.len);
  /* M2 (docs/gpu-abi.md §2 said so, but M1's ioctl path did not do it): a 3D
   * resource is not usable by a context until CTX_ATTACH_RESOURCE (0x0202)
   * binds it.  M1's kernel probe called virtio_gpu_3d_ctx_attach() itself, so
   * the gap never showed; Mesa's first submit was rejected by the host with
   * "Illegal resource 4097" until this line existed. */
  if (rc == 0 && ctx_id) rc = g3d_ctx_attach(ctx_id, ra.res_id);
  if (rc != 0) {
    if (rc == -ENOSPC) return -ENOSPC;
    return rc == -EFAULT ? -EFAULT : -EIO;
  }
  ra.nr_pages = got;
  return vm_kwrite(p, arg, &ra, sizeof(ra)) == 0 ? 0 : -EFAULT;
}

static int gpu_do_res_unref(struct process* p, int fd, uint64_t arg) {
  struct gpu_res_unref ru;
  if (vm_user_read(p, arg, &ru, sizeof(ru)) != 0) return -EFAULT;
  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  int owns = c && gpu_client_owns_res(c, ru.res_id);
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!owns) return -EINVAL;

  int rc = g3d_res_unref(ru.res_id);
  if (rc != 0) return -EIO;
  fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  c = gpu_client_find(pid, fd);
  if (c) gpu_client_forget_res(c, ru.res_id);
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  return 0;
}

static int gpu_do_submit(struct process* p, int fd, uint64_t arg) {
  struct gpu_submit sub;
  if (vm_user_read(p, arg, &sub, sizeof(sub)) != 0) return -EFAULT;
  if (sub.ctx_id == 0 || sub.len == 0 || (sub.len & 3) || sub.len > GPU_ABI_MAX_SUBMIT)
    return -EINVAL;
  if (!gpu_abi_submit_flags_ok(sub.flags)) return -EINVAL;
  if (!sub.va) return -EINVAL;
  if (sub.len > USER_VA_TOP || sub.va > USER_VA_TOP - sub.len) return -EINVAL;

  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  int owns = c && c->ctx_id != 0 && c->ctx_id != GPU_CTX_RESERVED &&
             c->ctx_id == sub.ctx_id;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!owns) return -EINVAL;

  if (vm_touch(p, sub.va, sub.len, 0) != 0) return -EFAULT;
  if (vm_user_read(p, sub.va, gpu_submit_buf, (int)sub.len) != 0) return -EFAULT;

  uint32_t fence = 0;
  int rc;
  if (sub.flags & GPU_SUBMIT_F_UNFENCED) {
    rc = g3d_submit_unfenced(sub.ctx_id, gpu_submit_buf, (uint32_t)sub.len);
    sub.fence_id = 0; /* nothing to wait on */
  } else {
    rc = g3d_submit(sub.ctx_id, gpu_submit_buf, (uint32_t)sub.len, &fence);
    if (rc == 0) sub.fence_id = fence;
  }
  if (rc != 0) return -EIO;
  return vm_kwrite(p, arg, &sub, sizeof(sub)) == 0 ? 0 : -EFAULT;
}

static int gpu_do_fence_wait(struct process* p, int fd, uint64_t arg) {
  struct gpu_fence_wait fw;
  if (vm_user_read(p, arg, &fw, sizeof(fw)) != 0) return -EFAULT;
  if (fw.fence_id == 0) return -EINVAL;
  (void)fd;
  int rc = g3d_wait(fw.fence_id);
  if (rc == -EAGAIN && fw.timeout_ms != 0) rc = -ETIMEDOUT;
  return rc == 0 ? 0 : rc;
}

static int gpu_do_transfer(struct process* p, int fd, uint64_t arg) {
  struct gpu_transfer t;
  if (vm_user_read(p, arg, &t, sizeof(t)) != 0) return -EFAULT;
  if (!gpu_abi_transfer_ok(&t)) return -EINVAL;
  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  int owns = c && gpu_client_owns_res(c, t.res_id);
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!owns) return -EINVAL;

  struct virtio_gpu_box box;
  box.x = t.box.x;
  box.y = t.box.y;
  box.z = t.box.z;
  box.w = t.box.w;
  box.h = t.box.h;
  box.d = t.box.d;
  int rc = g3d_transfer(t.res_id, t.to_host, &box, t.level, t.stride,
                        t.layer_stride, t.offset);
  return rc == 0 ? 0 : -EIO;
}

/* The one entry point fs.c / the compat ioctl path call. */
int gpu_abi_ioctl(struct process* p, int fd, unsigned long req, uint64_t arg) {
  /* Request identity first: an unknown number is ENOTTY even without a
   * process/argument, which is also what makes the routing unit-testable. */
  int sz = gpu_abi_req_size(req);
  if (sz < 0) return -ENOTTY;
  if (sz > (int)sizeof(struct gpu_transfer)) return -EINVAL;
  if (!p || arg == 0) return -EINVAL;
  switch (req) {
    case GPU_IOC_GET_CAPSET_INFO: return gpu_do_capset_info(p, arg);
    case GPU_IOC_GET_CAPSET: return gpu_do_capset(p, arg);
    case GPU_IOC_CTX_CREATE: return gpu_do_ctx_create(p, fd, arg);
    case GPU_IOC_CTX_DESTROY: return gpu_do_ctx_destroy(p, fd, arg);
    case GPU_IOC_RES_CREATE: return gpu_do_res_create(p, fd, arg);
    case GPU_IOC_RES_ATTACH: return gpu_do_res_attach(p, fd, arg);
    case GPU_IOC_RES_UNREF: return gpu_do_res_unref(p, fd, arg);
    case GPU_IOC_SUBMIT: return gpu_do_submit(p, fd, arg);
    case GPU_IOC_FENCE_WAIT: return gpu_do_fence_wait(p, fd, arg);
    case GPU_IOC_TRANSFER: return gpu_do_transfer(p, fd, arg);
    default: return -ENOTTY;
  }
}
