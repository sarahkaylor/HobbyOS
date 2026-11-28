/* ============================================================================
 *  nv_rm_mock: the mock Resource Manager behind /dev/nvshim.
 *
 *  This is the smallest RM that can answer the ABI honestly: it hands out
 *  object handles backed by real pool memory, a GPU VA space with real
 *  interval bookkeeping (double maps and partial unmaps are refused), a
 *  channel per allocation, and a per-channel fence sequence whose retirement
 *  is an explicit state transition.
 *
 *  What it is NOT: an engine, an RM object model, or a model of VRAM.  No
 *  command stream is parsed or executed -- submit counts the bytes, reads them
 *  (read_va) and drops them -- but an object's *memory* is real since WP-F3:
 *  nv_dma_alloc()'s frames, zeroed, and the same frames a client's CPU mapping
 *  points at.  That is what makes "write it through the mapping, read it in the
 *  kernel" a statement about memory rather than about bookkeeping.  Nothing
 *  here proves anything about the card; that is what [NV] rows from a real boot
 *  are for.
 *
 *  Fence retirement is deliberately two steps: submit() ENQUEUES, kick()
 *  RETIRES.  nv_shim.c kicks right after submit (the mock's "the engine made
 *  progress"), so a client sees the fence retired by the time SUBMIT returns;
 *  nv_rm_mock_hold(1) stops the kick, which is how the unit tier observes a
 *  fence that has *not* retired.  Collapsing the two steps would make the
 *  fence-retire test true by construction and untestable.
 *
 *  Locking: one process-context spinlock (plain spinlock_acquire, interrupts
 *  stay on -- no interrupt handler touches the mock; see docs/lock-scope.md).
 *  Ops never block, never sleep and never call back into nv_shim.c.  The only
 *  nesting is shim lock -> mock lock, and nv_shim.c is the only caller.
 * ========================================================================== */

#include "nv_rm.h"
#include "errno.h"
#include "frame.h"  /* the object's pool memory: allocated, zeroed, freed here */
#include "kmem.h"   /* phys_to_virt: the kernel's view of an object          */
#include "lock.h"
#include "nv_dma.h" /* nv_dma_alloc / nv_dma_bus / nv_dma_free               */

#define MOCK_MAX_ALLOC   64
#define MOCK_MAX_VM      8
#define MOCK_MAX_CHANNEL 16
#define MOCK_MAX_ENGINE_CLASS 16 /* class objects across all channels */
#define MOCK_MAX_RANGE   64 /* VA intervals per VM */
#define MOCK_MAX_PENDING 64 /* fences enqueued but not retired */

#define MOCK_PAGE 4096ull
/* The arena is a byte budget, not an address range: each object's memory is a
 * real run of pool frames (nv_dma_alloc) and the device address is resolved
 * from it.  This number is what GET_PARAM FB_SIZE reports -- the quantity a
 * client sizes a heap from -- and 4 GiB is enough for a probe and small enough
 * to exhaust in a test. */
#define MOCK_VRAM_SIZE (4ull << 30)
/* A VA space is bookkeeping, not memory, so its size is independent of the
 * arena.  1 TiB because that is the total a stock nouveau client asks for
 * (drm_nouveau_vm_init {1<<39, 1<<39}); the objects mapped into it still get
 * their bus addresses from the 4 GiB arena above. */
#define MOCK_VA_LIMIT (1ull << 40)

#define MOCK_DEV_ID 1u

/* ---- The DECLARED GR topology of the part this mock stands for ----------
 *
 * AD102-300 = GeForce RTX 4090, the card this project owns (PLAN.md §0/§1).
 * NVIDIA's Ada whitepaper gives the full AD102 die as 12 GPC / 72 TPC /
 * 144 SM / 176 ROP and the 4090's enabled configuration as 11 GPC / 64 TPC /
 * 128 SM (ROPs undiminished).  That makes the declaration falsifiable: a real
 * GSP-RM answers with the enabled masks, and on this part that must be 11/64
 * -- see nvkm/subdev/gsp/rm/r535/gsp.c (GET_GSP_STATIC_INFO) and
 * r535_gr_units().
 *
 * This is a DECLARATION, not a measurement.  Nothing on this node has ever
 * received that reply; that is why it is tagged NV_RM_TOPO_DECLARED and why
 * every GETPARAM GRAPH_UNITS row prints src=declared.  rop_nr is recorded for
 * the record only: the Ada packing has no ROP field. */
#define MOCK_GR_GPC_NR    11u
#define MOCK_GR_TPC_TOTAL 64u
#define MOCK_GR_ROP_NR    176u

