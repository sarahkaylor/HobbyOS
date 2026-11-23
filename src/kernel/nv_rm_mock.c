/* ============================================================================
 *  nv_rm_mock: the mock Resource Manager behind /dev/nvshim.
 *
 *  This is the smallest RM that can answer the ABI honestly: it hands out
 *  object handles, a software bus-address arena, a GPU VA space with real
 *  interval bookkeeping (double maps and partial unmaps are refused), a
 *  channel per allocation, and a per-channel fence sequence whose retirement
 *  is an explicit state transition.
 *
 *  What it is NOT: an engine, an RM object model, or a model of VRAM.  No
 *  command stream is parsed or executed -- submit counts the bytes and drops
 *  them -- and no address it returns is ever dereferenced.  A test that passes
 *  against this proves the ABI's bookkeeping and sequencing, nothing about the
 *  card; that is what [NV] rows from a real boot are for.
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
#include "lock.h"

#define MOCK_MAX_ALLOC   64
#define MOCK_MAX_VM      8
#define MOCK_MAX_CHANNEL 16
#define MOCK_MAX_RANGE   64 /* VA intervals per VM */
#define MOCK_MAX_PENDING 64 /* fences enqueued but not retired */

#define MOCK_PAGE 4096ull
/* The arena is a software address range, not memory.  Its size is what
 * GET_PARAM FB_SIZE reports, so it is also the number a client would size a
 * heap from; 4 GiB is enough for a probe and small enough to exhaust in a
 * test. */
#define MOCK_VRAM_SIZE (4ull << 30)
#define MOCK_VRAM_BUS  0x100000000ull /* first bus address handed out */
/* A VA space is bookkeeping, not memory, so its size is independent of the
 * arena.  1 TiB because that is the total a stock nouveau client asks for
 * (drm_nouveau_vm_init {1<<39, 1<<39}); the objects mapped into it still get
 * their bus addresses from the 4 GiB arena above. */
#define MOCK_VA_LIMIT (1ull << 40)

#define MOCK_DEV_ID 1u

struct mock_alloc {
  int used;
  uint32_t dev;
  uint64_t handle;
  uint64_t bus;
  uint64_t size;
};

struct mock_range {
  int used;
  uint64_t va;
  uint64_t range;
  uint64_t handle;
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
  struct mock_alloc alloc[MOCK_MAX_ALLOC];
  struct mock_vm vm[MOCK_MAX_VM];
  struct mock_channel ch[MOCK_MAX_CHANNEL];
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
  g_mock.pend_head = 0;
  g_mock.pend_tail = 0;
  g_mock.pend_n = 0;
  for (int i = 0; i < MOCK_MAX_ALLOC; i++) {
    g_mock.alloc[i].used = 0;
    g_mock.alloc[i].dev = 0;
    g_mock.alloc[i].handle = 0;
    g_mock.alloc[i].bus = 0;
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
  if (!(domain & (2u | 4u))) return -EINVAL; /* VRAM or GART only */
  int rc = 0;
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
  a->handle = ++g_mock.next_handle;
  a->bus = MOCK_VRAM_BUS + g_mock.bump;
  a->size = len;
  g_mock.bump += len;
  *handle = a->handle;
  *bus = a->bus;
  *size = a->size;
out:
  spinlock_release(&g_mock.lock);
  return rc;
}

static int mock_mem_free(uint32_t dev, uint64_t handle) {
  int rc = -EINVAL;
  spinlock_acquire(&g_mock.lock);
  struct mock_alloc *a = mock_alloc_find(dev, handle);
  if (a) {
    a->used = 0;
    a->dev = 0;
    rc = 0;
  }
  spinlock_release(&g_mock.lock);
  /* The arena is a bump allocator: a freed object's range is not reused until
   * the last device reference closes.  Documented, and enough for a probe;
   * the shim's own per-client accounting is what stops a real leak. */
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
  }
  spinlock_release(&g_mock.lock);
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

const struct nv_rm_if nv_rm_mock = {
    "mock",
    mock_device_open,
    mock_device_close,
    mock_mem_alloc,
    mock_mem_free,
    mock_vm_create,
    mock_vm_destroy,
    mock_vm_bind,
    mock_channel_create,
    mock_channel_destroy,
    mock_submit,
    mock_submit_va,
    mock_fence_test,
    mock_kick,
};
