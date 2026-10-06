/* GX (docs/graphics-accel.md): x64 kernel GPU path.
 *
 * Split out of virtio_gpu.c at the GX freeze so lane G2 owns this file
 * alone (ARM lives in virtio_gpu.c).
 *
 * G2 revision — two presentation paths behind virtio_gpu_flush_rects():
 *
 *  1. virtio-pci GPU (PRIMARY).  QEMU's virtio-gpu-pci / virtio-gpu-gl-pci
 *     are modern virtio 1.0 devices (PCI 1af4:1050).  QEMU has NO
 *     transitional/legacy virtio-gpu: on QEMU 10.2.1 even
 *     `disable-modern=on,disable-legacy=off` still reports 1050 with no
 *     I/O BAR, so there is no legacy I/O-port transport to model on
 *     virtio_net.c -- the modern transport below IS the virtio path the
 *     x64 launcher devices (G3) use.  It implements: PCI capability walk
 *     (common/notify/ISR), feature negotiation (VIRTIO_F_VERSION_1),
 *     split-queue setup, 2D resource create + attach-backing + set-scanout,
 *     then per-rect TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.  Completion is a
 *     bounded poll of the used ring: x64 has no generic IRQ-registration
 *     hook (trap.c is frozen and special-cases only net/input; virtio_net
 *     TX and x64 virtio_blk poll the same way).  The device's INTx is
 *     disabled via the PCI command register and the ISR is read after each
 *     batch as ack hygiene.  The 64-bit modern BAR lives at
 *     0xC000000000 on q35 (above the kernel's 0-8GiB identity map), so it
 *     is remapped through the shared kernel PD cpu_pd7[] exactly like the
 *     NVMe path in virtio_blk.c.
 *
 *  2. Bochs VGA (BGA) fallback, used when no virtio GPU is present:
 *     the existing BGA bring-up, with virtio_gpu_flush_rects() copying
 *     row-wise per clamped rect only (BGA has no partial-transfer
 *     protocol; fewer bytes copied per damage rect is the win).
 *
 * virtio_gpu_init() probes virtio-pci first, then BGA, and names the
 * active path in one serial line ("VirtIO GPU: virtio-pci" / "VirtIO
 * GPU: BGA").
 */
#ifdef __x86_64__

/* WD lane: set to 1 to re-enable the flush-path probes used in the
 * x64 white-window investigation (serial " [WD] ..." lines).  Default
 * 0 keeps the shipped kernel quiet. */
#ifndef WD_X64_FLUSH_PROBE
#define WD_X64_FLUSH_PROBE 0
#endif

#include "virtio_gpu.h"
#include "display_mode.h"
#include "lock.h"
#include "arch/cpu.h"

/* R6: mode constants live in display_mode.h (single shared definition). */
#define GPU_SCREEN_W DISPLAY_WIDTH
#define GPU_SCREEN_H DISPLAY_HEIGHT

/* Present-path modes (exported to the unit tests via _active_mode()). */
#define VGPU_MODE_NONE   0
#define VGPU_MODE_VIRTIO 1
#define VGPU_MODE_BGA    2

/* Damage batches are clamped by the syscall contract (0..32 rects). */
#define VGPU_RECT_MAX 32

/* Bring-up diagnostics: prints "G2DBG ..." lines when 1.  Final form: 0
 * (development logs with these prints are quoted in GX-REPORT-G2.md). */
#define G2_VPCI_DBG 0

static int gpu_mode = VGPU_MODE_NONE;
static spinlock_t gpu_lock;

static uint32_t framebuffer[GPU_SCREEN_W * GPU_SCREEN_H] __attribute__((aligned(2097152)));

uint32_t bga_framebuffer_phys = 0;

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);

/* Pure helpers shared with src/kernel/virtio_gpu_test.c (x64 region). */
int virtio_gpu_x64_is_gpu_dev(uint16_t vendor, uint16_t device);
int virtio_gpu_x64_clamp_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t* ox, int32_t* oy, int32_t* ow, int32_t* oh);
uint64_t virtio_gpu_x64_rect_offset(int32_t x, int32_t y);
int virtio_gpu_x64_active_mode(void);

/* ------------------------------------------------------------------ */
/* Bochs VGA (BGA) fallback                                            */
/* ------------------------------------------------------------------ */

static inline void outw(uint16_t port, uint16_t val) {
  __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
  __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t ret;
  __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
  uint32_t address = ((uint32_t)1 << 31) |
                     ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) |
                     ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(0x0CF8, address);
  return inl(0x0CFC);
}

static void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
  uint32_t address = ((uint32_t)1 << 31) |
                     ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) |
                     ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(0x0CF8, address);
  outl(0x0CFC, val);
}

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4

#define VBE_DISPI_DISABLED          0x00
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB               0x40

