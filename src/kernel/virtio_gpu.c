#include "virtio_gpu.h"
#include "lock.h"
#include "arch/cpu.h"


#ifdef __x86_64__

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

#else

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

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[16];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[16];
    uint16_t avail_event;
} __attribute__((packed));

struct virtq {
    struct virtq_desc desc[16];
    struct virtq_avail avail;
    uint8_t padding[4096 - (256 + sizeof(struct virtq_avail))];
    struct virtq_used used;
} __attribute__((aligned(4096)));

static struct virtq gpu_vq __attribute__((aligned(4096)));
static uint8_t* gpu_mmio = 0;
static uint16_t gpu_ack_used_idx = 0;
static spinlock_t gpu_lock;

static uint32_t framebuffer[1024 * 768] __attribute__((aligned(2097152)));

static inline void reg_write32(uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(gpu_mmio + offset) = val;
}

static inline uint32_t reg_read32(uint32_t offset) {
    return *(volatile uint32_t*)(gpu_mmio + offset);
}

/**
 * Sends a command to the VirtIO GPU device via the control virtqueue.
 * Uses a 2-descriptor chain (Request, Response).
 * 
 * Returns:
 *   0 on success, -1 on failure.
 */
static int virtio_gpu_do_cmd(void* req, uint32_t req_size, void* resp, uint32_t resp_size) {
    uint64_t flags = spinlock_acquire_irqsave(&gpu_lock);
    if (!gpu_mmio) {
        spinlock_release_irqrestore(&gpu_lock, flags);
        return -1;
    }

    gpu_vq.desc[0].addr = (uint64_t)req;
    gpu_vq.desc[0].len = req_size;
    gpu_vq.desc[0].flags = 1; // VIRTQ_DESC_F_NEXT
    gpu_vq.desc[0].next = 1;

    gpu_vq.desc[1].addr = (uint64_t)resp;
    gpu_vq.desc[1].len = resp_size;
    gpu_vq.desc[1].flags = 2; // VIRTQ_DESC_F_WRITE
    gpu_vq.desc[1].next = 0;

    uint16_t head_idx = gpu_vq.avail.idx;
    gpu_vq.avail.ring[head_idx % 16] = 0;
    
    arch_memory_barrier();
    gpu_vq.avail.idx++;
    arch_memory_barrier();

    reg_write32(VIRTIO_QUEUE_NOTIFY, 0);

    // Poll for completion (simple spinlock for now)
    while (*(volatile uint16_t*)&gpu_vq.used.idx == gpu_ack_used_idx) {
        // We could wfi here if we had an IRQ handler, but let's just spin with lock held 
        // for simplicity since GPU commands are usually fast.
    }

    gpu_ack_used_idx = gpu_vq.used.idx;
    arch_memory_barrier();

    struct virtio_gpu_ctrl_hdr* hdr = (struct virtio_gpu_ctrl_hdr*)resp;
    int res = (hdr->type >= 0x1200 ? -1 : 0);
    spinlock_release_irqrestore(&gpu_lock, flags);
    return res;
}

/**
 * Sends a command with an additional data buffer to the VirtIO GPU device.
 * Uses a 3-descriptor chain (Request, Data, Response).
 */
static int virtio_gpu_do_cmd_with_data(void* req, uint32_t req_size, void* data, uint32_t data_size, void* resp, uint32_t resp_size) {
    uint64_t flags = spinlock_acquire_irqsave(&gpu_lock);
    if (!gpu_mmio) {
        spinlock_release_irqrestore(&gpu_lock, flags);
        return -1;
    }

    gpu_vq.desc[0].addr = (uint64_t)req;
    gpu_vq.desc[0].len = req_size;
    gpu_vq.desc[0].flags = 1; // VIRTQ_DESC_F_NEXT
    gpu_vq.desc[0].next = 1;

    gpu_vq.desc[1].addr = (uint64_t)data;
    gpu_vq.desc[1].len = data_size;
    gpu_vq.desc[1].flags = 1; // VIRTQ_DESC_F_NEXT (Read-only data)
    gpu_vq.desc[1].next = 2;

    gpu_vq.desc[2].addr = (uint64_t)resp;
    gpu_vq.desc[2].len = resp_size;
    gpu_vq.desc[2].flags = 2; // VIRTQ_DESC_F_WRITE
    gpu_vq.desc[2].next = 0;

    uint16_t head_idx = gpu_vq.avail.idx;
    gpu_vq.avail.ring[head_idx % 16] = 0;
    
    arch_memory_barrier();
    gpu_vq.avail.idx++;
    arch_memory_barrier();

    reg_write32(VIRTIO_QUEUE_NOTIFY, 0);

    while (*(volatile uint16_t*)&gpu_vq.used.idx == gpu_ack_used_idx) {}

    gpu_ack_used_idx = gpu_vq.used.idx;
    arch_memory_barrier();

    spinlock_release_irqrestore(&gpu_lock, flags);
    return 0;
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
    for (int i = 0; i < 32; i++) {
        uint8_t* mmio = MMIO_BASE(i);
        uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
        uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
        if (magic == 0x74726976 && devid == 16) {
            gpu_mmio = mmio;
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
    
    reg_write32(VIRTIO_QUEUE_NUM, 16);
    reg_write32(VIRTIO_QUEUE_ALIGN, 4096);
    reg_write32(VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&gpu_vq / 4096));

    status |= 4; reg_write32(VIRTIO_STATUS, status);

    // Initialize display
    struct virtio_gpu_resource_create_2d create = {0};
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = 1;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create.width = 1024;
    create.height = 768;

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
    scanout.r.width = 1024;
    scanout.r.height = 768;
    scanout.scanout_id = 0;
    scanout.resource_id = 1;

    virtio_gpu_do_cmd(&scanout, sizeof(scanout), &resp, sizeof(resp));

    // Clear to black
    for (int i = 0; i < 1024 * 768; i++) framebuffer[i] = 0xFF000000;
    virtio_gpu_flush();

    return 0;
}

/**
 * Flushes the current framebuffer contents to the host display.
 * Performs a Transfer-to-Host-2D followed by a Resource-Flush.
 */
void virtio_gpu_flush(void) {
    if (!gpu_mmio) return;

    struct virtio_gpu_transfer_to_host_2d transfer = {0};
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r.width = 1024;
    transfer.r.height = 768;
    transfer.resource_id = 1;

    struct virtio_gpu_ctrl_hdr resp;
    virtio_gpu_do_cmd(&transfer, sizeof(transfer), &resp, sizeof(resp));

    struct virtio_gpu_resource_flush flush = {0};
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.width = 1024;
    flush.r.height = 768;
    flush.resource_id = 1;

    virtio_gpu_do_cmd(&flush, sizeof(flush), &resp, sizeof(resp));
}

/**
 * Returns a pointer to the kernel's physical framebuffer memory.
 */
uint32_t* virtio_gpu_get_framebuffer(void) {
    return framebuffer;
}

#endif