/* The topology the next device_open() will report.  Held outside g_mock's
 * reset so a test's choice survives a teardown, exactly like hold(). */
static struct nv_rm_gpu_topo g_mock_topo = {MOCK_GR_GPC_NR, MOCK_GR_TPC_TOTAL,
                                            MOCK_GR_ROP_NR, NV_RM_TOPO_DECLARED};

int nv_rm_mock_set_topo(uint32_t source, uint32_t gpc_nr, uint32_t tpc_total,
                        uint32_t rop_nr) {
  if (source != NV_RM_TOPO_NONE && source != NV_RM_TOPO_DECLARED) return -EINVAL;
  if (source == NV_RM_TOPO_NONE) {
    g_mock_topo.gpc_nr = g_mock_topo.tpc_total = g_mock_topo.rop_nr = 0;
  } else if (gpc_nr == 0 || gpc_nr > 0xffu || tpc_total == 0 || tpc_total > 0xffffu) {
    return -EINVAL; /* a model that cannot be packed the way nouveau packs it */
  } else {
    g_mock_topo.gpc_nr = gpc_nr;
    g_mock_topo.tpc_total = tpc_total;
    g_mock_topo.rop_nr = rop_nr;
  }
  g_mock_topo.source = source;
  return 0;
}


struct mock_alloc {
  int used;
  uint32_t dev;
  uint32_t mappable; /* domain asked for a CPU mapping: mem_phys() may name it */
  uint64_t handle;
  uint64_t bus;
  uint64_t phys; /* first byte of the object's pool run */
  uint64_t size;
};

struct mock_range {
  int used;
  uint64_t va;
  uint64_t range;
  uint64_t handle;
  uint64_t bo_offset; /* offset inside the object this binding starts at */
};

struct mock_vm {
  int used;
  uint32_t dev;
  uint32_t id;
  uint64_t size;
  struct mock_range r[MOCK_MAX_RANGE];
};

struct mock_channel {
  int used;
  uint32_t dev;
  uint32_t vm;
  uint32_t id;
  uint64_t pushbuf;
  uint64_t issued; /* last fence id handed out on this channel */
  uint64_t done;   /* last fence id retired on this channel */
};

/* One HW class object (a GR/CE context) parented on a channel.  It has no
 * memory and no engine behind it: it is the RM-side object a real Ada channel
 * would carry (nvkm_gsp_rm_alloc), and its handle is stamped by this table's
 * counter -- never by the client -- so the ABI can prove where it came from. */
struct mock_engine_class {
  int used;
  uint32_t dev;
  uint32_t ch;     /* parent channel */
  int32_t oclass;  /* NVIF class id (0xc7b5 etc.) */
  uint64_t handle; /* RM handle, unique per RM lifetime */
};

struct mock_pending {
  uint32_t ch;
  uint64_t fence;
};

static struct {
  spinlock_t lock;
  int refs;    /* device_open() references */
  int ready;   /* tables are initialised */
  int hold;    /* kick() does nothing */
  uint64_t bump;
  uint64_t next_handle;
  uint64_t next_vm;
  uint64_t next_ch;
  uint64_t next_ec;
  struct mock_alloc alloc[MOCK_MAX_ALLOC];
  struct mock_vm vm[MOCK_MAX_VM];
  struct mock_channel ch[MOCK_MAX_CHANNEL];
  struct mock_engine_class ec[MOCK_MAX_ENGINE_CLASS];
  struct mock_pending pend[MOCK_MAX_PENDING];
  uint32_t pend_head, pend_tail, pend_n;
} g_mock;

/* Clears every object table and the arena.  The lock, the device refcount and
 * the hold flag are NOT touched: the hold is a seam on the mock itself, and a
 * held kick must survive a device teardown so a test cannot silently lose it. */