static void bga_write(uint16_t index, uint16_t data) {
  outw(VBE_DISPI_IOPORT_INDEX, index);
  outw(VBE_DISPI_IOPORT_DATA, data);
}

/* Probe + program the Bochs VGA (PCI 1234:1111) at the shared desktop
 * mode (GPU_SCREEN_W x GPU_SCREEN_H x 32, display_mode.h) with LFB.
 * Returns 0 on success. */
static int bga_setup(void) {
  int found = 0;
  for (uint32_t bus = 0; bus < 256 && !found; bus++) {
    for (uint32_t slot = 0; slot < 32; slot++) {
      uint32_t id = pci_read_config(bus, slot, 0, 0);
      if ((id & 0xFFFF) == 0x1234 && ((id >> 16) & 0xFFFF) == 0x1111) {
        uint32_t bar0 = pci_read_config(bus, slot, 0, 0x10);
        bga_framebuffer_phys = bar0 & 0xFFFFFFF0;
        uint32_t cmd = pci_read_config(bus, slot, 0, 0x04);
        pci_write_config(bus, slot, 0, 0x04, cmd | 0x02);
        found = 1;
        break;
      }
    }
  }

  if (!found || bga_framebuffer_phys == 0) return -1;

  bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
  bga_write(VBE_DISPI_INDEX_XRES, GPU_SCREEN_W);
  bga_write(VBE_DISPI_INDEX_YRES, GPU_SCREEN_H);
  bga_write(VBE_DISPI_INDEX_BPP, 32);
  bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB);
  return 0;
}

/* Row-wise copy of one clamped rect into the BGA linear framebuffer. */
static void bga_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  volatile uint32_t* dst = (volatile uint32_t*)(uint64_t)bga_framebuffer_phys;
  for (int32_t row = 0; row < h; row++) {
    const uint32_t* s = framebuffer + (uint64_t)(y + row) * GPU_SCREEN_W + (uint64_t)x;
    volatile uint32_t* d = dst + (uint64_t)(y + row) * GPU_SCREEN_W + (uint64_t)x;
    for (int32_t col = 0; col < w; col++) d[col] = s[col];
  }
}

/* ------------------------------------------------------------------ */
/* virtio-pci GPU (modern virtio 1.0 transport)                        */
/* ------------------------------------------------------------------ */

#define VIRTIO_PCI_VENDOR         0x1AF4
#define VIRTIO_PCI_DEV_GPU_MODERN 0x1050
#define VIRTIO_PCI_DEV_GPU_LEGACY 0x1010

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3

/* PCI command register (offset 0x04) bits used here. */
#define PCI_CMD_MEM_SPACE  0x0002u
#define PCI_CMD_BUS_MASTER 0x0004u
#define PCI_CMD_INTX_DIS   0x0400u

/* Common configuration field offsets (virtio 1.0 spec 4.1.4.3). */
#define VPCI_CC_DEV_FEAT_SEL     0x00u
#define VPCI_CC_DEV_FEAT         0x04u
#define VPCI_CC_DRV_FEAT_SEL     0x08u
#define VPCI_CC_DRV_FEAT         0x0Cu
#define VPCI_CC_DEVICE_STATUS    0x14u
#define VPCI_CC_QUEUE_SEL        0x16u
#define VPCI_CC_QUEUE_SIZE       0x18u
#define VPCI_CC_QUEUE_MSIX_VEC   0x1Au
#define VPCI_CC_QUEUE_ENABLE     0x1Cu
#define VPCI_CC_QUEUE_NOTIFY_OFF 0x1Eu
#define VPCI_CC_QUEUE_DESC       0x20u
#define VPCI_CC_QUEUE_DRIVER     0x28u
#define VPCI_CC_QUEUE_DEVICE     0x30u

#define VPCI_STATUS_ACK         1
#define VPCI_STATUS_DRIVER      2
#define VPCI_STATUS_DRIVER_OK   4
#define VPCI_STATUS_FEATURES_OK 8

#define VPCI_F_VERSION_1 0x1u /* feature bit 32 = high-word bit 0 */

#define VGPU_QUEUE_ID 0
#define VGPU_QDESC    64

/* Descriptor flags. */
#define VQ_DESC_F_NEXT  1
#define VQ_DESC_F_WRITE 2

/* Completion spin bound: each iteration is a pause; a healthy QEMU
 * completes a control command in well under this (measured at bring-up). */
#define VGPU_CMD_SPIN_LIMIT 400000000ull

struct vpci_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed));

struct vpci_used_elem {
  uint32_t id;
  uint32_t len;
} __attribute__((packed));

/* Split virtqueue: separate desc/avail/used buffers (allowed by the
 * modern transport; no single-page PFN constraint). */
