/* GX (docs/graphics-accel.md): ARM kernel GPU path (virtio-gpu over
 * virtio-mmio).  Split at the GX freeze so lane G1 owns this file alone
 * (x64 lives in virtio_gpu_x64.c).
 *
 * G1 (ARM driver v2 — damage-rect flush + IRQ-acknowledged virtio-gpu):
 *
 *  - virtio_gpu_flush_rects() presents damage rects for real: per clamped
 *    rect a TRANSFER_TO_HOST_2D (r = rect, offset = y*stride + x*4, the
 *    source pointer QEMU's virtio-gpu walks row-wise) followed by a
 *    RESOURCE_FLUSH of the same rect, resource 1.  A full-screen rect is
 *    byte-for-byte the old full flush; virtio_gpu_flush() = rects(full).
 *
 *  - Completion is IRQ-acknowledged: the device's virtio-mmio IRQ is
 *    harvested at init exactly like virtio-blk's (48 + slot), enabled
 *    from main.c after interrupts_enable(), and acked in the shared
 *    virtio IRQ dispatch (virtio_gpu_handle_irq).  The wait holds NO
 *    spinlock: a bounded tight spin catches the usual (fast) completion,
 *    then the core parks on wfi (local-tick bounded, ~10 ms) with a
 *    throttled cpu_heartbeat refresh so a peer LOSTWAKE reaper can never
 *    age this core's claim out mid-wait.
 *
 *  - Queue bookkeeping: rotating descriptor-head allocation (a chain is
 *    never reused while a command may still be in flight) and used.idx
 *    consumed as a TOTAL (gpu_ack_used_idx = used.idx), not a delta.
 */
#ifndef __x86_64__

#include "virtio_gpu.h"
#include "lock.h"
#include "arch/cpu.h"

extern void uart_puts(const char* s);
extern int print_int(int val);
extern uint64_t timer_get_ms(void);
extern volatile uint64_t cpu_heartbeat_ms[];

// VirtIO MMIO offsets
#define VIRTIO_MAGIC        0x000
#define VIRTIO_VERSION      0x004
#define VIRTIO_DEVICE_ID    0x008
#define VIRTIO_VENDOR_ID    0x00C
#define VIRTIO_DEVICE_FEAT  0x010
#define VIRTIO_DEVICE_FEAT_SEL 0x014
#define VIRTIO_DRIVER_FEAT  0x020
#define VIRTIO_DRIVER_FEAT_SEL 0x024
#define VIRTIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_QUEUE_SEL    0x030
#define VIRTIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_QUEUE_NUM    0x038
#define VIRTIO_QUEUE_ALIGN  0x03C
#define VIRTIO_QUEUE_PFN    0x040
#define VIRTIO_QUEUE_NOTIFY 0x050
#define VIRTIO_INTERRUPT_STATUS 0x060
#define VIRTIO_INTERRUPT_ACK 0x064
#define VIRTIO_STATUS       0x070

#define MMIO_BASE(slot) ((uint8_t*)0x0A000000 + (slot) * 0x200)

#define GPU_QUEUE_NUM 16
#define GPU_FB_W 1024
#define GPU_FB_H 768
#define GPU_FB_STRIDE_BYTES (GPU_FB_W * 4)

/* Tight-spin window of the completion wait (ms) before the core parks on
 * wfi; completions are normally far below this, so the parked path only
 * engages for pathological device/host stalls (see GX-REPORT-G1). */
#define GPU_WAIT_SPIN_MS 4

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed));

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[GPU_QUEUE_NUM];
  uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
} __attribute__((packed));

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[GPU_QUEUE_NUM];
  uint16_t avail_event;
} __attribute__((packed));

struct virtq {
  struct virtq_desc desc[GPU_QUEUE_NUM];
  struct virtq_avail avail;
  uint8_t padding[4096 - (GPU_QUEUE_NUM * 16 + sizeof(struct virtq_avail))];
  struct virtq_used used;
} __attribute__((aligned(4096)));

static struct virtq gpu_vq __attribute__((aligned(4096)));
static uint8_t* gpu_mmio = 0;
static uint16_t gpu_ack_used_idx = 0; /* consumed used.idx (a TOTAL, not a delta) */
static uint16_t gpu_desc_next = 0;    /* rotating descriptor-chain head */
static spinlock_t gpu_lock;           /* queue/MMIO register access */
static spinlock_t gpu_req_lock;       /* single-in-flight token */
static volatile int gpu_in_use = 0;

