/* ============================================================================
 *  /dev/nvshim: the nouveau-subset device ABI, served before GSP-RM exists.
 *
 *  This TU is the only place that translates user structs and user pointers
 *  into RM operations.  It owns the client table that binds every object a
 *  client holds to one (process-group pid, fd) pair, so one process can never
 *  submit on, bind, or free another's objects, and closing the fd destroys
 *  exactly what that client created.
 *
 *  The backend is src/kernel/nv_rm_mock.c behind struct nv_rm_if (nv_rm.h).
 *  Nothing here touches hardware, so the node is present and useful on every
 *  machine -- including an ARM build with no card, which is why this file has
 *  no __x86_64__ guard (unlike the driver proper in nv.c).  A client that
 *  needs to know whether real hardware is behind it asks DEVICE_ALLOC and
 *  reads `mock`.
 *
 *  Deliberately NOT here: GSP-RM objects, and every row in the refusal table of
 *  nv_shim.h -- each answered -ENOSYS and named on the console, because a
 *  missing handler that silently succeeds is the failure mode this project
 *  shoots itself in the foot with.
 *
 *  CPU-MAPPED BOs (WP-F3).  A GEM object whose domain asks for a CPU mapping
 *  (CPU | MAPPABLE) is backed by pool RAM the RM owns, and mmap(2) of this node
 *  maps THOSE frames into the client: the mapping is the object, not a copy of
 *  it.  The offset names the object (map_handle = handle << 12, DRM's
 *  drm_vma_node_offset_addr spelling), so no new ioctl number was invented --
 *  DRM has never had a mmap request row, and adding one would have been a
 *  second way to say the same thing.
 * ========================================================================== */

#include "nv_shim.h"
#include "nv_drm_uapi.h" /* nouveau's real 'd' surface (round 2) */
#include "nv_rm.h"

#include "arch/cpu.h" /* get_cpuid for the per-CPU row buffer */
#include "errno.h"
#include "fs.h" /* WP-PRIME: the dma-buf fd (file_dmabuf_install/_obj) */
#include "lock.h"
#include "process.h"
#include "timer.h"
#include "vm.h"

extern void uart_puts(const char *s);
extern void print_int(int v);
extern void uart_print_hex(uint64_t v);

#define SHIM_MAX_CLIENTS 8
#define SHIM_MAX_ALLOCS 32
#define SHIM_MAX_VMS 4
#define SHIM_MAX_CHANNELS 8
#define SHIM_MAX_SYNCOBJS NVSHIM_DRM_MAX_SYNCOBJS
#define SHIM_MAX_SIGPEND 32
/* WP-NVIF: objects in one client's NVIF tree.  A stock winsys probes with one
 * context (1 device + up to 5 engine objects), destroys it, then makes one per
 * real context, so 24 leaves room for a handful of live contexts and still
 * bounds the table. */
#define SHIM_MAX_NVIF 24
/* Policy ceilings, mirrored in nv_shim.h's comment: a single allocation below
 * the mock arena's size, a per-client total well below it so one client cannot
 * exhaust the mock. */
#define SHIM_MAX_ALLOC_BYTES (512ull * 1024 * 1024)
#define SHIM_MAX_VM_SIZE (4ull << 30)
#define SHIM_PAGE 4096ull
/* "creation in flight" sentinel, the gpu_abi.c RESERVED pattern: a second
 * request that finds it answers -EBUSY/-EINVAL instead of double-creating. */
#define SHIM_RESERVED 0xFFFFFFFFu
#define SHIM_FENCE_WAIT_MAX_MS 1000u
#define SHIM_WAIT_SPINS 100000 /* hard bound on FENCE_WAIT's poll loop */

struct shim_sigpend {
  uint32_t sobj;  /* a syncobj this client owns */
  uint32_t ch;    /* the channel whose fence must retire first */
  uint64_t fence;
};

/* WP-F3: one live CPU mapping of a GEM object.  Tracked so GEM_CLOSE (or the
 * fd going away) can take the mapping down BEFORE the RM frees the frames --
 * a leaf that outlives its frame would point at whatever the allocator hands
 * out next.  Linux keeps the object alive while a VMA refers to it; the mock
 * has no object refcount, so the order is inverted instead, and a test asserts
 * the mapping is gone after GEM_CLOSE.
 *
 * WP-PRIME: `prime`/`owner` extend the same record to mappings made through a
 * dma-buf fd.  A mapping is still not itself a reference (this kernel has no
 * munmap hook to learn a VMA went away), so it is dropped by its OWNER going
 * away -- the node fd's handle, or the dma-buf fd -- or by the object reaching
 * refs == 0, whichever comes first. */
struct shim_map {
  int used;
  int pid;   /* process GROUP pid (tgid): the AS to unmap from */
  int fd;    /* the user fd that made it: the node, or the dma-buf */
  uint64_t handle;
  uint64_t va;
  uint64_t len;
  int prime;   /* 1 = made through a dma-buf fd */
  void *owner; /* prime: the struct shim_dmabuf it was made from */
};

/* WP-NVIF: one object in a client's NVIF tree (nv_nvif_uapi.h).  `cookie` is
 * the CLIENT's value, taken from nvshim_nvif_new.object: nouveau keys objects
 * by exactly that field (nouveau_abi16_obj_find(ioctl->object)), which is why
 * a winsys may pass (uintptr_t)its own struct and name it again later.  No
 * kernel address is stored or handed out. */
struct shim_nvif_obj {
  uint64_t cookie;
  int32_t oclass; /* the class it was created as (NVSHIM_NVIF_CLASS_*) */
  uint32_t ch;    /* channel it lives under; 0 = the client's root (device) */
};

struct shim_client {
  int used;
  int pid; /* process-group pid: threads share ownership */
  int fd;
  uint32_t dev_id; /* SHIM_RESERVED while DEVICE_ALLOC is in flight */
  struct nv_rm_device_info info;
  int nalloc;
  uint64_t alloc_handle[SHIM_MAX_ALLOCS];
  uint64_t alloc_bus[SHIM_MAX_ALLOCS];
  uint64_t alloc_size[SHIM_MAX_ALLOCS];
  uint32_t alloc_domain[SHIM_MAX_ALLOCS];
  uint64_t alloc_bytes;
  int nvm;
  uint32_t vm_id[SHIM_MAX_VMS];
  int nch;
  uint32_t ch_id[SHIM_MAX_CHANNELS];
  /* Channels created through the DRM face carry a push buffer the kernel
   * allocated (drm_nouveau_channel_alloc has domains, not a handle), so the
   * shim owns it and must free it with the channel.  0 = none (the 'N' face's
   * CHANNEL_ALLOC takes a client handle and owns nothing). */
  uint64_t ch_pushbuf[SHIM_MAX_CHANNELS];

  /* ---- DRM face ('d' requests; nv_drm_uapi.h) ----
   * There is no device handle on this half of the ABI: the open fd IS the
   * device, exactly as in DRM.  One VA space per client, because
   * drm_nouveau_vm_bind carries no VM identifier. */
  int drm;
  uint32_t drm_vm;
  int nsobj;
  uint32_t sobj[SHIM_MAX_SYNCOBJS];
  uint8_t sobj_sig[SHIM_MAX_SYNCOBJS];
  int npend;
  struct shim_sigpend pend[SHIM_MAX_SIGPEND];

  /* WP-F3: what the last EXEC fetched out of the push buffers it was given
   * (bytes + FNV-1a 32).  Read only by the kernel unit tier's seam. */
  int exec_have;
  uint32_t exec_sum;
  uint32_t exec_len;

  /* WP-NVIF: the object tree this client built through nr 0x47. */
  int nnvif;
  struct shim_nvif_obj nvif[SHIM_MAX_NVIF];
};

static struct shim_client g_clients[SHIM_MAX_CLIENTS];
static spinlock_t g_shim_lock;
/* WP-F3: live BO mappings, process-global because a mapping outlives the ioctl
 * that made it and has to be found from GEM_CLOSE / fd close. */
#define SHIM_MAX_MAPS 16
static struct shim_map g_maps[SHIM_MAX_MAPS];

/* ---- WP-PRIME: object records and dma-bufs ------------------------------
 *
 * BEFORE PRIME a handle WAS the object: GEM_CLOSE (or fd close) called the
 * RM's mem_free and that was the end of it.  A dma-buf breaks that identity --
 * the pages must outlive the handle that made them, for as long as an exported
 * fd exists -- so the object gets its own refcounted record here:
 *
 *     refs = one per client GEM-handle TABLE ENTRY (a handle entry, not the
 *            numeric handle: importing one fd twice in a client is two
 *            entries and two references)
 *          + one per exported dma-buf fd
 *
 * `handle` is the RM's handle, the object's identity (RM-global); it is also
 * the number a client sees, which is the documented divergence from Linux's
 * per-file handle namespace (nv_drm_uapi.h, "PRIME contract"). */
struct shim_bo {
  int used;
  uint32_t dev;
  uint64_t handle;
  uint64_t bus;
  uint64_t size;
  uint32_t domain;
  uint64_t phys; /* cached CPU view: 0 = not resolved yet */
  int refs;
};
#define SHIM_MAX_BOS 64 /* the mock's own table (MOCK_MAX_ALLOC) is 64 */
static struct shim_bo g_bos[SHIM_MAX_BOS];

/* One exported dma-buf.  Owned by exactly one open file DESCRIPTION: the fd
 * table's own ref_count covers dup(2)/fork/SCM_RIGHTS, so this object does not
 * recount them, and nv_shim_dmabuf_release() runs when the last reference to
 * that description goes. */
struct shim_dmabuf {
  int used;
  uint32_t dev;   /* the device that exported it */
  uint64_t handle;
  uint64_t size;
  uint32_t domain;
  int id;         /* serial for the console rows */
};
#define SHIM_MAX_DMABUF 16
static struct shim_dmabuf g_dmabufs[SHIM_MAX_DMABUF];
static int g_dmabuf_serial;
/* One bounce buffer for command streams: the RM (and later GSP-RM) takes a
 * kernel pointer, and SUBMIT is the only variable-length request. */
static uint8_t g_cs_buf[NVSHIM_MAX_CS] __attribute__((aligned(16)));

/* Per-stage row budgets.  One [NVSHIM] row per stage, and the row names the
 * stage (PLAN.md §6.4); a client in a loop cannot flood the console.
 *
 * A row is composed in a per-CPU buffer and emitted with ONE uart_puts, so an
 * interrupt-context writer cannot interleave a fragment into the middle of it.
 * (The first cut printed the head and the payload with separate calls: the
 * budget was zero-initialised, the head was suppressed, the payload was not,
 * and the log read "dev=1 chipset=..." with no stage name.  The budget now
 * starts at SHIM_ROW_BUDGET and the whole row is one buffer.) */
#define SHIM_ROW_MAX 160
static char g_row[MAX_CPUS][SHIM_ROW_MAX];
static int g_row_n[MAX_CPUS]; /* -1: this row's budget is spent */