static void mock_reset(void) {
  g_mock.bump = 0;
  g_mock.next_handle = 0;
  g_mock.next_vm = 0;
  g_mock.next_ch = 0;
  g_mock.next_ec = 0;
  g_mock.pend_head = 0;
  g_mock.pend_tail = 0;
  g_mock.pend_n = 0;
  for (int i = 0; i < MOCK_MAX_ALLOC; i++) {
    g_mock.alloc[i].used = 0;
    g_mock.alloc[i].dev = 0;
    g_mock.alloc[i].mappable = 0;
    g_mock.alloc[i].handle = 0;
    g_mock.alloc[i].bus = 0;
    g_mock.alloc[i].phys = 0;
    g_mock.alloc[i].size = 0;
  }
  for (int i = 0; i < MOCK_MAX_VM; i++) {
    g_mock.vm[i].used = 0;
    g_mock.vm[i].dev = 0;
    g_mock.vm[i].id = 0;
    g_mock.vm[i].size = 0;
    for (int k = 0; k < MOCK_MAX_RANGE; k++) {
      g_mock.vm[i].r[k].used = 0;
      g_mock.vm[i].r[k].va = 0;
      g_mock.vm[i].r[k].range = 0;
      g_mock.vm[i].r[k].handle = 0;
      g_mock.vm[i].r[k].bo_offset = 0;
    }
  }
  for (int i = 0; i < MOCK_MAX_CHANNEL; i++) {
    g_mock.ch[i].used = 0;
    g_mock.ch[i].dev = 0;
    g_mock.ch[i].vm = 0;
    g_mock.ch[i].id = 0;
    g_mock.ch[i].pushbuf = 0;
    g_mock.ch[i].issued = 0;
    g_mock.ch[i].done = 0;
  }
  for (int i = 0; i < MOCK_MAX_ENGINE_CLASS; i++) {
    g_mock.ec[i].used = 0;
    g_mock.ec[i].dev = 0;
    g_mock.ec[i].ch = 0;
    g_mock.ec[i].oclass = 0;
    g_mock.ec[i].handle = 0;
  }
  for (int i = 0; i < MOCK_MAX_PENDING; i++) {
    g_mock.pend[i].ch = 0;
    g_mock.pend[i].fence = 0;
  }
}

/* ---- device ------------------------------------------------------------ */

static int mock_device_open(uint32_t *dev_id, struct nv_rm_device_info *info) {
  if (!dev_id || !info) return -EINVAL;
  spinlock_acquire(&g_mock.lock);
  if (g_mock.refs == 0) {
    mock_reset();
    g_mock.ready = 1;
  }
  g_mock.refs++;
  *dev_id = MOCK_DEV_ID;
  info->chipset = 0x192; /* AD102 (PMC_BOOT_0 bits 20-28) */
  info->device_id = 0x2684;
  info->vendor_id = 0x10de;
  info->pad0 = 0;
  info->gsp_required = 1;
  info->mock = 1;
  info->fb_size = MOCK_VRAM_SIZE;
  info->name[0] = 'A'; info->name[1] = 'D'; info->name[2] = '1';
  info->name[3] = '0'; info->name[4] = '2'; info->name[5] = 0;
  info->name[6] = 0;   info->name[7] = 0;
  info->revision = 0xa1; /* PMC_BOOT_0 bits 0-7 on the AD102 this mock stands for */
  /* No VRAM BAR window exists on a node whose memory is pool RAM: nouveau's
   * VRAM_BAR_SIZE is an aperture size, and answering fb_size here would tell
   * nvk "full BAR" (bar_size == vram_size) about an aperture that is not
   * there. */
  info->bar1_fb_size = 0;
  info->topo = g_mock_topo; /* a declared model, or none; see the top of file */
  spinlock_release(&g_mock.lock);
  return 0;
}

static void mock_device_close(uint32_t dev_id) {
  if (dev_id != MOCK_DEV_ID) return;
  spinlock_acquire(&g_mock.lock);
  if (g_mock.refs > 0) g_mock.refs--;
  if (g_mock.refs == 0) {
    mock_reset();
    g_mock.ready = 0; /* a later open re-initialises */
  }
  spinlock_release(&g_mock.lock);
}

/* ---- memory objects ---------------------------------------------------- */

static struct mock_alloc *mock_alloc_find(uint32_t dev, uint64_t handle) {
  for (int i = 0; i < MOCK_MAX_ALLOC; i++)
    if (g_mock.alloc[i].used && g_mock.alloc[i].dev == dev &&
        g_mock.alloc[i].handle == handle)
      return &g_mock.alloc[i];
  return 0;
}