static struct vpci_desc gpu_desc[VGPU_QDESC] __attribute__((aligned(16)));

static struct {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[VGPU_QDESC];
  uint16_t used_event;
} __attribute__((packed)) gpu_avail __attribute__((aligned(16)));

static struct {
  uint16_t flags;
  uint16_t idx;
  struct vpci_used_elem ring[VGPU_QDESC];
  uint16_t avail_event;
} __attribute__((packed)) gpu_used __attribute__((aligned(16)));

static volatile uint8_t* gpu_cc = 0;     /* common config */
static volatile uint8_t* gpu_notify = 0; /* notify base */
static volatile uint8_t* gpu_isr = 0;    /* isr status */
static uint32_t gpu_notify_mult = 4;
static uint16_t gpu_notify_off = 0;
static uint16_t gpu_qsize = 0;
static uint16_t gpu_avail_next = 0;
static uint16_t gpu_used_ack = 0;
static uint32_t gpu_timeouts = 0;
static int gpu_failed = 0;
static uint64_t gpu_mmio_win = 0; /* 2MiB-aligned phys base mapped into VA */

/* DMA-stable request/response buffers for the control queue. */
static struct virtio_gpu_resource_create_2d gpu_req_create;
static struct virtio_gpu_resource_attach_backing gpu_req_attach;
static struct virtio_gpu_mem_entry gpu_req_mem;
static struct virtio_gpu_set_scanout gpu_req_scanout;
static struct virtio_gpu_transfer_to_host_2d gpu_req_transfer;
static struct virtio_gpu_resource_flush gpu_req_flush;
static struct virtio_gpu_ctrl_hdr gpu_resp;

#if G2_VPCI_DBG
static int gpu_dbg_cmds = 0;
#endif

static inline uint32_t gpu_cc_r32(uint32_t off) { return *(volatile uint32_t*)(gpu_cc + off); }
static inline void gpu_cc_w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(gpu_cc + off) = v; }
static inline uint16_t gpu_cc_r16(uint32_t off) { return *(volatile uint16_t*)(gpu_cc + off); }
static inline void gpu_cc_w16(uint32_t off, uint16_t v) { *(volatile uint16_t*)(gpu_cc + off) = v; }
static inline uint8_t gpu_cc_r8(uint32_t off) { return *(volatile uint8_t*)(gpu_cc + off); }
static inline void gpu_cc_w8(uint32_t off, uint8_t v) { *(volatile uint8_t*)(gpu_cc + off) = v; }

static inline void gpu_cc_w64(uint32_t off, uint64_t v) {
  gpu_cc_w32(off, (uint32_t)v);
  gpu_cc_w32(off + 4, (uint32_t)(v >> 32));
}

/* --- MMIO window -----------------------------------------------------
 * The q35 modern virtio BARs are 64-bit windows far above the kernel's
 * 0-8GiB identity map (observed: BAR4 at 0xC000000000).  Remap a 2MiB/4MiB
 * VA window onto the BAR through the shared kernel PD7 (cpu_pd7[496..497],
 * 7.992-8.0GiB VA - dead PCI-hole space with 6GiB RAM), the same pattern
 * virtio_blk.c's NVMe path uses (cpu_pd7[480] + CR3 reload).  PD7 is
 * shared with every v2 address space, so syscall-context flushes (running
 * on a process CR3) reach it too. */
#define GPU_MMIO_VA_BASE 0x1FE000000ULL
#define GPU_MMIO_PD_IDX  ((GPU_MMIO_VA_BASE >> 21) & 0x1FF)

extern uint64_t cpu_pd7[512];

static int gpu_map_bar(uint64_t bar_phys, uint64_t size) {
  uint64_t win = bar_phys & ~0x1FFFFFULL;
  uint64_t end = bar_phys + size;
  if (end <= bar_phys) return -1;
  uint64_t need = ((end - win) + 0x1FFFFF) / 0x200000; /* 1..2 pages */
  if (need == 0 || need > 2) return -1;

  for (uint64_t i = 0; i < need; i++) {
    uint64_t phys = (win + i * 0x200000) & 0xFFFFFFFFFFE00000ULL;
    cpu_pd7[GPU_MMIO_PD_IDX + i] = phys | 0x9B; /* P|RW|PWT|PCD|PS */
  }

  /* Flush this CPU's TLB (init runs on the boot core before SMP). */
  uint64_t cr3_val;
  __asm__ volatile(
      "mov %%cr3, %0\n"
      "mov %0, %%cr3"
      : "=r"(cr3_val)
      :
      : "memory");

  gpu_mmio_win = win;
  return 0;
}

static inline volatile uint8_t* gpu_mmio_va(uint64_t phys) {
  return (volatile uint8_t*)(GPU_MMIO_VA_BASE + (phys - gpu_mmio_win));
}