static uint32_t row_cpu(void) {
  uint32_t cpu = get_cpuid();
  return cpu < MAX_CPUS ? cpu : 0;
}
static void row_put(char c) {
  uint32_t cpu = row_cpu();
  if (g_row_n[cpu] < 0 || g_row_n[cpu] >= SHIM_ROW_MAX - 2) return;
  g_row[cpu][g_row_n[cpu]++] = c;
}
static void row_s(const char *s) {
  while (s && *s) row_put(*s++);
}
static void row_say(int v) {
  char b[12];
  int n = 0;
  if (v < 0) {
    row_put('-');
    v = -v;
  }
  if (v == 0) {
    row_put('0');
    return;
  }
  while (v > 0 && n < (int)sizeof b) {
    b[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (n > 0) row_put(b[--n]);
}
static void row_hex(uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  row_s("0x");
  int started = 0;
  for (int sh = 60; sh >= 0; sh -= 4) {
    int d = (int)((v >> sh) & 0xF);
    if (d || started || sh == 0) {
      row_put(hx[d]);
      started = 1;
    }
  }
}
static void row_begin(const char *stage, int *budget) {
  uint32_t cpu = row_cpu();
  if (*budget <= 0) {
    g_row_n[cpu] = -1;
    return;
  }
  (*budget)--;
  g_row_n[cpu] = 0;
  row_s("[NVSHIM] ");
  row_s(stage);
  row_s(" ");
}
#define SHIM_ROW_BUDGET 8
static int r_dev = SHIM_ROW_BUDGET, r_alloc = SHIM_ROW_BUDGET,
           r_vm = SHIM_ROW_BUDGET, r_bind = SHIM_ROW_BUDGET,
           r_ch = SHIM_ROW_BUDGET, r_submit = SHIM_ROW_BUDGET,
           r_fence = SHIM_ROW_BUDGET, r_refuse = SHIM_ROW_BUDGET,
           r_nvif = 96, /* WP-NVIF: one row per object operation, success AND
                         * refusal.  The budget is per BOOT, not per client, so
                         * the four unit suites plus the EL0 probe need room for
                         * every row that proves the tree; 24 ran out inside the
                         * sclass suite and hid the engine refusals -- the very
                         * rows this lane exists to produce. */
           r_mmap = 12, /* WP-F3: the CPU-mapping stage, success AND refusal -- a
                    * refused mapping names itself, and the tier exercises every
                    * refusal shape, so 8 was one short */
           r_param = 8, /* WP-UNITS: GETPARAM refusals get their own budget, one
                    * row per refusal.  Sharing r_refuse hid the one row that
                    * proves WHY a node without a topology refuses GRAPH_UNITS
                    * (mock_has_no_topo): by the last suite that bucket had been
                    * spent on other stages.  A refusal that cannot be seen is
                    * the failure mode this project keeps paying for. */
           r_drm = 128; /* the 'd' face: one row per stage.  8 truncated the gate
                        * probe's evidence; 32 was exhausted by the time the
                        * WP-F3 mapping test ran, which suppressed the very
                        * `drm exec ... stream=` row that proves EXEC read the
                        * client's bytes; 64 then ran out before WP-UNITS' last
                        * suite, hiding the `drm getparam param=13 ... src=
                        * declared` row this lane exists to produce.  Each row
                        * is still one line, and the budget is per BOOT. */
/* WP-PRIME's own bucket: export, import, every refusal and every close-order
 * row names itself, and the suites drive the same rows several times (three
 * close orders), so this is sized for the evidence, not for one call. */
static int r_prime = 24;

static void row_end(void) {
  uint32_t cpu = row_cpu();
  if (g_row_n[cpu] <= 0) return;
  g_row[cpu][g_row_n[cpu]++] = '\n';
  g_row[cpu][g_row_n[cpu]] = '\0';
  uart_puts(g_row[cpu]);
  g_row_n[cpu] = 0;
}

/* Spin relaxation in FENCE_WAIT's bounded poll.  x86 and ARM name this
 * differently, and this TU is built for both (unlike nv.c). */
static inline void shim_cpu_relax(void) {
#if defined(__x86_64__)
  __asm__ volatile("pause");
#elif defined(__aarch64__)
  __asm__ volatile("yield");
#else
  __asm__ volatile("");
#endif
}

static const struct nv_rm_if *rm(void) { return nv_rm_mock_if(); }

/* Shared by the 'N' and 'd' faces (defined with the DRM section below): one
 * param table, so an answer cannot depend on which magic the client used. */
static int drm_param_value(const struct nv_rm_device_info *info, uint64_t param,
                           uint64_t *value);

int nv_shim_present(void) { return 1; }

int nv_shim_features(void) {
  return NVSHIM_FEAT_DEVICE | NVSHIM_FEAT_ALLOC | NVSHIM_FEAT_VM_BIND |
         NVSHIM_FEAT_CHANNEL | NVSHIM_FEAT_SUBMIT | NVSHIM_FEAT_FENCE |
         NVSHIM_FEAT_RM_MOCK | NVSHIM_FEAT_PRIME;
}

/* ---- request tables ---------------------------------------------------- */

int nv_shim_req_size(unsigned long req) {
  if (req == NVSHIM_IOC_DEVICE_ALLOC) return (int)sizeof(struct nvshim_device);
  if (req == NVSHIM_IOC_DEVICE_FREE) return (int)sizeof(struct nvshim_device_free);
  if (req == NVSHIM_IOC_GET_PARAM) return (int)sizeof(struct nvshim_param);
  if (req == NVSHIM_IOC_ALLOC) return (int)sizeof(struct nvshim_alloc);
  if (req == NVSHIM_IOC_FREE) return (int)sizeof(struct nvshim_free);
  if (req == NVSHIM_IOC_VM_INIT) return (int)sizeof(struct nvshim_vm_init);
  if (req == NVSHIM_IOC_VM_BIND) return (int)sizeof(struct nvshim_vm_bind);
  if (req == NVSHIM_IOC_CHANNEL_ALLOC) return (int)sizeof(struct nvshim_channel);
  if (req == NVSHIM_IOC_CHANNEL_FREE) return (int)sizeof(struct nvshim_channel_free);
  if (req == NVSHIM_IOC_SUBMIT) return (int)sizeof(struct nvshim_submit);
  if (req == NVSHIM_IOC_FENCE_WAIT) return (int)sizeof(struct nvshim_fence_wait);
  if (nv_shim_refusal_name(req)) return 0;
  return -1;
}

const char *nv_shim_refusal_name(unsigned long req) {
  if (req == NVSHIM_IOC_GROBJ) return "grobj_alloc";
  if (req == NVSHIM_IOC_NOTIFIER) return "notifier_obj";
  if (req == NVSHIM_IOC_NVIF) return "nvif";
  if (req == NVSHIM_IOC_SVM_INIT) return "svm";
  if (req == NVSHIM_IOC_GEM_PUSHBUF) return "gem_pushbuf";
  if (req == NVSHIM_IOC_SETPARAM) return "setparam";
  return 0;
}

/* ---- pure validators --------------------------------------------------- */

uint64_t nv_shim_align_up(uint64_t len, uint32_t align) {
  if (len == 0) return 0;
  if (align == 0) return len;
  uint64_t a = (uint64_t)align;
  if (len > ~0ull - (a - 1)) return 0; /* overflow: caller answers -ENOMEM */
  return (len + a - 1) & ~(a - 1);
}

int nv_shim_alloc_ok(const struct nvshim_alloc *a) {
  if (!a) return 0;
  /* Only the two domains the mock has an address space for; CPU/MAPPABLE/
   * COHERENT describe a mapping this ABI does not offer yet. */
  if (a->domain != NVSHIM_DOMAIN_VRAM && a->domain != NVSHIM_DOMAIN_GART) return 0;
  if (a->size == 0 || a->size > SHIM_MAX_VM_SIZE) return 0;
  return 1;
}

int nv_shim_bind_ok(const struct nvshim_vm_bind *b) {
  if (!b) return 0;
  if (b->op != NVSHIM_VM_BIND_OP_MAP && b->op != NVSHIM_VM_BIND_OP_UNMAP) return 0;
  if ((b->va & (SHIM_PAGE - 1)) || b->range == 0 || (b->range & (SHIM_PAGE - 1)))
    return 0;
  if (b->va >= SHIM_MAX_VM_SIZE || b->range > SHIM_MAX_VM_SIZE - b->va) return 0;
  return 1;
}

int nv_shim_submit_ok(const struct nvshim_submit *s) {
  if (!s) return 0;
  if (s->flags != 0) return 0;
  if (s->len == 0 || (s->len & 3) || s->len > NVSHIM_MAX_CS) return 0;
  return 1;
}

/* ---- client table ------------------------------------------------------ */

static int shim_pid(struct process *p) {
  if (!p) return -1;
  struct process *g = process_group(p);
  return g ? g->pid : p->pid;
}

static struct shim_client *client_find(int pid, int fd) {
  if (pid < 0 || fd < 0) return 0;
  for (int i = 0; i < SHIM_MAX_CLIENTS; i++)
    if (g_clients[i].used && g_clients[i].pid == pid && g_clients[i].fd == fd)
      return &g_clients[i];
  return 0;
}

static void client_clear(struct shim_client *c) {
  c->used = 1;
  c->dev_id = 0;
  c->nalloc = 0;
  c->alloc_bytes = 0;
  c->nvm = 0;
  c->nch = 0;
  for (int i = 0; i < SHIM_MAX_ALLOCS; i++) {
    c->alloc_handle[i] = 0;
    c->alloc_bus[i] = 0;
    c->alloc_size[i] = 0;
    c->alloc_domain[i] = 0;
  }
  for (int i = 0; i < SHIM_MAX_VMS; i++) c->vm_id[i] = 0;
  for (int i = 0; i < SHIM_MAX_CHANNELS; i++) {
    c->ch_id[i] = 0;
    c->ch_pushbuf[i] = 0;
  }
  c->drm = 0;
  c->drm_vm = 0;
  c->nsobj = 0;
  for (int i = 0; i < SHIM_MAX_SYNCOBJS; i++) {
    c->sobj[i] = 0;
    c->sobj_sig[i] = 0;
  }
  c->npend = 0;
  for (int i = 0; i < SHIM_MAX_SIGPEND; i++) {
    c->pend[i].sobj = 0;
    c->pend[i].ch = 0;
    c->pend[i].fence = 0;
  }
  c->exec_have = 0;
  c->exec_sum = 0;
  c->exec_len = 0;
  c->nnvif = 0;
  for (int i = 0; i < SHIM_MAX_NVIF; i++) {
    c->nvif[i].cookie = 0;
    c->nvif[i].oclass = 0;
    c->nvif[i].ch = 0;
  }
  for (unsigned i = 0; i < sizeof(c->info); i++) ((unsigned char *)&c->info)[i] = 0;
}

static struct shim_client *client_new(int pid, int fd) {
  for (int i = 0; i < SHIM_MAX_CLIENTS; i++)
    if (!g_clients[i].used) {
      struct shim_client *c = &g_clients[i];
      c->used = 1;
      c->pid = pid;
      c->fd = fd;
      client_clear(c);
      return c;
    }
  return 0;
}

/* Is `dev` this client's (fully created) device? */
static int client_owns_device(const struct shim_client *c, uint32_t dev) {
  return c && c->used && c->dev_id != 0 && c->dev_id != SHIM_RESERVED &&
         c->dev_id == dev;
}

static int client_find_alloc(const struct shim_client *c, uint64_t handle) {
  for (int i = 0; i < c->nalloc; i++)
    if (c->alloc_handle[i] == handle) return i;
  return -1;
}

static int client_find_vm(const struct shim_client *c, uint32_t vm) {
  for (int i = 0; i < c->nvm; i++)
    if (c->vm_id[i] == vm) return i;
  return -1;
}

static int client_find_ch(const struct shim_client *c, uint32_t ch) {
  for (int i = 0; i < c->nch; i++)
    if (c->ch_id[i] == ch) return i;
  return -1;
}

/* Slot to fill once an RM creation returns: the first RESERVED entry, which is
 * the one this request put there.  Returns -1 if the client is gone (or the
 * reservation was torn down while the RM call was in flight). */
static int client_reserved_alloc(const struct shim_client *c) {
  for (int i = 0; i < c->nalloc; i++)
    if (c->alloc_handle[i] == SHIM_RESERVED) return i;
  return -1;
}
static int client_reserved_vm(const struct shim_client *c) {
  for (int i = 0; i < c->nvm; i++)
    if (c->vm_id[i] == SHIM_RESERVED) return i;
  return -1;
}
static int client_reserved_ch(const struct shim_client *c) {
  for (int i = 0; i < c->nch; i++)
    if (c->ch_id[i] == SHIM_RESERVED) return i;
  return -1;
}

/* ---- object refcounts (WP-PRIME) --------------------------------------
 * Every function in this block runs with g_shim_lock held.  `refs` counts
 * client handle-table entries and exported dma-bufs; the frames are freed
 * exactly once, by whoever drops the last reference. */
static struct shim_bo *bo_find(uint32_t dev, uint64_t handle) {
  for (int i = 0; i < SHIM_MAX_BOS; i++)
    if (g_bos[i].used && g_bos[i].dev == dev && g_bos[i].handle == handle)
      return &g_bos[i];
  return 0;
}

/* Register a freshly created RM object with one reference (the handle entry the
 * caller is about to publish).  0 = registered, -1 = table full. */
static int bo_add(uint32_t dev, uint64_t handle, uint64_t bus, uint64_t size,
                  uint32_t domain) {
  if (bo_find(dev, handle)) return -1; /* already tracked: a caller's bug */
  for (int i = 0; i < SHIM_MAX_BOS; i++)
    if (!g_bos[i].used) {
      struct shim_bo *b = &g_bos[i];
      b->used = 1;
      b->dev = dev;
      b->handle = handle;
      b->bus = bus;
      b->size = size;
      b->domain = domain;
      b->phys = 0;
      b->refs = 1;
      return 0;
    }
  return -1;
}

static int bo_ref(uint32_t dev, uint64_t handle) {
  struct shim_bo *b = bo_find(dev, handle);
  if (!b) return -1;
  b->refs++;
  return 0;
}

/* Drop one reference.  Returns 1 when this WAS the last one: the caller must
 * take the object's mappings down and then free the frames. */
static int bo_unref(uint32_t dev, uint64_t handle) {
  struct shim_bo *b = bo_find(dev, handle);
  if (!b) return 1; /* not tracked (never registered): the caller owns frames */
  if (--b->refs > 0) return 0;
  b->used = 0;
  b->dev = 0;
  b->handle = 0;
  b->bus = 0;
  b->size = 0;
  b->domain = 0;
  b->phys = 0;
  b->refs = 0;
  return 1;
}

/* The name a PRIME refusal prints.  Exported (nv_shim.h) so the tier asserts
 * the NAME, not only the errno -- a refusal nobody can read is the failure
 * mode this project keeps paying for. */
/* The name a PRIME refusal prints.  It is COPIED into the caller's buffer, not
 * returned as a pointer: clang folds a function that only SELECTS among string
 * literals into a .rodata array of string pointers, and this project's AArch64
 * image is slid by Limine, so such a table is a link-address hazard the
 * relocation audit gates on (tools/kernel_reloc_audit.py).  Bytes need no
 * relocation; a pointer does.  (Measured: the pointer-returning form added a
 * 6-word .rodata table and failed the ARM link.) */
static const char k_prime_reasons[][36] = {
    "ok", "no_share", "cross_device", "not_a_dmabuf",
    "domain_never_offered_a_cpu_mapping", "flags",
};
#define PRIME_NUM_REASONS (int)(sizeof k_prime_reasons / sizeof k_prime_reasons[0])

void nv_shim_prime_refusal_name(int why, char *dst, int cap) {
  if (!dst || cap <= 0) return;
  const char *s = "unknown";
  if (why >= 0 && why < PRIME_NUM_REASONS) s = k_prime_reasons[why];
  int i = 0;
  while (s[i] && i < cap - 1) {
    dst[i] = s[i];
    i++;
  }
  dst[i] = 0;
}

/* Drop a PRIME refusal's name into the current row. */
static void row_prime_why(int why) {
  char b[NVSHIM_PRIME_REASON_MAX];
  nv_shim_prime_refusal_name(why, b, (int)sizeof b);
  row_s(b);
}

/* Dropping a live dma-buf's mapping table entries (defined with the mapping
 * block; declared here because teardown runs before it). */
static void shim_maps_drop_object(struct process *p, uint64_t handle);

/* Detach a client and destroy everything it owns.  Runs without the lock: the
 * RM calls are outside it, exactly as every other op does.
 *
 * WP-PRIME: an object may outlive the client (an exported dma-buf holds a
 * reference), so this drops REFERENCES rather than freeing outright; the frames
 * go when the last one goes. */
static void client_teardown(struct process *p, int fd,
                            const struct shim_client *snap) {
  for (int i = snap->nch - 1; i >= 0; i--) {
    if (snap->ch_id[i] && snap->ch_id[i] != SHIM_RESERVED)
      rm()->channel_destroy(snap->dev_id, snap->ch_id[i]);
    /* The push buffer went in after the channel and must come out after it.
     * It is the shim's own object (no handle ever published, never exported),
     * so it is freed directly. */
    if (snap->ch_pushbuf[i])
      rm()->mem_free(snap->dev_id, snap->ch_pushbuf[i]);
  }
  for (int i = snap->nvm - 1; i >= 0; i--)
    if (snap->vm_id[i] && snap->vm_id[i] != SHIM_RESERVED)
      rm()->vm_destroy(snap->dev_id, snap->vm_id[i]);
  for (int i = snap->nalloc - 1; i >= 0; i--) {
    uint64_t h = snap->alloc_handle[i];
    if (!h || h == SHIM_RESERVED) continue;
    spinlock_acquire(&g_shim_lock);
    int last = bo_unref(snap->dev_id, h);
    spinlock_release(&g_shim_lock);
    if (!last) continue; /* an exported fd still holds these pages */
    shim_maps_drop_object(p, h);
    rm()->mem_free(snap->dev_id, h);
  }
  (void)fd;
  if (snap->dev_id && snap->dev_id != SHIM_RESERVED)
    rm()->device_close(snap->dev_id);
}

/* ---- 1. device identify ------------------------------------------------ */

static int op_device_alloc(struct process *p, int fd, uint64_t arg) {
  struct nvshim_device d;
  if (vm_user_read(p, arg, &d, (int)sizeof d) != 0) return -EFAULT;
  if (d.abi != NVSHIM_ABI_VERSION || d.flags != 0) {
    row_begin("device refused", &r_refuse);
    row_s("abi=");
    row_say((int)d.abi);
    row_s(" flags=");
    row_say((int)d.flags);
    row_s(" reason=version");
    row_end();
    return -EINVAL;
  }

  int pid = shim_pid(p);
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (c && c->dev_id != 0) {
    spinlock_release(&g_shim_lock);
    return -EBUSY;
  }
  if (!c) c = client_new(pid, fd);
  if (!c) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  client_clear(c);
  c->dev_id = SHIM_RESERVED;
  spinlock_release(&g_shim_lock);

  uint32_t dev_id = 0;
  struct nv_rm_device_info info;
  for (unsigned i = 0; i < sizeof(info); i++) ((unsigned char *)&info)[i] = 0;
  int rc = rm()->device_open(&dev_id, &info);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int ours = (c && c->dev_id == SHIM_RESERVED);
  if (rc == 0 && ours) {
    c->dev_id = dev_id;
    c->info = info;
  } else if (ours) {
    c->dev_id = 0;
    c->used = 0;
    c->pid = 0;
    c->fd = -1;
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc == -ENODEV ? -ENODEV : -EIO;
  if (!ours) {
    /* The fd closed while the device was being created. */
    rm()->device_close(dev_id);
    return -EBADF;
  }

  d.dev_id = dev_id;
  d.chipset = info.chipset;
  d.device_id = info.device_id;
  d.gsp_required = info.gsp_required;
  d.mock = info.mock;
  d.features = nv_shim_features();
  for (unsigned i = 0; i < sizeof(d.name); i++) d.name[i] = info.name[i];
  if (vm_kwrite(p, arg, &d, (int)sizeof d) != 0) return -EFAULT;

  row_begin("device", &r_dev);
  row_s("dev=");
  row_say((int)dev_id);
  row_s(" chipset=");
  row_hex(info.chipset);
  row_s(" pci=");
  row_hex(info.device_id);
  row_s(" mock=");
  row_say((int)info.mock);
  row_s(" feats=");
  row_hex((uint64_t)nv_shim_features());
  row_end();
  return 0;
}

/* Shared body of DEVICE_FREE / fd close: `expect` 0 means "whatever is there".
 * Returns 0 or -EINVAL (no such device on this client). */

/* WP-F3: defined with the CPU-mapping block; teardown must take mappings down
 * before any frame is freed, and it runs before that block is compiled. */
static void shim_maps_drop(struct process *p, int fd, uint64_t handle);

static int device_release(struct process *p, int fd, uint32_t expect) {
  int pid = shim_pid(p);
  struct shim_client snap;
  int found = 0;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (c && c->dev_id != 0) {
    if (expect == 0 || c->dev_id == expect) {
      snap = *c;
      c->dev_id = 0;
      c->used = 0;
      c->pid = 0;
      c->fd = -1;
      found = 1;
    }
  }
  spinlock_release(&g_shim_lock);

  if (!found) return -EINVAL;
  /* WP-F3: mappings first -- their leaves name the RM's frames, so the frames
   * must not be freed while a mapping is live. */
  shim_maps_drop(p, fd, 0);
  client_teardown(p, fd, &snap);
  return 0;
}

static int op_device_free(struct process *p, int fd, uint64_t arg) {
  struct nvshim_device_free df;
  if (vm_user_read(p, arg, &df, (int)sizeof df) != 0) return -EFAULT;
  int rc = device_release(p, fd, df.dev_id);
  if (rc == 0) {
    row_begin("device_free", &r_dev);
    row_s("dev=");
    row_say((int)df.dev_id);
    row_end();
  }
  return rc;
}

void nv_shim_release_fd(struct process *p, int fd) { (void)device_release(p, fd, 0); }

/* ---- 2. getparam ------------------------------------------------------- */

/* Defined with the param table below; declared here because the 'N' face
 * answers through it too. */
const char *nv_shim_param_refusal_reason(uint64_t param);

static int op_get_param(struct process *p, int fd, uint64_t arg) {
  struct nvshim_param q;
  if (vm_user_read(p, arg, &q, (int)sizeof q) != 0) return -EFAULT;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  struct nv_rm_device_info info;
  int have = (c && c->dev_id != 0 && c->dev_id != SHIM_RESERVED);
  if (have) info = c->info;
  spinlock_release(&g_shim_lock);
  if (!have) return -ENODEV;

  /* One param table for both faces (drm_param_value): the numbers are
   * nouveau's, and the answer must not depend on which magic the client used to
   * ask.  A param that reads the card is named, not faked. */
  uint64_t value = 0;
  int rc = drm_param_value(&info, q.param, &value);
  if (rc == -ENOSYS) {
    row_begin("getparam refused", &r_param);
    row_s("param=");
    row_say((int)q.param);
    row_s(" reason=");
    row_s(nv_shim_param_refusal_reason(q.param));
    row_end();
    return -ENOSYS;
  }
  if (rc != 0) return rc;
  q.value = value;
  if (vm_kwrite(p, arg, &q, (int)sizeof q) != 0) return -EFAULT;
  return 0;
}

/* ---- 3. memory objects ------------------------------------------------- */

static int op_alloc(struct process *p, int fd, uint64_t arg) {
  struct nvshim_alloc a;
  if (vm_user_read(p, arg, &a, (int)sizeof a) != 0) return -EFAULT;
  if (!nv_shim_alloc_ok(&a)) {
    row_begin("alloc refused", &r_refuse);
    row_s("size=");
    row_hex(a.size);
    row_s(" domain=");
    row_hex(a.domain);
    row_s(" reason=geometry");
    row_end();
    return -EINVAL;
  }
  uint64_t want = nv_shim_align_up(a.size, (uint32_t)SHIM_PAGE);
  if (want == 0) return -ENOMEM;

  int pid = shim_pid(p);
  uint32_t dev = 0;
  int slot = -1;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!client_owns_device(c, a.dev_id)) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  if (c->nalloc >= SHIM_MAX_ALLOCS) {
    spinlock_release(&g_shim_lock);
    row_begin("alloc refused", &r_refuse);
    row_s("reason=table_full");
    row_end();
    return -ENOSPC;
  }
  if (want > SHIM_MAX_ALLOC_BYTES || c->alloc_bytes > SHIM_MAX_ALLOC_BYTES - want) {
    spinlock_release(&g_shim_lock);
    row_begin("alloc refused", &r_refuse);
    row_s("want=");
    row_hex(want);
    row_s(" held=");
    row_hex(c->alloc_bytes);
    row_s(" reason=client_bytes");
    row_end();
    return -ENOMEM;
  }
  dev = c->dev_id;
  slot = c->nalloc;
  c->alloc_handle[slot] = SHIM_RESERVED;
  c->alloc_bus[slot] = 0;
  c->alloc_size[slot] = 0;
  c->nalloc++;
  spinlock_release(&g_shim_lock);

  uint64_t handle = 0, bus = 0, granted = 0;
  int rc = rm()->mem_alloc(dev, want, a.domain, &handle, &bus, &granted);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int done = 0;
  if (rc == 0 && c) {
    int at = client_reserved_alloc(c);
    if (at >= 0 && handle <= 0xFFFFFFFFull &&
        bo_add(dev, handle, bus, granted, a.domain) == 0) {
      c->alloc_handle[at] = handle;
      c->alloc_bus[at] = bus;
      c->alloc_size[at] = granted;
      c->alloc_domain[at] = a.domain;
      c->alloc_bytes += granted;
      done = 1;
    }
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc; /* -ENOMEM / -ENOSPC / -EINVAL / -ENODEV */
  if (!done) {
    rm()->mem_free(dev, handle);
    return -EBADF;
  }

  a.size = granted;
  a.handle = (uint32_t)handle;
  a.bus = bus;
  if (vm_kwrite(p, arg, &a, (int)sizeof a) != 0) return -EFAULT;

  row_begin("alloc", &r_alloc);
  row_s("handle=");
  row_say((int)handle);
  row_s(" bus=");
  row_hex(bus);
  row_s(" size=");
  row_hex(granted);
  row_s(" domain=");
  row_hex(a.domain);
  row_end();
  return 0;
}

static int op_free(struct process *p, int fd, uint64_t arg) {
  struct nvshim_free f;
  if (vm_user_read(p, arg, &f, (int)sizeof f) != 0) return -EFAULT;

  int pid = shim_pid(p);
  uint32_t dev = 0;
  uint64_t handle = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!client_owns_device(c, f.dev_id)) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  int at = client_find_alloc(c, f.handle);
  if (at < 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  dev = c->dev_id;
  handle = c->alloc_handle[at];
  c->alloc_bytes -= c->alloc_size[at];
  for (int i = at; i + 1 < c->nalloc; i++) {
    c->alloc_handle[i] = c->alloc_handle[i + 1];
    c->alloc_bus[i] = c->alloc_bus[i + 1];
    c->alloc_size[i] = c->alloc_size[i + 1];
    c->alloc_domain[i] = c->alloc_domain[i + 1];
  }
  c->nalloc--;
  spinlock_release(&g_shim_lock);

  /* WP-PRIME: drop this handle's REFERENCE.  The frames go only when the last
   * reference goes -- an exported dma-buf keeps the object (and its pages)
   * alive after the handle that made it is gone. */
  spinlock_acquire(&g_shim_lock);
  int last = bo_unref(dev, handle);
  spinlock_release(&g_shim_lock);
  if (!last) return 0;
  shim_maps_drop_object(p, handle);
  int rc = rm()->mem_free(dev, handle);
  return rc == 0 ? 0 : -EIO;
}

/* ---- 4. GPU VA space --------------------------------------------------- */

static int op_vm_init(struct process *p, int fd, uint64_t arg) {
  struct nvshim_vm_init v;
  if (vm_user_read(p, arg, &v, (int)sizeof v) != 0) return -EFAULT;
  if (v.size == 0 || (v.size & (SHIM_PAGE - 1)) || v.size > SHIM_MAX_VM_SIZE)
    return -EINVAL;

  int pid = shim_pid(p);
  uint32_t dev = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!client_owns_device(c, v.dev_id)) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  if (c->nvm >= SHIM_MAX_VMS) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  dev = c->dev_id;
  c->vm_id[c->nvm++] = SHIM_RESERVED;
  spinlock_release(&g_shim_lock);

  uint32_t vm_id = 0;
  int rc = rm()->vm_create(dev, v.size, &vm_id);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int done = 0;
  if (rc == 0 && c) {
    int at = client_reserved_vm(c);
    if (at >= 0) {
      c->vm_id[at] = vm_id;
      done = 1;
    }
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc;
  if (!done) {
    rm()->vm_destroy(dev, vm_id);
    return -EBADF;
  }
  v.vm_id = vm_id;
  if (vm_kwrite(p, arg, &v, (int)sizeof v) != 0) return -EFAULT;

  row_begin("vm", &r_vm);
  row_s("vm=");
  row_say((int)vm_id);
  row_s(" size=");
  row_hex(v.size);
  row_end();
  return 0;
}

static int op_vm_bind(struct process *p, int fd, uint64_t arg) {
  struct nvshim_vm_bind b;
  if (vm_user_read(p, arg, &b, (int)sizeof b) != 0) return -EFAULT;
  if (!nv_shim_bind_ok(&b)) {
    row_begin("vm_bind refused", &r_refuse);
    row_s("va=");
    row_hex(b.va);
    row_s(" range=");
    row_hex(b.range);
    row_s(" op=");
    row_say((int)b.op);
    row_s(" reason=geometry");
    row_end();
    return -EINVAL;
  }

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  if (!client_owns_device(c, b.dev_id) || client_find_vm(c, b.vm_id) < 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  if (b.op == NVSHIM_VM_BIND_OP_MAP) {
    int at = client_find_alloc(c, b.handle);
    if (at < 0 || b.bo_offset > c->alloc_size[at] ||
        b.range > c->alloc_size[at] - b.bo_offset) {
      spinlock_release(&g_shim_lock);
      row_begin("vm_bind refused", &r_refuse);
      row_s("handle=");
      row_say((int)b.handle);
      row_s(" reason=not_this_clients_object");
      row_end();
      return -EINVAL;
    }
  }
  spinlock_release(&g_shim_lock);
  int rc = rm()->vm_bind(b.dev_id, b.vm_id, b.op, b.handle, b.va, b.range,
                         b.bo_offset);
  if (rc != 0) {
    row_begin("vm_bind refused", &r_refuse);
    row_s("rm=");
    row_say(rc);
    row_end();
    return rc;
  }
  row_begin("bind", &r_bind);
  row_s("op=");
  row_say((int)b.op);
  row_s(" va=");
  row_hex(b.va);
  row_s(" range=");
  row_hex(b.range);
  row_s(" handle=");
  row_say((int)b.handle);
  row_end();
  return 0;
}

/* ---- 5. channels ------------------------------------------------------- */

static int op_channel_alloc(struct process *p, int fd, uint64_t arg) {
  struct nvshim_channel ch;
  if (vm_user_read(p, arg, &ch, (int)sizeof ch) != 0) return -EFAULT;

  int pid = shim_pid(p);
  uint32_t dev = 0, vm = 0;
  uint64_t pushbuf = 0, psize = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!client_owns_device(c, ch.dev_id) || client_find_vm(c, ch.vm_id) < 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  int at = client_find_alloc(c, ch.handle);
  if (at < 0) {
    spinlock_release(&g_shim_lock);
    row_begin("channel refused", &r_refuse);
    row_s("handle=");
    row_say((int)ch.handle);
    row_s(" reason=not_this_clients_object");
    row_end();
    return -EINVAL;
  }
  if (c->alloc_size[at] < SHIM_PAGE) {
    spinlock_release(&g_shim_lock);
    row_begin("channel refused", &r_refuse);
    row_s("reason=pushbuf_too_small");
    row_end();
    return -EINVAL;
  }
  if (c->nch >= SHIM_MAX_CHANNELS) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  dev = c->dev_id;
  vm = ch.vm_id;
  pushbuf = c->alloc_bus[at];
  psize = c->alloc_size[at];
  c->ch_pushbuf[c->nch] = 0;
  c->ch_id[c->nch++] = SHIM_RESERVED;
  spinlock_release(&g_shim_lock);

  uint32_t ch_id = 0, engine = 0;
  int rc = rm()->channel_create(dev, vm, pushbuf, (uint32_t)psize, &ch_id,
                                &engine);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int done = 0;
  if (rc == 0 && c) {
    int slot = client_reserved_ch(c);
    if (slot >= 0) {
      c->ch_id[slot] = ch_id;
      done = 1;
    }
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc;
  if (!done) {
    rm()->channel_destroy(dev, ch_id);
    return -EBADF;
  }
  ch.ch_id = ch_id;
  ch.pushbuf = pushbuf;
  ch.pushbuf_size = (uint32_t)psize;
  ch.engine = engine;
  if (vm_kwrite(p, arg, &ch, (int)sizeof ch) != 0) return -EFAULT;

  row_begin("channel", &r_ch);
  row_s("ch=");
  row_say((int)ch_id);
  row_s(" engine=");
  row_say((int)engine);
  row_s(" pushbuf=");
  row_hex(pushbuf);
  row_end();
  return 0;
}

static int op_channel_free(struct process *p, int fd, uint64_t arg) {
  struct nvshim_channel_free cf;
  if (vm_user_read(p, arg, &cf, (int)sizeof cf) != 0) return -EFAULT;

  uint32_t dev = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  int at = c ? client_find_ch(c, cf.ch_id) : -1;
  if (!c || at < 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  dev = c->dev_id;
  for (int i = at; i + 1 < c->nch; i++) {
    c->ch_id[i] = c->ch_id[i + 1];
    c->ch_pushbuf[i] = c->ch_pushbuf[i + 1];
  }
  c->nch--;
  spinlock_release(&g_shim_lock);

  rm()->channel_destroy(dev, cf.ch_id);
  return 0;
}

/* ---- 6. submit and fences ---------------------------------------------- */

static int shim_fence_poll(uint32_t ch, uint64_t fence) {
  return rm()->fence_test(ch, fence);
}

static int op_submit(struct process *p, int fd, uint64_t arg) {
  struct nvshim_submit s;
  if (vm_user_read(p, arg, &s, (int)sizeof s) != 0) return -EFAULT;
  if (!nv_shim_submit_ok(&s)) {
    row_begin("submit refused", &r_refuse);
    row_s("len=");
    row_say((int)s.len);
    row_s(" reason=stream_shape");
    row_end();
    return -EINVAL;
  }

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  int owned = (c && client_find_ch(c, s.ch_id) >= 0);
  spinlock_release(&g_shim_lock);
  if (!owned) return -EINVAL;

  /* The RM takes a kernel pointer; the command stream is user memory. */
  if (vm_user_read(p, s.va, g_cs_buf, (int)s.len) != 0) return -EFAULT;

  uint64_t fence = 0;
  int rc = rm()->submit(s.ch_id, g_cs_buf, s.len, &fence);
  if (rc != 0) return rc;
  /* The engine-progress stand-in: the mock retires at submit.  A real RM
   * retires from its interrupt path instead; FENCE_WAIT is the same either
   * way. */
  rm()->kick();

  s.fence = fence;
  if (vm_kwrite(p, arg, &s, (int)sizeof s) != 0) return -EFAULT;

  row_begin("submit", &r_submit);
  row_s("ch=");
  row_say((int)s.ch_id);
  row_s(" len=");
  row_say((int)s.len);
  row_s(" fence=");
  row_hex(fence);
  row_end();
  return 0;
}

static int op_fence_wait(struct process *p, int fd, uint64_t arg) {
  struct nvshim_fence_wait fw;
  if (vm_user_read(p, arg, &fw, (int)sizeof fw) != 0) return -EFAULT;
  if (fw.fence == 0) return -EINVAL;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  int owned = (c && client_find_ch(c, fw.ch_id) >= 0);
  spinlock_release(&g_shim_lock);
  if (!owned) return -EINVAL;

  int rc = shim_fence_poll(fw.ch_id, fw.fence);
  if (rc == 0) {
    row_begin("fence", &r_fence);
    row_s("ch=");
    row_say((int)fw.ch_id);
    row_s(" fence=");
    row_hex(fw.fence);
    row_s(" retired=1");
    row_end();
    return 0;
  }
  if (rc != -EAGAIN) return -EINVAL; /* this channel never issued that fence */

  if (fw.timeout_ms == 0) {
    row_begin("fence", &r_fence);
    row_s("ch=");
    row_say((int)fw.ch_id);
    row_s(" fence=");
    row_hex(fw.fence);
    row_s(" retired=0 timeout=poll");
    row_end();
    return -ETIMEDOUT;
  }
  /* A client-chosen wait may not pin the CPU: clamp, and say so in the row.
   * The spin also has a hard iteration bound so a timer that never advances
   * (the unit tier before timer_init) cannot hang the call. */
  uint32_t ms = fw.timeout_ms > SHIM_FENCE_WAIT_MAX_MS ? SHIM_FENCE_WAIT_MAX_MS
                                                       : fw.timeout_ms;
  int clamped = fw.timeout_ms > SHIM_FENCE_WAIT_MAX_MS;
  uint64_t t0 = timer_get_ms();
  int spins = 0;
  while (rc == -EAGAIN) {
    if (spins++ >= SHIM_WAIT_SPINS) break;
    if (timer_get_ms() - t0 >= (uint64_t)ms) break;
    for (int k = 0; k < 500; k++) shim_cpu_relax();
    rc = shim_fence_poll(fw.ch_id, fw.fence);
  }
  if (rc == 0) {
    row_begin("fence", &r_fence);
    row_s("ch=");
    row_say((int)fw.ch_id);
    row_s(" fence=");
    row_hex(fw.fence);
    row_s(" retired=1 waited=1");
    row_end();
    return 0;
  }
  if (rc != -EAGAIN) return -EINVAL;
  row_begin("fence", &r_fence);
  row_s("ch=");
  row_say((int)fw.ch_id);
  row_s(" fence=");
  row_hex(fw.fence);
  row_s(" retired=0 timeout=ms clamped=");
  row_say(clamped);
  row_end();
  return -ETIMEDOUT;
}

/* ============================================================================
 *  The DRM face: DRM_IOCTL_NOUVEAU_* on magic 'd' (nv_drm_uapi.h)
 *
 *  Same client table, same mock RM, different contract.  Three differences
 *  from the 'N' rows above are forced by the uAPI, and they are the point:
 *
 *   1. THE FD IS THE DEVICE.  nouveau has no DEVICE_ALLOC ioctl: opening the
 *      render node creates a drm_file, and the first request that needs a
 *      device opens the RM one (drm_client_ensure).
 *   2. ONE VA SPACE PER CLIENT.  drm_nouveau_vm_bind carries no VM identifier,
 *      so VM_INIT creates the client's single space and VM_BIND always targets
 *      it.  VM_INIT after a BO or a channel exists is -ENOSYS, which is what
 *      the uAPI documents.
 *   3. COMPLETION IS A SYNCOBJ.  drm_nouveau_exec has no fence field: it
 *      signals the syncobjs named in sig_ptr, and the client waits on those.
 *      So this section also serves the binary syncobj rows -- without them an
 *      EXEC's completion would be unobservable and the gate could not be met.
 *
 *  Size validation.  The request number encodes the struct width, and DRM
 *  validates it before the driver sees the argument.  Three answers for three
 *  mistakes: -ENOTTY for a number nobody implements, -ENOSYS for a known but
 *  refused one (named on the console), -EINVAL for a known number whose encoded
 *  size is not the struct it pairs with.  Direction bits are not enforced,
 *  exactly as Linux's drm_ioctl does not enforce them.
 * ========================================================================== */

struct drm_row {
  uint8_t nr;
  uint8_t size; /* the exact _IOC size this number pairs with */
};

static const struct drm_row g_drm_rows[] = {
    {0x00, 64}, /* DRM_IOCTL_VERSION           struct drm_version           */
    {0x09, 8},  /* DRM_IOCTL_GEM_CLOSE         struct drm_gem_close         */
    {0x0c, 16}, /* DRM_IOCTL_GET_CAP           struct drm_get_cap           */
    {0x2d, 12}, /* DRM_IOCTL_PRIME_HANDLE_TO_FD  (WP-PRIME)                 */
    {0x2e, 12}, /* DRM_IOCTL_PRIME_FD_TO_HANDLE  (WP-PRIME)                 */
    {0x40, 16}, /* NOUVEAU_GETPARAM            struct drm_nouveau_getparam  */
    {0x42, 88}, /* NOUVEAU_CHANNEL_ALLOC       struct ..._channel_alloc     */
    {0x43, 4},  /* NOUVEAU_CHANNEL_FREE        struct ..._channel_free      */
    {0x50, 16}, /* NOUVEAU_VM_INIT             struct ..._vm_init           */
    {0x51, 40}, /* NOUVEAU_VM_BIND             struct ..._vm_bind           */
    {0x52, 40}, /* NOUVEAU_EXEC                struct ..._exec              */
    {0x80, 48}, /* NOUVEAU_GEM_NEW             struct ..._gem_new           */
    {0x84, 40}, /* NOUVEAU_GEM_INFO            struct ..._gem_info          */
    {0xbf, 8},  /* SYNCOBJ_CREATE              struct drm_syncobj_create    */
    {0xc0, 8},  /* SYNCOBJ_DESTROY             struct drm_syncobj_destroy   */
    {0xc3, 40}, /* SYNCOBJ_WAIT                struct drm_syncobj_wait      */
    {0xc4, 16}, /* SYNCOBJ_RESET               struct drm_syncobj_array     */
    {0xc5, 16}, /* SYNCOBJ_SIGNAL              struct drm_syncobj_array     */
};

/* The refusal rows' names are FIXED-SIZE ARRAYS, not `const char *`.  The
 * kernel applies no load-time relocations to static data, and the AArch64 image
 * runs at Limine's slide, so a `const char *` member would hold the link address
 * of the string (which names unrelated memory at runtime).  This is the hazard
 * blk_cache_press_test.c documents for the same kind of table; the arrays keep
 * the names inside the table itself, which the code addresses PC-relative. */
#define DRM_REFUSAL_NAME_MAX 28 /* longest row is 23 bytes + NUL */

static const struct {
  uint8_t nr;
  char name[DRM_REFUSAL_NAME_MAX];
} g_drm_refusals[] = {
    {NVSHIM_DRM_NR_GEM_FLINK, "gem_flink"},
    {NVSHIM_DRM_NR_SETPARAM, "setparam"},
    {NVSHIM_DRM_NR_GROBJ_ALLOC, "grobj_alloc"},
    {NVSHIM_DRM_NR_NOTIFIEROBJ_ALLOC, "notifier_obj"},
    {NVSHIM_DRM_NR_GPUOBJ_FREE, "gpuobj_free"},
    {NVSHIM_DRM_NR_SVM_INIT, "svm_init"},
    {NVSHIM_DRM_NR_SVM_BIND, "svm_bind"},
    {NVSHIM_DRM_NR_GET_ZCULL_INFO, "get_zcull_info"},
    {NVSHIM_DRM_NR_GEM_PUSHBUF, "gem_pushbuf"},
    {NVSHIM_DRM_NR_GEM_CPU_PREP, "gem_cpu_prep"},
    {NVSHIM_DRM_NR_GEM_CPU_FINI, "gem_cpu_fini"},
    {NVSHIM_DRM_NR_SYNCOBJ_HANDLE_TO_FD, "syncobj_handle_to_fd"},
    {NVSHIM_DRM_NR_SYNCOBJ_FD_TO_HANDLE, "syncobj_fd_to_handle"},
    {NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_WAIT, "syncobj_timeline_wait"},
    {NVSHIM_DRM_NR_SYNCOBJ_QUERY, "syncobj_query"},
    {NVSHIM_DRM_NR_SYNCOBJ_TRANSFER, "syncobj_transfer"},
    {NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_SIGNAL, "syncobj_timeline_signal"},
    {NVSHIM_DRM_NR_SYNCOBJ_EVENTFD, "syncobj_eventfd"},
};

/* -1 = nobody implements this number, 0 = refused, >0 = the size it pairs
 * with.  Keyed on the number so a refusal cannot be dodged by sending a
 * different size, and a size mismatch on an implemented row is not mistaken
 * for an unknown number. */
static int drm_row_size(uint32_t nr) {
  /* NVIF is the one row whose encoded size is a BUFFER LENGTH (header + a
   * per-type payload), so the table can only state its minimum; the handler
   * applies each type's own shape rule.  Returning it here rather than 0 keeps
   * "known and served" distinguishable from "refused" for callers of this
   * function (the test tier asserts both). */
  if (nr == (uint8_t)NVSHIM_DRM_NR_NVIF) return (int)NVSHIM_NVIF_MIN_SIZE;
  for (unsigned i = 0; i < sizeof g_drm_rows / sizeof g_drm_rows[0]; i++)
    if (g_drm_rows[i].nr == (uint8_t)nr) return (int)g_drm_rows[i].size;
  for (unsigned i = 0; i < sizeof g_drm_refusals / sizeof g_drm_refusals[0]; i++)
    if (g_drm_refusals[i].nr == (uint8_t)nr) return 0;
  return -1;
}

static const char *drm_refusal_name(uint32_t nr) {
  for (unsigned i = 0; i < sizeof g_drm_refusals / sizeof g_drm_refusals[0]; i++)
    if (g_drm_refusals[i].nr == (uint8_t)nr) return g_drm_refusals[i].name;
  return 0;
}

/** These two answer about the DRM surface only: a request that does not carry
 * magic 'd' is not theirs, so an 'N' row that happens to share a number (10 is
 * both NVSHIM_IOC_SUBMIT and DRM_IOCTL_GEM_FLINK) cannot be mistaken for one. */
int nv_shim_drm_req_size(unsigned long req) {
  if (NVSHIM_DRM_TYPE((uint32_t)req) != (uint32_t)NVSHIM_DRM_MAGIC) return -1;
  return drm_row_size(NVSHIM_DRM_NR(req));
}

const char *nv_shim_drm_refusal_name(unsigned long req) {
  if (NVSHIM_DRM_TYPE((uint32_t)req) != (uint32_t)NVSHIM_DRM_MAGIC) return 0;
  return drm_refusal_name(NVSHIM_DRM_NR(req));
}

static uint32_t drm_strlen(const char *s) {
  uint32_t n = 0;
  while (s[n]) n++;
  return n;
}

/* The fd's RM device, opened on first use.  Returns 0 and *dev. */
static int drm_client_ensure(struct process *p, int fd, uint32_t *dev) {
  int pid = shim_pid(p);

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (c && c->dev_id != 0 && c->dev_id != SHIM_RESERVED) {
    *dev = c->dev_id;
    spinlock_release(&g_shim_lock);
    return 0;
  }
  if (!c) c = client_new(pid, fd);
  if (!c) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  c->drm = 1;
  c->dev_id = SHIM_RESERVED;
  spinlock_release(&g_shim_lock);

  uint32_t dev_id = 0;
  struct nv_rm_device_info info;
  for (unsigned i = 0; i < sizeof(info); i++) ((unsigned char *)&info)[i] = 0;
  int rc = rm()->device_open(&dev_id, &info);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int ours = (c && c->dev_id == SHIM_RESERVED);
  if (rc == 0 && ours) {
    c->dev_id = dev_id;
    c->info = info;
    *dev = dev_id;
  } else if (ours) {
    c->dev_id = 0;
    c->used = 0;
    c->pid = 0;
    c->fd = -1;
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc == -ENODEV ? -ENODEV : -EIO;
  if (!ours) {
    rm()->device_close(dev_id);
    return -EBADF;
  }
  row_begin("drm device", &r_drm);
  row_s("dev=");
  row_say((int)dev_id);
  row_s(" chipset=");
  row_hex(info.chipset);
  row_s(" mock=");
  row_say((int)info.mock);
  row_end();
  return 0;
}

/* Param table for both faces: 0 = answered, -ENOSYS = needs the card,
 * -EINVAL = not a nouveau param number. */

/* The value GETPARAM GRAPH_UNITS carries, packed exactly the way this
 * hardware's stack packs it.  SOURCE: nvkm/subdev/gsp/rm/r535/gr.c
 *     r535_gr_units():  return (gsp->gr.tpcs << 8) | gsp->gr.gpcs;
 * -- TPC TOTAL in bits 8-23, GPC count in bits 0-7, no ROP field.  (The
 * pre-Turing gf100_gr_units() adds rops at bit 32; Mesa masks those bits away,
 * and on Ada r535_gr_units never sets them, so a GSP-shaped answer is 32-bit.)
 * Where the two numbers come from is the RM's business: on this hardware they
 * exist only inside GSP-RM's GET_GSP_STATIC_INFO reply.  This function only
 * packs what the RM declares -- it cannot invent a topology, and
 * source == NV_RM_TOPO_NONE does not reach here at all. */
uint64_t nv_shim_gr_units_value(const struct nv_rm_gpu_topo *t) {
  if (!t || t->source == NV_RM_TOPO_NONE) return 0;
  return ((uint64_t)(t->tpc_total & 0xffffu) << 8) | (uint64_t)(t->gpc_nr & 0xffu);
}

/* Why a param has no answer here.  Two different absences, two names: a
 * topology the RM never received, versus a card this node does not have.
 * Exported so the tier holds the NAME to a value (a refusal whose reason is
 * only in a console string is not tested; mutation M2 exists to prove it). */
const char *nv_shim_param_refusal_reason(uint64_t param) {
  return param == NVSHIM_PARAM_GRAPH_UNITS ? "mock_has_no_topo" : "mock_has_no_card";
}

/* The word in the GRAPH_UNITS row that says where the number came from. */
static const char *topo_source_name(uint32_t source) {
  return source == NV_RM_TOPO_DECLARED ? "declared" : "none";
}

static int drm_param_value(const struct nv_rm_device_info *info, uint64_t param,
                           uint64_t *value) {
  switch (param) {
  case NVSHIM_PARAM_PCI_VENDOR: *value = info->vendor_id; return 0;
  case NVSHIM_PARAM_PCI_DEVICE: *value = info->device_id; return 0;
  case NVSHIM_PARAM_FB_SIZE: *value = info->fb_size; return 0;
  case NVSHIM_PARAM_CHIPSET_ID: *value = info->chipset; return 0;
  /* An aperture size, not an amount of memory (see nv_rm.h). */
  case NVSHIM_PARAM_VRAM_BAR_SIZE: *value = info->bar1_fb_size; return 0;
  case NVSHIM_DRM_PARAM_EXEC_PUSH_MAX: *value = NVSHIM_DRM_MAX_PUSH; return 0;
  /* Tile mode / PTE kind are not accepted by VM_BIND here, so the honest
   * answer is 0 (nouveau's own "feature absent" spelling). */
  case NVSHIM_DRM_PARAM_HAS_VMA_TILEMODE: *value = 0; return 0;
  /* The one param on this path a stock winsys cannot continue without.  It is
   * answered from the RM's topology -- a declared model when there is one, a
   * named refusal when there is not. */
  case NVSHIM_PARAM_GRAPH_UNITS:
    if (info->topo.source == NV_RM_TOPO_NONE) return -ENOSYS;
    *value = nv_shim_gr_units_value(&info->topo);
    return 0;
  case NVSHIM_PARAM_BUS_TYPE:
  case NVSHIM_PARAM_AGP_SIZE:
  case NVSHIM_PARAM_VM_VRAM_BASE:
  case NVSHIM_PARAM_PTIMER_TIME:
  case NVSHIM_PARAM_HAS_BO_USAGE:
  case NVSHIM_PARAM_HAS_PAGEFLIP:
  case NVSHIM_DRM_PARAM_VRAM_USED:
    return -ENOSYS; /* reads the card or the RM's allocator */
  default:
    return -EINVAL;
  }
}

/* ---- generic DRM rows -------------------------------------------------- */

static int drm_put_string(struct process *p, uint64_t uptr, uint64_t *len_io,
                          const char *src) {
  uint32_t n = drm_strlen(src) + 1;
  if (uptr) {
    if (*len_io < (uint64_t)n) return -EINVAL; /* buffer too small: DRM's rule */
    if (vm_kwrite(p, uptr, src, (int)n) != 0) return -EFAULT;
  }
  *len_io = n;
  return 0;
}

static int op_drm_version(struct process *p, uint64_t arg) {
  struct nvshim_drm_version v;
  if (vm_user_read(p, arg, &v, (int)sizeof v) != 0) return -EFAULT;
  v.major = 1;
  v.minor = 3;
  v.patchlevel = 1; /* Mesa's nouveau winsys requires >= 1.3.1 */
  v.pad = 0;
  int rc = drm_put_string(p, v.name, &v.name_len, "nouveau");
  if (rc == 0) rc = drm_put_string(p, v.date, &v.date_len, "20261123");
  /* The name is what a winsys matches on; the desc says what is really behind
   * the node, so a log line from a real client records the truth. */
  if (rc == 0) rc = drm_put_string(p, v.desc, &v.desc_len, "HobbyOS nvshim mock RM");
  if (rc != 0) return rc;
  if (vm_kwrite(p, arg, &v, (int)sizeof v) != 0) return -EFAULT;
  return 0;
}

static int op_drm_get_cap(struct process *p, uint64_t arg) {
  struct nvshim_drm_cap cap;
  if (vm_user_read(p, arg, &cap, (int)sizeof cap) != 0) return -EFAULT;
  switch (cap.capability) {
  case NVSHIM_DRM_CAP_SYNCOBJ: cap.value = 1; break;
  case NVSHIM_DRM_CAP_SYNCOBJ_TIMELINE: cap.value = 0; break;
  case NVSHIM_DRM_CAP_DUMB_BUFFER: cap.value = 0; break;
  default: return -EINVAL;
  }
  if (vm_kwrite(p, arg, &cap, (int)sizeof cap) != 0) return -EFAULT;
  return 0;
}

/* ---- GETPARAM ---------------------------------------------------------- */

static int op_drm_getparam(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_getparam q;
  if (vm_user_read(p, arg, &q, (int)sizeof q) != 0) return -EFAULT;

  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  struct nv_rm_device_info info;
  int have = (c && c->dev_id == dev);
  if (have) info = c->info;
  spinlock_release(&g_shim_lock);
  if (!have) return -ENODEV;

  uint64_t value = 0;
  rc = drm_param_value(&info, q.param, &value);
  if (rc == -ENOSYS) {
    row_begin("drm getparam refused", &r_param);
    row_s("param=");
    row_say((int)q.param);
    row_s(" reason=");
    row_s(nv_shim_param_refusal_reason(q.param));
    row_end();
    return rc;
  }
  if (rc != 0) return rc;
  q.value = value;
  if (vm_kwrite(p, arg, &q, (int)sizeof q) != 0) return -EFAULT;
  row_begin("drm getparam", &r_drm);
  row_s("param=");
  row_say((int)q.param);
  row_s(" value=");
  row_hex(value);
  /* GRAPH_UNITS is a decomposition, and which decomposition matters: a client
   * (and this project's own reports) must be able to see the GPC/TPC split and
   * that the number is DECLARED rather than measured.  Same row, three more
   * tokens -- the value alone would hide both facts. */
  if (q.param == NVSHIM_PARAM_GRAPH_UNITS) {
    row_s(" gpc=");
    row_say((int)info.topo.gpc_nr);
    row_s(" tpc=");
    row_say((int)info.topo.tpc_total);
    row_s(" src=");
    row_s(topo_source_name(info.topo.source));
  }
  row_end();
  return 0;
}

/* ---- memory objects ---------------------------------------------------- */

/* DRM_NOUVEAU_GEM_DOMAIN_* bits that describe a CPU mapping.  Round 2 refused
 * them all; WP-F3 serves one shape of them (CPU|MAPPABLE) and names every other
 * shape, because "the client asked for something I did not do" must never look
 * like success.
 *
 *  domain bits          meaning here
 *  ------------------   ----------------------------------------------------
 *  VRAM, GART           object memory; no CPU mapping (no BAR is mapped)
 *  CPU | MAPPABLE       pool-backed and CPU-mappable: mmap(2) may name it
 *  ... | COHERENT       allowed alongside MAPPABLE; it asserts the mapping
 *                       needs no cache maintenance, which is true of this
 *                       memory (Normal-NC pool RAM the device already sees)
 *  MAPPABLE w/o CPU     refused: there is no BAR to map VRAM through
 *  COHERENT w/o MAPPABLE refused: a coherence promise with no mapping
 *  NO_SHARE             accepted and recorded (WP-PRIME): the object is made
 *                       and maps locally, and PRIME EXPORT of it is refused by
 *                       name (reason=no_share).  Before PRIME existed there was
 *                       no export to refuse, so the bit was refused here
 *                       instead of being a silent drop.
 */
#define DRM_DOMAIN_CPU_BITS                                                        \
  (NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE | NVSHIM_DOMAIN_COHERENT |           \
   NVSHIM_DOMAIN_NO_SHARE)

/* 0 = a domain this node can serve; -1 = refused (caller names the reason). */
static int drm_domain_ok(uint32_t domain) {
  if ((domain & (NVSHIM_DOMAIN_VRAM | NVSHIM_DOMAIN_GART | NVSHIM_DOMAIN_CPU)) == 0)
    return -1; /* nothing to allocate from */
  /* WP-PRIME: NO_SHARE is ACCEPTED here now and honoured where it means
   * something -- PRIME export refuses such an object by name.  Before PRIME
   * existed the bit was refused at allocation because there was no export to
   * refuse, and accepting it would have been a silent drop. */
  if ((domain & NVSHIM_DOMAIN_MAPPABLE) && !(domain & NVSHIM_DOMAIN_CPU))
    return -1; /* no BAR: VRAM cannot be CPU-mapped here */
  if ((domain & NVSHIM_DOMAIN_COHERENT) && !(domain & NVSHIM_DOMAIN_MAPPABLE))
    return -1;
  return 0;
}

/* Does this domain promise a CPU mapping the client may mmap?  Only the shape
 * drm_domain_ok() accepts is ever asked about. */
static int drm_domain_cpu_mappable(uint32_t domain) {
  return (domain & (NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE)) ==
         (NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE);
}

/* DRM's fake offset for an object: drm_vma_node_offset_addr() hands userspace
 * (handle << PAGE_SHIFT) and expects it back in mmap(2)'s offset.  0 means "this
 * object is not CPU-mappable", which is what round 2 always answered. */
static uint64_t drm_map_handle(uint32_t domain, uint64_t handle) {
  if (!drm_domain_cpu_mappable(domain)) return 0;
  return handle << 12;
}

static int op_drm_gem_new(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_gem_new g;
  if (vm_user_read(p, arg, &g, (int)sizeof g) != 0) return -EFAULT;

  uint32_t domain = g.info.domain;
  int bad = 0;
  if (g.info.size == 0 || g.info.size > SHIM_MAX_VM_SIZE) bad = 1;
  if (drm_domain_ok(domain) != 0) bad = 1;
  if (g.channel_hint != 0) bad = 1;
  if (g.align != 0 && g.align != 4096u) bad = 1; /* the arena is 4 KiB-granular */
  if (bad) {
    row_begin("drm gem_new refused", &r_refuse);
    row_s("size=");
    row_hex(g.info.size);
    row_s(" domain=");
    row_hex(domain);
    row_s(" align=");
    row_hex(g.align);
    row_s(" reason=geometry");
    row_end();
    return -EINVAL;
  }

  uint64_t want = nv_shim_align_up(g.info.size, (uint32_t)SHIM_PAGE);
  if (want == 0) return -ENOMEM;

  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  int pid = shim_pid(p);
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!c || c->dev_id != dev) {
    spinlock_release(&g_shim_lock);
    return -ENODEV;
  }
  if (c->nalloc >= SHIM_MAX_ALLOCS) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  if (want > SHIM_MAX_ALLOC_BYTES || c->alloc_bytes > SHIM_MAX_ALLOC_BYTES - want) {
    spinlock_release(&g_shim_lock);
    row_begin("drm gem_new refused", &r_refuse);
    row_s("want=");
    row_hex(want);
    row_s(" reason=client_bytes");
    row_end();
    return -ENOMEM;
  }
  int slot = c->nalloc;
  c->alloc_handle[slot] = SHIM_RESERVED;
  c->alloc_bus[slot] = 0;
  c->alloc_size[slot] = 0;
  c->nalloc++;
  spinlock_release(&g_shim_lock);

  uint64_t handle = 0, bus = 0, granted = 0;
  rc = rm()->mem_alloc(dev, want, domain, &handle, &bus, &granted);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int done = 0;
  if (rc == 0 && c) {
    int at = client_reserved_alloc(c);
    if (at >= 0 && handle <= 0xFFFFFFFFull &&
        bo_add(dev, handle, bus, granted, domain) == 0) {
      c->alloc_handle[at] = handle;
      c->alloc_bus[at] = bus;
      c->alloc_size[at] = granted;
      c->alloc_domain[at] = domain;
      c->alloc_bytes += granted;
      done = 1;
    }
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc;
  if (!done) {
    rm()->mem_free(dev, handle);
    return -EBADF;
  }

  g.info.handle = (uint32_t)handle;
  g.info.size = granted;
  g.info.offset = bus;
  g.info.map_handle = drm_map_handle(domain, handle);
  g.info.tile_mode = 0;
  g.info.tile_flags = 0;
  if (vm_kwrite(p, arg, &g, (int)sizeof g) != 0) return -EFAULT;

  row_begin("drm gem_new", &r_drm);
  row_s("handle=");
  row_say((int)handle);
  row_s(" size=");
  row_hex(granted);
  row_s(" offset=");
  row_hex(bus);
  row_s(" domain=");
  row_hex(domain);
  row_s(" map=");
  row_hex(g.info.map_handle);
  row_end();
  return 0;
}

static int op_drm_gem_info(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_gem_info gi;
  if (vm_user_read(p, arg, &gi, (int)sizeof gi) != 0) return -EFAULT;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  int at = c ? client_find_alloc(c, gi.handle) : -1;
  uint64_t bus = 0, size = 0;
  uint32_t domain = 0;
  if (at >= 0) {
    bus = c->alloc_bus[at];
    size = c->alloc_size[at];
    domain = c->alloc_domain[at];
  }
  spinlock_release(&g_shim_lock);
  /* A handle this client does not own is -EINVAL, the same answer the 'N' face
   * gives for a foreign object (Linux DRM uses -ENOENT here; see the report). */
  if (at < 0) return -EINVAL;

  gi.domain = domain;
  gi.size = size;
  gi.offset = bus;
  gi.map_handle = drm_map_handle(domain, gi.handle);
  if (vm_kwrite(p, arg, &gi, (int)sizeof gi) != 0) return -EFAULT;
  row_begin("drm gem_info", &r_drm);
  row_s("handle=");
  row_say((int)gi.handle);
  row_s(" offset=");
  row_hex(bus);
  row_s(" size=");
  row_hex(size);
  row_s(" map=");
  row_hex(gi.map_handle);
  row_end();
  return 0;
}

/* ---- CPU mappings (WP-F3) ---------------------------------------------- */

/* Take down every mapping this (pid, fd[, handle]) made.  Always before the RM
 * frees the object's frames: a leaf is a raw frame address, so a mapping that
 * outlived its object would read (and write) whatever the frame allocator
 * hands out next.  Collection happens under the lock, the unmapping outside it
 * -- vm_munmap_range takes vm_lock, and no path may hold vm_lock then want the
 * shim's (docs/lock-scope.md). */
static void shim_maps_drop(struct process *p, int fd, uint64_t handle) {
  uint64_t va[SHIM_MAX_MAPS], len[SHIM_MAX_MAPS];
  int n = 0;
  int pid = shim_pid(p);
  struct process *grp = process_group(p);
  if (!grp) return;
  spinlock_acquire(&g_shim_lock);
  for (int i = 0; i < SHIM_MAX_MAPS; i++) {
    struct shim_map *m = &g_maps[i];
    /* `prime` maps belong to a dma-buf fd, never to the node fd: a node
     * GEM_CLOSE must not take them down. */
    if (!m->used || m->prime || m->pid != pid || m->fd != fd) continue;
    if (handle && m->handle != handle) continue;
    va[n] = m->va;
    len[n] = m->len;
    n++;
    m->used = 0;
    m->va = 0;
    m->len = 0;
    m->handle = 0;
  }
  spinlock_release(&g_shim_lock);
  for (int i = 0; i < n; i++) (void)vm_munmap_range(grp, va[i], len[i]);
}

/* Unmap and forget every mapping of one object, of either kind.  Used when the
 * object reaches refs == 0: its frames are about to go back to the pool, so no
 * VMA may still name them.  Entries can belong to OTHER process groups (a
 * dma-buf imported and mapped by a second client), which is why the group is
 * resolved from the stored tgid rather than from the caller.  A group that is
 * already gone has had its address space torn down, so there is nothing to
 * unmap there. */
static void shim_maps_drop_object(struct process *p, uint64_t handle) {
  uint64_t va[SHIM_MAX_MAPS], len[SHIM_MAX_MAPS];
  int tgs[SHIM_MAX_MAPS];
  int n = 0;
  spinlock_acquire(&g_shim_lock);
  for (int i = 0; i < SHIM_MAX_MAPS; i++) {
    struct shim_map *m = &g_maps[i];
    if (!m->used || m->handle != handle) continue;
    va[n] = m->va;
    len[n] = m->len;
    tgs[n] = m->pid;
    n++;
    m->used = 0;
    m->va = 0;
    m->len = 0;
    m->handle = 0;
    m->prime = 0;
    m->owner = 0;
  }
  spinlock_release(&g_shim_lock);
  for (int i = 0; i < n; i++) {
    struct process *grp = 0;
    if (p && shim_pid(p) == tgs[i]) grp = process_group(p);
    else {
      struct process *pcb = tgs[i] > 0 ? process_get_pcb(tgs[i]) : 0;
      grp = pcb ? process_group(pcb) : 0;
    }
    if (grp) (void)vm_munmap_range(grp, va[i], len[i]);
  }
}

/* Unmap and forget the mappings ONE dma-buf fd made.  `fd` < 0 (or a NULL
 * group) means "every mapping of this buffer", which is what the fd table's
 * context-free release path gets; `pid` < 0 means any group. */
static void shim_maps_drop_prime(void *owner, int pid, int fd,
                                 struct process *fallback) {
  uint64_t va[SHIM_MAX_MAPS], len[SHIM_MAX_MAPS];
  int tgs[SHIM_MAX_MAPS];
  int n = 0;
  spinlock_acquire(&g_shim_lock);
  for (int i = 0; i < SHIM_MAX_MAPS; i++) {
    struct shim_map *m = &g_maps[i];
    if (!m->used || !m->prime || m->owner != owner) continue;
    if (pid >= 0 && m->pid != pid) continue;
    if (fd >= 0 && m->fd != fd) continue;
    va[n] = m->va;
    len[n] = m->len;
    tgs[n] = m->pid;
    n++;
    m->used = 0;
    m->va = 0;
    m->len = 0;
    m->handle = 0;
    m->prime = 0;
    m->owner = 0;
  }
  spinlock_release(&g_shim_lock);
  for (int i = 0; i < n; i++) {
    struct process *grp = 0;
    if (fallback && shim_pid(fallback) == tgs[i]) grp = process_group(fallback);
    else {
      struct process *pcb = tgs[i] > 0 ? process_get_pcb(tgs[i]) : 0;
      grp = pcb ? process_group(pcb) : 0;
    }
    if (grp) (void)vm_munmap_range(grp, va[i], len[i]);
  }
}

/* Build one MAP_SHARED region over the object's own frames [phys, phys+len)
 * and record it so the right owner can take it down later.  Shared by the node
 * fd's mmap (prime = 0, `owner` unused) and the dma-buf fd's mmap (prime = 1),
 * which is what makes "an mmap of the fd is the same pages" a property of one
 * code path instead of two that agree by luck.
 * Returns the region's base VA, or a negative errno (nothing is left mapped). */
static int64_t shim_region_build(struct process *grp, uint64_t phys, uint64_t len,
                                 uint16_t prot, int pid, int fd, uint64_t handle,
                                 int prime, void *owner) {
  if (!grp || !grp->as || !phys || !len) return -EINVAL;
  struct vm_object *obj = vm_object_create(VM_OBJ_NVBO, len);
  if (!obj) return -ENOMEM;
  for (uint64_t off = 0; off < len; off += SHIM_PAGE) {
    if (!vm_object_install_foreign(obj, off, phys + off)) {
      vm_object_unref(obj);
      return -ENOMEM;
    }
  }
  uint64_t base = 0;
  int hrc = vm_region_alloc_hole(grp->as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2,
                                len, prot, VMK_SHARED, VM_MAP_SHARED, obj, 0,
                                &base);
  vm_object_unref(obj); /* the region record holds its own reference (S4) */
  if (hrc != 0) return -ENOMEM;

  spinlock_acquire(&g_shim_lock);
  int slot = -1;
  for (int i = 0; i < SHIM_MAX_MAPS; i++)
    if (!g_maps[i].used) {
      slot = i;
      break;
    }
  if (slot >= 0) {
    g_maps[slot].used = 1;
    g_maps[slot].pid = pid;
    g_maps[slot].fd = fd;
    g_maps[slot].handle = handle;
    g_maps[slot].va = base;
    g_maps[slot].len = len;
    g_maps[slot].prime = prime;
    g_maps[slot].owner = owner;
  }
  spinlock_release(&g_shim_lock);
  if (slot < 0) {
    (void)vm_munmap_range(grp, base, len);
    return -ENOSPC;
  }
  return (int64_t)base;
}

/* mmap(2) of a GEM object (declared in nv_shim.h; vm_mmap routes here). */
int64_t nv_shim_mmap(struct process *grp, int fd, uint64_t offset, uint64_t len,
                     uint16_t prot, uint32_t flags) {
  if (!grp || !grp->as) return -EINVAL;
  /* A GPU buffer mapping is MAP_SHARED or it is a lie: MAP_PRIVATE would mean
   * "give me a copy", and the whole point is that there is no copy. */
  if ((flags & VM_MAP_SHARED) == 0 ||
      (flags & (VM_MAP_PRIVATE | VM_MAP_ANONYMOUS | VM_MAP_FIXED | VM_MAP_NOREPLACE))) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("flags=");
    row_hex(flags);
    row_s(" reason=needs_shared");
    row_end();
    return -EINVAL;
  }
  if (offset == 0 || (offset & (SHIM_PAGE - 1))) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("off=");
    row_hex(offset);
    row_s(" reason=no_such_object");
    row_end();
    return -EINVAL;
  }
  if (len == 0 || (len & (SHIM_PAGE - 1))) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("len=");
    row_hex(len);
    row_s(" reason=length");
    row_end();
    return -EINVAL;
  }
  if (!(prot & VM_PROT_READ) || (prot & VM_PROT_EXEC)) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("prot=");
    row_hex(prot);
    row_s(" reason=protection");
    row_end();
    return -EINVAL;
  }
  uint64_t handle = offset >> 12; /* drm_vma_node_offset_addr's encoding */
  if (handle == 0 || handle > 0xFFFFFFFFull) return -EINVAL;

  int pid = shim_pid(grp);
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  int at = c ? client_find_alloc(c, (uint32_t)handle) : -1;
  uint32_t domain = 0, dev = 0;
  uint64_t size = 0;
  if (at >= 0) {
    domain = c->alloc_domain[at];
    size = c->alloc_size[at];
    dev = c->dev_id;
  }
  spinlock_release(&g_shim_lock);

  if (at < 0) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("handle=");
    row_say((int)handle);
    row_s(" reason=not_this_clients_object");
    row_end();
    return -EINVAL;
  }
  if (!drm_domain_cpu_mappable(domain)) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("handle=");
    row_say((int)handle);
    row_s(" domain=");
    row_hex(domain);
    row_s(" reason=domain_never_offered_a_cpu_mapping");
    row_end();
    return -EINVAL;
  }
  if (len > size) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("handle=");
    row_say((int)handle);
    row_s(" len=");
    row_hex(len);
    row_s(" size=");
    row_hex(size);
    row_s(" reason=beyond_object");
    row_end();
    return -EINVAL;
  }

  uint64_t phys = 0, plen = 0;
  int rc = rm()->mem_phys(dev, handle, &phys, &plen);
  if (rc != 0 || !phys || plen < len) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("handle=");
    row_say((int)handle);
    row_s(" rm=");
    row_say(rc);
    row_s(" reason=no_cpu_memory");
    row_end();
    return rc == -ENOTSUP ? -ENOTSUP : -EINVAL;
  }

  /* One vm_object names the object's page list; the ordinary fault path maps
   * leaf pages from it.  Nothing is copied, and no page is touched until the
   * client touches it.  The builder is shared with the dma-buf path below, so
   * "same pages" is one piece of code, not two that can drift. */
  int64_t built = shim_region_build(grp, phys, len, prot, pid, fd, handle, 0, 0);
  if (built < 0) {
    row_begin("drm mmap refused", &r_mmap);
    row_s("handle=");
    row_say((int)handle);
    if (built == -ENOSPC) row_s(" reason=map_table_full");
    else row_s(" reason=page_list_or_no_va");
    row_end();
    return built;
  }
  uint64_t base = (uint64_t)built;

  /* The object must still be the one we validated: the client could have closed
   * it while the region was being built.  Cheap re-check, and the mapping is
   * taken down rather than left pointing at freed frames. */
  int still = 0;
  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  at = c ? client_find_alloc(c, (uint32_t)handle) : -1;
  still = (at >= 0 && c->dev_id == dev && c->alloc_size[at] == size &&
           c->alloc_domain[at] == domain);
  spinlock_release(&g_shim_lock);
  if (!still) {
    shim_maps_drop(grp, fd, handle);
    row_begin("drm mmap refused", &r_mmap);
    row_s("handle=");
    row_say((int)handle);
    row_s(" reason=object_closed_during_map");
    row_end();
    return -EBADF;
  }

  row_begin("drm mmap", &r_mmap);
  row_s("handle=");
  row_say((int)handle);
  row_s(" va=");
  row_hex(base);
  row_s(" len=");
  row_hex(len);
  row_s(" phys=");
  row_hex(phys);
  row_s(" domain=");
  row_hex(domain);
  row_end();
  return (int64_t)base;
}

