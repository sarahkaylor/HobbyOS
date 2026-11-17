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

/* Limits, mirrored in docs/gpu-abi.md.  M3 raised the two that a Unity frame
 * actually reaches (docs/rimworld-virgl-m3.md §1): the per-client resource
 * table was 32 and the system-wide attached-backing pool 64 MiB -- numbers
 * sized for M1's kernel probe, not for a game with texture atlases, FBOs and
 * a vertex buffer per mesh.
 *
 * The pool is now 512 MiB (the transport's GPU3D_PAGE_POOL) and each client
 * may hold at most GPU_ABI_CLIENT_BYTES of it, so one process cannot starve
 * the others; the per-resource cap stays 64 MiB (a 4096x4096 RGBA atlas, 8x
 * a 1080p render target) because a single ATTACH_BACKING sends one entry per
 * page and 16384 pages is the transport's per-attach ceiling. */
#define GPU_ABI_MAX_CLIENTS 8
/* L80: 1024 was M3's guess and the main menu is where it runs out.  Measured
 * on the merged tree: nres=1024 with `res_create refused` from then on, every
 * frame, for the rest of the run -- the engine cannot allocate the menu's
 * textures/render targets and keeps asking.  4096 covers the 618 resources a
 * scene creates (M3) with room for a menu's atlases and the swap chain, at
 * ~96 bytes of kernel BSS each. */
#define GPU_ABI_MAX_RES_PER_CLIENT 16384
#define GPU_ABI_MAX_PAGES 131072         /* per resource == 512 MiB */
#define GPU_ABI_MAX_BLOB 16384
#define GPU_ABI_MAX_SUBMIT (128 * 1024)
#define GPU_ABI_MAX_DIM 16384
#define GPU_ABI_MAX_LAYERS 2048
/* 64 MiB was chosen against the M3 workload and it refuses the thing a real
 * planet needs: RimWorld's world-gen builds an 8192x4096 RGBA ramp (134 MB,
 * 178 MB with its mip pyramid) and, on the merged tree, every one of those
 * creates was refused as "geometry" and reached the game as
 * GL_OUT_OF_MEMORY.  512 MiB admits the 8192-wide family; GPU_ABI_MAX_PAGES
 * moved with it so ATTACH_BACKING can cover one. */
#define GPU_ABI_MAX_BYTES (512ull * 1024 * 1024)
/* Per-client share of the attached-backing pool.  384 MiB was M3's estimate
 * ("a Unity scene creates 618 resources"); on the merged tree a real RimWorld
 * def load reaches it: `res_attach refused: client backing full ... held=
 * 402653184 nres=1421 cap_bytes=402653184` and then asks ~1900 more times,
 * 4-64 KiB at a time, for the rest of the run -- the engine cannot upload the
 * texture it is loading and the loading screen never advances.  1.5 GiB is the
 * ceiling one client may hold now; the transport's pool (GPU3D_PAGE_POOL) is
 * raised with it.  These are policy numbers, not memory: every attached page
 * is the client's own page.
 *
 * 1.5 GiB still ran out during world generation: RimWorld's process holds
 * 4096 live resources at that point (the per-client table, not the bytes) and
 * the buffers it needs for the globe mesh were refused with ENOSPC, which
 * leaves a GL buffer object with no backing -- `index_bo->buffer == NULL`, so
 * core Mesa skips every draw and the progress bar never moves.  Both ceilings
 * are raised together: 4 GiB and 16384 resources. */
#define GPU_ABI_CLIENT_BYTES (4096ull * 1024 * 1024)

struct gpu_abi_client {
  int used;
  int pid; /* process-group pid (threads share ownership) */
  int fd;
  uint32_t ctx_id;
  uint32_t res_ids[GPU_ABI_MAX_RES_PER_CLIENT];
  uint32_t res_pages[GPU_ABI_MAX_RES_PER_CLIENT]; /* parallel: backing pages */
  int nres;
  uint64_t pages; /* == sum(res_pages[0..nres)), kept for the ENOSPC row */
  uint64_t bytes; /* attached backing in bytes */
};

static struct gpu_abi_client gpu_clients[GPU_ABI_MAX_CLIENTS];
static spinlock_t gpu_abi_lock; /* zero-initialised == unlocked */
static uint64_t gpu_pages[GPU_ABI_MAX_PAGES] __attribute__((aligned(4096)));
static uint8_t gpu_submit_buf[GPU_ABI_MAX_SUBMIT] __attribute__((aligned(4096)));
static uint8_t gpu_blob_buf[GPU_ABI_MAX_BLOB] __attribute__((aligned(16)));

/* Serial rows for refusals (M3): a limit that fires must print the exact
 * request, or the lane cannot say what the ABI is missing. */
extern void uart_puts(const char* s);
extern void print_int(int v);
static void gpu_abi_say(const char* s, int v) {
  uart_puts(s);
  print_int(v);
}

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
static int g3d_ctx_attach(uint32_t ctx, uint32_t res) {
  (void)ctx; (void)res;
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
  /* format 0 is not a format.  bind == 0 IS legal: it is what a buffer has
   * before it is first bound (M3 measured Unity's glBufferData doing exactly
   * that -- 64x1, bind=0x0 -- and M1/M2 rejected it). */
  if (r->format == 0) return 0;
  (void)r->bind;
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
      uint64_t pages = c->res_pages[i];
      c->res_ids[i] = c->res_ids[c->nres - 1];
      c->res_pages[i] = c->res_pages[c->nres - 1];
      c->res_ids[c->nres - 1] = 0;
      c->res_pages[c->nres - 1] = 0;
      c->nres--;
      /* Per-client accounting: the backing this resource held goes back to
       * the client's share as well as to the pool (M3). */
      c->pages = (c->pages >= pages) ? c->pages - pages : 0;
      uint64_t b = pages * 4096ull;
      c->bytes = (c->bytes >= b) ? c->bytes - b : 0;
      return;
    }
  }
}