/* --- PCI capability walk --------------------------------------------- */

struct vpci_cap {
  uint8_t kind; /* cfg_type */
  uint8_t bar;
  uint32_t offset;
  uint32_t length;
  uint32_t mult; /* notify-offset multiplier (notify cap only) */
};

static int vpci_collect_caps(uint8_t bus, uint8_t slot, struct vpci_cap* caps, int max_caps) {
  uint32_t st = pci_read_config(bus, slot, 0, 0x04) >> 16;
  if (!(st & (1u << 4))) return 0; /* no capability list */

  uint8_t ptr = pci_read_config(bus, slot, 0, 0x34) & 0xFC;
  int n = 0, guard = 0;
  while (ptr && guard++ < 64 && n < max_caps) {
    uint32_t d0 = pci_read_config(bus, slot, 0, ptr);
    if ((d0 & 0xFF) == 0x09) { /* vendor-specific: virtio structure */
      caps[n].kind = (d0 >> 24) & 0xFF;
      caps[n].bar = pci_read_config(bus, slot, 0, ptr + 4) & 0xFF;
      caps[n].offset = pci_read_config(bus, slot, 0, ptr + 8);
      caps[n].length = pci_read_config(bus, slot, 0, ptr + 12);
      caps[n].mult = 0;
      if (caps[n].kind == VIRTIO_PCI_CAP_NOTIFY_CFG)
        caps[n].mult = pci_read_config(bus, slot, 0, ptr + 16);
      n++;
    }
    ptr = (d0 >> 8) & 0xFC;
  }
  return n;
}

static uint64_t vpci_bar_phys(uint8_t bus, uint8_t slot, uint8_t bar) {
  if (bar > 5) return 0;
  uint32_t lo = pci_read_config(bus, slot, 0, 0x10 + bar * 4);
  uint64_t base = lo & 0xFFFFFFF0u;
  if ((lo & 0x6) == 0x4 && bar < 5) { /* 64-bit BAR */
    uint32_t hi = pci_read_config(bus, slot, 0, 0x10 + (bar + 1) * 4);
    base |= ((uint64_t)hi) << 32;
  }
  return base;
}

/* --- control queue command submission -------------------------------- */

static int gpu_transact(const void* req, uint32_t req_len, const void* data, uint32_t data_len) {
  if (gpu_failed || gpu_qsize == 0 || !gpu_cc) return -1;

  gpu_desc[0].addr = (uint64_t)req;
  gpu_desc[0].len = req_len;
  gpu_desc[0].flags = VQ_DESC_F_NEXT;
  gpu_desc[0].next = 1;
  if (data) {
    gpu_desc[1].addr = (uint64_t)data;
    gpu_desc[1].len = data_len;
    gpu_desc[1].flags = VQ_DESC_F_NEXT;
    gpu_desc[1].next = 2;
    gpu_desc[2].addr = (uint64_t)&gpu_resp;
    gpu_desc[2].len = sizeof(gpu_resp);
    gpu_desc[2].flags = VQ_DESC_F_WRITE;
    gpu_desc[2].next = 0;
  } else {
    gpu_desc[1].addr = (uint64_t)&gpu_resp;
    gpu_desc[1].len = sizeof(gpu_resp);
    gpu_desc[1].flags = VQ_DESC_F_WRITE;
    gpu_desc[1].next = 0;
  }
  gpu_resp.type = 0;

  gpu_avail.ring[gpu_avail_next % gpu_qsize] = 0; /* head descriptor */
  arch_memory_barrier();
  gpu_avail.idx = (uint16_t)(gpu_avail_next + 1);
  gpu_avail_next++;
  arch_memory_barrier();

  *(volatile uint16_t*)(gpu_notify + (uint64_t)gpu_notify_off * gpu_notify_mult) =
      (uint16_t)VGPU_QUEUE_ID;
  arch_memory_barrier();

  uint16_t ack0 = gpu_used_ack;
  uint64_t spins = 0;
  while (*(volatile uint16_t*)&gpu_used.idx == ack0) {
    __asm__ volatile("pause");
    if (++spins >= VGPU_CMD_SPIN_LIMIT) {
      /* Device wedged: leave gpu_used_ack stale (the completion is absorbed
       * by the next transaction's wait) and stop trusting the device after
       * repeated timeouts. */
      gpu_timeouts++;
#if G2_VPCI_DBG
      uart_puts("G2DBG cmd TIMEOUT used.idx=");
      uart_print_hex(*(volatile uint16_t*)&gpu_used.idx);
      uart_puts(" ack0=");
      uart_print_hex(ack0);
      uart_puts(" avail_next=");
      print_int((int)gpu_avail_next);
      uart_puts("\n");
#endif
      if (gpu_timeouts >= 4) gpu_failed = 1;
      return -1;
    }
  }
  arch_memory_barrier();
  gpu_used_ack = *(volatile uint16_t*)&gpu_used.idx;
  arch_memory_barrier();

#if G2_VPCI_DBG
  if (gpu_dbg_cmds < 12) {
    uart_puts("G2DBG cmd ok spins=");
    print_int((int)(spins > 500000000ull ? 500000000 : (int)spins));
    uart_puts(" rt=");
    uart_print_hex(gpu_resp.type);
    uart_puts(" req=");
    uart_print_hex(*(volatile uint32_t*)req);
    uart_puts("\n");
  }
  gpu_dbg_cmds++;
#endif

  if (gpu_resp.type >= 0x1200) return -1; /* RESP_ERR_* */
  return 0;
}