/* ---- PRIME / dma-buf (WP-PRIME) ----------------------------------------
 *
 * The contract is nv_drm_uapi.h's "PRIME contract" block.  In one sentence:
 * an exported fd names a dma-buf that holds ONE reference on the GEM object,
 * mmap of that fd maps the object's own frames, and the frames are freed by
 * whoever drops the last reference -- handle entry or exported fd.
 *
 * The fd itself lives in the ordinary file table (fs.c FILE_TYPE_DMABUF), so
 * dup(2), fork, close(2) and SCM_RIGHTS need no new machinery here: the file
 * description's ref_count is what keeps the dma-buf alive, and
 * nv_shim_dmabuf_release() runs exactly once, when the last reference goes.
 * ---------------------------------------------------------------------- */

static struct shim_dmabuf *dmabuf_alloc_locked(void) {
  for (int i = 0; i < SHIM_MAX_DMABUF; i++)
    if (!g_dmabufs[i].used) {
      struct shim_dmabuf *b = &g_dmabufs[i];
      b->used = 1;
      b->dev = 0;
      b->handle = 0;
      b->size = 0;
      b->domain = 0;
      b->id = ++g_dmabuf_serial;
      return b;
    }
  return 0;
}

static int op_drm_prime_handle_to_fd(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_prime_handle ph;
  if (vm_user_read(p, arg, &ph, (int)sizeof ph) != 0) return -EFAULT;
  if (ph.flags & ~(uint32_t)(NVSHIM_DRM_PRIME_CLOEXEC | NVSHIM_DRM_PRIME_RDWR)) {
    row_begin("prime refuse", &r_prime);
    row_s("dir=export handle=");
    row_say((int)ph.handle);
    row_s(" reason=");
    row_prime_why(NVSHIM_PRIME_BAD_FLAGS);
    row_s(" flags=");
    row_hex(ph.flags);
    row_end();
    return -EINVAL;
  }

  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  int pid = shim_pid(p);
  uint32_t domain = 0;
  uint64_t handle = 0, size = 0;
  struct shim_dmabuf *buf = 0;
  int why = NVSHIM_PRIME_OK;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  int at = c ? client_find_alloc(c, ph.handle) : -1;
  if (at < 0) {
    spinlock_release(&g_shim_lock);
    row_begin("prime refuse", &r_prime);
    row_s("dir=export handle=");
    row_say((int)ph.handle);
    row_s(" reason=not_this_clients_object");
    row_end();
    return -EINVAL;
  }
  domain = c->alloc_domain[at];
  handle = c->alloc_handle[at];
  size = c->alloc_size[at];
  if (domain & NVSHIM_DOMAIN_NO_SHARE) {
    spinlock_release(&g_shim_lock);
    row_begin("prime refuse", &r_prime);
    row_s("dir=export handle=");
    row_say((int)ph.handle);
    row_s(" reason=");
    row_prime_why(NVSHIM_PRIME_NO_SHARE);
    row_end();
    return -EINVAL;
  }
  buf = dmabuf_alloc_locked();
  if (buf && bo_ref(dev, handle) != 0) {
    buf->used = 0;
    buf = 0;
    why = NVSHIM_PRIME_NOT_DMABUF; /* not a tracked object: refuse, never guess */
  }
  if (buf) {
    buf->dev = dev;
    buf->handle = handle;
    buf->size = size;
    buf->domain = domain;
  }
  spinlock_release(&g_shim_lock);

  if (!buf) {
    row_begin("prime refuse", &r_prime);
    row_s("dir=export handle=");
    row_say((int)ph.handle);
    row_s(" reason=");
    if (why == NVSHIM_PRIME_OK) row_s("no_dmabuf_slot");
    else row_prime_why(why);
    row_end();
    return why == NVSHIM_PRIME_OK ? -ENOSPC : -EINVAL;
  }

  /* The fd is a real one: the file table installs it (lowest free fd, the
   * DRM_CLOEXEC bit from `flags`), and from here on its lifetime is the file
   * description's, not ours. */
  int ufd = file_dmabuf_install(p, buf, (ph.flags & NVSHIM_DRM_PRIME_CLOEXEC) != 0);
  if (ufd < 0) {
    spinlock_acquire(&g_shim_lock);
    buf->used = 0;
    int last = bo_unref(dev, handle);
    spinlock_release(&g_shim_lock);
    if (last) {
      shim_maps_drop_object(p, handle);
      rm()->mem_free(dev, handle);
    }
    row_begin("prime refuse", &r_prime);
    row_s("dir=export handle=");
    row_say((int)ph.handle);
    row_s(" reason=no_fd_slot rc=");
    row_say(ufd);
    row_end();
    return ufd == -ENFILE ? -ENFILE : -EMFILE;
  }

  ph.fd = ufd;
  if (vm_kwrite(p, arg, &ph, (int)sizeof ph) != 0) {
    /* The fd is installed; close(2) semantics apply, so drop it here. */
    (void)file_close(p, ufd);
    return -EFAULT;
  }
  row_begin("prime export", &r_prime);
  row_s("handle=");
  row_say((int)ph.handle);
  row_s(" fd=");
  row_say(ufd);
  row_s(" size=");
  row_hex(size);
  row_s(" domain=");
  row_hex(domain);
  row_s(" buf=");
  row_say(buf->id);
  row_end();
  return 0;
}