static int mock_mem_alloc(uint32_t dev, uint64_t len, uint32_t domain,
                          uint64_t *handle, uint64_t *bus, uint64_t *size) {
  if (!handle || !bus || !size) return -EINVAL;
  if (len == 0 || (len & (MOCK_PAGE - 1))) return -EINVAL;
  if (!(domain & (NV_RM_DOM_VRAM | NV_RM_DOM_GART | NV_RM_DOM_CPU)))
    return -EINVAL; /* no domain at all: neither device- nor CPU-visible */

  /* The object's memory is REAL since WP-F3: a contiguous run of pool frames,
   * zeroed, with the device address resolved by the DMA facility.  Allocated
   * before the table lock -- nv_dma_alloc takes the frame allocator's lock and
   * zeroes the run, which is not work to do under the mock's own lock. */
  struct nv_dma_map m;
  int rc = nv_dma_alloc(len, &m);
  if (rc != 0) return rc; /* -ENOMEM / -E2BIG / -ENOTSUP / -EIO, named by nv_dma */
  uint64_t b = nv_dma_bus(&m);
  if (b == NV_DMA_BUS_INVALID) {
    nv_dma_free(&m);
    return -ENOTSUP; /* translation is live: there is no address to publish */
  }

  spinlock_acquire(&g_mock.lock);
  if (!g_mock.ready) {
    rc = -ENODEV;
    goto out;
  }
  if (len > MOCK_VRAM_SIZE - g_mock.bump) {
    rc = -ENOMEM;
    goto out;
  }
  struct mock_alloc *a = 0;
  for (int i = 0; i < MOCK_MAX_ALLOC; i++)
    if (!g_mock.alloc[i].used) { a = &g_mock.alloc[i]; break; }
  if (!a) {
    rc = -ENOSPC;
    goto out;
  }
  a->used = 1;
  a->dev = dev;
  a->mappable = (domain & NV_RM_DOM_CPU_MAP) ? 1u : 0u;
  a->handle = ++g_mock.next_handle;
  a->bus = b;
  a->phys = m.phys;
  a->size = len;
  g_mock.bump += len;
  *handle = a->handle;
  *bus = a->bus;
  *size = a->size;
out:
  spinlock_release(&g_mock.lock);
  if (rc != 0) nv_dma_free(&m); /* the frames were never published */
  return rc;
}

static int mock_mem_free(uint32_t dev, uint64_t handle) {
  int rc = -EINVAL;
  struct nv_dma_map m;
  m.phys = 0;
  m.bus = 0;
  m.len = 0;
  m.pages = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_alloc *a = mock_alloc_find(dev, handle);
  if (a) {
    m.phys = a->phys;
    m.len = a->size;
    m.pages = (uint32_t)(a->size / NV_DMA_PAGE);
    a->used = 0;
    a->dev = 0;
    a->mappable = 0;
    a->phys = 0;
    rc = 0;
  }
  spinlock_release(&g_mock.lock);
  /* Frames go back outside the lock: nv_dma_free walks the run. */
  if (rc == 0 && m.phys) nv_dma_free(&m);
  /* The arena counter is a bump allocator: a freed object's range is not
   * reused until the last device reference closes.  Documented, and enough for
   * a probe; the shim's own per-client accounting is what stops a real leak. */
  return rc;
}

/* The CPU view of an object: the shim's mmap path calls this, and nothing else
 * may.  A domain that did not ask for a CPU mapping is refused by name (-ENOTSUP)
 * rather than mapped -- that refusal is what keeps "VRAM" from quietly meaning
 * "system memory a client can write". */
static int mock_mem_phys(uint32_t dev, uint64_t handle, uint64_t *phys,
                         uint64_t *len) {
  if (!phys || !len) return -EINVAL;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_alloc *a = mock_alloc_find(dev, handle);
  if (!a) {
    rc = -EINVAL;
  } else if (!a->mappable) {
    rc = -ENOTSUP;
  } else {
    *phys = a->phys;
    *len = a->size;
  }
  spinlock_release(&g_mock.lock);
  return rc;
}

/* ---- GPU VA space ------------------------------------------------------ */

static struct mock_vm *mock_vm_find(uint32_t dev, uint32_t id) {
  for (int i = 0; i < MOCK_MAX_VM; i++)
    if (g_mock.vm[i].used && g_mock.vm[i].dev == dev && g_mock.vm[i].id == id)
      return &g_mock.vm[i];
  return 0;
}