/* Read the ISR (clearing any pending interrupt state).  INTx is disabled
 * in the PCI command register; this is ack hygiene only. */
static void gpu_isr_ack(void) {
  if (gpu_isr) (void)*(volatile uint8_t*)gpu_isr;
}

/* --- bring-up --------------------------------------------------------- */

static int vgpu_resource_setup(void) {
  gpu_req_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  gpu_req_create.hdr.flags = 0;
  gpu_req_create.hdr.fence_id = 0;
  gpu_req_create.hdr.ctx_id = 0;
  gpu_req_create.hdr.padding = 0;
  gpu_req_create.resource_id = 1;
  gpu_req_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  gpu_req_create.width = GPU_SCREEN_W;
  gpu_req_create.height = GPU_SCREEN_H;
  int r = gpu_transact(&gpu_req_create, sizeof(gpu_req_create), 0, 0);
#if G2_VPCI_DBG
  uart_puts("G2DBG create rc=");
  print_int(r);
  uart_puts("\n");
#endif
  if (r != 0) return -1;

  gpu_req_mem.addr = (uint64_t)framebuffer;
  gpu_req_mem.length = sizeof(framebuffer);
  gpu_req_mem.padding = 0;
  gpu_req_attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  gpu_req_attach.hdr.flags = 0;
  gpu_req_attach.hdr.fence_id = 0;
  gpu_req_attach.hdr.ctx_id = 0;
  gpu_req_attach.hdr.padding = 0;
  gpu_req_attach.resource_id = 1;
  gpu_req_attach.nr_entries = 1;
  r = gpu_transact(&gpu_req_attach, sizeof(gpu_req_attach), &gpu_req_mem,
                   sizeof(gpu_req_mem));
#if G2_VPCI_DBG
  uart_puts("G2DBG attach rc=");
  print_int(r);
  uart_puts("\n");
#endif
  if (r != 0) return -1;

  gpu_req_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  gpu_req_scanout.hdr.flags = 0;
  gpu_req_scanout.hdr.fence_id = 0;
  gpu_req_scanout.hdr.ctx_id = 0;
  gpu_req_scanout.hdr.padding = 0;
  gpu_req_scanout.r.x = 0;
  gpu_req_scanout.r.y = 0;
  gpu_req_scanout.r.width = GPU_SCREEN_W;
  gpu_req_scanout.r.height = GPU_SCREEN_H;
  gpu_req_scanout.scanout_id = 0;
  gpu_req_scanout.resource_id = 1;
  r = gpu_transact(&gpu_req_scanout, sizeof(gpu_req_scanout), 0, 0);
#if G2_VPCI_DBG
  uart_puts("G2DBG scanout rc=");
  print_int(r);
  uart_puts("\n");
#endif
  return r;
}