static int op_drm_prime_fd_to_handle(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_prime_handle ph;
  if (vm_user_read(p, arg, &ph, (int)sizeof ph) != 0) return -EFAULT;
  if (ph.flags != 0) {
    row_begin("prime refuse", &r_prime);
    row_s("dir=import reason=");
    row_prime_why(NVSHIM_PRIME_BAD_FLAGS);
    row_s(" flags=");
    row_hex(ph.flags);
    row_end();
    return -EINVAL;
  }
  if (ph.fd < 0) return -EINVAL;

  /* Resolve the dma-buf through the fd table: this accepts ANY fd the process
   * holds, including one received over SCM_RIGHTS (fs_msg_install_gfd puts the
   * message's reference in this very table). */
  struct shim_dmabuf *buf = (struct shim_dmabuf *)file_dmabuf_obj(p, ph.fd);
  if (!buf) {
    row_begin("prime refuse", &r_prime);
    row_s("dir=import fd=");
    row_say(ph.fd);
    row_s(" reason=");
    row_prime_why(NVSHIM_PRIME_NOT_DMABUF);
    row_end();
    return -EINVAL;
  }

  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  /* Cross-device import is a NAMED refusal: this node has one mock device, so
   * a buffer exported by another one is not something the RM seam can attach
   * (there is no dma_buf_attach equivalent), and importing it silently would
   * be a lie about which device owns the memory. */
  if (buf->dev != dev) {
    row_begin("prime refuse", &r_prime);
    row_s("dir=import buf=");
    row_say(buf->id);
    row_s(" reason=");
    row_prime_why(NVSHIM_PRIME_CROSS_DEVICE);
    row_s(" bufdev=");
    row_say((int)buf->dev);
    row_s(" dev=");
    row_say((int)dev);
    row_end();
    return -EINVAL;
  }

  int pid = shim_pid(p);
  int why = NVSHIM_PRIME_OK;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!c || c->dev_id != dev) {
    spinlock_release(&g_shim_lock);
    return -ENODEV;
  }
  if (c->nalloc >= SHIM_MAX_ALLOCS) {
    spinlock_release(&g_shim_lock);
    row_begin("prime refuse", &r_prime);
    row_s("dir=import reason=table_full");
    row_end();
    return -ENOSPC;
  }
  if (c->alloc_bytes > SHIM_MAX_ALLOC_BYTES - buf->size) {
    spinlock_release(&g_shim_lock);
    row_begin("prime refuse", &r_prime);
    row_s("dir=import reason=client_bytes");
    row_end();
    return -ENOMEM;
  }
  if (bo_ref(dev, buf->handle) != 0) why = NVSHIM_PRIME_NOT_DMABUF;
  int slot = -1;
  if (why == NVSHIM_PRIME_OK) {
    slot = c->nalloc;
    c->alloc_handle[slot] = buf->handle;
    c->alloc_bus[slot] = 0;
    c->alloc_size[slot] = buf->size;
    c->alloc_domain[slot] = buf->domain;
    c->nalloc++;
    c->alloc_bytes += buf->size;
    /* The object's own address, so GEM_INFO on an imported handle answers the
     * truth rather than 0. */
    struct shim_bo *b = bo_find(dev, buf->handle);
    if (b) c->alloc_bus[slot] = b->bus;
  }
  spinlock_release(&g_shim_lock);
  if (why != NVSHIM_PRIME_OK) {
    row_begin("prime refuse", &r_prime);
    row_s("dir=import reason=");
    row_prime_why(why);
    row_end();
    return -EINVAL;
  }

  ph.handle = (uint32_t)buf->handle;
  if (vm_kwrite(p, arg, &ph, (int)sizeof ph) != 0) return -EFAULT;
  row_begin("prime import", &r_prime);
  row_s("buf=");
  row_say(buf->id);
  row_s(" fd=");
  row_say(ph.fd);
  row_s(" handle=");
  row_say((int)ph.handle);
  row_s(" size=");
  row_hex(buf->size);
  row_end();
  return 0;
}