/* Record the backing a resource just attached, under the client lock. */
static void gpu_client_note_pages(int pid, int fd, uint32_t res_id, uint32_t pages) {
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  if (!c) return;
  for (int i = 0; i < c->nres; i++) {
    if (c->res_ids[i] == res_id) {
      c->res_pages[i] = pages;
      c->pages += pages;
      c->bytes += (uint64_t)pages * 4096ull;
      return;
    }
  }
}

/* Destroy every object this client owns.  MUST be called without the client
 * lock: it sends control commands (its own gpu_lock) and must never nest
 * inside a table lock.  Safe on an already-cleared client. */
static void gpu_client_teardown_unlocked(struct gpu_abi_client* c) {
  c->nres = 0; /* the 3D ctx_destroy reclaims every resource of the ctx */
  c->pages = 0;
  c->bytes = 0;
  if (c->ctx_id) {
    g3d_ctx_destroy(c->ctx_id);
    c->ctx_id = 0;
  }
}

/* fd close / process teardown hook (fs.c).  On ARM this compiles to a
 * lookup that never matches, because no client can ever be created. */
static void gpu_m2_release_maps(struct process* p, int pid, int fd);

void gpu_abi_release_fd(struct process* p, int fd) {
  if (!p) return;
  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;
  gpu_m2_release_maps(p, pid, fd); /* P6: user mappings of this fd's blobs */
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
    c->pages = 0;
    c->bytes = 0;
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
  if (!vm_range_is_user(process_group(p)->as, va, len)) return -EINVAL; /* P1 */
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
  c->pages = 0;
  c->bytes = 0;
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
  c->pages = 0;
  c->bytes = 0;
  c->used = 0;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);

  return g3d_ctx_destroy(ctx_id) == 0 ? 0 : -EIO;
}

static int gpu_do_res_create(struct process* p, int fd, uint64_t arg) {
  struct gpu_res_create rc_struct;
  if (vm_user_read(p, arg, &rc_struct, sizeof(rc_struct)) != 0) return -EFAULT;
  if (!gpu_abi_res_ok(&rc_struct)) {
    /* Name the refusal: a bare -EINVAL here reaches the client as
     * GL_OUT_OF_MEMORY two layers up (M3). */
    gpu_abi_say("[GPU] res_create refused: geometry ctx=", (int)rc_struct.ctx_id);
    gpu_abi_say(" target=", (int)rc_struct.target);
    gpu_abi_say(" fmt=", (int)rc_struct.format);
    gpu_abi_say(" bind=", (int)rc_struct.bind);
    gpu_abi_say(" w=", (int)rc_struct.width);
    gpu_abi_say(" h=", (int)rc_struct.height);
    gpu_abi_say(" d=", (int)rc_struct.depth);
    gpu_abi_say(" arr=", (int)rc_struct.array_size);
    gpu_abi_say(" lvl=", (int)rc_struct.last_level);
    gpu_abi_say(" nsa=", (int)rc_struct.nr_samples);
    gpu_abi_say("\n", 0);
    return -EINVAL;
  }
  struct process* g = process_group(p);
  int pid = g ? g->pid : p->pid;

  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  if (!c || c->ctx_id == 0 || c->ctx_id == GPU_CTX_RESERVED ||
      c->ctx_id != rc_struct.ctx_id || c->nres >= GPU_ABI_MAX_RES_PER_CLIENT) {
    int full = (c && c->nres >= GPU_ABI_MAX_RES_PER_CLIENT);
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    /* L80: bounded.  A full table is a per-frame event (the engine retries
     * every create), and this row is six console writes; unbounded it became
     * 2 298 rows in one 25-minute run and buried every other row in the log.
     * The first four name the shape, then one per 1024 refusals names the
     * rate, and the count is always in the row. */
    {
      static unsigned refused_n;
      unsigned n = ++refused_n;
      if (n <= 4 || (n % 1024) == 0) {
        gpu_abi_say("[GPU] res_create refused: client c=", c ? 1 : 0);
        gpu_abi_say(" nres=", c ? c->nres : -1);
        gpu_abi_say(" ctx_held=", c ? (int)c->ctx_id : -1);
        gpu_abi_say(" ctx_req=", (int)rc_struct.ctx_id);
        gpu_abi_say(" pid=", pid);
        gpu_abi_say(" fd=", fd);
        gpu_abi_say(" refused=", (int)n);
        gpu_abi_say("\n", 0);
      }
    }
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
  int over = (owns && (c->bytes + ra.len > GPU_ABI_CLIENT_BYTES));
  uint64_t held = over ? c->bytes : 0;
  int nres_now = over ? c->nres : 0;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!owns) return -EINVAL;
  if (over) {
    /* Say the exact demand: a per-client refusal must name the resource, the
     * bytes it wants and what the client already holds. */
    gpu_abi_say("[GPU] res_attach refused: client backing full res=", (int)ra.res_id);
    gpu_abi_say(" wants=", (int)ra.len);
    gpu_abi_say(" held=", (int)held);
    gpu_abi_say(" nres=", nres_now);
    gpu_abi_say(" cap_bytes=", (int)GPU_ABI_CLIENT_BYTES);
    gpu_abi_say("\n", 0);
    return -ENOSPC;
  }

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
  fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  gpu_client_note_pages(pid, fd, ra.res_id, got);
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  ra.nr_pages = got;
  return vm_kwrite(p, arg, &ra, sizeof(ra)) == 0 ? 0 : -EFAULT;
}

static void gpu_m2_unref_hook(struct process* p, int pid, int fd, uint32_t res_id);

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
  gpu_m2_unref_hook(p, pid, fd, ru.res_id); /* P6: a mapped blob's user PTEs */

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
  if (!vm_range_is_user(process_group(p)->as, sub.va, sub.len)) return -EINVAL; /* P1 */

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
static int gpu_abi_m2_ioctl(struct process* p, int fd, unsigned long req, uint64_t arg);