static int vgpu_bringup(uint32_t bus, uint32_t slot) {
  /* Memory space + bus master; INTx disabled (completion is polled). */
  uint32_t cmd = pci_read_config(bus, slot, 0, 0x04);
  pci_write_config(bus, slot, 0, 0x04,
                   cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DIS);

  struct vpci_cap caps[8];
  int ncaps = vpci_collect_caps(bus, slot, caps, 8);
  if (ncaps <= 0) return -1;

  struct vpci_cap* cc = 0; /* common */
  struct vpci_cap* nc = 0; /* notify */
  struct vpci_cap* ic = 0; /* isr */
  for (int i = 0; i < ncaps; i++) {
    if (caps[i].kind == VIRTIO_PCI_CAP_COMMON_CFG) cc = &caps[i];
    else if (caps[i].kind == VIRTIO_PCI_CAP_NOTIFY_CFG) nc = &caps[i];
    else if (caps[i].kind == VIRTIO_PCI_CAP_ISR_CFG) ic = &caps[i];
  }
  if (!cc || !nc || !ic) return -1;
  if (cc->bar != nc->bar || cc->bar != ic->bar) return -1;

  uint64_t bar = vpci_bar_phys(bus, slot, cc->bar);
  if (bar < 0x100000 || (bar & 0xFFFFFFFFull) == 0xFFFFFFF0ull) return -1;

  uint64_t span = cc->offset + cc->length;
  if (nc->offset + nc->length > span) span = nc->offset + nc->length;
  if (ic->offset + ic->length > span) span = ic->offset + ic->length;
  if (gpu_map_bar(bar, span) != 0) return -1;

  gpu_cc = gpu_mmio_va(bar + cc->offset);
  gpu_notify = gpu_mmio_va(bar + nc->offset);
  gpu_isr = gpu_mmio_va(bar + ic->offset);
  gpu_notify_mult = nc->mult ? nc->mult : 4;

#if G2_VPCI_DBG
  uart_puts("G2DBG vpci bar=");
  uart_print_hex(bar);
  uart_puts(" cc=");
  uart_print_hex(cc->offset);
  uart_puts(" nc=");
  uart_print_hex(nc->offset);
  uart_puts(" len=");
  uart_print_hex(nc->length);
  uart_puts(" mult=");
  print_int((int)nc->mult);
  uart_puts("\n");
#endif

  /* Reset, then ACK|DRIVER. */
  gpu_cc_w8(VPCI_CC_DEVICE_STATUS, 0);
  int guard = 0;
  while (gpu_cc_r8(VPCI_CC_DEVICE_STATUS) != 0) {
    arch_memory_barrier();
    if (++guard > 1000000) return -1;
  }
  gpu_cc_w8(VPCI_CC_DEVICE_STATUS, VPCI_STATUS_ACK);
  gpu_cc_w8(VPCI_CC_DEVICE_STATUS, VPCI_STATUS_ACK | VPCI_STATUS_DRIVER);

  /* Feature negotiation: accept VIRTIO_F_VERSION_1 only (2D-only driver). */
  gpu_cc_w32(VPCI_CC_DEV_FEAT_SEL, 0);
  uint32_t feat_lo = gpu_cc_r32(VPCI_CC_DEV_FEAT);
  gpu_cc_w32(VPCI_CC_DEV_FEAT_SEL, 1);
  uint32_t feat_hi = gpu_cc_r32(VPCI_CC_DEV_FEAT);
#if G2_VPCI_DBG
  uart_puts("G2DBG feat hi=");
  uart_print_hex(feat_hi);
  uart_puts(" lo=");
  uart_print_hex(feat_lo);
  uart_puts("\n");
#endif
  if (!(feat_hi & VPCI_F_VERSION_1)) return -1;

  gpu_cc_w32(VPCI_CC_DRV_FEAT_SEL, 0);
  gpu_cc_w32(VPCI_CC_DRV_FEAT, 0);
  gpu_cc_w32(VPCI_CC_DRV_FEAT_SEL, 1);
  gpu_cc_w32(VPCI_CC_DRV_FEAT, VPCI_F_VERSION_1);

  gpu_cc_w8(VPCI_CC_DEVICE_STATUS,
            VPCI_STATUS_ACK | VPCI_STATUS_DRIVER | VPCI_STATUS_FEATURES_OK);
  arch_memory_barrier();
  if (!(gpu_cc_r8(VPCI_CC_DEVICE_STATUS) & VPCI_STATUS_FEATURES_OK)) return -1;

  /* Control queue (0): size, addresses, enable. */
  gpu_cc_w16(VPCI_CC_QUEUE_SEL, VGPU_QUEUE_ID);
  uint16_t maxsz = gpu_cc_r16(VPCI_CC_QUEUE_SIZE);
  if (maxsz == 0) return -1;
  uint16_t qsz = (maxsz < VGPU_QDESC) ? maxsz : VGPU_QDESC;
  gpu_cc_w16(VPCI_CC_QUEUE_SIZE, qsz);
  gpu_cc_w16(VPCI_CC_QUEUE_MSIX_VEC, 0xFFFF); /* no MSI-X vector */
  gpu_qsize = qsz;
  gpu_cc_w64(VPCI_CC_QUEUE_DESC, (uint64_t)gpu_desc);
  gpu_cc_w64(VPCI_CC_QUEUE_DRIVER, (uint64_t)&gpu_avail);
  gpu_cc_w64(VPCI_CC_QUEUE_DEVICE, (uint64_t)&gpu_used);
  gpu_cc_w16(VPCI_CC_QUEUE_ENABLE, 1);
  gpu_notify_off = gpu_cc_r16(VPCI_CC_QUEUE_NOTIFY_OFF);

#if G2_VPCI_DBG
  uart_puts("G2DBG q max=");
  print_int((int)maxsz);
  uart_puts(" use=");
  print_int((int)qsz);
  uart_puts(" noff=");
  print_int((int)gpu_notify_off);
  uart_puts("\n");
#endif

  gpu_cc_w8(VPCI_CC_DEVICE_STATUS, VPCI_STATUS_ACK | VPCI_STATUS_DRIVER |
                                       VPCI_STATUS_FEATURES_OK | VPCI_STATUS_DRIVER_OK);

  /* 2D resource + scanout. */
  return vgpu_resource_setup();
}