/* mmap(2) of a dma-buf fd (declared in nv_shim.h; vm.c routes here). */
int64_t nv_shim_dmabuf_mmap(struct process *grp, int fd, uint64_t offset,
                            uint64_t len, uint16_t prot, uint32_t flags) {
  if (!grp || !grp->as) return -EINVAL;
  if ((flags & VM_MAP_SHARED) == 0 ||
      (flags & (VM_MAP_PRIVATE | VM_MAP_ANONYMOUS | VM_MAP_FIXED | VM_MAP_NOREPLACE))) {
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" reason=needs_shared flags=");
    row_hex(flags);
    row_end();
    return -EINVAL;
  }
  if (offset != 0) { /* a dma-buf fd has one offset: the start of the buffer */
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" off=");
    row_hex(offset);
    row_s(" reason=offset_must_be_zero");
    row_end();
    return -EINVAL;
  }
  if (len == 0 || (len & (SHIM_PAGE - 1))) {
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" len=");
    row_hex(len);
    row_s(" reason=length");
    row_end();
    return -EINVAL;
  }
  if (!(prot & VM_PROT_READ) || (prot & VM_PROT_EXEC)) {
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" prot=");
    row_hex(prot);
    row_s(" reason=protection");
    row_end();
    return -EINVAL;
  }

  struct shim_dmabuf *buf = (struct shim_dmabuf *)file_dmabuf_obj(grp, fd);
  if (!buf) {
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" reason=");
    row_prime_why(NVSHIM_PRIME_NOT_DMABUF);
    row_end();
    return -EINVAL;
  }
  if (!drm_domain_cpu_mappable(buf->domain)) {
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" buf=");
    row_say(buf->id);
    row_s(" domain=");
    row_hex(buf->domain);
    row_s(" reason=");
    row_prime_why(NVSHIM_PRIME_NOT_MAPPABLE);
    row_end();
    return -EINVAL;
  }
  if (len > buf->size) {
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" len=");
    row_hex(len);
    row_s(" size=");
    row_hex(buf->size);
    row_s(" reason=beyond_object");
    row_end();
    return -EINVAL;
  }

  /* The CPU view comes from the RM, exactly as the node-fd path asks for it:
   * the pages are the object's, so there is nothing to copy and nothing to
   * translate.  Cached on the object once resolved. */
  uint64_t phys = 0, plen = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_bo *b = bo_find(buf->dev, buf->handle);
  if (b && b->phys) {
    phys = b->phys;
    plen = b->size;
  }
  uint32_t bdev = buf->dev;
  uint64_t bhandle = buf->handle;
  spinlock_release(&g_shim_lock);
  if (!phys) {
    int rc = rm()->mem_phys(bdev, bhandle, &phys, &plen);
    if (rc != 0 || !phys) {
      row_begin("prime mmap refused", &r_prime);
      row_s("fd=");
      row_say(fd);
      row_s(" rm=");
      row_say(rc);
      row_s(" reason=");
      row_prime_why(NVSHIM_PRIME_NOT_MAPPABLE);
      row_end();
      return rc == -ENOTSUP ? -ENOTSUP : -EINVAL;
    }
    spinlock_acquire(&g_shim_lock);
    b = bo_find(bdev, bhandle);
    if (b) b->phys = phys;
    spinlock_release(&g_shim_lock);
  }
  if (plen < len) return -EINVAL;

  int64_t base = shim_region_build(grp, phys, len, prot, shim_pid(grp), fd,
                                   bhandle, 1, buf);
  if (base < 0) {
    row_begin("prime mmap refused", &r_prime);
    row_s("fd=");
    row_say(fd);
    row_s(" reason=no_va rc=");
    row_say((int)base);
    row_end();
    return base;
  }
  row_begin("prime mmap", &r_prime);
  row_s("fd=");
  row_say(fd);
  row_s(" buf=");
  row_say(buf->id);
  row_s(" handle=");
  row_say((int)bhandle);
  row_s(" va=");
  row_hex((uint64_t)base);
  row_s(" phys=");
  row_hex(phys);
  row_end();
  return base;
}

/* The last reference to a dma-buf file description is going away: forget its
 * mappings, drop its reference on the GEM object and (if that was the last
 * one) hand the frames back.  Called by fs.c's close paths, outside the file
 * slot's lock, because unmapping takes vm_lock. */
