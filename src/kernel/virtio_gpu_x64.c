/* GX (docs/graphics-accel.md): x64 kernel GPU path.
 *
 * Split out of virtio_gpu.c at the GX freeze so lane G2 owns this file
 * alone (ARM lives in virtio_gpu.c).  Current content: Bochs VGA (BGA)
 * bring-up + the full-frame flush.  G2 adds the legacy virtio-pci GPU
 * transport (modeled on virtio_net.c's legacy path) and the BGA
 * damage-rect fallback; virtio_gpu_flush_rects() below is the entry point
 * both use.
 */
#ifdef __x86_64__

#include "virtio_gpu.h"
#include "lock.h"
#include "arch/cpu.h"

static spinlock_t gpu_lock;
static uint32_t framebuffer[1024 * 768] __attribute__((aligned(2097152)));
static uint8_t* gpu_mmio = 0;

uint32_t bga_framebuffer_phys = 0;

static inline void outw(uint16_t port, uint16_t val) {
  __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
  uint16_t ret;
  __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
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

int virtio_gpu_init(void) {
  spinlock_init(&gpu_lock);
  for (int i = 0; i < 1024 * 768; i++) framebuffer[i] = 0xFF000000;

  int found = 0;
  for (uint32_t bus = 0; bus < 256; bus++) {
    for (uint32_t slot = 0; slot < 32; slot++) {
      uint32_t id = pci_read_config(bus, slot, 0, 0);
      uint16_t vendor = id & 0xFFFF;
      uint16_t device = (id >> 16) & 0xFFFF;
      if (vendor == 0x1234 && device == 0x1111) {
        uint32_t bar0 = pci_read_config(bus, slot, 0, 0x10);
        bga_framebuffer_phys = bar0 & 0xFFFFFFF0;

        uint32_t cmd = pci_read_config(bus, slot, 0, 0x04);
        pci_write_config(bus, slot, 0, 0x04, cmd | 0x02);
        found = 1;
        break;
      }
    }
    if (found) break;
  }

  if (!found || bga_framebuffer_phys == 0) {
    return -1;
  }

  bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
  bga_write(VBE_DISPI_INDEX_XRES, 1024);
  bga_write(VBE_DISPI_INDEX_YRES, 768);
  bga_write(VBE_DISPI_INDEX_BPP, 32);
  bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB);

  gpu_mmio = (uint8_t*)1;
  virtio_gpu_flush();
  return 0;
}

void virtio_gpu_flush(void) {
  if (!gpu_mmio || bga_framebuffer_phys == 0) return;
  volatile uint32_t* dest = (volatile uint32_t*)(uint64_t)bga_framebuffer_phys;
  for (int i = 0; i < 1024 * 768; i++) {
    dest[i] = framebuffer[i];
  }
}

uint32_t* virtio_gpu_get_framebuffer(void) {
  return framebuffer;
}

/* GX (docs/graphics-accel.md): damage-rect present -- freeze default falls
 * back to the full-frame path; G2 replaces this with the virtio-pci / BGA
 * rect implementations. */
void virtio_gpu_flush_rects(const struct virtio_gpu_xrect *rects, int count) {
  (void)rects;
  (void)count;
  virtio_gpu_flush();
}

#endif /* __x86_64__ */