int gpu_abi_ioctl(struct process* p, int fd, unsigned long req, uint64_t arg) {
  /* P6: the Venus requests (11..20, docs/gpu-abi.md §10) have their own
   * dispatcher; M1's below is unchanged. */
  if (gpu_abi_m2_req_size(req) >= 0) return gpu_abi_m2_ioctl(p, fd, req, arg);
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

/* ================================================================== */
/* P6 / Cyberpunk F3: the Venus requests (docs/gpu-abi.md §10).        */
/* ================================================================== */

int gpu_abi_m2_req_size(unsigned long req) {
  switch (req) {
    case GPU_IOC_GET_PARAM: return (int)sizeof(struct gpu_param);
    case GPU_IOC_GET_CAPSET_INFO2: return (int)sizeof(struct gpu_capset_info2);
    case GPU_IOC_GET_CAPSET2: return (int)sizeof(struct gpu_capset2);
    case GPU_IOC_CTX_CREATE2: return (int)sizeof(struct gpu_ctx_create2);
    case GPU_IOC_RES_CREATE_BLOB: return (int)sizeof(struct gpu_res_create_blob);
    case GPU_IOC_RES_MAP_BLOB: return (int)sizeof(struct gpu_res_map_blob);
    case GPU_IOC_RES_UNMAP_BLOB: return (int)sizeof(struct gpu_res_unmap_blob);
    case GPU_IOC_SUBMIT2: return (int)sizeof(struct gpu_submit2);
    case GPU_IOC_FENCE_WAIT2: return (int)sizeof(struct gpu_fence_wait2);
    case GPU_IOC_SCANOUT_BLOB: return 64;
    default: return -1;
  }
}

int gpu_abi_ctx2_ok(const struct gpu_ctx_create2* c, uint32_t max_rings) {
  if (!c) return 0;
  if (c->capset_id == 0 || c->capset_id > 0xFF) return 0;
  if (c->num_rings == 0 || c->num_rings > max_rings || c->num_rings > GPU_MAX_RINGS)
    return 0;
  if (c->flags != 0) return 0;
  return 1;
}

int gpu_abi_blob_ok(const struct gpu_res_create_blob* b) {
  if (!b) return 0;
  if (b->ctx_id == 0) return 0;
  if (b->blob_mem != GPU_BLOB_MEM_HOST3D) return 0; /* v2: host blobs only */
  if (b->blob_flags & ~(uint32_t)(GPU_BLOB_FLAG_MAPPABLE | GPU_BLOB_FLAG_SHAREABLE |
                                  GPU_BLOB_FLAG_CROSS_DEVICE))
    return 0;
  if (b->size == 0 || b->size > (64ull << 30)) return 0; /* > the largest window */
  if (b->cmd_size) {
    if (!b->cmd || (b->cmd_size & 3u) || b->cmd_size > GPU_ABI_MAX_SUBMIT) return 0;
  }
  return 1;
}

int gpu_abi_submit2_ok(const struct gpu_submit2* s, uint32_t num_rings) {
  if (!s) return 0;
  if (s->ctx_id == 0) return 0;
  if (s->flags & ~(uint32_t)GPU_SUBMIT2_F_FENCE) return 0;
  if (s->ring_idx >= num_rings || s->ring_idx >= GPU_MAX_RINGS) return 0;
  if ((s->cs_size & 3u) || s->cs_size > GPU_ABI_MAX_SUBMIT) return 0;
  if (s->cs_size && !s->cs) return 0;
  if (!s->cs_size && !(s->flags & GPU_SUBMIT2_F_FENCE)) return 0; /* nothing to do */
  return 1;
}

int gpu_abi_map_cache_mode(uint32_t map_info) {
  switch (map_info & 0x0Fu) {
    case GPU_MAP_CACHE_CACHED: return 0; /* WB */
    case GPU_MAP_CACHE_WC: return 1;     /* WC */
    default: return 2;                   /* UNCACHED, NONE, unknown: UC */
  }
}

uint64_t gpu_abi_deadline_ms(uint64_t now_ms, uint64_t timeout_ns) {
  if (timeout_ns == ~0ull) return ~0ull;
  uint64_t ms = timeout_ns / 1000000ull;
  if (timeout_ns % 1000000ull) ms++; /* never earlier than asked */
  if (ms == 0) ms = 1;
  if (now_ms > ~0ull - ms) return ~0ull;
  return now_ms + ms;
}

#ifdef __x86_64__
#include "virtio_gpu_blob.h"
#include "frame.h"
#include "kmem.h"
extern uint64_t timer_get_ms(void);
extern uint64_t timer_get_us(void);
/* Kick-poll budget of one FENCE_WAIT2 before it parks (see there). */
#ifndef GPU_M2_KICK_SPIN_US
#define GPU_M2_KICK_SPIN_US 4000
#endif

/* One user mapping of a blob (for teardown at fd close). */
#define GPU_M2_MAX_MAPS 4096
struct gpu_m2_map {
  int used;
  int pid; /* process group */
  int fd;
  uint32_t res_id;
  uint64_t va, len;
};
static struct gpu_m2_map* gpu_m2_maps; /* frame-allocated (~160 KiB), see gpu_m2_init */
static uint32_t gpu_m2_map_count, gpu_m2_map_races, gpu_m2_unmap_count; /* P8 stop 11 */
/* Rings per context (index = client slot); 0 = not a CTX_CREATE2 context. */
static uint32_t gpu_m2_rings[GPU_ABI_MAX_CLIENTS];

/* Copy-in staging for command streams and capsets: one blocking lock, never
 * held across a park (FENCE_WAIT2 does not use it). */
static uint8_t* gpu_m2_buf; /* frame-allocated 128 KiB, see gpu_m2_init */
static ylock_t gpu_m2_buf_lock;
static volatile int gpu_m2_buf_state; /* 0 uninit, 1 init in progress, 2 ready */

/* One-time setup on the first Venus request: the staging lock, and the
 * staging buffer + mapping table as frame allocations (the kernel image's
 * static footprint is budgeted, docs/blk-cache.md §11).  0, or -ENOMEM. */
static void gpu_m2_free_run(uint64_t pa, int n) {
  for (int i = 0; i < n; i++) frame_free(pa + (uint64_t)i * 4096);
}

static int gpu_m2_init(void) {
  int st = __atomic_load_n(&gpu_m2_buf_state, __ATOMIC_ACQUIRE);
  if (st == 2) return 0;
  if (st == 3) return -ENOMEM;
  int e = 0;
  if (__atomic_compare_exchange_n(&gpu_m2_buf_state, &e, 1, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    ylock_init(&gpu_m2_buf_lock);
    int nb = (int)((GPU_ABI_MAX_SUBMIT + 4095) / 4096);
    int nm = (int)((sizeof(struct gpu_m2_map) * GPU_M2_MAX_MAPS + 4095) / 4096);
    uint64_t pb = frame_alloc_contig(nb), pm = frame_alloc_contig(nm);
    if (!pb || !pm) {
      if (pb) gpu_m2_free_run(pb, nb);
      if (pm) gpu_m2_free_run(pm, nm);
      uart_puts("[VENUS] gpu_abi: frame allocation failed\n");
      __atomic_store_n(&gpu_m2_buf_state, 3, __ATOMIC_RELEASE);
      return -ENOMEM;
    }
    gpu_m2_buf = (uint8_t*)phys_to_virt(pb);
    struct gpu_m2_map* m = (struct gpu_m2_map*)phys_to_virt(pm);
    for (int i = 0; i < GPU_M2_MAX_MAPS; i++) m[i].used = 0;
    __atomic_store_n(&gpu_m2_maps, m, __ATOMIC_RELEASE);
    __atomic_store_n(&gpu_m2_buf_state, 2, __ATOMIC_RELEASE);
    return 0;
  }
  while ((st = __atomic_load_n(&gpu_m2_buf_state, __ATOMIC_ACQUIRE)) == 1 || st == 0)
    __asm__ volatile("pause");
  return st == 2 ? 0 : -ENOMEM;
}

static void gpu_m2_buf_acquire(void) { ylock_acquire(&gpu_m2_buf_lock); }

static void gpu_m2_buf_release(void) { ylock_release(&gpu_m2_buf_lock); }

static int gpu_m2_pid(struct process* p) {
  struct process* g = process_group(p);
  return g ? g->pid : p->pid;
}

/* The caller's context id and its ring count, if it owns `ctx_id`. */
static int gpu_m2_owns_ctx(struct process* p, int fd, uint32_t ctx_id, uint32_t* rings) {
  int pid = gpu_m2_pid(p);
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  int ok = c && c->ctx_id == ctx_id && ctx_id != 0 && ctx_id != GPU_CTX_RESERVED;
  if (ok && rings) *rings = gpu_m2_rings[c - gpu_clients];
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  return ok;
}

static int gpu_m2_owns_res(struct process* p, int fd, uint32_t res_id) {
  int pid = gpu_m2_pid(p);
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  int ok = c && gpu_client_owns_res(c, res_id);
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  return ok;
}

static int gpu_m2_get_param(struct process* p, uint64_t arg) {
  struct gpu_param gp;
  if (vm_user_read(p, arg, &gp, sizeof(gp)) != 0) return -EFAULT;
  switch (gp.param) {
    case GPU_PARAM_ABI_VERSION: gp.value = GPU_ABI_VERSION_VENUS; break;
    case GPU_PARAM_FEATURES: gp.value = (uint64_t)virtio_gpu_3d_features(); break;
    case GPU_PARAM_HOST_VISIBLE_SIZE:
      gp.value = (virtio_gpu_3d_features() & GPU_FEAT_HOST_VISIBLE) ? virtio_gpu_x64_shm_len : 0;
      break;
    case GPU_PARAM_MAX_RINGS: gp.value = GPU_MAX_RINGS; break;
    case GPU_PARAM_NUM_CAPSETS: gp.value = (uint64_t)virtio_gpu_3d_num_capsets(); break;
    case GPU_PARAM_FENCE_SLOTS: gp.value = VGPU_ASYNC_SLOTS; break;
    default: return -EINVAL;
  }
  return vm_kwrite(p, arg, &gp, sizeof(gp)) == 0 ? 0 : -EFAULT;
}

static int gpu_m2_capset_info2(struct process* p, uint64_t arg) {
  struct gpu_capset_info2 ci;
  if (vm_user_read(p, arg, &ci, sizeof(ci)) != 0) return -EFAULT;
  int rc = virtio_gpu_3d_capset_at(ci.index, &ci.capset_id, &ci.max_version, &ci.max_size);
  if (rc != 0) return rc;
  return vm_kwrite(p, arg, &ci, sizeof(ci)) == 0 ? 0 : -EFAULT;
}

static int gpu_m2_capset2(struct process* p, uint64_t arg) {
  struct gpu_capset2 cs;
  if (vm_user_read(p, arg, &cs, sizeof(cs)) != 0) return -EFAULT;
  if (!cs.blob || cs.size == 0) return -EINVAL;
  uint32_t cap = cs.size > GPU_ABI_MAX_BLOB ? GPU_ABI_MAX_BLOB : cs.size;
  uint32_t n = 0;
  gpu_m2_buf_acquire();
  int rc = virtio_gpu_3d_capset_get(cs.capset_id, cs.version, gpu_m2_buf, cap, &n);
  if (rc == 0 && vm_kwrite(p, cs.blob, gpu_m2_buf, (int)n) != 0) rc = -EFAULT;
  gpu_m2_buf_release();
  if (rc != 0) return rc;
  cs.size = n;
  return vm_kwrite(p, arg, &cs, sizeof(cs)) == 0 ? 0 : -EFAULT;
}

static int gpu_m2_ctx_create2(struct process* p, int fd, uint64_t arg) {
  struct gpu_ctx_create2 cc;
  if (vm_user_read(p, arg, &cc, sizeof(cc)) != 0) return -EFAULT;
  if (gpu_abi_present() != 1) return -ENODEV;
  if (!(virtio_gpu_3d_features() & GPU_FEAT_CONTEXT_INIT)) return -ENODEV;
  if (!gpu_abi_ctx2_ok(&cc, GPU_MAX_RINGS)) return -EINVAL;
  cc.name[sizeof(cc.name) - 1] = '\0';
  int pid = gpu_m2_pid(p);

  /* Reserve the client slot exactly as CTX_CREATE does (one ctx per fd). */
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  if (c && c->ctx_id != 0) {
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    return -EBUSY;
  }
  if (!c)
    for (int i = 0; i < GPU_ABI_MAX_CLIENTS; i++)
      if (!gpu_clients[i].used) {
        c = &gpu_clients[i];
        break;
      }
  if (!c) {
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    return -ENOSPC;
  }
  c->used = 1;
  c->pid = pid;
  c->fd = fd;
  c->ctx_id = GPU_CTX_RESERVED;
  c->nres = 0;
  c->pages = 0;
  c->bytes = 0;
  gpu_m2_rings[c - gpu_clients] = 0;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);

  uint32_t id = 0;
  int rc = virtio_gpu_3d_ctx_create2(cc.capset_id, cc.num_rings, cc.name, &id);

  fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  c = gpu_client_find(pid, fd);
  int still_ours = (c && c->ctx_id == GPU_CTX_RESERVED);
  if (rc == 0 && still_ours) {
    c->ctx_id = id;
    gpu_m2_rings[c - gpu_clients] = cc.num_rings;
  } else if (still_ours) {
    c->used = 0;
    c->pid = 0;
    c->fd = -1;
    c->ctx_id = 0;
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (rc != 0) return rc == -EINVAL ? -EINVAL : -EIO;
  if (!still_ours) {
    g3d_ctx_destroy(id);
    return -EBADF;
  }
  cc.ctx_id = id;
  return vm_kwrite(p, arg, &cc, sizeof(cc)) == 0 ? 0 : -EFAULT;
}

static int gpu_m2_res_create_blob(struct process* p, int fd, uint64_t arg) {
  struct gpu_res_create_blob rb;
  if (vm_user_read(p, arg, &rb, sizeof(rb)) != 0) return -EFAULT;
  if (!gpu_abi_blob_ok(&rb)) {
    gpu_abi_say("[GPU] blob_create refused: mem=", (int)rb.blob_mem);
    gpu_abi_say(" flags=", (int)rb.blob_flags);
    gpu_abi_say(" size_k=", (int)(rb.size >> 10));
    gpu_abi_say(" cmd_size=", (int)rb.cmd_size);
    uart_puts("\n");
    return -EINVAL;
  }
  if (!gpu_m2_owns_ctx(p, fd, rb.ctx_id, 0)) return -EINVAL;
  int pid = gpu_m2_pid(p);
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  struct gpu_abi_client* c = gpu_client_find(pid, fd);
  int full = !c || c->nres >= GPU_ABI_MAX_RES_PER_CLIENT;
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (full) return -ENOSPC;

  uint32_t id = 0;
  int rc;
  if (rb.cmd_size) {
    gpu_m2_buf_acquire();
    if (vm_user_read(p, rb.cmd, gpu_m2_buf, (int)rb.cmd_size) != 0)
      rc = -EFAULT;
    else
      rc = virtio_gpu_3d_blob_create(rb.ctx_id, rb.blob_mem, rb.blob_flags, rb.blob_id,
                                     rb.size, gpu_m2_buf, rb.cmd_size, &id);
    gpu_m2_buf_release();
  } else {
    rc = virtio_gpu_3d_blob_create(rb.ctx_id, rb.blob_mem, rb.blob_flags, rb.blob_id,
                                   rb.size, 0, 0, &id);
  }
  if (rc != 0) return rc == -EFAULT ? -EFAULT : (rc == -ENOSPC ? -ENOSPC : -EIO);

  fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  c = gpu_client_find(pid, fd);
  int stored = 0;
  if (c && c->ctx_id == rb.ctx_id && c->nres < GPU_ABI_MAX_RES_PER_CLIENT) {
    c->res_ids[c->nres] = id;
    c->res_pages[c->nres] = 0; /* host memory: nothing counted against the pool */
    c->nres++;
    stored = 1;
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!stored) {
    g3d_res_unref(id);
    return -EBADF;
  }
  rb.res_id = id;
  return vm_kwrite(p, arg, &rb, sizeof(rb)) == 0 ? 0 : -EFAULT;
}

/* Install [phys, phys+size) into the caller's address space with the
 * memory type map_info asks for; the leaves are VMK_FB-class (shared, never
 * freed by teardown).  Returns 0 + *va. */
static int gpu_m2_map_user(struct process* p, uint64_t phys, uint64_t size,
                           uint32_t map_info, uint64_t* va) {
  struct process* g = process_group(p);
  struct addr_space* as = g ? g->as : p->as;
  if (!as || as->ver != AS_V2) return -EFAULT;
  uint64_t lo = USER_MMAP_BASE_V2, hi = USER_MMAP_LIMIT_V2;
  if (as->layout == VM_LAYOUT_COMPAT) {
    lo = 0x700000000000ull;
    hi = 0x7f0000000000ull;
  }
  uint64_t base = 0;
  if (vm_region_alloc_hole(as, lo, hi, size, VM_PROT_READ | VM_PROT_WRITE, VMK_FB,
                           VM_MAP_SHARED, 0, 0, &base) != 0)
    return -ENOMEM;
  int mode = gpu_abi_map_cache_mode(map_info);
  for (uint64_t off = 0; off < size; off += 4096) {
    if (vm_map_page(as, base + off, phys + off, VM_PROT_READ | VM_PROT_WRITE, VMK_FB) != 0 ||
        (mode != 0 && vm_arch_set_cache(as, base + off, mode) != 0)) {
      vm_munmap_range(g ? g : p, base, size);
      return -ENOMEM;
    }
  }
  *va = base;
  return 0;
}

static int gpu_m2_res_map_blob(struct process* p, int fd, uint64_t arg) {
  struct gpu_res_map_blob mb;
  if (vm_user_read(p, arg, &mb, sizeof(mb)) != 0) return -EFAULT;
  if (mb.res_id == 0 || !gpu_m2_owns_res(p, fd, mb.res_id)) return -EINVAL;
  int pid = gpu_m2_pid(p);
  /* Already mapped by this client: same answer. */
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  for (int i = 0; i < GPU_M2_MAX_MAPS; i++) {
    struct gpu_m2_map* m = &gpu_m2_maps[i];
    if (m->used && m->pid == pid && m->fd == fd && m->res_id == mb.res_id) {
      mb.va = m->va;
      mb.size = m->len;
      spinlock_release_irqrestore(&gpu_abi_lock, fl);
      uint64_t ph = 0, sz = 0;
      virtio_gpu_3d_blob_map(mb.res_id, &ph, &sz, &mb.map_info); /* already mapped: no-op */
      return vm_kwrite(p, arg, &mb, sizeof(mb)) == 0 ? 0 : -EFAULT;
    }
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);

  uint64_t phys = 0, size = 0;
  uint32_t mi = 0;
  int rc = virtio_gpu_3d_blob_map(mb.res_id, &phys, &size, &mi);
  if (rc != 0) return rc;
  uint64_t va = 0;
  rc = gpu_m2_map_user(p, phys, size, mi, &va);
  if (rc != 0) {
    virtio_gpu_3d_blob_unmap(mb.res_id);
    return rc;
  }
  int slot = -1;
  uint64_t won_va = 0, won_len = 0;
  fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  /* Two threads can map the same blob at once (venus' bo_map is "not
   * thread-safe but fine" on DRM, where both mmaps stay valid).  Keep ONE
   * record per (client, blob): the loser drops its own PTEs and returns the
   * winner's VA, so an unmap can never leave a second live mapping behind on
   * a window extent that is about to be reused. */
  for (int i = 0; i < GPU_M2_MAX_MAPS; i++) {
    struct gpu_m2_map* m = &gpu_m2_maps[i];
    if (m->used && m->pid == pid && m->fd == fd && m->res_id == mb.res_id) {
      won_va = m->va;
      won_len = m->len;
      break;
    }
  }
  for (int i = 0; !won_va && i < GPU_M2_MAX_MAPS; i++)
    if (!gpu_m2_maps[i].used) {
      slot = i;
      gpu_m2_maps[i].used = 1;
      gpu_m2_maps[i].pid = pid;
      gpu_m2_maps[i].fd = fd;
      gpu_m2_maps[i].res_id = mb.res_id;
      gpu_m2_maps[i].va = va;
      gpu_m2_maps[i].len = size;
      break;
    }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (won_va) {
    struct process* g = process_group(p);
    vm_munmap_range(g ? g : p, va, size);
    gpu_m2_map_races++;
    mb.va = won_va;
    mb.size = won_len;
    mb.map_info = mi;
    return vm_kwrite(p, arg, &mb, sizeof(mb)) == 0 ? 0 : -EFAULT;
  }
  if (slot < 0) {
    struct process* g = process_group(p);
    vm_munmap_range(g ? g : p, va, size);
    virtio_gpu_3d_blob_unmap(mb.res_id);
    return -ENOSPC;
  }
  gpu_m2_map_count++;
  static int map_rows = 0;
  if (map_rows++ < 16) {
    gpu_abi_say("[VENUS] user map res=", (int)mb.res_id);
    gpu_abi_say(" size_k=", (int)(size >> 10));
    gpu_abi_say(" map_info=", (int)mi);
    gpu_abi_say(" mode=", gpu_abi_map_cache_mode(mi));
    uart_puts(" va=");
    extern void uart_print_hex(uint64_t val);
    uart_print_hex(va);
    uart_puts("\n");
  }
  mb.va = va;
  mb.size = size;
  mb.map_info = mi;
  return vm_kwrite(p, arg, &mb, sizeof(mb)) == 0 ? 0 : -EFAULT;
}

/* P8 stop 11 (fault dump, arch/x64/trap.c): every live blob mapping of the
 * faulting process group, so a register that points into one is named. */
void gpu_m2_dump_maps(struct process* p) {
  if (!p || !__atomic_load_n(&gpu_m2_maps, __ATOMIC_ACQUIRE)) return;
  int pid = gpu_m2_pid(p), n = 0;
  uint64_t bytes = 0, lo = ~0ull, hi = 0;
  extern void uart_print_hex(uint64_t val);
  for (int i = 0; i < GPU_M2_MAX_MAPS; i++) {
    struct gpu_m2_map* m = &gpu_m2_maps[i];
    if (!m->used || m->pid != pid) continue;
    n++;
    bytes += m->len;
    if (m->va < lo) lo = m->va;
    if (m->va + m->len > hi) hi = m->va + m->len;
    if (n <= 48) {
      uart_puts("[SMAP] res=");
      print_int((int)m->res_id);
      uart_puts(" va=");
      uart_print_hex(m->va);
      uart_puts(" len=");
      uart_print_hex(m->len);
      uart_puts("\n");
    }
  }
  gpu_abi_say("[SMAP] maps=", (int)gpu_m2_map_count);
  gpu_abi_say(" unmaps=", (int)gpu_m2_unmap_count);
  gpu_abi_say(" races=", (int)gpu_m2_map_races);
  gpu_abi_say(" live=", n);
  gpu_abi_say(" mib=", (int)(bytes >> 20));
  uart_puts(" span=");
  uart_print_hex(n ? lo : 0);
  uart_puts("..");
  uart_print_hex(hi);
  uart_puts("\n");
}

/* Drop this client's user mapping of res_id (if any): PTEs, then the record. */
static void gpu_m2_drop_map(struct process* p, int pid, int fd, uint32_t res_id) {
  uint64_t va = 0, len = 0;
  if (!__atomic_load_n(&gpu_m2_maps, __ATOMIC_ACQUIRE)) return; /* no Venus request ran */
  uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
  for (int i = 0; i < GPU_M2_MAX_MAPS; i++) {
    struct gpu_m2_map* m = &gpu_m2_maps[i];
    if (m->used && m->pid == pid && m->fd == fd && m->res_id == res_id) {
      va = m->va;
      len = m->len;
      m->used = 0;
      break;
    }
  }
  spinlock_release_irqrestore(&gpu_abi_lock, fl);
  if (!va) return;
  gpu_m2_unmap_count++;
  struct process* g = process_group(p);
  struct process* owner = g ? g : p;
  if (owner && owner->as) {
    struct vm_region* r = vm_region_find(owner->as, va);
    if (r && r->kind == VMK_FB) vm_munmap_range(owner, va, len);
  }
}

static void gpu_m2_unref_hook(struct process* p, int pid, int fd, uint32_t res_id) {
  gpu_m2_drop_map(p, pid, fd, res_id);
}

static int gpu_m2_res_unmap_blob(struct process* p, int fd, uint64_t arg) {
  struct gpu_res_unmap_blob ub;
  if (vm_user_read(p, arg, &ub, sizeof(ub)) != 0) return -EFAULT;
  if (ub.res_id == 0 || !gpu_m2_owns_res(p, fd, ub.res_id)) return -EINVAL;
  gpu_m2_drop_map(p, gpu_m2_pid(p), fd, ub.res_id);
  return virtio_gpu_3d_blob_unmap(ub.res_id) == 0 ? 0 : -EIO;
}

static void gpu_m2_release_maps(struct process* p, int pid, int fd) {
  if (!__atomic_load_n(&gpu_m2_maps, __ATOMIC_ACQUIRE)) return;
  for (;;) {
    uint32_t res = 0;
    uint64_t fl = spinlock_acquire_irqsave(&gpu_abi_lock);
    for (int i = 0; i < GPU_M2_MAX_MAPS; i++)
      if (gpu_m2_maps[i].used && gpu_m2_maps[i].pid == pid && gpu_m2_maps[i].fd == fd) {
        res = gpu_m2_maps[i].res_id;
        break;
      }
    spinlock_release_irqrestore(&gpu_abi_lock, fl);
    if (!res) return;
    gpu_m2_drop_map(p, pid, fd, res);
  }
}

static int gpu_m2_submit2(struct process* p, int fd, uint64_t arg) {
  struct gpu_submit2 sb;
  if (vm_user_read(p, arg, &sb, sizeof(sb)) != 0) return -EFAULT;
  uint32_t rings = 0;
  if (!gpu_m2_owns_ctx(p, fd, sb.ctx_id, &rings) || rings == 0) return -EINVAL;
  if (!gpu_abi_submit2_ok(&sb, rings)) return -EINVAL;
  if (sb.cs_size && !vm_range_is_user(process_group(p)->as, sb.cs, sb.cs_size))
    return -EINVAL;
  int fenced = (sb.flags & GPU_SUBMIT2_F_FENCE) ? 1 : 0;
  uint64_t seq = 0;
  int rc;
  if (sb.cs_size) {
    gpu_m2_buf_acquire();
    if (vm_user_read(p, sb.cs, gpu_m2_buf, (int)sb.cs_size) != 0)
      rc = -EFAULT;
    else
      rc = virtio_gpu_3d_submit2(sb.ctx_id, sb.ring_idx, fenced, gpu_m2_buf, sb.cs_size, &seq);
    gpu_m2_buf_release();
  } else {
    rc = virtio_gpu_3d_submit2(sb.ctx_id, sb.ring_idx, fenced, 0, 0, &seq);
  }
  if (rc != 0) return rc == -EFAULT ? -EFAULT : (rc == -EINVAL ? -EINVAL : -EIO);
  sb.seqno = seq;
  return vm_kwrite(p, arg, &sb, sizeof(sb)) == 0 ? 0 : -EFAULT;
}

static int gpu_m2_fence_wait2(struct process* p, int fd, uint64_t arg) {
  struct gpu_fence_wait2 fw;
  virtio_gpu_3d_fence_unpark(); /* P8 stop 11: a restart is not parked */
  if (vm_user_read(p, arg, &fw, sizeof(fw)) != 0) return -EFAULT;
  uint32_t rings = 0;
  if (!gpu_m2_owns_ctx(p, fd, fw.ctx_id, &rings) || fw.ring_idx >= rings || fw.seqno == 0)
    return -EINVAL;
  int rc = virtio_gpu_3d_fence_check(fw.ctx_id, fw.ring_idx, fw.seqno);
  if (rc != -EAGAIN) return rc;
  if (fw.timeout_ns == 0) return -ETIMEDOUT;
  /* Kick-poll before parking (docs/gpu-abi.md §10.5): QEMU retires a finished
   * fence when it handles a queue notification, otherwise only from its 10 ms
   * poll timer.  Spin with kicks for up to GPU_M2_KICK_SPIN_US (once per wait,
   * not again after a restart), then park for the IRQ. */
  if (fw.deadline_ms == 0) {
    uint64_t t0 = timer_get_us(), budget = GPU_M2_KICK_SPIN_US;
    if (fw.timeout_ns / 1000ull < budget) budget = fw.timeout_ns / 1000ull;
    while (timer_get_us() - t0 < budget) {
      rc = virtio_gpu_3d_fence_kick_check(fw.ctx_id, fw.ring_idx, fw.seqno);
      if (rc != -EAGAIN) return rc;
      for (int k = 0; k < 2000; k++) __asm__ volatile("pause");
    }
  }
  uint64_t now = timer_get_ms();
  if (fw.deadline_ms == 0) {
    /* First park: fix the deadline in the caller's struct so the restarted
     * syscall keeps it (docs/gpu-abi.md §10.5). */
    fw.deadline_ms = gpu_abi_deadline_ms(now, fw.timeout_ns);
    if (vm_kwrite(p, arg, &fw, sizeof(fw)) != 0) return -EFAULT;
  } else if (fw.deadline_ms != ~0ull && now >= fw.deadline_ms) {
    /* A fence that never retires is the one thing a hang report needs. */
    static int to_rows = 0;
    if (to_rows++ < 16) {
      uint64_t issued = 0, done = 0;
      int inflight = 0;
      virtio_gpu_3d_fence_state(fw.ctx_id, fw.ring_idx, &issued, &done, &inflight);
      gpu_abi_say("[VENUS] fence_wait2 timeout pid=", gpu_m2_pid(p));
      gpu_abi_say(" ctx=", (int)fw.ctx_id);
      gpu_abi_say(" ring=", (int)fw.ring_idx);
      uart_puts(" seqno=");
      extern void uart_print_hex(uint64_t val);
      uart_print_hex(fw.seqno);
      uart_puts(" issued=");
      uart_print_hex(issued);
      uart_puts(" done=");
      uart_print_hex(done);
      gpu_abi_say(" ctx_inflight=", inflight);
      uart_puts("\n");
    }
    return -ETIMEDOUT;
  }
  /* A restarted wait (deadline already set): drain + kick before parking
   * again.  The device's INTx can be lost -- on q35 it shares line 10 with the
   * legacy virtio-net, and an edge-mode PIC drops the GPU's edge while the
   * net holds the line (measured: a d3d12tri waiter parked 2 s on a fence the
   * used ring had long retired).  So a parked waiter also re-checks every
   * tick; the IRQ remains the fast path. */
  if (fw.deadline_ms != 0) {
    rc = virtio_gpu_3d_fence_kick_check(fw.ctx_id, fw.ring_idx, fw.seqno);
    if (rc != -EAGAIN) {
      if (rc == 0) virtio_gpu_3d_note_tick_retire();
      return rc;
    }
    /* Still pending after >= 2 s: dump the context's fence state (rate-limited)
     * -- the stall report an infinite WSI wait otherwise never produces. */
    static struct { int tid; uint64_t seqno, first_ms; } seen[32];
    static uint64_t last_dump_ms;
    static int dumps;
    int k, slot = -1;
    for (k = 0; k < 32; k++)
      if (seen[k].tid == p->pid && seen[k].seqno == fw.seqno) break;
    if (k == 32) {
      for (k = 0; k < 32; k++)
        if (seen[k].tid == 0 || now - seen[k].first_ms > 60000) { slot = k; break; }
      if (slot < 0) slot = (int)(now % 32);
      seen[slot].tid = p->pid;
      seen[slot].seqno = fw.seqno;
      seen[slot].first_ms = now;
      k = slot;
    }
    if (now - seen[k].first_ms >= 2000 && now - last_dump_ms >= 1000 && dumps < 64) {
      last_dump_ms = now;
      dumps++;
      gpu_abi_say("[VENUS] stall pid=", gpu_m2_pid(p));
      gpu_abi_say(" tid=", p->pid);
      gpu_abi_say(" waits_ms=", (int)(now - seen[k].first_ms));
      gpu_abi_say(" ctx=", (int)fw.ctx_id);
      gpu_abi_say(" ring=", (int)fw.ring_idx);
      uart_puts(" seqno=");
      extern void uart_print_hex(uint64_t val);
      uart_print_hex(fw.seqno);
      uart_puts("\n");
      virtio_gpu_3d_fence_dump(fw.ctx_id);
    }
  }
  /* Park until the IRQ wakes us, the deadline, or the next tick. */
  uint64_t wake = now + 1;
  if (fw.deadline_ms != ~0ull && fw.deadline_ms < wake) wake = fw.deadline_ms;
  return virtio_gpu_3d_fence_park(fw.ctx_id, fw.ring_idx, fw.seqno, wake);
}

static int gpu_abi_m2_ioctl(struct process* p, int fd, unsigned long req, uint64_t arg) {
  if (!p || arg == 0) return -EINVAL;
  if (gpu_abi_present() != 1) return -ENODEV;
  if (gpu_m2_init() != 0) return -ENOMEM;
  switch (req) {
    case GPU_IOC_GET_PARAM: return gpu_m2_get_param(p, arg);
    case GPU_IOC_GET_CAPSET_INFO2: return gpu_m2_capset_info2(p, arg);
    case GPU_IOC_GET_CAPSET2: return gpu_m2_capset2(p, arg);
    case GPU_IOC_CTX_CREATE2: return gpu_m2_ctx_create2(p, fd, arg);
    case GPU_IOC_RES_CREATE_BLOB: return gpu_m2_res_create_blob(p, fd, arg);
    case GPU_IOC_RES_MAP_BLOB: return gpu_m2_res_map_blob(p, fd, arg);
    case GPU_IOC_RES_UNMAP_BLOB: return gpu_m2_res_unmap_blob(p, fd, arg);
    case GPU_IOC_SUBMIT2: return gpu_m2_submit2(p, fd, arg);
    case GPU_IOC_FENCE_WAIT2: return gpu_m2_fence_wait2(p, fd, arg);
    case GPU_IOC_SCANOUT_BLOB: return -ENOSYS;
    default: return -ENOTTY;
  }
}
#else
static void gpu_m2_release_maps(struct process* p, int pid, int fd) {
  (void)p; (void)pid; (void)fd;
}
static void gpu_m2_unref_hook(struct process* p, int pid, int fd, uint32_t res_id) {
  (void)p; (void)pid; (void)fd; (void)res_id;
}
static int gpu_abi_m2_ioctl(struct process* p, int fd, unsigned long req, uint64_t arg) {
  (void)p; (void)fd; (void)req; (void)arg;
  return -ENODEV; /* ARM: no 3D transport */
}
#endif