void nv_shim_dmabuf_release(struct process *grp, int fd, void *obj) {
  struct shim_dmabuf *buf = (struct shim_dmabuf *)obj;
  if (!buf) return;
  int pid = (grp && fd >= 0) ? shim_pid(grp) : -1;
  shim_maps_drop_prime(buf, pid, pid >= 0 ? fd : -1, grp);
  uint32_t dev = 0;
  uint64_t handle = 0;
  int id = 0;
  spinlock_acquire(&g_shim_lock);
  if (buf->used) {
    dev = buf->dev;
    handle = buf->handle;
    id = buf->id;
    buf->used = 0;
    buf->dev = 0;
    buf->handle = 0;
  }
  int last = handle ? bo_unref(dev, handle) : 0;
  spinlock_release(&g_shim_lock);
  if (handle && last) {
    shim_maps_drop_object(grp, handle);
    rm()->mem_free(dev, handle);
  }
  row_begin("prime release", &r_prime);
  row_s("buf=");
  row_say(id);
  row_s(" fd=");
  row_say(fd);
  row_s(" last=");
  row_say(last);
  row_end();
}

/* --- kernel-tier seams (not ABI; no client can reach these) -------------- */

/* Make a live dma-buf claim another device.  The mock has exactly one device
 * (MOCK_DEV_ID), so the cross-device import branch has no reachable input from
 * userspace; without this seam the refusal would be untested prose. */
int nv_shim_test_prime_set_dev(void *obj, uint32_t dev) {
  struct shim_dmabuf *buf = (struct shim_dmabuf *)obj;
  if (!buf) return -EINVAL;
  spinlock_acquire(&g_shim_lock);
  if (!buf->used) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  buf->dev = dev;
  spinlock_release(&g_shim_lock);
  return 0;
}

/* Live references on the object behind a dma-buf: handle entries + exports.
 * -1 when `obj` is not a live buffer.  This is how a close-order test states
 * "still alive" / "freed" instead of inferring it. */
int nv_shim_test_prime_refs(void *obj) {
  struct shim_dmabuf *buf = (struct shim_dmabuf *)obj;
  if (!buf) return -1;
  int refs = -1;
  spinlock_acquire(&g_shim_lock);
  if (buf->used) {
    struct shim_bo *b = bo_find(buf->dev, buf->handle);
    refs = b ? b->refs : 0;
  }
  spinlock_release(&g_shim_lock);
  return refs;
}