/* Harvested virtio-mmio IRQ (48 + slot; -1 = no device).  main.c enables
 * it after interrupts_enable() and routes it to virtio_gpu_handle_irq. */
int virtio_gpu_irq = -1;

static uint32_t framebuffer[GPU_FB_W * GPU_FB_H] __attribute__((aligned(2097152)));

static inline void reg_write32(uint32_t offset, uint32_t val) {
  *(volatile uint32_t*)(gpu_mmio + offset) = val;
}

static inline uint32_t reg_read32(uint32_t offset) {
  return *(volatile uint32_t*)(gpu_mmio + offset);
}

/* ---- pure rect geometry (kept device-free for the unit suite) --------- */

/**
 * Clamps a screen-space rect to the 1024x768 panel: skips empty rects
 * (w<=0 || h<=0) and intersects the rest with [0,1024)x[0,768).  Edges
 * are computed in 64-bit so INT32-extreme w/h cannot overflow.
 *
 * Returns 1 and fills `out` with the clamped rect when at least one pixel
 * is on-screen, 0 when the rect is empty or fully off-screen.
 */
int virtio_gpu_rect_clamp(const struct virtio_gpu_xrect *in,
                          struct virtio_gpu_xrect *out) {
  if (in->w <= 0 || in->h <= 0)
    return 0;

  int64_t x0 = in->x;
  int64_t y0 = in->y;
  int64_t x1 = x0 + in->w;
  int64_t y1 = y0 + in->h;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > GPU_FB_W) x1 = GPU_FB_W;
  if (y1 > GPU_FB_H) y1 = GPU_FB_H;
  if (x1 <= x0 || y1 <= y0)
    return 0;

  out->x = (int32_t)x0;
  out->y = (int32_t)y0;
  out->w = (int32_t)(x1 - x0);
  out->h = (int32_t)(y1 - y0);
  return 1;
}

/**
 * Byte offset of pixel (x,y) in the B8G8R8A8 framebuffer (stride 1024*4):
 * the TRANSFER_TO_HOST_2D source pointer QEMU's virtio-gpu walks row-wise
 * from (offset + stride*h).  x/y must be CLAMPED on-screen values
 * (0..1023 / 0..767); for the full-screen rect this yields 0, i.e. the
 * legacy full-flush offset.
 */
uint32_t virtio_gpu_rect_transfer_offset(int32_t x, int32_t y) {
  return (uint32_t)y * GPU_FB_STRIDE_BYTES + (uint32_t)x * 4u;
}

/* ---- single-in-flight token + completion wait ------------------------- */

/**
 * Acquires the single-in-flight token (no spinlock is held on return).
 * The virtio-gpu control queue is served one command at a time by the
 * host, and both the rotating descriptor allocation and used-idx
 * bookkeeping assume nothing is in flight while a new chain is built.
 * Losers re-check without holding anything, refreshing the liveness
 * heartbeat on a throttled cadence.
 */
static void gpu_acquire(void) {
  uint64_t spins = 0;
  for (;;) {
    uint64_t f = spinlock_acquire_irqsave(&gpu_req_lock);
    if (!gpu_in_use) {
      gpu_in_use = 1;
      spinlock_release_irqrestore(&gpu_req_lock, f);
      return;
    }
    spinlock_release_irqrestore(&gpu_req_lock, f);
    if ((++spins & 0x3FF) == 0)
      cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();
    cpu_relax();
  }
}

static void gpu_release(void) {
  uint64_t f = spinlock_acquire_irqsave(&gpu_req_lock);
  gpu_in_use = 0;
  spinlock_release_irqrestore(&gpu_req_lock, f);
}

/**
 * Waits until used.idx advances past `ack_before`.  MUST be called with no
 * lock held (the caller releases gpu_lock right after submitting).
 *
 * Discipline (docs/graphics-accel.md D3): a bounded tight spin covers the
 * usual (fast) completion; past GPU_WAIT_SPIN_MS the core parks on wfi,
 * which the per-core local timer re-arms every ~10 ms (so the park is
 * bounded even if the completion IRQ lands on another core).  IRQs are
 * enabled across the whole wait (interrupts_save_enable; the same
 * primitive ARP-wait-in-syscall uses) so a completion interrupt can be
 * taken and acknowledged here, and the syscall never sits in a long
 * IRQ-off stretch.  The heartbeat bank is refreshed on every park wake
 * (and throttle-spaced during the spin) so a peer LOSTWAKE reaper can
 * never age this core out mid-wait.
 */