static int mock_vm_create(uint32_t dev, uint64_t size, uint32_t *vm_id) {
  if (!vm_id) return -EINVAL;
  if (size == 0 || (size & (MOCK_PAGE - 1)) || size > MOCK_VA_LIMIT) return -EINVAL;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  if (!g_mock.ready) { rc = -ENODEV; goto out; }
  struct mock_vm *v = 0;
  for (int i = 0; i < MOCK_MAX_VM; i++)
    if (!g_mock.vm[i].used) { v = &g_mock.vm[i]; break; }
  if (!v) { rc = -ENOSPC; goto out; }
  v->used = 1;
  v->dev = dev;
  v->id = (uint32_t)++g_mock.next_vm;
  v->size = size;
  *vm_id = v->id;
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

static void mock_vm_destroy(uint32_t dev, uint32_t vm_id) {
  spinlock_acquire(&g_mock.lock);
  struct mock_vm *v = mock_vm_find(dev, vm_id);
  if (v) {
    v->used = 0;
    v->dev = 0;
  }
  spinlock_release(&g_mock.lock);
}

static int mock_vm_bind(uint32_t dev, uint32_t vm_id, uint32_t op,
                        uint64_t handle, uint64_t va, uint64_t range,
                        uint64_t bo_offset) {
  if ((va & (MOCK_PAGE - 1)) || range == 0 || (range & (MOCK_PAGE - 1)))
    return -EINVAL;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_vm *v = mock_vm_find(dev, vm_id);
  if (!v) { rc = -EINVAL; goto out; }
  if (va > v->size || range > v->size - va) { rc = -EINVAL; goto out; }

  if (op == 0) { /* map */
    struct mock_alloc *a = mock_alloc_find(dev, handle);
    if (!a) { rc = -EINVAL; goto out; }
    if (bo_offset > a->size || range > a->size - bo_offset) {
      rc = -EINVAL;
      goto out;
    }
    for (int i = 0; i < MOCK_MAX_RANGE; i++) {
      struct mock_range *r = &v->r[i];
      if (!r->used) continue;
      /* Overlap, both directions, without an overflow-prone add. */
      if (va < r->va + r->range && r->va < va + range) { rc = -EINVAL; goto out; }
    }
    struct mock_range *slot = 0;
    for (int i = 0; i < MOCK_MAX_RANGE; i++)
      if (!v->r[i].used) { slot = &v->r[i]; break; }
    if (!slot) { rc = -ENOSPC; goto out; }
    slot->used = 1;
    slot->va = va;
    slot->range = range;
    slot->handle = handle;
    slot->bo_offset = bo_offset;
  } else if (op == 1) { /* unmap: exact match only */
    struct mock_range *slot = 0;
    for (int i = 0; i < MOCK_MAX_RANGE; i++)
      if (v->r[i].used && v->r[i].va == va && v->r[i].range == range) {
        slot = &v->r[i];
        break;
      }
    if (!slot) { rc = -EINVAL; goto out; }
    slot->used = 0;
  } else {
    rc = -EINVAL;
  }
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

/* The walk the bindings exist for.  `va` -> the byte it names: the range that
 * covers it, the object that range bound, and the object's own physical/bus
 * address plus the offset into it.  A VA in a hole, past the space, or in a
 * binding whose object has no memory is refused -- there is no address to
 * answer, and answering the binding's start would be a plausible-looking lie.
 * Returns *range = bytes from `va` to the end of that one binding (never past
 * it), which is what lets the caller see the walk is an extent, not a value. */
static int mock_vm_resolve(uint32_t dev, uint32_t vm_id, uint64_t va,
                           uint64_t *phys, uint64_t *bus, uint64_t *range) {
  if (!phys || !bus || !range) return -EINVAL;
  int rc = -EINVAL;
  spinlock_acquire(&g_mock.lock);
  struct mock_vm *v = mock_vm_find(dev, vm_id);
  if (!v || va >= v->size) goto out;
  for (int i = 0; i < MOCK_MAX_RANGE; i++) {
    struct mock_range *r = &v->r[i];
    if (!r->used || va < r->va || va >= r->va + r->range) continue;
    struct mock_alloc *a = mock_alloc_find(dev, r->handle);
    if (!a || !a->phys) goto out; /* bound, but no memory behind the binding */
    uint64_t bo = r->bo_offset + (va - r->va);
    if (bo >= a->size) goto out;
    *phys = a->phys + bo;
    *bus = a->bus + bo;
    *range = r->va + r->range - va;
    rc = 0;
    break;
  }
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

/* ---- channels ---------------------------------------------------------- */

static struct mock_channel *mock_ch_find(uint32_t ch_id) {
  for (int i = 0; i < MOCK_MAX_CHANNEL; i++)
    if (g_mock.ch[i].used && g_mock.ch[i].id == ch_id)
      return &g_mock.ch[i];
  return 0;
}

static int mock_channel_create(uint32_t dev, uint32_t vm_id, uint64_t pushbuf,
                               uint32_t pushbuf_size, uint32_t *ch_id,
                               uint32_t *engine) {
  if (!ch_id || !engine) return -EINVAL;
  if (pushbuf_size < 4096) return -EINVAL;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_vm *v = mock_vm_find(dev, vm_id);
  if (!v) { rc = -EINVAL; goto out; }
  struct mock_alloc *a = 0;
  for (int i = 0; i < MOCK_MAX_ALLOC; i++)
    if (g_mock.alloc[i].used && g_mock.alloc[i].dev == dev &&
        g_mock.alloc[i].bus == pushbuf) {
      a = &g_mock.alloc[i];
      break;
    }
  if (!a) { rc = -EINVAL; goto out; }
  if (pushbuf_size > a->size) { rc = -EINVAL; goto out; }
  struct mock_channel *c = 0;
  for (int i = 0; i < MOCK_MAX_CHANNEL; i++)
    if (!g_mock.ch[i].used) { c = &g_mock.ch[i]; break; }
  if (!c) { rc = -ENOSPC; goto out; }
  c->used = 1;
  c->dev = dev;
  c->vm = vm_id;
  c->id = (uint32_t)++g_mock.next_ch;
  c->pushbuf = pushbuf;
  c->issued = 0;
  c->done = 0;
  *ch_id = c->id;
  *engine = 1; /* NVSHIM_ENGINE_GR */
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

static void mock_channel_destroy(uint32_t dev, uint32_t ch_id) {
  spinlock_acquire(&g_mock.lock);
  struct mock_channel *c = mock_ch_find(ch_id);
  if (c && c->dev == dev) {
    c->used = 0;
    c->dev = 0;
    /* A class object is parented on its channel's RM object, so it dies with
     * the channel -- the same rule a real nvkm_gsp_rm_alloc'd object follows. */
    for (int i = 0; i < MOCK_MAX_ENGINE_CLASS; i++) {
      struct mock_engine_class *e = &g_mock.ec[i];
      if (e->used && e->dev == dev && e->ch == ch_id) {
        e->used = 0;
        e->dev = 0;
        e->ch = 0;
      }
    }
  }
  spinlock_release(&g_mock.lock);
}

/* ---- engine class objects ---------------------------------------------- */

static struct mock_engine_class *mock_ec_find(uint32_t dev, uint64_t handle) {
  for (int i = 0; i < MOCK_MAX_ENGINE_CLASS; i++)
    if (g_mock.ec[i].used && g_mock.ec[i].dev == dev &&
        g_mock.ec[i].handle == handle)
      return &g_mock.ec[i];
  return 0;
}

static int mock_engine_class_create(uint32_t dev, uint32_t ch_id, int32_t oclass,
                                    uint64_t *handle) {
  if (!handle || oclass == 0) return -EINVAL;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  if (!g_mock.ready) {
    rc = -ENODEV;
    goto out;
  }
  struct mock_channel *c = mock_ch_find(ch_id);
  if (!c || c->dev != dev) {
    rc = -EINVAL;
    goto out;
  }
  struct mock_engine_class *e = 0;
  for (int i = 0; i < MOCK_MAX_ENGINE_CLASS; i++)
    if (!g_mock.ec[i].used) {
      e = &g_mock.ec[i];
      break;
    }
  if (!e) {
    rc = -ENOSPC;
    goto out;
  }
  e->used = 1;
  e->dev = dev;
  e->ch = ch_id;
  e->oclass = oclass;
  e->handle = ++g_mock.next_ec; /* the RM assigns it; the client never does */
  *handle = e->handle;
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

static int mock_engine_class_destroy(uint32_t dev, uint64_t handle) {
  int rc = -EINVAL;
  spinlock_acquire(&g_mock.lock);
  struct mock_engine_class *e = mock_ec_find(dev, handle);
  if (e) {
    e->used = 0;
    e->dev = 0;
    e->ch = 0;
    rc = 0;
  }
  spinlock_release(&g_mock.lock);
  return rc;
}

/* ---- submit and fences ------------------------------------------------- */

/* Enqueue one fence on a live channel.  Caller holds g_mock.lock. */
static int mock_enqueue_locked(struct mock_channel *c, uint64_t *fence) {
  if (g_mock.pend_n == MOCK_MAX_PENDING) return -ENOSPC;
  /* The stream is accepted, counted and dropped: there is no engine. */
  c->issued++;
  g_mock.pend[g_mock.pend_tail].ch = c->id;
  g_mock.pend[g_mock.pend_tail].fence = c->issued;
  g_mock.pend_tail = (g_mock.pend_tail + 1) % MOCK_MAX_PENDING;
  g_mock.pend_n++;
  *fence = c->issued;
  return 0;
}

static int mock_submit(uint32_t ch_id, const void *cs, uint32_t len,
                       uint64_t *fence) {
  if (!cs || !fence || len == 0) return -EINVAL;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_channel *c = mock_ch_find(ch_id);
  if (!c) { rc = -EINVAL; goto out; }
  rc = mock_enqueue_locked(c, fence);
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

/* Is [va, va+len) entirely inside the VM's mapped ranges?  Ranges never
 * overlap (bind refuses an overlap), so walking from the range that covers the
 * current offset is enough; a gap anywhere in the run is a refusal. */
static int mock_va_covered(const struct mock_vm *v, uint64_t va, uint32_t len) {
  if (va >= v->size || (uint64_t)len > v->size - va) return 0;
  uint64_t off = 0;
  while (off < (uint64_t)len) {
    uint64_t at = va + off;
    uint64_t best = 0;
    for (int i = 0; i < MOCK_MAX_RANGE; i++) {
      const struct mock_range *r = &v->r[i];
      if (!r->used) continue;
      if (at >= r->va && at < r->va + r->range) {
        uint64_t end = r->va + r->range;
        if (end > best) best = end;
      }
    }
    if (best == 0) return 0;
    off = best - va; /* best > at >= va, so this always advances */
  }
  return 1;
}

static int mock_submit_va(uint32_t ch_id, uint64_t va, uint32_t len,
                          uint64_t *fence) {
  if (!fence || len == 0) return -EINVAL;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_channel *c = mock_ch_find(ch_id);
  struct mock_vm *v = c ? mock_vm_find(c->dev, c->vm) : 0;
  if (!c || !v) { rc = -EINVAL; goto out; }
  if (!mock_va_covered(v, va, len)) { rc = -EINVAL; goto out; }
  rc = mock_enqueue_locked(c, fence);
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

/* The kernel's view of a command stream.  A push buffer on the EXEC path is
 * named by GPU VA and by nothing else -- there is no user pointer -- so a
 * submit that never looked here would be accepting bytes it had not seen.
 *
 * Resolution is page-wise on purpose: a push may start mid-object (a binding
 * carries a bo_offset) and may cross from one binding into another, and both
 * shapes must read the object bytes the GPU VA actually names.  A binding whose
 * object has no pool memory (an object the RM never backed) is -EFAULT: that is
 * the control for "mapped bookkeeping is not memory". */
static int mock_read_va(uint32_t ch_id, uint64_t va, void *dst, uint32_t len) {
  if (!dst || len == 0) return -EINVAL;
  uint8_t *out = (uint8_t *)dst;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_channel *c = mock_ch_find(ch_id);
  struct mock_vm *v = c ? mock_vm_find(c->dev, c->vm) : 0;
  if (!c || !v) {
    rc = -EINVAL;
    goto out;
  }
  uint32_t done = 0;
  while (done < len) {
    uint64_t at = va + (uint64_t)done;
    struct mock_range *r = 0;
    for (int i = 0; i < MOCK_MAX_RANGE; i++) {
      struct mock_range *cand = &v->r[i];
      if (cand->used && at >= cand->va && at < cand->va + cand->range) {
        r = cand;
        break;
      }
    }
    if (!r) {
      rc = -EINVAL; /* a hole in the VA space: nothing to submit */
      goto out;
    }
    struct mock_alloc *a = mock_alloc_find(c->dev, r->handle);
    if (!a || !a->phys) {
      rc = -EFAULT; /* bound, but no memory behind the binding */
      goto out;
    }
    uint64_t bo = r->bo_offset + (at - r->va);
    if (bo >= a->size) {
      rc = -EFAULT;
      goto out;
    }
    uint32_t span = (uint32_t)(MOCK_PAGE - (at & (MOCK_PAGE - 1)));
    if (span > len - done) span = len - done;
    if (span > a->size - bo) span = (uint32_t)(a->size - bo);
    const uint8_t *src = (const uint8_t *)phys_to_virt(a->phys + bo);
    for (uint32_t k = 0; k < span; k++) out[done + k] = src[k];
    done += span;
  }
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

static int mock_fence_test(uint32_t ch_id, uint64_t fence) {
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_channel *c = mock_ch_find(ch_id);
  if (!c || fence == 0 || fence > c->issued)
    rc = -EINVAL;
  else
    rc = (fence <= c->done) ? 0 : -EAGAIN;
  spinlock_release(&g_mock.lock);
  return rc;
}

static void mock_kick(void) {
  spinlock_acquire(&g_mock.lock);
  if (g_mock.hold || !g_mock.ready) {
    spinlock_release(&g_mock.lock);
    return;
  }
  while (g_mock.pend_n) {
    struct mock_pending *p = &g_mock.pend[g_mock.pend_head];
    struct mock_channel *c = mock_ch_find(p->ch);
    if (c) c->done = p->fence; /* per channel: FIFO, so a high-water mark */
    g_mock.pend_head = (g_mock.pend_head + 1) % MOCK_MAX_PENDING;
    g_mock.pend_n--;
  }
  spinlock_release(&g_mock.lock);
}

/* ---- mock observation seams -------------------------------------------- */

void nv_rm_mock_hold(int on) {
  spinlock_acquire(&g_mock.lock);
  g_mock.hold = on ? 1 : 0;
  spinlock_release(&g_mock.lock);
}

int nv_rm_mock_live(void) {
  int n = 0;
  spinlock_acquire(&g_mock.lock);
  for (int i = 0; i < MOCK_MAX_ALLOC; i++) n += g_mock.alloc[i].used ? 1 : 0;
  for (int i = 0; i < MOCK_MAX_VM; i++) n += g_mock.vm[i].used ? 1 : 0;
  for (int i = 0; i < MOCK_MAX_CHANNEL; i++) n += g_mock.ch[i].used ? 1 : 0;
  for (int i = 0; i < MOCK_MAX_ENGINE_CLASS; i++) n += g_mock.ec[i].used ? 1 : 0;
  n += (int)g_mock.pend_n;
  spinlock_release(&g_mock.lock);
  return n;
}

int nv_rm_mock_pending(void) {
  int n;
  spinlock_acquire(&g_mock.lock);
  n = (int)g_mock.pend_n;
  spinlock_release(&g_mock.lock);
  return n;
}

/* ---- the RM table ------------------------------------------------------
 *
 * This is NOT a `const struct nv_rm_if nv_rm_mock = { "mock", mock_device_open,
 * ... };` initializer, and the reason is a property of this kernel rather than
 * of this file: a static initializer holding function addresses (and a
 * `const char *`) is folded into .rodata as the LINK address, and the kernel
 * applies no load-time relocations to static data.  That is the same hazard
 * blk_cache_press_test.c documents for `const char *` members and unix_test.c
 * works around with RT_ADDR.  x86_64 happens to load the image at its link
 * address, so a folded table is right there; AArch64 boots at Limine's slide
 * (measured: 0x3FA400000 in the unit tier), so a folded function address names
 * nothing.  Measured before this fix: the first call through the table branched
 * to 0x401485C0 -- the link address of mock_device_open -- and took an EL1
 * synchronous exception (EC=0, ISS=0, IL=1: the CPU fetched zeros, an
 * unallocated encoding, and reported "unknown reason").
 *
 * So the table is built once, at runtime, by assignment.  Assignment to a
 * non-const object cannot be folded into .rodata, so every address in it is
 * computed PC-relative by the compiler and carries the slide.
 */
static struct nv_rm_if g_rm_if;
static spinlock_t g_rm_if_lock = {0};
static int g_rm_if_ready;

static void mock_if_fill(void) {
  g_rm_if.name = "mock";
  g_rm_if.device_open = mock_device_open;
  g_rm_if.device_close = mock_device_close;
  g_rm_if.mem_alloc = mock_mem_alloc;
  g_rm_if.mem_free = mock_mem_free;
  g_rm_if.mem_phys = mock_mem_phys;
  g_rm_if.vm_create = mock_vm_create;
  g_rm_if.vm_destroy = mock_vm_destroy;
  g_rm_if.vm_bind = mock_vm_bind;
  g_rm_if.vm_resolve = mock_vm_resolve;
  g_rm_if.channel_create = mock_channel_create;
  g_rm_if.channel_destroy = mock_channel_destroy;
  g_rm_if.engine_class_create = mock_engine_class_create;
  g_rm_if.engine_class_destroy = mock_engine_class_destroy;
  g_rm_if.submit = mock_submit;
  g_rm_if.submit_va = mock_submit_va;
  g_rm_if.read_va = mock_read_va;
  g_rm_if.fence_test = mock_fence_test;
  g_rm_if.kick = mock_kick;
}

const struct nv_rm_if *nv_rm_mock_if(void) {
  /* One word, monotonic 0 -> 1, written after the table is complete: a caller
   * that sees 1 sees the whole table (the atomics are the lock; the fast path
   * exists so an ioctl does not take a second lock it never contends). */
  if (g_rm_if_ready) return &g_rm_if;
  spinlock_acquire(&g_rm_if_lock);
  if (!g_rm_if_ready) {
    mock_if_fill();
    g_rm_if_ready = 1;
  }
  spinlock_release(&g_rm_if_lock);
  return &g_rm_if;
}

int nv_rm_mock_read_bo(uint32_t dev_id, uint64_t handle, uint64_t off,
                       void *dst, uint32_t len) {
  if (!dst || len == 0) return -EINVAL;
  uint8_t *out = (uint8_t *)dst;
  int rc = 0;
  spinlock_acquire(&g_mock.lock);
  struct mock_alloc *a = mock_alloc_find(dev_id, handle);
  if (!a || !a->phys) {
    rc = -EINVAL;
  } else if (off > a->size || (uint64_t)len > a->size - off) {
    rc = -EINVAL;
  } else {
    const uint8_t *src = (const uint8_t *)phys_to_virt(a->phys + off);
    for (uint32_t k = 0; k < len; k++) out[k] = src[k];
  }
  spinlock_release(&g_mock.lock);
  return rc;
}