/* Probe the PCI bus for a modern virtio GPU and bring it up. */
static int vgpu_probe_and_bringup(void) {
  for (uint32_t bus = 0; bus < 256; bus++) {
    for (uint32_t slot = 0; slot < 32; slot++) {
      uint32_t id = pci_read_config(bus, slot, 0, 0);
      if (id == 0xFFFFFFFF) continue;
      uint16_t vendor = id & 0xFFFF;
      uint16_t device = (id >> 16) & 0xFFFF;
      if (!virtio_gpu_x64_is_gpu_dev(vendor, device)) continue;
      if (device != VIRTIO_PCI_DEV_GPU_MODERN) {
#if G2_VPCI_DBG
        uart_puts("G2DBG legacy 1010 seen; no modern caps\n");
#endif
        continue; /* legacy I/O transport not implementable in QEMU */
      }
      if (vgpu_bringup(bus, slot) == 0) return 0;
    }
  }
  return -1;
}

/* --- per-rect present ------------------------------------------------- */

static int vgpu_rect_present(int32_t x, int32_t y, int32_t w, int32_t h) {
  gpu_req_transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  gpu_req_transfer.hdr.flags = 0;
  gpu_req_transfer.hdr.fence_id = 0;
  gpu_req_transfer.hdr.ctx_id = 0;
  gpu_req_transfer.hdr.padding = 0;
  gpu_req_transfer.r.x = (uint32_t)x;
  gpu_req_transfer.r.y = (uint32_t)y;
  gpu_req_transfer.r.width = (uint32_t)w;
  gpu_req_transfer.r.height = (uint32_t)h;
  gpu_req_transfer.offset = virtio_gpu_x64_rect_offset(x, y);
  gpu_req_transfer.resource_id = 1;
  gpu_req_transfer.padding = 0;
  if (gpu_transact(&gpu_req_transfer, sizeof(gpu_req_transfer), 0, 0) != 0) return -1;

  gpu_req_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  gpu_req_flush.hdr.flags = 0;
  gpu_req_flush.hdr.fence_id = 0;
  gpu_req_flush.hdr.ctx_id = 0;
  gpu_req_flush.hdr.padding = 0;
  gpu_req_flush.r.x = (uint32_t)x;
  gpu_req_flush.r.y = (uint32_t)y;
  gpu_req_flush.r.width = (uint32_t)w;
  gpu_req_flush.r.height = (uint32_t)h;
  gpu_req_flush.resource_id = 1;
  gpu_req_flush.padding = 0;
  return gpu_transact(&gpu_req_flush, sizeof(gpu_req_flush), 0, 0);
}

/* ------------------------------------------------------------------ */
/* public entry points                                                 */
/* ------------------------------------------------------------------ */

/* Pure helpers shared with the unit tests (src/kernel/virtio_gpu_test.c). */

int virtio_gpu_x64_is_gpu_dev(uint16_t vendor, uint16_t device) {
  return vendor == VIRTIO_PCI_VENDOR &&
         (device == VIRTIO_PCI_DEV_GPU_MODERN || device == VIRTIO_PCI_DEV_GPU_LEGACY);
}

/* Clamp a screen-space rect to the desktop panel (display_mode.h).  Returns
 * 1 and writes the clipped rect when any pixel remains, 0 for empty/off-screen
 * rects.  64-bit math: rect coordinates come from user space, so x+w / y+h
 * can overflow int32. */
int virtio_gpu_x64_clamp_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t* ox, int32_t* oy, int32_t* ow, int32_t* oh) {
  if (!ox || !oy || !ow || !oh) return 0;
  int64_t x0 = x, y0 = y;
  int64_t x1 = (int64_t)x + (int64_t)w;
  int64_t y1 = (int64_t)y + (int64_t)h;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > GPU_SCREEN_W) x1 = GPU_SCREEN_W;
  if (y1 > GPU_SCREEN_H) y1 = GPU_SCREEN_H;
  if (x1 <= x0 || y1 <= y0) return 0;
  *ox = (int32_t)x0;
  *oy = (int32_t)y0;
  *ow = (int32_t)(x1 - x0);
  *oh = (int32_t)(y1 - y0);
  return 1;
}