static void gpu_wait_completed(uint16_t ack_before) {
  uint64_t daif = interrupts_save_enable();
  uint64_t t0 = timer_get_ms();
  uint32_t iters = 0;
  uint32_t parked_wakes = 0;

  for (;;) {
    if (*(volatile uint16_t*)&gpu_vq.used.idx != ack_before) {
      cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();
      break;
    }
    arch_memory_barrier();
    if ((++iters & 0x3F) != 0)
      continue;

    uint64_t now = timer_get_ms();
    cpu_heartbeat_ms[get_cpuid()] = now;
    if (now - t0 < GPU_WAIT_SPIN_MS)
      continue;

    parked_wakes++;
    if (parked_wakes <= 3 || (parked_wakes & 0x3FF) == 0) {
      uart_puts("[GPUDIAG] gpu wait > ");
      print_int((int)(now - t0));
      uart_puts(" ms: ack=");
      print_int((int)ack_before);
      uart_puts(" used=");
      print_int((int)(*(volatile uint16_t*)&gpu_vq.used.idx));
      uart_puts("\n");
    }
    safe_wfi();
  }

  interrupts_restore(daif);
}

/* ---- command path ----------------------------------------------------- */

/**
 * Builds and notifies the next control-queue chain starting at the
 * rotating head; caller holds gpu_lock.  Chains are 3 descriptors
 * (request, data, response) or 2 (request, response); next-pointers wrap
 * through the modulo-16 ring.  Single-in-flight guarantees a chain is
 * complete before its descriptors are reused.
 */
static void gpu_chain_build(void* req, uint32_t req_size, void* data,
                            uint32_t data_size, void* resp, uint32_t resp_size,
                            int with_data) {
  uint16_t head = gpu_desc_next;
  uint16_t d1 = (uint16_t)((head + 1) % GPU_QUEUE_NUM);
  uint16_t d2 = (uint16_t)((head + 2) % GPU_QUEUE_NUM);

  gpu_vq.desc[head].addr = (uint64_t)req;
  gpu_vq.desc[head].len = req_size;
  gpu_vq.desc[head].flags = 1; /* VIRTQ_DESC_F_NEXT */
  gpu_vq.desc[head].next = d1;

  if (with_data) {
    gpu_vq.desc[d1].addr = (uint64_t)data;
    gpu_vq.desc[d1].len = data_size;
    gpu_vq.desc[d1].flags = 1; /* NEXT: device reads the payload */
    gpu_vq.desc[d1].next = d2;

    gpu_vq.desc[d2].addr = (uint64_t)resp;
    gpu_vq.desc[d2].len = resp_size;
    gpu_vq.desc[d2].flags = 2; /* VIRTQ_DESC_F_WRITE */
    gpu_vq.desc[d2].next = 0;

    gpu_desc_next = (uint16_t)((head + 3) % GPU_QUEUE_NUM);
  } else {
    gpu_vq.desc[d1].addr = (uint64_t)resp;
    gpu_vq.desc[d1].len = resp_size;
    gpu_vq.desc[d1].flags = 2; /* VIRTQ_DESC_F_WRITE */
    gpu_vq.desc[d1].next = 0;

    gpu_desc_next = (uint16_t)((head + 2) % GPU_QUEUE_NUM);
  }

  gpu_vq.avail.ring[gpu_vq.avail.idx % GPU_QUEUE_NUM] = head;
  arch_memory_barrier();
  gpu_vq.avail.idx++;
  arch_memory_barrier();

  reg_write32(VIRTIO_QUEUE_NOTIFY, 0);
}

/**
 * Sends one command to the VirtIO GPU device, waiting for completion with
 * the lock-free wait above.  Returns 0 on success, -1 on device error
 * (or when no device is present).
 */