static int op_drm_gem_close(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_gem_close gc;
  if (vm_user_read(p, arg, &gc, (int)sizeof gc) != 0) return -EFAULT;

  uint32_t dev = 0;
  uint64_t handle = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  int at = c ? client_find_alloc(c, gc.handle) : -1;
  if (at < 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  dev = c->dev_id;
  handle = c->alloc_handle[at];
  c->alloc_bytes -= c->alloc_size[at];
  for (int i = at; i + 1 < c->nalloc; i++) {
    c->alloc_handle[i] = c->alloc_handle[i + 1];
    c->alloc_bus[i] = c->alloc_bus[i + 1];
    c->alloc_size[i] = c->alloc_size[i + 1];
    c->alloc_domain[i] = c->alloc_domain[i + 1];
  }
  c->nalloc--;
  spinlock_release(&g_shim_lock);

  /* WP-F3: the mapping's leaves name frames of this object; take it down before
   * the RM frees them.  WP-PRIME: this client's mapping goes with this handle
   * even when the OBJECT survives (an exported fd holds it), because a mapping
   * is owned by the handle that made it -- not a reference of its own. */
  shim_maps_drop(p, fd, handle);
  spinlock_acquire(&g_shim_lock);
  int last = bo_unref(dev, handle);
  spinlock_release(&g_shim_lock);
  if (!last) {
    row_begin("drm gem_close", &r_drm);
    row_s("handle=");
    row_say((int)gc.handle);
    row_s(" kept refs=");
    row_say(1);
    row_s(" reason=prime_fd_open");
    row_end();
    return 0;
  }
  shim_maps_drop_object(p, handle);
  int rc = rm()->mem_free(dev, handle);
  if (rc != 0) return -EIO;
  row_begin("drm gem_close", &r_drm);
  row_s("handle=");
  row_say((int)gc.handle);
  row_s(" freed=1 refs=0");
  row_end();
  return 0;
}

/* ---- VM_INIT / VM_BIND ------------------------------------------------- */

static int op_drm_vm_init(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_vm_init v;
  if (vm_user_read(p, arg, &v, (int)sizeof v) != 0) return -EFAULT;

  if (v.kernel_managed_addr == 0 || (v.kernel_managed_addr & (SHIM_PAGE - 1)) ||
      (v.kernel_managed_size & (SHIM_PAGE - 1)))
    return -EINVAL;
  if (v.kernel_managed_size > NVSHIM_DRM_VA_MAX ||
      v.kernel_managed_addr > NVSHIM_DRM_VA_MAX - v.kernel_managed_size)
    return -EINVAL;
  uint64_t total = v.kernel_managed_addr + v.kernel_managed_size;

  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  int pid = shim_pid(p);
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!c || c->dev_id != dev) {
    spinlock_release(&g_shim_lock);
    return -ENODEV;
  }
  /* The uAPI: "must be called before any BOs or channels are created; if
   * called afterwards DRM_IOCTL_NOUVEAU_VM_INIT fails with -ENOSYS." */
  if (c->nalloc > 0 || c->nch > 0) {
    spinlock_release(&g_shim_lock);
    row_begin("drm vm refused", &r_refuse);
    row_s("reason=objects_exist");
    row_end();
    return -ENOSYS;
  }
  if (c->drm_vm != 0) {
    spinlock_release(&g_shim_lock);
    return -EBUSY; /* one VA space per file: VM_BIND has no VM id */
  }
  if (c->nvm >= SHIM_MAX_VMS) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  c->vm_id[c->nvm++] = SHIM_RESERVED;
  spinlock_release(&g_shim_lock);

  uint32_t vm_id = 0;
  rc = rm()->vm_create(dev, total, &vm_id);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int done = 0;
  if (rc == 0 && c) {
    int at = client_reserved_vm(c);
    if (at >= 0) {
      c->vm_id[at] = vm_id;
      c->drm_vm = vm_id;
      done = 1;
    }
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc;
  if (!done) {
    rm()->vm_destroy(dev, vm_id);
    return -EBADF;
  }
  row_begin("drm vm", &r_drm);
  row_s("vm=");
  row_say((int)vm_id);
  row_s(" umd_end=");
  row_hex(v.kernel_managed_addr);
  row_s(" total=");
  row_hex(total);
  row_end();
  return 0;
}

static int drm_bind_op_shape_ok(const struct nvshim_drm_vm_bind_op *op) {
  if (op->op != NVSHIM_DRM_VM_BIND_OP_MAP && op->op != NVSHIM_DRM_VM_BIND_OP_UNMAP) return 0;
  if (op->flags != 0) return 0; /* SPARSE is refused before this is called */
  if ((op->addr & (SHIM_PAGE - 1)) || op->range == 0) return 0;
  if (op->range & (SHIM_PAGE - 1)) return 0;
  if (op->addr >= NVSHIM_DRM_VA_MAX || op->range > NVSHIM_DRM_VA_MAX - op->addr) return 0;
  return 1;
}

static int op_drm_vm_bind(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_vm_bind b;
  if (vm_user_read(p, arg, &b, (int)sizeof b) != 0) return -EFAULT;
  if (b.flags != 0) {
    row_begin("drm bind refused", &r_refuse);
    row_s("flags=");
    row_hex(b.flags);
    row_s(" reason=async_not_implemented");
    row_end();
    return -ENOSYS;
  }
  if (b.wait_count != 0 || b.sig_count != 0) {
    row_begin("drm bind refused", &r_refuse);
    row_s("reason=syncobj_waits");
    row_end();
    return -ENOSYS;
  }
  if (b.op_count == 0 || b.op_count > NVSHIM_DRM_MAX_VM_BIND_OPS || b.op_ptr == 0)
    return -EINVAL;

  struct nvshim_drm_vm_bind_op ops[NVSHIM_DRM_MAX_VM_BIND_OPS];

  for (uint32_t i = 0; i < b.op_count; i++) {
    if (vm_user_read(p, b.op_ptr + (uint64_t)i * sizeof(struct nvshim_drm_vm_bind_op),
                     &ops[i], (int)sizeof ops[i]) != 0)
      return -EFAULT;
    if (ops[i].flags & NVSHIM_DRM_VM_BIND_SPARSE) {
      row_begin("drm bind refused", &r_refuse);
      row_s("reason=sparse_not_implemented");
      row_end();
      return -ENOSYS;
    }
    if (!drm_bind_op_shape_ok(&ops[i])) {
      row_begin("drm bind refused", &r_refuse);
      row_s("op=");
      row_say((int)ops[i].op);
      row_s(" addr=");
      row_hex(ops[i].addr);
      row_s(" range=");
      row_hex(ops[i].range);
      row_s(" reason=geometry");
      row_end();
      return -EINVAL;
    }
  }

  uint32_t dev = 0, vm = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  /* Pass 2, under the lock: every referenced object must be this client's and
   * cover the requested range.  Validating the whole batch before applying any
   * of it is what keeps a bad op[3] from leaving op[0..2] mapped. */
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  if (!c || c->dev_id != dev || c->drm_vm == 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  vm = c->drm_vm;
  for (uint32_t i = 0; i < b.op_count; i++) {
    if (ops[i].op != NVSHIM_DRM_VM_BIND_OP_MAP) continue;
    int at = client_find_alloc(c, ops[i].handle);
    if (at < 0 || ops[i].bo_offset > c->alloc_size[at] ||
        ops[i].range > c->alloc_size[at] - ops[i].bo_offset) {
      int bad_handle = (int)ops[i].handle;
      spinlock_release(&g_shim_lock);
      row_begin("drm bind refused", &r_refuse);
      row_s("handle=");
      row_say(bad_handle);
      row_s(" reason=not_this_clients_object");
      row_end();
      return -EINVAL;
    }
  }
  spinlock_release(&g_shim_lock);

  for (uint32_t i = 0; i < b.op_count; i++) {
    rc = rm()->vm_bind(dev, vm, ops[i].op,
                       ops[i].op == NVSHIM_DRM_VM_BIND_OP_MAP ? ops[i].handle : 0,
                       ops[i].addr, ops[i].range, ops[i].bo_offset);
    if (rc != 0) {
      row_begin("drm bind refused", &r_refuse);
      row_s("op_index=");
      row_say((int)i);
      row_s(" rm=");
      row_say(rc);
      row_end();
      return rc;
    }
  }

  row_begin("drm bind", &r_drm);
  row_s("vm=");
  row_say((int)vm);
  row_s(" ops=");
  row_say((int)b.op_count);
  row_s(" first_addr=");
  row_hex(ops[0].addr);
  row_s(" first_range=");
  row_hex(ops[0].range);
  row_end();
  return 0;
}

/* ---- channels ---------------------------------------------------------- */

static int op_drm_channel_alloc(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_channel_alloc ca;
  if (vm_user_read(p, arg, &ca, (int)sizeof ca) != 0) return -EFAULT;

  if (ca.notifier_handle != 0) {
    row_begin("drm channel refused", &r_refuse);
    row_s("reason=notifier_object");
    row_end();
    return -ENOSYS; /* notifier objects are RM objects: none in the mock */
  }
  /* The legacy context-DMA fields, as nouveau itself reads them since Kepler:
   * fb_ctxdma_handle == ~0 means "no legacy context DMA, use tt_ctxdma_handle
   * as the ENGINE SELECTOR" (nouveau_abi16_ioctl_channel_alloc), and the reply
   * normalises both to 0.  A stock winsys sends exactly {~0, NOUVEAU_FIFO_
   * ENGINE_GR}, so refusing that shape refused the parent of every NVIF child.
   * Only GR has a mock channel; every other selector is named, not treated as
   * GR. */
  if (ca.fb_ctxdma_handle == 0xFFFFFFFFu) {
    if (ca.tt_ctxdma_handle != 0 && ca.tt_ctxdma_handle != NVSHIM_ENGINE_GR) {
      row_begin("drm channel refused", &r_refuse);
      row_s("reason=engine_selector tt=");
      row_hex(ca.tt_ctxdma_handle);
      row_end();
      return -ENOSYS; /* VP/PPP/BSP/CE/NVDEC channels need the real RM */
    }
  } else if (ca.fb_ctxdma_handle != 0 || ca.tt_ctxdma_handle != 0) {
    row_begin("drm channel refused", &r_refuse);
    row_s("reason=legacy_ctxdma");
    row_end();
    return -ENOSYS; /* pre-nvif context DMAs: the RM has none */
  }
  if (ca.nr_subchan != 0) {
    row_begin("drm channel refused", &r_refuse);
    row_s("reason=subchannel_classes");
    row_end();
    return -ENOSYS;
  }
  uint32_t domain = ca.pushbuf_domains;
  if (domain == 0) domain = NVSHIM_DOMAIN_VRAM;
  if ((domain & (NVSHIM_DOMAIN_VRAM | NVSHIM_DOMAIN_GART)) == 0 || (domain & DRM_DOMAIN_CPU_BITS))
    return -EINVAL;
  /* One domain for the push buffer: VRAM wins when both are asked for, and the
   * answer is reported back in pushbuf_domains. */
  domain = (domain & NVSHIM_DOMAIN_VRAM) ? NVSHIM_DOMAIN_VRAM : NVSHIM_DOMAIN_GART;

  uint32_t dev = 0, vm = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  int pid = shim_pid(p);
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  if (!c || c->dev_id != dev || c->drm_vm == 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  if (c->nch >= SHIM_MAX_CHANNELS) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  vm = c->drm_vm;
  int slot = c->nch;
  c->ch_id[slot] = SHIM_RESERVED;
  c->ch_pushbuf[slot] = 0;
  c->nch++;
  spinlock_release(&g_shim_lock);

  uint64_t pb_handle = 0, pb_bus = 0, pb_size = 0;
  rc = rm()->mem_alloc(dev, (uint64_t)NVSHIM_DRM_PUSHBUF_SIZE, domain, &pb_handle,
                       &pb_bus, &pb_size);
  uint32_t ch_id = 0, engine = 0;
  if (rc == 0)
    rc = rm()->channel_create(dev, vm, pb_bus, (uint32_t)pb_size, &ch_id, &engine);
  if (rc != 0 && pb_handle) rm()->mem_free(dev, pb_handle);

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  int done = 0;
  if (rc == 0 && c) {
    int at = client_reserved_ch(c);
    if (at >= 0 && ch_id <= 0x7FFFFFFFu) {
      c->ch_id[at] = ch_id;
      c->ch_pushbuf[at] = pb_handle;
      done = 1;
    }
  }
  spinlock_release(&g_shim_lock);

  if (rc != 0) return rc;
  if (!done) {
    rm()->channel_destroy(dev, ch_id);
    rm()->mem_free(dev, pb_handle);
    return -EBADF;
  }

  ca.channel = (int32_t)ch_id;
  ca.pushbuf_domains = domain;
  ca.nr_subchan = 0;
  /* nouveau normalises the engine-selector spelling in its reply; a client that
   * reads the fields back must see the same "no legacy ctxdma" values. */
  ca.fb_ctxdma_handle = 0;
  ca.tt_ctxdma_handle = 0;
  for (unsigned i = 0; i < 8; i++) {
    ca.subchan[i].handle = 0;
    ca.subchan[i].grclass = 0;
  }
  if (vm_kwrite(p, arg, &ca, (int)sizeof ca) != 0) return -EFAULT;

  row_begin("drm channel", &r_drm);
  row_s("ch=");
  row_say((int)ch_id);
  row_s(" pushbuf=");
  row_hex(pb_bus);
  row_s(" size=");
  row_hex(pb_size);
  row_end();
  return 0;
}

static int op_drm_channel_free(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_channel_free cf;
  if (vm_user_read(p, arg, &cf, (int)sizeof cf) != 0) return -EFAULT;

  uint32_t dev = 0;
  uint64_t pb = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  int at = c ? client_find_ch(c, (uint32_t)cf.channel) : -1;
  if (at < 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  dev = c->dev_id;
  pb = c->ch_pushbuf[at];
  for (int i = at; i + 1 < c->nch; i++) {
    c->ch_id[i] = c->ch_id[i + 1];
    c->ch_pushbuf[i] = c->ch_pushbuf[i + 1];
  }
  c->nch--;
  spinlock_release(&g_shim_lock);

  rm()->channel_destroy(dev, (uint32_t)cf.channel);
  if (pb) rm()->mem_free(dev, pb);
  row_begin("drm channel_free", &r_drm);
  row_s("ch=");
  row_say((int)cf.channel);
  row_end();
  return 0;
}

/* ---- syncobjs (binary only) -------------------------------------------- */

static int client_find_sobj(const struct shim_client *c, uint32_t sobj) {
  for (int i = 0; i < c->nsobj; i++)
    if (c->sobj[i] == sobj) return i;
  return -1;
}

/* A syncobj is signalled when the fence it is waiting on has retired.  Called
 * with g_shim_lock held: fence_test is a constant-time read on the mock, so the
 * shim -> mock lock order holds and nothing blocks inside the lock. */
static void client_reap_locked(struct shim_client *c) {
  for (int i = 0; i < c->npend;) {
    int st = rm()->fence_test(c->pend[i].ch, c->pend[i].fence);
    if (st == -EAGAIN) {
      i++;
      continue;
    }
    /* 0 = retired; -EINVAL = that channel is gone, so the fence can never
     * retire.  Either way the entry is finished; dropping it keeps the table
     * from filling with entries that can never be cleared. */
    int at = client_find_sobj(c, c->pend[i].sobj);
    if (at >= 0) c->sobj_sig[at] = 1;
    for (int k = i; k + 1 < c->npend; k++) c->pend[k] = c->pend[k + 1];
    c->npend--;
  }
}

static int drm_user_u32s(struct process *p, uint64_t uptr, uint32_t *out, uint32_t n) {
  if (n == 0) return 0;
  if (uptr == 0) return -EINVAL;
  if (vm_user_read(p, uptr, out, (int)(n * (uint32_t)sizeof(uint32_t))) != 0) return -EFAULT;
  return 0;
}

static int op_drm_syncobj_create(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_syncobj_create sc;
  if (vm_user_read(p, arg, &sc, (int)sizeof sc) != 0) return -EFAULT;
  if (sc.flags & ~NVSHIM_DRM_SYNCOBJ_CREATE_SIGNALED) return -EINVAL;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  if (!c) {
    spinlock_release(&g_shim_lock);
    return -ENODEV;
  }
  if (c->nsobj >= SHIM_MAX_SYNCOBJS) {
    spinlock_release(&g_shim_lock);
    return -ENOSPC;
  }
  uint32_t handle = 0;
  for (uint32_t cand = 1;; cand++) {
    if (client_find_sobj(c, cand) < 0) {
      handle = cand;
      break;
    }
  }
  c->sobj[c->nsobj] = handle;
  c->sobj_sig[c->nsobj] = (sc.flags & NVSHIM_DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0;
  c->nsobj++;
  spinlock_release(&g_shim_lock);

  sc.handle = handle;
  if (vm_kwrite(p, arg, &sc, (int)sizeof sc) != 0) return -EFAULT;
  row_begin("drm syncobj", &r_drm);
  row_s("create handle=");
  row_say((int)handle);
  row_s(" signaled=");
  row_say((sc.flags & NVSHIM_DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0);
  row_end();
  return 0;
}

static int op_drm_syncobj_destroy(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_syncobj_destroy sd;
  if (vm_user_read(p, arg, &sd, (int)sizeof sd) != 0) return -EFAULT;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  int at = c ? client_find_sobj(c, sd.handle) : -1;
  if (at < 0) {
    spinlock_release(&g_shim_lock);
    return -EINVAL;
  }
  for (int i = at; i + 1 < c->nsobj; i++) {
    c->sobj[i] = c->sobj[i + 1];
    c->sobj_sig[i] = c->sobj_sig[i + 1];
  }
  c->nsobj--;
  /* Drop pendings that referred to it: their signal has nowhere to land. */
  for (int i = 0; i < c->npend;) {
    if (c->pend[i].sobj == sd.handle) {
      for (int k = i; k + 1 < c->npend; k++) c->pend[k] = c->pend[k + 1];
      c->npend--;
    } else {
      i++;
    }
  }
  spinlock_release(&g_shim_lock);
  return 0;
}

/* Are the named syncobjs signalled?  0 = all (or one, if !all), -ETIME = not
 * yet, -EINVAL = a handle this client does not own. */
static int drm_sobj_ready(struct process *p, int fd, const uint32_t *hs, uint32_t n,
                          int all, uint32_t *first_signaled) {
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  if (!c) {
    spinlock_release(&g_shim_lock);
    return -ENODEV;
  }
  client_reap_locked(c);
  int any = 0, every = 1;
  uint32_t first = 0;
  for (uint32_t i = 0; i < n; i++) {
    int at = client_find_sobj(c, hs[i]);
    if (at < 0) {
      spinlock_release(&g_shim_lock);
      return -EINVAL;
    }
    if (c->sobj_sig[at]) {
      if (!any) first = i;
      any = 1;
    } else {
      every = 0;
    }
  }
  if (first_signaled) *first_signaled = first;
  spinlock_release(&g_shim_lock);
  if (all ? every : any) return 0;
  return -ETIME;
}

static int op_drm_syncobj_wait(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_syncobj_wait w;
  if (vm_user_read(p, arg, &w, (int)sizeof w) != 0) return -EFAULT;
  if (w.count_handles == 0 || w.count_handles > SHIM_MAX_SYNCOBJS) return -EINVAL;
  if (w.flags & (NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
                 NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
                 NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)) {
    row_begin("drm syncobj refused", &r_refuse);
    row_s("wait flags=");
    row_hex(w.flags);
    row_end();
    return -ENOSYS; /* needs fence registration / deadlines, not a poll */
  }
  if (w.flags & ~NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) return -EINVAL;

  uint32_t handles[SHIM_MAX_SYNCOBJS];
  int rc = drm_user_u32s(p, w.handles, handles, w.count_handles);
  if (rc != 0) return rc;

  int all = (w.flags & NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? 1 : 0;
  rc = drm_sobj_ready(p, fd, handles, w.count_handles, all, &w.first_signaled);
  if (rc != -ETIME) {
    if (rc == 0) {
      if (vm_kwrite(p, arg, &w, (int)sizeof w) != 0) return -EFAULT;
      row_begin("drm syncobj", &r_drm);
      row_s("wait n=");
      row_say((int)w.count_handles);
      row_s(" retired=1");
      row_end();
    }
    return rc;
  }

  /* Not signalled yet.  timeout_nsec is an absolute CLOCK_MONOTONIC deadline, 0
   * means poll, and ~0 means "forever" -- which a kernel that may not park
   * inside a syscall clamps, and says so. */
  if (w.timeout_nsec == 0 || w.count_handles == 0) {
    row_begin("drm syncobj", &r_drm);
    row_s("wait n=");
    row_say((int)w.count_handles);
    row_s(" retired=0 timeout=poll");
    row_end();
    return -ETIME;
  }
  uint64_t now_ms = timer_get_ms();
  uint64_t deadline_ms;
  int clamped = 0;
  if ((uint64_t)w.timeout_nsec == ~0ull) {
    deadline_ms = now_ms + SHIM_FENCE_WAIT_MAX_MS;
    clamped = 1;
  } else {
    deadline_ms = (uint64_t)w.timeout_nsec / 1000000ull;
    if (deadline_ms > now_ms + SHIM_FENCE_WAIT_MAX_MS) {
      deadline_ms = now_ms + SHIM_FENCE_WAIT_MAX_MS;
      clamped = 1;
    }
  }
  int spins = 0;
  while (rc == -ETIME) {
    if (spins++ >= SHIM_WAIT_SPINS) break;
    if (timer_get_ms() >= deadline_ms) break;
    for (int k = 0; k < 500; k++) shim_cpu_relax();
    rc = drm_sobj_ready(p, fd, handles, w.count_handles, all, &w.first_signaled);
  }
  if (rc == 0) {
    if (vm_kwrite(p, arg, &w, (int)sizeof w) != 0) return -EFAULT;
    row_begin("drm syncobj", &r_drm);
    row_s("wait n=");
    row_say((int)w.count_handles);
    row_s(" retired=1 waited=1 clamped=");
    row_say(clamped);
    row_end();
    return 0;
  }
  if (rc != -ETIME) return rc;
  row_begin("drm syncobj", &r_drm);
  row_s("wait n=");
  row_say((int)w.count_handles);
  row_s(" retired=0 timeout=ms clamped=");
  row_say(clamped);
  row_end();
  return -ETIME;
}

/* RESET (flags == 0) and SIGNAL (flags == 1) share drm_syncobj_array. */
static int op_drm_syncobj_set(struct process *p, int fd, uint64_t arg, int signal) {
  struct nvshim_drm_syncobj_array sa;
  if (vm_user_read(p, arg, &sa, (int)sizeof sa) != 0) return -EFAULT;
  if (sa.count_handles == 0 || sa.count_handles > SHIM_MAX_SYNCOBJS) return -EINVAL;
  uint32_t handles[SHIM_MAX_SYNCOBJS];
  int rc = drm_user_u32s(p, sa.handles, handles, sa.count_handles);
  if (rc != 0) return rc;

  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  if (!c) {
    spinlock_release(&g_shim_lock);
    return -ENODEV;
  }
  for (uint32_t i = 0; i < sa.count_handles; i++) {
    int at = client_find_sobj(c, handles[i]);
    if (at < 0) {
      spinlock_release(&g_shim_lock);
      return -EINVAL;
    }
    c->sobj_sig[at] = signal ? 1 : 0;
  }
  spinlock_release(&g_shim_lock);
  return 0;
}

/* ---- EXEC -------------------------------------------------------------- */

/* FNV-1a 32, byte at a time: a command stream is arbitrary bytes, and the kernel
 * may not take wide loads at odd offsets on any arch (hobbyos-kernel-
 * constraints).  The unit tier digests with the same function, so its expected
 * value comes from the bytes rather than from a copy of the loop. */
static uint32_t shim_sum_update(uint32_t h, const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    h ^= (uint32_t)p[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t nv_shim_test_sum(const void *buf, uint32_t len) {
  if (!buf || len == 0) return 2166136261u;
  return shim_sum_update(2166136261u, (const uint8_t *)buf, len);
}

#define SHIM_EXEC_CHUNK 4096u

static int op_drm_exec(struct process *p, int fd, uint64_t arg) {
  struct nvshim_drm_exec ex;
  if (vm_user_read(p, arg, &ex, (int)sizeof ex) != 0) return -EFAULT;

  if (ex.wait_count != 0) {
    row_begin("drm exec refused", &r_refuse);
    row_s("reason=wait_syncobjs");
    row_end();
    return -ENOSYS; /* a pre-submit wait is a blocking wait: no parking here */
  }
  if (ex.push_count == 0 || ex.push_count > NVSHIM_DRM_MAX_PUSH || ex.push_ptr == 0)
    return -EINVAL;
  if (ex.sig_count > NVSHIM_DRM_MAX_SYNCS) return -EINVAL;
  if (ex.sig_count && ex.sig_ptr == 0) return -EINVAL;

  struct nvshim_drm_exec_push pushes[NVSHIM_DRM_MAX_PUSH];
  for (uint32_t i = 0; i < ex.push_count; i++) {
    if (vm_user_read(p, ex.push_ptr + (uint64_t)i * sizeof(struct nvshim_drm_exec_push),
                     &pushes[i], (int)sizeof pushes[i]) != 0)
      return -EFAULT;
    if ((pushes[i].va & 3) || pushes[i].va_len == 0 || (pushes[i].va_len & 3) ||
        (pushes[i].flags & ~NVSHIM_DRM_EXEC_PUSH_NO_PREFETCH)) {
      row_begin("drm exec refused", &r_refuse);
      row_s("va=");
      row_hex(pushes[i].va);
      row_s(" len=");
      row_say((int)pushes[i].va_len);
      row_s(" reason=push_shape");
      row_end();
      return -EINVAL;
    }
  }

  struct nvshim_drm_sync syncs[NVSHIM_DRM_MAX_SYNCS];
  for (uint32_t i = 0; i < ex.sig_count; i++) {
    if (vm_user_read(p, ex.sig_ptr + (uint64_t)i * sizeof(struct nvshim_drm_sync),
                     &syncs[i], (int)sizeof syncs[i]) != 0)
      return -EFAULT;
    if ((syncs[i].flags & NVSHIM_DRM_SYNC_TYPE_MASK) == NVSHIM_DRM_SYNC_TIMELINE_SYNCOBJ) {
      row_begin("drm exec refused", &r_refuse);
      row_s("reason=timeline_syncobj");
      row_end();
      return -ENOSYS; /* binary syncobjs only */
    }
    if ((syncs[i].flags & NVSHIM_DRM_SYNC_TYPE_MASK) != NVSHIM_DRM_SYNC_SYNCOBJ ||
        syncs[i].flags != NVSHIM_DRM_SYNC_SYNCOBJ || syncs[i].timeline_value != 0) {
      row_begin("drm exec refused", &r_refuse);
      row_s("sync flags=");
      row_hex(syncs[i].flags);
      row_s(" reason=unknown_sync_type");
      row_end();
      return -EINVAL;
    }
  }

  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  int pid = shim_pid(p);
  uint32_t ch = ex.channel;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  int owned_ch = (c && c->dev_id == dev && client_find_ch(c, ch) >= 0);
  int sobj_ok = 1;
  if (owned_ch) {
    for (uint32_t i = 0; i < ex.sig_count; i++)
      if (client_find_sobj(c, syncs[i].handle) < 0) sobj_ok = 0;
  }
  spinlock_release(&g_shim_lock);
  if (!owned_ch) {
    row_begin("drm exec refused", &r_refuse);
    row_s("ch=");
    row_say((int)ch);
    row_s(" reason=not_this_clients_channel");
    row_end();
    return -EINVAL;
  }
  if (!sobj_ok) return -EINVAL;

  /* WP-F3: READ every push buffer before enqueuing any of them.  A GPU VA is
   * the only name EXEC has for the bytes (there is no user pointer on this
   * request), so this is where the kernel finally looks at what it is
   * accepting -- and where a push that was written through a CPU mapping is
   * seen exactly as the client wrote it, because the mapping IS this memory.
   * Fetching the whole batch first also means a bad push cannot leave the
   * earlier ones enqueued. */
  uint32_t sum = 2166136261u;
  uint32_t bytes = 0;
  for (uint32_t i = 0; i < ex.push_count; i++) {
    uint32_t off = 0;
    while (off < pushes[i].va_len) {
      uint32_t chunk = pushes[i].va_len - off;
      if (chunk > SHIM_EXEC_CHUNK) chunk = SHIM_EXEC_CHUNK;
      rc = rm()->read_va(ch, pushes[i].va + (uint64_t)off, g_cs_buf, chunk);
      if (rc != 0) {
        row_begin("drm exec refused", &r_refuse);
        row_s("push_index=");
        row_say((int)i);
        row_s(" va=");
        row_hex(pushes[i].va + (uint64_t)off);
        row_s(" rm=");
        row_say(rc);
        row_s(" reason=stream_not_in_memory");
        row_end();
        return rc == -EFAULT ? -EFAULT : -EINVAL;
      }
      sum = shim_sum_update(sum, g_cs_buf, chunk);
      bytes += chunk;
      off += chunk;
    }
  }

  uint64_t fence = 0;
  for (uint32_t i = 0; i < ex.push_count; i++) {
    rc = rm()->submit_va(ch, pushes[i].va, pushes[i].va_len, &fence);
    if (rc != 0) {
      /* Earlier pushes of this batch are already enqueued; a real RM would
       * unwind them from its own job list.  Named so the log shows it. */
      row_begin("drm exec refused", &r_refuse);
      row_s("push_index=");
      row_say((int)i);
      row_s(" va=");
      row_hex(pushes[i].va);
      row_s(" rm=");
      row_say(rc);
      row_end();
      return rc;
    }
  }
  /* The engine-progress stand-in, exactly as the 'N' face's SUBMIT does. */
  rm()->kick();

  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  if (c) {
    /* WP-F3: record what this EXEC fetched, for the unit tier's seam.  It is
     * written under the same lock as every other client field, and it is the
     * client's own record: another client's EXEC cannot overwrite it. */
    c->exec_have = 1;
    c->exec_sum = sum;
    c->exec_len = bytes;
    client_reap_locked(c);
    for (uint32_t i = 0; i < ex.sig_count; i++) {
      int at = client_find_sobj(c, syncs[i].handle);
      if (at < 0) continue;
      if (rm()->fence_test(ch, fence) == 0) {
        c->sobj_sig[at] = 1; /* retired already: no pending entry needed */
      } else if (c->npend < SHIM_MAX_SIGPEND) {
        c->pend[c->npend].sobj = syncs[i].handle;
        c->pend[c->npend].ch = ch;
        c->pend[c->npend].fence = fence;
        c->npend++;
      } else {
        /* Table full: the syncobj would never be signalled, so say so. */
        spinlock_release(&g_shim_lock);
        return -ENOSPC;
      }
    }
  }
  spinlock_release(&g_shim_lock);

  row_begin("drm exec", &r_drm);
  row_s("ch=");
  row_say((int)ch);
  row_s(" pushes=");
  row_say((int)ex.push_count);
  row_s(" sigs=");
  row_say((int)ex.sig_count);
  row_s(" bytes=");
  row_say((int)bytes);
  row_s(" stream=");
  row_hex(sum);
  row_s(" fence=");
  row_hex(fence);
  row_end();
  return 0;
}

/* ---- kernel-tier observation seams (not ABI) --------------------------- */

/* -ENOENT: this client has not completed an EXEC yet.  (errno.h has no
 * ENODATA; the distinction that matters is "recorded" vs "never recorded".) */
int nv_shim_test_last_exec(struct process *p, int fd, uint32_t *sum, uint32_t *len) {
  int rc = -ENOENT;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(shim_pid(p), fd);
  if (c && c->exec_have) {
    if (sum) *sum = c->exec_sum;
    if (len) *len = c->exec_len;
    rc = 0;
  }
  spinlock_release(&g_shim_lock);
  return rc;
}

/* ============================================================================
 *  NVIF -- DRM_IOCTL_NOUVEAU_NVIF (nr 0x47).  Contract: nv_nvif_uapi.h.
 *
 *  This is the object model a stock winsys uses.  Mesa's nouveau winsys
 *  (src/nouveau/winsys/nouveau_device.c) does, in this order:
 *
 *      NVIF NEW   route 0x00, oclass NV_DEVICE, cookie = its device struct
 *      MTHD       route 0x00, method NV_DEVICE_V0_INFO
 *      ... CHANNEL_ALLOC ...
 *      SCLASS     route 0xff, token = channel
 *      NVIF NEW   route 0xff, token = channel, oclass = <engine class>
 *
 *  What this node can answer WITHOUT a GPU is the first three rows plus the
 *  SCLASS list, because they are bookkeeping over identity the mock RM already
 *  carries.  The fourth -- instantiating an HW class on a channel -- is a real
 *  GSP object; it is refused with -ENOSYS and a row naming the class id, which
 *  is the point of the lane: the caller learns exactly where the wall is.
 *
 *  Locking: user copies happen outside g_shim_lock (they can fault); the object
 *  table is only touched under it; rows are emitted after it is released.
 * ========================================================================== */

/* The engine classes an AD102 channel offers, in the order nouveau's engine
 * tables list them (gr/ga102.c then ce/ga102.c).  Names are ours -- a row has
 * to say WHICH class it refused in words -- and the ids are class.h's.
 *
 * The last two columns are what the refusal must name, because "need=gsp_rm"
 * alone does not say what to build.  SOURCE, the Ada path:
 *   NVIF NEW <engine class> -> nvkm_rm_gr_obj_ctor / nvkm_rm_engine_obj_ctor
 *   -> nvkm_rm_engine_obj_new(&chan->rm.object, chan->id, oclass)
 *   -> GR/2D/M2MF:  nvkm_gsp_rm_alloc(chan, handle, class, 0, &obj->rm)
 *      CE:          rm->api->ce->alloc() -> nvkm_gsp_rm_alloc_get/wr with
 *                   NVC0B5_ALLOCATION_PARAMETERS{engineType = COPY0 + inst}
 * (nvkm/subdev/gsp/rm/engine.c, .../r535/ce.c).  So every engine object is one
 * RM allocation parented on the CHANNEL's RM object, and neither exists here:
 * this node's channel is bookkeeping.  The two missing things are therefore
 * GSP-RM itself and that channel RM object -- and that is what the row says. */
struct nvif_class_row {
  int32_t oclass;
  char name[24];
  const char *engine; /* engine instance the class runs on, as nvk names it */
  const char *alloc;  /* the RM allocation that would create the object */
};

static const struct nvif_class_row g_nvif_engines[] = {
    {NVSHIM_NVIF_CLASS_FERMI_TWOD_A, "fermi_twod_a", "eng2d", "rm_alloc"},
    {NVSHIM_NVIF_CLASS_KEPLER_INLINE_TO_MEM_B, "kepler_inline_m2mf", "m2mf", "rm_alloc"},
    {NVSHIM_NVIF_CLASS_AMPERE_B, "ampere_b_gr", "eng3d", "rm_alloc"},
    {NVSHIM_NVIF_CLASS_AMPERE_COMPUTE_B, "ampere_compute_b", "compute", "rm_alloc"},
    {NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_A, "ampere_dma_copy_a", "copy0", "ce_alloc"},
    {NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B, "ampere_dma_copy_b", "copy1", "ce_alloc"},
};
#define NVIF_ENGINE_CLASSES (int)(sizeof g_nvif_engines / sizeof g_nvif_engines[0])

/* The same table, as data a test can assert on: class -> (row name, engine,
 * RM allocation).  -1 for a class SCLASS does not report.  The refusal row is
 * printed from these fields, so a change to what the wall claims cannot pass
 * the tier without changing the table. */
int nv_shim_nvif_engine_class(int32_t oclass, const char **name,
                              const char **engine, const char **alloc) {
  for (int i = 0; i < NVIF_ENGINE_CLASSES; i++) {
    if (g_nvif_engines[i].oclass != oclass) continue;
    if (name) *name = g_nvif_engines[i].name;
    if (engine) *engine = g_nvif_engines[i].engine;
    if (alloc) *alloc = g_nvif_engines[i].alloc;
    return 0;
  }
  return -1;
}

/* The product name a PCI id table would supply.  0x2684 is the RTX 4090
 * (PLAN.md §0); a winsys overwrites this from its own table, so it is
 * identification, not a capability. */
static const char g_nvif_product[] = "NVIDIA GeForce RTX 4090";

static void nvif_strlcpy(char *dst, const char *src, int cap) {
  int i = 0;
  if (cap <= 0) return;
  while (i + 1 < cap && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

/* One row for a refusal, and the errno it carries.  A refusal that is not
 * named on the console is the failure mode this project keeps paying for. */
static int nvif_refuse(const char *stage, const char *reason, int rc) {
  row_begin(stage, &r_nvif);
  row_s("refused reason=");
  row_s(reason);
  row_end();
  return rc;
}

/* Is this client's NVIF tree holding `cookie`?  -1 = no.  Under the lock. */
static int nvif_find_locked(const struct shim_client *c, uint64_t cookie) {
  for (int i = 0; i < c->nnvif; i++)
    if (c->nvif[i].cookie == cookie) return i;
  return -1;
}

/* ---- NEW --------------------------------------------------------------- */

static int nvif_new(struct process *p, int fd, uint32_t argc, uint64_t arg,
                    const struct nvshim_nvif_hdr *h) {
  if (argc < (uint32_t)(sizeof *h + sizeof(struct nvshim_nvif_new)))
    return nvif_refuse("nvif new", "size", -EINVAL);

  struct nvshim_nvif_new nw;
  if (vm_user_read(p, arg + sizeof *h, &nw, (int)sizeof nw) != 0) return -EFAULT;
  if (nw.version != 0) return nvif_refuse("nvif new", "version", -EINVAL);

  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;

  if (h->route == NVSHIM_NVIF_ROUTE_NVIF) {
    /* nouveau: route 0x00 is the client's own root, so nothing may be named
     * as a parent and the only class it accepts is NV_DEVICE. */
    if (h->object != 0 || nw.oclass != NVSHIM_NVIF_CLASS_DEVICE)
      return nvif_refuse("nvif new", "device_shape", -EINVAL);
    if (nw.object == 0) return nvif_refuse("nvif new", "cookie", -EINVAL);

    int pid = shim_pid(p);
    spinlock_acquire(&g_shim_lock);
    struct shim_client *c = client_find(pid, fd);
    int have = c ? nvif_find_locked(c, nw.object) : -1;
    int rcl = 0;
    if (!c)
      rcl = -EBADF;
    else if (have >= 0)
      rcl = -EEXIST; /* the same cookie twice: nouveau's answer */
    else if (c->nnvif >= SHIM_MAX_NVIF)
      rcl = -ENOSPC;
    else {
      int at = c->nnvif++;
      c->nvif[at].cookie = nw.object;
      c->nvif[at].oclass = nw.oclass;
      c->nvif[at].ch = 0;
    }
    spinlock_release(&g_shim_lock);
    if (rcl == -EEXIST) return nvif_refuse("nvif new class=0x80", "cookie_exists", rcl);
    if (rcl == -ENOSPC) return nvif_refuse("nvif new class=0x80", "table_full", rcl);
    if (rcl != 0) return rcl;

    row_begin("nvif new", &r_nvif);
    row_s("class=0x80 cookie=");
    row_hex(nw.object);
    row_s(" dev=");
    row_say((int)dev);
    row_s(" served=device");
    row_end();
    return 0;
  }

  /* route 0xff: a child of the object named by `token` -- for every class a
   * winsys asks for, that is a channel. */
  if (h->token == 0 || h->token > 0xFFFFFFFFull)
    return nvif_refuse("nvif new", "channel", -EINVAL);

  int pid = shim_pid(p);
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  int owns = (c && client_find_ch(c, (uint32_t)h->token) >= 0);
  spinlock_release(&g_shim_lock);
  if (!owns) return nvif_refuse("nvif new", "channel", -EINVAL);

  /* SCLASS reported these; instantiating one is an RM object (a GR/CE/etc
   * context) on a real channel, so it is refused BY NAME, with the id -- and
   * with the two things that are actually missing: an RM channel object to
   * parent the allocation on, and the RM allocation itself. */
  for (int i = 0; i < NVIF_ENGINE_CLASSES; i++) {
    if (g_nvif_engines[i].oclass != nw.oclass) continue;
    row_begin("nvif new", &r_nvif);
    row_s("refused class=");
    row_hex((uint64_t)(uint32_t)nw.oclass);
    row_s(" name=");
    row_s(g_nvif_engines[i].name);
    row_s(" need=gsp_rm missing=rm_chan_object alloc=");
    row_s(g_nvif_engines[i].alloc);
    row_s(" engine=");
    row_s(g_nvif_engines[i].engine);
    row_end();
    return -ENOSYS;
  }

  row_begin("nvif new", &r_nvif);
  row_s("refused class=");
  row_hex((uint64_t)(uint32_t)nw.oclass);
  row_s(" reason=no_such_class");
  row_end();
  return -EINVAL;
}

/* ---- DEL --------------------------------------------------------------- */

static int nvif_del(struct process *p, int fd, uint32_t argc, uint64_t arg,
                    const struct nvshim_nvif_hdr *h) {
  (void)arg;
  /* nouveau requires route 0 and no payload at all. */
  if (h->route != NVSHIM_NVIF_ROUTE_NVIF)
    return nvif_refuse("nvif del", "route", -EINVAL);
  if (argc != (uint32_t)sizeof *h) return nvif_refuse("nvif del", "size", -EINVAL);

  int pid = shim_pid(p);
  int found = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  int at = c ? nvif_find_locked(c, h->object) : -1;
  if (at >= 0) {
    for (int i = at; i + 1 < c->nnvif; i++) c->nvif[i] = c->nvif[i + 1];
    c->nnvif--;
    c->nvif[c->nnvif].cookie = 0;
    c->nvif[c->nnvif].oclass = 0;
    c->nvif[c->nnvif].ch = 0;
    found = 1;
  }
  spinlock_release(&g_shim_lock);

  /* An unknown cookie is not an error -- nouveau's del handler returns 0 and
   * a winsys relies on it (it destroys objects it may never have made). */
  row_begin("nvif del", &r_nvif);
  row_s("object=");
  row_hex(h->object);
  row_s(" found=");
  row_say(found);
  row_end();
  return 0;
}

/* ---- SCLASS ------------------------------------------------------------ */

static int nvif_sclass(struct process *p, int fd, uint32_t argc, uint64_t arg,
                       const struct nvshim_nvif_hdr *h) {
  if (h->route == NVSHIM_NVIF_ROUTE_NVIF)
    return nvif_refuse("nvif sclass", "route", -EINVAL);
  if (argc < (uint32_t)(sizeof *h + sizeof(struct nvshim_nvif_sclass)))
    return nvif_refuse("nvif sclass", "size", -EINVAL);

  struct nvshim_nvif_sclass sc;
  if (vm_user_read(p, arg + sizeof *h, &sc, (int)sizeof sc) != 0) return -EFAULT;
  if (sc.version != 0) return nvif_refuse("nvif sclass", "version", -EINVAL);
  if (sc.count > NVSHIM_NVIF_MAX_SCLASS)
    return nvif_refuse("nvif sclass", "count", -EINVAL);
  /* nouveau's shape rule: the buffer must hold exactly `count` entries. */
  if (argc != (uint32_t)(sizeof *h + sizeof sc) +
                  (uint32_t)sc.count * (uint32_t)sizeof(struct nvshim_nvif_sclass_oclass))
    return nvif_refuse("nvif sclass", "size", -EINVAL);

  if (h->token == 0 || h->token > 0xFFFFFFFFull)
    return nvif_refuse("nvif sclass", "channel", -EINVAL);

  int pid = shim_pid(p);
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  int owns = (c && client_find_ch(c, (uint32_t)h->token) >= 0);
  spinlock_release(&g_shim_lock);
  if (!owns) return nvif_refuse("nvif sclass", "channel", -EINVAL);

  /* New entries go in the caller's array (truncated to what it can hold); the
   * reply's count is the number the channel HAS, exactly as abi16 reports it. */
  int n = sc.count < NVIF_ENGINE_CLASSES ? (int)sc.count : NVIF_ENGINE_CLASSES;
  struct nvshim_nvif_sclass_oclass list[NVSHIM_NVIF_MAX_SCLASS];
  for (int i = 0; i < n; i++) {
    list[i].oclass = g_nvif_engines[i].oclass;
    list[i].minver = -1; /* "any version": the spelling of nouveau's tables */
    list[i].maxver = -1;
  }
  if (n > 0 &&
      vm_kwrite(p, arg + sizeof *h + sizeof sc, list,
                n * (int)sizeof(list[0])) != 0)
    return -EFAULT;
  sc.count = (uint8_t)NVIF_ENGINE_CLASSES;
  if (vm_kwrite(p, arg + sizeof *h, &sc, (int)sizeof sc) != 0) return -EFAULT;

  row_begin("nvif sclass", &r_nvif);
  row_s("ch=");
  row_say((int)h->token);
  row_s(" count=");
  row_say(NVIF_ENGINE_CLASSES);
  row_s(" first=");
  row_hex((uint64_t)(uint32_t)g_nvif_engines[0].oclass);
  row_s(" 3d=");
  /* What was actually WRITTEN to the caller for the 3D entry (not the table
   * constant), so a regression in the fill shows up in this row. */
  row_hex((uint64_t)(uint32_t)(n > 2 ? list[2].oclass : g_nvif_engines[2].oclass));
  row_end();
  return 0;
}

/* ---- MTHD -------------------------------------------------------------- */

static int nvif_mthd(struct process *p, int fd, uint32_t argc, uint64_t arg,
                     const struct nvshim_nvif_hdr *h) {
  if (h->route != NVSHIM_NVIF_ROUTE_NVIF)
    return nvif_refuse("nvif mthd", "route", -EINVAL);
  if (argc < (uint32_t)(sizeof *h + sizeof(struct nvshim_nvif_mthd)))
    return nvif_refuse("nvif mthd", "size", -EINVAL);

  struct nvshim_nvif_mthd m;
  if (vm_user_read(p, arg + sizeof *h, &m, (int)sizeof m) != 0) return -EFAULT;
  if (m.version != 0) return nvif_refuse("nvif mthd", "version", -EINVAL);

  /* The object must be a DEVICE object this client made.  A method on any
   * other object is -EINVAL, which is what abi16 answers. */
  int pid = shim_pid(p);
  int32_t oclass = 0;
  spinlock_acquire(&g_shim_lock);
  struct shim_client *c = client_find(pid, fd);
  int at = c ? nvif_find_locked(c, h->object) : -1;
  if (at >= 0) oclass = c->nvif[at].oclass;
  spinlock_release(&g_shim_lock);
  if (oclass != NVSHIM_NVIF_CLASS_DEVICE)
    return nvif_refuse("nvif mthd", "object", -EINVAL);

  if (m.method == NVSHIM_DEVICE_V0_TIME) {
    row_begin("nvif mthd", &r_nvif);
    row_s("refused method=1 name=device_time need=ptimer");
    row_end();
    return -ENOSYS;
  }
  if (m.method == NVSHIM_DEVICE_V0_GCX_READY) {
    row_begin("nvif mthd", &r_nvif);
    row_s("refused method=2 name=gcx_ready need=gsp_rm");
    row_end();
    return -ENOSYS;
  }
  if (m.method != NVSHIM_DEVICE_V0_INFO) {
    row_begin("nvif mthd", &r_nvif);
    row_s("refused method=");
    row_say((int)m.method);
    row_s(" reason=no_such_method");
    row_end();
    return -EINVAL;
  }

  /* The payload must be exactly one nv_device_info_v0 with version 0. */
  if (argc != (uint32_t)(sizeof *h + sizeof m + sizeof(struct nvshim_device_info_v0)))
    return nvif_refuse("nvif mthd", "size", -EINVAL);

  struct nvshim_device_info_v0 info;
  if (vm_user_read(p, arg + sizeof *h + sizeof m, &info, (int)sizeof info) != 0)
    return -EFAULT;
  if (info.version != 0) return nvif_refuse("nvif mthd", "version", -EINVAL);

  /* Fill from the RM's identity, which is what a real nvkm_udevice does: the
   * device object answers about the card, not about the node. */
  uint32_t dev = 0;
  int rc = drm_client_ensure(p, fd, &dev);
  if (rc != 0) return rc;
  struct nv_rm_device_info di;
  int have_info = 0;
  spinlock_acquire(&g_shim_lock);
  c = client_find(pid, fd);
  if (c) {
    di = c->info;
    have_info = 1;
  }
  spinlock_release(&g_shim_lock);
  if (!have_info) return -EBADF;

  info.platform = NVSHIM_DEVICE_INFO_V0_PCIE;
  info.chipset = (uint16_t)di.chipset;
  info.revision = (uint8_t)di.revision;
  info.family = NVSHIM_DEVICE_INFO_V0_ADA;
  info.pad06[0] = 0;
  info.pad06[1] = 0;
  info.ram_size = di.fb_size;
  info.ram_user = di.fb_size; /* the mock has no IMEM carve-out to subtract */
  for (int i = 0; i < (int)sizeof info.chip; i++) info.chip[i] = 0;
  for (int i = 0; i < (int)sizeof info.name; i++) info.name[i] = 0;
  nvif_strlcpy(info.chip, di.name, (int)sizeof info.chip);
  nvif_strlcpy(info.name, g_nvif_product, (int)sizeof info.name);

  if (vm_kwrite(p, arg + sizeof *h + sizeof m, &info, (int)sizeof info) != 0)
    return -EFAULT;

  row_begin("nvif info", &r_nvif);
  row_s("chip=");
  row_s(info.chip);
  row_s(" chipset=");
  row_hex(info.chipset);
  row_s(" fam=");
  row_hex(info.family);
  row_s(" ram=");
  row_hex(info.ram_user);
  row_end();
  return 0;
}

/* ---- entry for the NVIF row ------------------------------------------- */

static int op_drm_nvif(struct process *p, int fd, uint32_t r, uint64_t arg) {
  uint32_t argc = NVSHIM_DRM_SIZE(r);
  struct nvshim_nvif_hdr h;

  /* Unusually, the encoded size IS the buffer length (see nv_nvif_uapi.h):
   * every type below validates its own shape from here on. */
  if (argc < (uint32_t)sizeof h) return nvif_refuse("nvif", "size", -EINVAL);
  if (vm_user_read(p, arg, &h, (int)sizeof h) != 0) return -EFAULT;
  if (h.version != 0) return nvif_refuse("nvif", "version", -EINVAL);
  if (h.route != NVSHIM_NVIF_ROUTE_NVIF && h.route != NVSHIM_NVIF_ROUTE_HIDDEN)
    return nvif_refuse("nvif", "route", -EINVAL);

  switch (h.type) {
  case NVSHIM_NVIF_NEW: return nvif_new(p, fd, argc, arg, &h);
  case NVSHIM_NVIF_DEL: return nvif_del(p, fd, argc, arg, &h);
  case NVSHIM_NVIF_MTHD: return nvif_mthd(p, fd, argc, arg, &h);
  case NVSHIM_NVIF_SCLASS: return nvif_sclass(p, fd, argc, arg, &h);
  case NVSHIM_NVIF_MAP:
  case NVSHIM_NVIF_UNMAP:
    /* Mapping an object's memory needs the object to HAVE RM memory at a GPU
     * address; the mock allocates none for NVIF objects. */
    row_begin("nvif map", &r_nvif);
    row_s("refused need=gsp_rm");
    row_end();
    return -ENOSYS;
  default:
    row_begin("nvif", &r_nvif);
    row_s("refused type=");
    row_say((int)h.type);
    row_s(" reason=no_such_type");
    row_end();
    return -EINVAL;
  }
}

/* ---- entry ------------------------------------------------------------- */

int nv_shim_drm_ioctl(struct process *p, int fd, unsigned long req, uint64_t arg) {
  /* Linux's ioctl takes cmd as an unsigned int; truncating here is what keeps
   * a 64-bit register whose high half is junk from looking like a different
   * request. */
  uint32_t r = (uint32_t)req;
  uint32_t nr = NVSHIM_DRM_NR(r);

  const char *ref = drm_refusal_name(nr);
  if (ref) {
    row_begin("drm refuse", &r_refuse);
    row_s(ref);
    row_s(" errno=ENOSYS");
    row_end();
    return -ENOSYS;
  }
  int want = drm_row_size(nr);
  if (want < 0) return -ENOTTY;
  if (!p || arg == 0) return -EINVAL;
  /* NVIF's encoded size is its buffer length (see nv_nvif_uapi.h), so it is
   * routed before the equality check every other row gets. */
  if (nr == (uint8_t)NVSHIM_DRM_NR_NVIF) {
    if (!nv_shim_present()) return -ENODEV;
    return op_drm_nvif(p, fd, r, arg);
  }
  /* The size the number encodes must be the struct it pairs with.  This is the
   * check a winsys built against a different header trips, and it is the one a
   * mutation test is pointed at. */
  if (NVSHIM_DRM_SIZE(r) != (uint32_t)want) {
    row_begin("drm refuse", &r_refuse);
    row_s("size nr=");
    row_say((int)nr);
    row_s(" want=");
    row_say(want);
    row_s(" got=");
    row_say((int)NVSHIM_DRM_SIZE(r));
    row_end();
    return -EINVAL;
  }
  if (!nv_shim_present()) return -ENODEV;

  switch (nr) {
  case 0x00: return op_drm_version(p, arg);
  case 0x09: return op_drm_gem_close(p, fd, arg);
  case 0x0c: return op_drm_get_cap(p, arg);
  case 0x2d: return op_drm_prime_handle_to_fd(p, fd, arg);
  case 0x2e: return op_drm_prime_fd_to_handle(p, fd, arg);
  case 0x40: return op_drm_getparam(p, fd, arg);
  case 0x42: return op_drm_channel_alloc(p, fd, arg);
  case 0x43: return op_drm_channel_free(p, fd, arg);
  case 0x50: return op_drm_vm_init(p, fd, arg);
  case 0x51: return op_drm_vm_bind(p, fd, arg);
  case 0x52: return op_drm_exec(p, fd, arg);
  case 0x80: return op_drm_gem_new(p, fd, arg);
  case 0x84: return op_drm_gem_info(p, fd, arg);
  case 0xbf: return op_drm_syncobj_create(p, fd, arg);
  case 0xc0: return op_drm_syncobj_destroy(p, fd, arg);
  case 0xc3: return op_drm_syncobj_wait(p, fd, arg);
  case 0xc4: return op_drm_syncobj_set(p, fd, arg, 0);
  case 0xc5: return op_drm_syncobj_set(p, fd, arg, 1);
  default: return -ENOTTY; /* unreachable: drm_row_size() covered the table */
  }
}

/* ---- entry ------------------------------------------------------------- */

int nv_shim_ioctl(struct process *p, int fd, unsigned long req, uint64_t arg) {
  /* Two surfaces on one node.  Magic 'd' is nouveau's real DRM surface
   * (nv_drm_uapi.h): real numbers, real structs, size-checked.  Magic 'N' is
   * round 1's own numbering, kept working and documented as deprecated.  A 'd'
   * request is dispatched first so a DRM client never inherits 'N' semantics. */
  if (NVSHIM_DRM_TYPE((uint32_t)req) == (uint32_t)NVSHIM_DRM_MAGIC)
    return nv_shim_drm_ioctl(p, fd, req, arg);

  /* Refused rows are answered before any argument is touched: a client asking
   * "is NVIF there?" must get -ENOSYS even with a garbage pointer, and the
   * refusal names itself on the console instead of looking like a bad call. */
  const char *ref = nv_shim_refusal_name(req);
  if (ref) {
    row_begin("refuse", &r_refuse);
    row_s(ref);
    row_s(" errno=ENOSYS");
    row_end();
    return -ENOSYS;
  }
  /* The 'N' table is keyed on the WHOLE encoded request, not on its number:
   * a request that differs only in the size field is not one of these rows and
   * answers -ENOTTY, which is round 1's tested behaviour and stays.  The DRM
   * face is the one that validates number-then-size (-EINVAL on a mismatch),
   * because that is DRM's contract. */
  if (nv_shim_req_size(req) < 0) return -ENOTTY;
  if (!p || arg == 0) return -EINVAL;
  if (!nv_shim_present()) return -ENODEV;

  if (req == NVSHIM_IOC_DEVICE_ALLOC) return op_device_alloc(p, fd, arg);
  if (req == NVSHIM_IOC_DEVICE_FREE) return op_device_free(p, fd, arg);
  if (req == NVSHIM_IOC_GET_PARAM) return op_get_param(p, fd, arg);
  if (req == NVSHIM_IOC_ALLOC) return op_alloc(p, fd, arg);
  if (req == NVSHIM_IOC_FREE) return op_free(p, fd, arg);
  if (req == NVSHIM_IOC_VM_INIT) return op_vm_init(p, fd, arg);
  if (req == NVSHIM_IOC_VM_BIND) return op_vm_bind(p, fd, arg);
  if (req == NVSHIM_IOC_CHANNEL_ALLOC) return op_channel_alloc(p, fd, arg);
  if (req == NVSHIM_IOC_CHANNEL_FREE) return op_channel_free(p, fd, arg);
  if (req == NVSHIM_IOC_SUBMIT) return op_submit(p, fd, arg);
  if (req == NVSHIM_IOC_FENCE_WAIT) return op_fence_wait(p, fd, arg);
  return -ENOTTY;
}
