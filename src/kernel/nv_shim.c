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
 *  Deliberately NOT here: mmap of an allocation (a client would need pages
 *  behind the bus address, and there are none), GSP-RM objects, and every row
 *  in the refusal table of nv_shim.h -- each answered -ENOSYS and named on the
 *  console, because a missing handler that silently succeeds is the failure
 *  mode this project shoots itself in the foot with.
 * ========================================================================== */

#include "nv_shim.h"
#include "nv_rm.h"

#include "arch/cpu.h" /* get_cpuid for the per-CPU row buffer */
#include "errno.h"
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
  uint64_t alloc_bytes;
  int nvm;
  uint32_t vm_id[SHIM_MAX_VMS];
  int nch;
  uint32_t ch_id[SHIM_MAX_CHANNELS];
};

static struct shim_client g_clients[SHIM_MAX_CLIENTS];
static spinlock_t g_shim_lock;
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
           r_fence = SHIM_ROW_BUDGET, r_refuse = SHIM_ROW_BUDGET;

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

static const struct nv_rm_if *rm(void) { return &nv_rm_mock; }

int nv_shim_present(void) { return 1; }

int nv_shim_features(void) {
  return NVSHIM_FEAT_DEVICE | NVSHIM_FEAT_ALLOC | NVSHIM_FEAT_VM_BIND |
         NVSHIM_FEAT_CHANNEL | NVSHIM_FEAT_SUBMIT | NVSHIM_FEAT_FENCE |
         NVSHIM_FEAT_RM_MOCK;
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
  }
  for (int i = 0; i < SHIM_MAX_VMS; i++) c->vm_id[i] = 0;
  for (int i = 0; i < SHIM_MAX_CHANNELS; i++) c->ch_id[i] = 0;
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

/* Detach a client and destroy everything it owns.  Runs without the lock: the
 * RM calls are outside it, exactly as every other op does. */
static void client_teardown(const struct shim_client *snap) {
  for (int i = snap->nch - 1; i >= 0; i--)
    if (snap->ch_id[i] && snap->ch_id[i] != SHIM_RESERVED)
      rm()->channel_destroy(snap->dev_id, snap->ch_id[i]);
  for (int i = snap->nvm - 1; i >= 0; i--)
    if (snap->vm_id[i] && snap->vm_id[i] != SHIM_RESERVED)
      rm()->vm_destroy(snap->dev_id, snap->vm_id[i]);
  for (int i = snap->nalloc - 1; i >= 0; i--)
    if (snap->alloc_handle[i] && snap->alloc_handle[i] != SHIM_RESERVED)
      rm()->mem_free(snap->dev_id, snap->alloc_handle[i]);
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
  client_teardown(&snap);
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

  switch (q.param) {
  case NVSHIM_PARAM_PCI_VENDOR: q.value = info.vendor_id; break;
  case NVSHIM_PARAM_PCI_DEVICE: q.value = info.device_id; break;
  case NVSHIM_PARAM_FB_SIZE: q.value = info.fb_size; break;
  case NVSHIM_PARAM_CHIPSET_ID: q.value = info.chipset; break;
  case NVSHIM_PARAM_VRAM_BAR_SIZE: q.value = info.fb_size; break;
  /* Every remaining param reads the card (PTIMER, bus type, graph units,
   * VM_VRAM_BASE) or describes a facility this ABI does not have.  Named, not
   * silent: -ENOSYS with the number, so a winsys can log exactly what it lost. */
  case NVSHIM_PARAM_BUS_TYPE:
  case NVSHIM_PARAM_AGP_SIZE:
  case NVSHIM_PARAM_VM_VRAM_BASE:
  case NVSHIM_PARAM_GRAPH_UNITS:
  case NVSHIM_PARAM_PTIMER_TIME:
  case NVSHIM_PARAM_HAS_BO_USAGE:
  case NVSHIM_PARAM_HAS_PAGEFLIP:
    row_begin("getparam refused", &r_refuse);
    row_s("param=");
    row_say((int)q.param);
    row_s(" reason=mock_has_no_card");
    row_end();
    return -ENOSYS;
  default:
    return -EINVAL;
  }
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
    if (at >= 0) {
      c->alloc_handle[at] = handle;
      c->alloc_bus[at] = bus;
      c->alloc_size[at] = granted;
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
  }
  c->nalloc--;
  spinlock_release(&g_shim_lock);

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
  for (int i = at; i + 1 < c->nch; i++) c->ch_id[i] = c->ch_id[i + 1];
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

/* ---- entry ------------------------------------------------------------- */

int nv_shim_ioctl(struct process *p, int fd, unsigned long req, uint64_t arg) {
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