static int gpu_cmd(void* req, uint32_t req_size, void* data, uint32_t data_size,
                   void* resp, uint32_t resp_size, int with_data) {
  if (!gpu_mmio)
    return -1;

  gpu_acquire();

  uint64_t flags = spinlock_acquire_irqsave(&gpu_lock);
  uint16_t ack_before = gpu_ack_used_idx;
  gpu_chain_build(req, req_size, data, data_size, resp, resp_size, with_data);
  spinlock_release_irqrestore(&gpu_lock, flags);

  /* blk_do_op pattern: no lock across the wait. */
  gpu_wait_completed(ack_before);

  flags = spinlock_acquire_irqsave(&gpu_lock);
  /* used.idx is the TOTAL completed-element count — take it as-is; it may
     advance by more than one element between observations when several
     commands were submitted back to back (the rotating chain allocation
     makes that safe, and this bookkeeping stays exact either way). */
  gpu_ack_used_idx = *(volatile uint16_t*)&gpu_vq.used.idx;
  arch_memory_barrier();

  /* Belt-and-braces ack (virtio_blk_do_op does the same): if the IRQ
     dispatch did not consume the completion status yet, clear it here so
     no stale completion interrupt is left pending. */
  uint32_t ist = reg_read32(VIRTIO_INTERRUPT_STATUS);
  if (ist)
    reg_write32(VIRTIO_INTERRUPT_ACK, ist);

  int res = 0;
  if (!with_data) {
    struct virtio_gpu_ctrl_hdr* hdr = (struct virtio_gpu_ctrl_hdr*)resp;
    res = (hdr->type >= 0x1200 ? -1 : 0);
  }
  spinlock_release_irqrestore(&gpu_lock, flags);

  gpu_release();
  return res;
}

/**
 * Sends a command with an additional data buffer (3-descriptor chain).
 */
static int virtio_gpu_do_cmd_with_data(void* req, uint32_t req_size, void* data,
                                       uint32_t data_size, void* resp, uint32_t resp_size) {
  return gpu_cmd(req, req_size, data, data_size, resp, resp_size, 1);
}

/**
 * Sends a 2-descriptor command (request, response).
 */
static int virtio_gpu_do_cmd(void* req, uint32_t req_size, void* resp, uint32_t resp_size) {
  return gpu_cmd(req, req_size, 0, 0, resp, resp_size, 0);
}

/**
 * IRQ dispatch entry — main.c routes the harvested virtio-mmio IRQ here
 * (virtio_blk_handle_irq's ack semantics, but lock-free: the submission
 * path may hold gpu_lock with IRQs masked on another core, and a status
 * read + ACK write pair is atomic against the device; the polling wait
 * path re-reads the ring regardless and gpu_cmd's tail acks any status
 * the handler did not consume, so a missed interrupt can never stall a
 * wait or leave the level line asserted).
 */
void virtio_gpu_handle_irq(void) {
  static int first;
  if (!gpu_mmio)
    return;
  uint32_t status = reg_read32(VIRTIO_INTERRUPT_STATUS);
  if (status) {
    reg_write32(VIRTIO_INTERRUPT_ACK, status);
    if (first < 3) {
      first++;
      uart_puts("[GPU] irq handled (status=");
      print_int((int)status);
      uart_puts(")\n");
    }
  }
}

/**
 * Scans for and initializes the VirtIO GPU device.
 * Configures the 2D resource, attaches the framebuffer backing memory, and sets up scanout.
 *
 * Returns:
 *   0 on success, -1 if the device is not found or fails to initialize.
 */