/* Byte offset of (x,y) in the (GPU_SCREEN_W * GPU_SCREEN_H) x32 backing
 * store -- the TRANSFER_TO_HOST_2D offset (spec: offset of the rectangle in
 * the resource, stride GPU_SCREEN_W*4). */
uint64_t virtio_gpu_x64_rect_offset(int32_t x, int32_t y) {
  return (uint64_t)y * (GPU_SCREEN_W * 4) + (uint64_t)x * 4;
}

int virtio_gpu_x64_active_mode(void) {
  return gpu_mode;
}

int virtio_gpu_init(void) {
  spinlock_init(&gpu_lock);
  for (int i = 0; i < GPU_SCREEN_W * GPU_SCREEN_H; i++) framebuffer[i] = 0xFF000000;

  if (vgpu_probe_and_bringup() == 0) {
    gpu_mode = VGPU_MODE_VIRTIO;
    uart_puts("VirtIO GPU: virtio-pci\n");
    virtio_gpu_flush();
    return 0;
  }

  if (bga_setup() == 0) {
    gpu_mode = VGPU_MODE_BGA;
    uart_puts("VirtIO GPU: BGA\n");
    virtio_gpu_flush();
    return 0;
  }

  gpu_mode = VGPU_MODE_NONE;
  return -1;
}

/* Full-screen present: one full-screen rect (byte-identical output to the
 * old full-frame path on BGA; a full transfer+flush on virtio-pci). */
void virtio_gpu_flush(void) {
  struct virtio_gpu_xrect full = {0, 0, GPU_SCREEN_W, GPU_SCREEN_H};
  virtio_gpu_flush_rects(&full, 1);
}

/* GX (docs/graphics-accel.md): damage-rect present.  Clamps/skips rects,
 * then presents each through the active path: per-rect
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH on virtio-pci, row-wise copy into
 * the BGA LFB otherwise. */
void virtio_gpu_flush_rects(const struct virtio_gpu_xrect* rects, int count) {
  if (!rects || count <= 0) return;
  if (count > VGPU_RECT_MAX) count = VGPU_RECT_MAX;

  /* WD probe (WD_X64_FLUSH_PROBE=1): time-series of the KERNEL fb at
   * browser-window monitor points on every large (near full-screen)
   * flush (first 1500), so a second mapper's blit can be correlated
   * with the kernel fb and the visible scanout.  Leaving on is ~60
   * bytes per full flush; harmless, but off by default. */
#if WD_X64_FLUSH_PROBE
  static int wd_probe_n = 0;
  for (int i = 0; i < count; i++) {
    int32_t x, y, w, h;
    if (!virtio_gpu_x64_clamp_rect(rects[i].x, rects[i].y, rects[i].w, rects[i].h,
                                   &x, &y, &w, &h))
      continue;
    if ((uint64_t)w * h >= 100000ULL && wd_probe_n < 1500) {
      wd_probe_n++;
      struct { int x, y; } pts[6] = {
        {102, 100}, {800, 300}, {102, 650}, {512, 300}, {512, 640}, {40, 200}
      };
      uart_puts("[WD] n=");
      print_int(wd_probe_n);
      uart_puts(" rect=(");
      print_int(x); uart_puts(","); print_int(y); uart_puts(",");
      print_int(w); uart_puts(","); print_int(h); uart_puts(")");
      for (int k = 0; k < 6; k++) {
        uint32_t v = framebuffer[(uint64_t)pts[k].y * GPU_SCREEN_W + pts[k].x];
        uart_puts(" P");
        print_int(k + 1);
        uart_puts("=");
        uart_print_hex(v);
      }
      uart_puts("\n");
      break;
    }
  }
#endif /* WD_X64_FLUSH_PROBE */

  uint64_t flags = spinlock_acquire_irqsave(&gpu_lock);
  if (gpu_mode == VGPU_MODE_VIRTIO) {
    for (int i = 0; i < count; i++) {
      int32_t x, y, w, h;
      if (!virtio_gpu_x64_clamp_rect(rects[i].x, rects[i].y, rects[i].w, rects[i].h,
                                     &x, &y, &w, &h))
        continue;
      if (vgpu_rect_present(x, y, w, h) != 0) break;
    }
    gpu_isr_ack();
  } else if (gpu_mode == VGPU_MODE_BGA) {
    for (int i = 0; i < count; i++) {
      int32_t x, y, w, h;
      if (!virtio_gpu_x64_clamp_rect(rects[i].x, rects[i].y, rects[i].w, rects[i].h,
                                     &x, &y, &w, &h))
        continue;
      bga_present_rect(x, y, w, h);
    }
  }
  spinlock_release_irqrestore(&gpu_lock, flags);
}

uint32_t* virtio_gpu_get_framebuffer(void) {
  return framebuffer;
}

#endif /* __x86_64__ */