int virtio_gpu_init(void) {
  spinlock_init(&gpu_lock);
  spinlock_init(&gpu_req_lock);
  gpu_in_use = 0;
  gpu_desc_next = 0;
  gpu_ack_used_idx = 0;
  gpu_mmio = 0;
  virtio_gpu_irq = -1;

  for (int i = 0; i < 32; i++) {
    uint8_t* mmio = MMIO_BASE(i);
    uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
    uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
    if (magic == 0x74726976 && devid == 16) {
      gpu_mmio = mmio;
      virtio_gpu_irq = 48 + i;
      break;
    }
  }

  if (!gpu_mmio) return -1;

  uint32_t status = 0;
  reg_write32(VIRTIO_STATUS, status);
  status |= 1; reg_write32(VIRTIO_STATUS, status);
  status |= 2; reg_write32(VIRTIO_STATUS, status);

  reg_write32(VIRTIO_DRIVER_FEAT_SEL, 1);
  reg_write32(VIRTIO_DRIVER_FEAT, 1);
  reg_write32(VIRTIO_DRIVER_FEAT_SEL, 0);
  reg_write32(VIRTIO_DRIVER_FEAT, 0);

  status |= 8; reg_write32(VIRTIO_STATUS, status);
  if (!(reg_read32(VIRTIO_STATUS) & 8)) return -1;

  reg_write32(VIRTIO_GUEST_PAGE_SIZE, 4096);
  reg_write32(VIRTIO_QUEUE_SEL, 0); // Control queue
  uint32_t max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
  if (max_size == 0) return -1;

  reg_write32(VIRTIO_QUEUE_NUM, GPU_QUEUE_NUM);
  reg_write32(VIRTIO_QUEUE_ALIGN, 4096);
  reg_write32(VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&gpu_vq / 4096));

  status |= 4; reg_write32(VIRTIO_STATUS, status);

  // Initialize display
  struct virtio_gpu_resource_create_2d create = {0};
  create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  create.resource_id = 1;
  create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  create.width = GPU_FB_W;
  create.height = GPU_FB_H;

  struct virtio_gpu_ctrl_hdr resp;
  virtio_gpu_do_cmd(&create, sizeof(create), &resp, sizeof(resp));

  struct virtio_gpu_resource_attach_backing attach = {0};
  attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  attach.resource_id = 1;
  attach.nr_entries = 1;

  struct virtio_gpu_mem_entry mem = {0};
  mem.addr = (uint64_t)framebuffer;
  mem.length = sizeof(framebuffer);

  virtio_gpu_do_cmd_with_data(&attach, sizeof(attach), &mem, sizeof(mem), &resp, sizeof(resp));

  struct virtio_gpu_set_scanout scanout = {0};
  scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  scanout.r.width = GPU_FB_W;
  scanout.r.height = GPU_FB_H;
  scanout.scanout_id = 0;
  scanout.resource_id = 1;

  virtio_gpu_do_cmd(&scanout, sizeof(scanout), &resp, sizeof(resp));

  // Clear to black
  for (int i = 0; i < GPU_FB_W * GPU_FB_H; i++) framebuffer[i] = 0xFF000000;
  virtio_gpu_flush();

  uart_puts("[GPU] virtio-gpu mmio irq=");
  print_int(virtio_gpu_irq);
  uart_puts("\n");
  return 0;
}

/**
 * Flushes the current framebuffer contents to the host display: the
 * full-screen rect (byte-for-byte the legacy full flush).
 */
void virtio_gpu_flush(void) {
  struct virtio_gpu_xrect full = {0, 0, GPU_FB_W, GPU_FB_H};
  virtio_gpu_flush_rects(&full, 1);
}

/**
 * GX (docs/graphics-accel.md): damage-rect present.  Clamps each rect to
 * the screen (skipping empty/off-screen ones), then per rect:
 * TRANSFER_TO_HOST_2D (r = rect, offset = y*stride + x*4) followed by
 * RESOURCE_FLUSH (r = rect), resource 1.  After this returns, the pixels
 * covered by the batch equal what a full virtio_gpu_flush() would show.
 * A device error stops the batch (the remaining rects stay un-presented).
 */
void virtio_gpu_flush_rects(const struct virtio_gpu_xrect *rects, int count) {
  if (!gpu_mmio || !rects)
    return;
  if (count <= 0)
    return;
  if (count > 32)
    count = 32; /* defensive; SYS_FLUSH_FB_RECTS validates 0..32 */

  for (int i = 0; i < count; i++) {
    struct virtio_gpu_xrect r;
    if (!virtio_gpu_rect_clamp(&rects[i], &r))
      continue;

    struct virtio_gpu_ctrl_hdr resp;

    struct virtio_gpu_transfer_to_host_2d transfer = {0};
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r.x = (uint32_t)r.x;
    transfer.r.y = (uint32_t)r.y;
    transfer.r.width = (uint32_t)r.w;
    transfer.r.height = (uint32_t)r.h;
    transfer.offset = virtio_gpu_rect_transfer_offset(r.x, r.y);
    transfer.resource_id = 1;
    if (virtio_gpu_do_cmd(&transfer, sizeof(transfer), &resp, sizeof(resp)) != 0)
      return;

    struct virtio_gpu_resource_flush flush = {0};
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x = (uint32_t)r.x;
    flush.r.y = (uint32_t)r.y;
    flush.r.width = (uint32_t)r.w;
    flush.r.height = (uint32_t)r.h;
    flush.resource_id = 1;
    if (virtio_gpu_do_cmd(&flush, sizeof(flush), &resp, sizeof(resp)) != 0)
      return;
  }
}

/**
 * Returns a pointer to the kernel's physical framebuffer memory.
 */
uint32_t* virtio_gpu_get_framebuffer(void) {
  return framebuffer;
}

#endif /* !__x86_64__ */
