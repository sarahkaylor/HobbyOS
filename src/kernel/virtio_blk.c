#include "virtio_blk.h"
#include "lock.h"
#include "process.h"
#include "arch/cpu.h"


#ifdef __x86_64__

#include "virtio_blk.h"
#include "lock.h"
#include "process.h"
#include "arch/cpu.h"

extern void uart_puts(const char* s);
extern void *memcpy(void *dest, const void *src, size_t n);

static spinlock_t blk_lock;
static spinlock_t blk_request_lock;
int virtio_blk_irq = -1;

struct nvme_cmd {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

struct nvme_cqe {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

static volatile uint32_t* nvme_regs;
static volatile uint32_t* admin_sq_db;
static volatile uint32_t* admin_cq_db;
static volatile uint32_t* io_sq_db;
static volatile uint32_t* io_cq_db;

static volatile struct nvme_cmd admin_sq[2] __attribute__((aligned(4096)));
static volatile struct nvme_cqe admin_cq[2] __attribute__((aligned(4096)));
static volatile struct nvme_cmd io_sq[2] __attribute__((aligned(4096)));
static volatile struct nvme_cqe io_cq[2] __attribute__((aligned(4096)));
static uint8_t bounce_buf[4096] __attribute__((aligned(4096)));

static uint16_t admin_sq_tail = 0;
static uint16_t admin_cq_head = 0;
static uint16_t io_sq_tail = 0;
static uint16_t io_cq_head = 0;
static uint16_t io_cid = 100;

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

static uint16_t submit_admin_cmd(struct nvme_cmd* cmd) {
    uint16_t cid = cmd->cid;
    
    admin_sq[admin_sq_tail].opcode = cmd->opcode;
    admin_sq[admin_sq_tail].flags = cmd->flags;
    admin_sq[admin_sq_tail].cid = cmd->cid;
    admin_sq[admin_sq_tail].nsid = cmd->nsid;
    admin_sq[admin_sq_tail].rsvd2 = cmd->rsvd2;
    admin_sq[admin_sq_tail].mptr = cmd->mptr;
    admin_sq[admin_sq_tail].prp1 = cmd->prp1;
    admin_sq[admin_sq_tail].prp2 = cmd->prp2;
    admin_sq[admin_sq_tail].cdw10 = cmd->cdw10;
    admin_sq[admin_sq_tail].cdw11 = cmd->cdw11;
    admin_sq[admin_sq_tail].cdw12 = cmd->cdw12;
    admin_sq[admin_sq_tail].cdw13 = cmd->cdw13;
    admin_sq[admin_sq_tail].cdw14 = cmd->cdw14;
    admin_sq[admin_sq_tail].cdw15 = cmd->cdw15;

    admin_sq_tail = (admin_sq_tail + 1) % 2;
    
    __asm__ volatile("" : : : "memory");
    
    *admin_sq_db = admin_sq_tail;
    
    while (admin_cq[admin_cq_head].cid != cid) {
        __asm__ volatile("pause");
    }
    
    uint16_t status = admin_cq[admin_cq_head].status;
    admin_cq[admin_cq_head].cid = 0xFFFF;
    admin_cq_head = (admin_cq_head + 1) % 2;
    *admin_cq_db = admin_cq_head;
    
    return status >> 1;
}

extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

static uint16_t submit_io_cmd(struct nvme_cmd* cmd) {
    uint16_t cid = cmd->cid;
    
    io_sq[io_sq_tail].opcode = cmd->opcode;
    io_sq[io_sq_tail].flags = cmd->flags;
    io_sq[io_sq_tail].cid = cmd->cid;
    io_sq[io_sq_tail].nsid = cmd->nsid;
    io_sq[io_sq_tail].rsvd2 = cmd->rsvd2;
    io_sq[io_sq_tail].mptr = cmd->mptr;
    io_sq[io_sq_tail].prp1 = cmd->prp1;
    io_sq[io_sq_tail].prp2 = cmd->prp2;
    io_sq[io_sq_tail].cdw10 = cmd->cdw10;
    io_sq[io_sq_tail].cdw11 = cmd->cdw11;
    io_sq[io_sq_tail].cdw12 = cmd->cdw12;
    io_sq[io_sq_tail].cdw13 = cmd->cdw13;
    io_sq[io_sq_tail].cdw14 = cmd->cdw14;
    io_sq[io_sq_tail].cdw15 = cmd->cdw15;

    io_sq_tail = (io_sq_tail + 1) % 2;
    
    __asm__ volatile("" : : : "memory");
    
    *io_sq_db = io_sq_tail;
    
    int spin_count = 0;
    while (io_cq[io_cq_head].cid != cid) {
        __asm__ volatile("pause");
        spin_count++;
        if (spin_count == 1000000) {
            uart_puts("submit_io_cmd: STILL SPINNING! io_cq[0].cid=");
            print_int(io_cq[0].cid);
            uart_puts(" io_cq[1].cid=");
            print_int(io_cq[1].cid);
            uart_puts(" io_cq_head=");
            print_int(io_cq_head);
            uart_puts(" CSTS=");
            uart_print_hex(nvme_regs[7]);
            uart_puts("\n");
            spin_count = 0;
        }
    }
    
    uint16_t status = io_cq[io_cq_head].status;
    io_cq[io_cq_head].cid = 0xFFFF;
    io_cq_head = (io_cq_head + 1) % 2;
    *io_cq_db = io_cq_head;
    
    return status >> 1;
}

void virtio_blk_handle_irq(void) {
}

int virtio_blk_init(void) {
    spinlock_init(&blk_lock);
    spinlock_init(&blk_request_lock);
    
    // Scan PCI bus for NVMe controller
    int found = 0;
    uint8_t nvme_bus = 0, nvme_slot = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read_config(bus, slot, 0, 0);
            if ((id & 0xFFFF) != 0xFFFF) {
                uint32_t class_rev = pci_read_config(bus, slot, 0, 0x08);
                uint8_t base_class = (class_rev >> 24) & 0xFF;
                uint8_t sub_class = (class_rev >> 16) & 0xFF;
                if (base_class == 0x01 && sub_class == 0x08) {
                    nvme_bus = bus;
                    nvme_slot = slot;
                    found = 1;
                    break;
                }
            }
        }
        if (found) break;
    }
    
    if (!found) {
        uart_puts("NVMe controller not found on PCI bus!\n");
        return -1;
    }
    
    uart_puts("Found NVMe controller on PCI bus!\n");
    
    // Read BAR0 (64-bit BAR)
    uint32_t bar0_low = pci_read_config(nvme_bus, nvme_slot, 0, 0x10);
    uint32_t bar0_high = pci_read_config(nvme_bus, nvme_slot, 0, 0x14);
    uint64_t nvme_regs_phys = (bar0_low & 0xFFFFFFF0) | ((uint64_t)bar0_high << 32);
    
    // Enable Memory Space and Bus Mastering
    uint32_t cmd = pci_read_config(nvme_bus, nvme_slot, 0, 0x04);
    pci_write_config(nvme_bus, nvme_slot, 0, 0x04, cmd | 0x06);
    
    // Setup register pointer
    nvme_regs = (volatile uint32_t*)(uint64_t)nvme_regs_phys;
    
    uart_puts("NVMe BAR0 Phys: ");
    uart_print_hex(nvme_regs_phys);
    uart_puts("\n");
    
    // Reset/Disable Controller
    nvme_regs[5] &= ~1; // CC.EN = 0
    while (nvme_regs[7] & 1) { // Wait for CSTS.RDY = 0
        __asm__ volatile("pause");
    }
    
    // Configure Admin Queues
    // AQA: ASQS = 1 (2 entries), ACQS = 1 (2 entries)
    nvme_regs[9] = 0x00010001;
    
    // Initialize Admin Queue memory
    for (int i = 0; i < 2; i++) {
        admin_cq[i].cid = 0xFFFF;
        io_cq[i].cid = 0xFFFF;
    }
    
    // ASQ
    *(volatile uint64_t*)&nvme_regs[10] = (uint64_t)&admin_sq;
    // ACQ
    *(volatile uint64_t*)&nvme_regs[12] = (uint64_t)&admin_cq;
    
    // Configure CC: IOSQES=6 (64 bytes), IOCQES=4 (16 bytes), MPS=0, AMS=0, CSS=0, EN=1
    nvme_regs[5] = 0x00460001;
    
    // Wait for CSTS.RDY = 1
    while (!(nvme_regs[7] & 1)) {
        __asm__ volatile("pause");
    }
    
    // Determine Doorbell Stride
    uint32_t dstrd = (nvme_regs[1] >> 0) & 0xF;
    uint32_t stride_words = (4 << dstrd) / 4;
    
    admin_sq_db = &nvme_regs[1024 + 0 * 2 * stride_words];
    admin_cq_db = &nvme_regs[1024 + (0 * 2 + 1) * stride_words];
    io_sq_db    = &nvme_regs[1024 + 1 * 2 * stride_words];
    io_cq_db    = &nvme_regs[1024 + (1 * 2 + 1) * stride_words];
    
    // Create IO Completion Queue 1
    struct nvme_cmd create_cq = {0};
    create_cq.opcode = 0x05; // Create I/O Completion Queue
    create_cq.cid = 1;
    create_cq.prp1 = (uint64_t)&io_cq;
    create_cq.cdw10 = (1 << 16) | 0x01; // Size = 2 (1), ID = 1
    create_cq.cdw11 = 0x01; // PC = 1
    uint16_t status_cq = submit_admin_cmd(&create_cq);
    uart_puts("Create IO CQ Status: ");
    uart_print_hex(status_cq);
    uart_puts("\n");
    
    // Create IO Submission Queue 1
    struct nvme_cmd create_sq = {0};
    create_sq.opcode = 0x01; // Create I/O Submission Queue
    create_sq.cid = 2;
    create_sq.prp1 = (uint64_t)&io_sq;
    create_sq.cdw10 = (1 << 16) | 0x01; // Size = 2 (1), ID = 1
    create_sq.cdw11 = (1 << 16) | 0x01; // CQID = 1, PC = 1
    uint16_t status_sq = submit_admin_cmd(&create_sq);
    uart_puts("Create IO SQ Status: ");
    uart_print_hex(status_sq);
    uart_puts("\n");
    
    uart_puts("NVMe driver initialized successfully!\n");
    return 0;
}

int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_request_lock);
    int res = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct nvme_cmd cmd = {0};
        cmd.opcode = 0x02; // Read
        cmd.nsid = 1;      // Namespace 1
        cmd.cid = io_cid++;
        cmd.prp1 = (uint64_t)&bounce_buf[0];
        cmd.cdw10 = (uint32_t)(sector + i);
        cmd.cdw11 = (uint32_t)((sector + i) >> 32);
        cmd.cdw12 = 0; // 1 block (0)
        
        uint16_t status = submit_io_cmd(&cmd);
        if (status != 0) {
            res = -1;
            break;
        }
        memcpy((uint8_t*)buf + (i * 512), (const void*)&bounce_buf[0], 512);
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    return res;
}

int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_request_lock);
    int res = 0;
    for (uint32_t i = 0; i < count; i++) {
        memcpy((void*)&bounce_buf[0], (const uint8_t*)buf + (i * 512), 512);
        struct nvme_cmd cmd = {0};
        cmd.opcode = 0x01; // Write
        cmd.nsid = 1;      // Namespace 1
        cmd.cid = io_cid++;
        cmd.prp1 = (uint64_t)&bounce_buf[0];
        cmd.cdw10 = (uint32_t)(sector + i);
        cmd.cdw11 = (uint32_t)((sector + i) >> 32);
        cmd.cdw12 = 0; // 1 block (0)
        
        uint16_t status = submit_io_cmd(&cmd);
        if (status != 0) {
            res = -1;
            break;
        }
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    return res;
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

// VirtIO Block types
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[8];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[8];
    uint16_t avail_event;
} __attribute__((packed));

// VirtIO MMIO V1 (Legacy) strictly dictates that the descriptor, available ring,
// and used ring arrays MUST be allocated contiguously in physical memory.
// The `used` ring must further strictly start on a Page Aligned boundary matching
// the `VIRTIO_QUEUE_ALIGN` configuration (typically 4096).
// We simulate this by defining a solitary struct, padding out the internal gap manually.
struct virtq {
    struct virtq_desc desc[8];
    struct virtq_avail avail;
    uint8_t padding[4096 - (128 + sizeof(struct virtq_avail))];
    struct virtq_used used;
} __attribute__((aligned(4096)));

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static struct virtq vq __attribute__((aligned(4096)));

static struct virtio_blk_req blk_req;
static uint8_t blk_status;

static uint8_t* blk_mmio = 0;
static uint16_t ack_used_idx = 0;
int virtio_blk_irq = -1;
static spinlock_t blk_lock;
static spinlock_t blk_request_lock;

static inline void reg_write32(uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(blk_mmio + offset) = val;
}

static inline uint32_t reg_read32(uint32_t offset) {
    return *(volatile uint32_t*)(blk_mmio + offset);
}

/**
 * Handles interrupts from the VirtIO block device.
 * Acknowledges the interrupt in the device's status register.
 */
void virtio_blk_handle_irq(void) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_lock);
    if (!blk_mmio) {
        spinlock_release_irqrestore(&blk_lock, flags);
        return;
    }
    uint32_t status = reg_read32(VIRTIO_INTERRUPT_STATUS);
    if (status) {
        reg_write32(VIRTIO_INTERRUPT_ACK, status);
    }
    spinlock_release_irqrestore(&blk_lock, flags);
}

/**
 * Scans for and initializes the VirtIO block device.
 * Performs the VirtIO legacy MMIO handshake, negotiates features, and sets up the virtqueue.
 * 
 * Returns:
 *   0 on success, -1 if the device is not found or fails to initialize.
 */
int virtio_blk_init(void) {
    spinlock_init(&blk_lock);
    spinlock_init(&blk_request_lock);
    // Scan for virtio block device (ID 2)
    for (int i = 0; i < 32; i++) {
        uint8_t* mmio = MMIO_BASE(i);
        uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
        uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
        if (magic == 0x74726976 && devid == 2) {
            blk_mmio = mmio;
            virtio_blk_irq = 48 + i;
            break;
        }
    }

    if (!blk_mmio) return -1; // Not found

    uint32_t status = 0;
    
    // Reset device
    reg_write32(VIRTIO_STATUS, status);
    
    // Acknowledge
    status |= 1; reg_write32(VIRTIO_STATUS, status);
    
    // Driver
    status |= 2; reg_write32(VIRTIO_STATUS, status);
    
    // Read features, accept them (by writing 0 since we don't need advanced features)
    // BUT we MUST negotiate VIRTIO_F_VERSION_1 (bit 32) since QEMU virt is MMIO Version 2
    reg_write32(VIRTIO_DRIVER_FEAT_SEL, 1);
    reg_write32(VIRTIO_DRIVER_FEAT, 1); // Set bit 32 (bit 0 of high word)
    
    // Write 0 to standard features
    reg_write32(VIRTIO_DRIVER_FEAT_SEL, 0);
    reg_write32(VIRTIO_DRIVER_FEAT, 0);

    // Features OK
    status |= 8; reg_write32(VIRTIO_STATUS, status);
    if (!(reg_read32(VIRTIO_STATUS) & 8)) return -1;

    // Setup queue 0
    reg_write32(VIRTIO_GUEST_PAGE_SIZE, 4096);
    reg_write32(VIRTIO_QUEUE_SEL, 0);
    uint32_t max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
    if (max_size == 0) return -1;
    
    reg_write32(VIRTIO_QUEUE_NUM, 8);
    reg_write32(VIRTIO_QUEUE_ALIGN, 4096);

    uint32_t pfn = (uint32_t)((uint64_t)&vq / 4096);
    reg_write32(VIRTIO_QUEUE_PFN, pfn);

    // Driver OK
    status |= 4; reg_write32(VIRTIO_STATUS, status);

    ack_used_idx = 0;
    return 0;
}

extern void uart_puts(const char* s);
extern void print_int(int val);

/**
 * Internal helper to perform a single-sector block operation (Read or Write).
 * Sets up the 3-descriptor chain (Header, Data, Status) and notifies the device.
 * Uses WFI to sleep until the device completes the request.
 * 
 * Parameters:
 *   sector - Target sector index on disk.
 *   buf    - Data buffer in memory.
 *   type   - VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT.
 * 
 * Returns:
 *   0 on success, -1 on failure.
 */
static int virtio_blk_do_op(uint64_t sector, void* buf, uint32_t type) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_lock);
    if (!blk_mmio) {
        spinlock_release_irqrestore(&blk_lock, flags);
        return -1;
    }

    uint16_t desc_idx = 0;

    blk_req.type = type;
    blk_req.reserved = 0;
    blk_req.sector = sector;

    // Descriptor 0: The request header
    vq.desc[0].addr = (uint64_t)&blk_req;
    vq.desc[0].len = sizeof(struct virtio_blk_req);
    vq.desc[0].flags = 1; // VIRTQ_DESC_F_NEXT
    vq.desc[0].next = 1;

    // Descriptor 1: The data buffer
    vq.desc[1].addr = (uint64_t)buf;
    vq.desc[1].len = 512; 
    vq.desc[1].flags = 1 | (type == VIRTIO_BLK_T_IN ? 2 : 0); // VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE
    vq.desc[1].next = 2;

    // Descriptor 2: The status byte
    vq.desc[2].addr = (uint64_t)&blk_status;
    vq.desc[2].len = 1;
    vq.desc[2].flags = 2; // VIRTQ_DESC_F_WRITE
    vq.desc[2].next = 0;

    // Publish to available ring
    uint16_t head_idx = vq.avail.idx;
    vq.avail.ring[head_idx % 8] = 0; // head descriptor is 0
    
    // Memory barrier
    arch_memory_barrier();
    
    vq.avail.idx++;
    
    arch_memory_barrier();

    // Notify device
    reg_write32(VIRTIO_QUEUE_NOTIFY, 0);

    while (*(volatile uint16_t*)&vq.used.idx == ack_used_idx) {
        // spin
    }

    ack_used_idx = vq.used.idx;
    
    // Memory barrier before reading status
    arch_memory_barrier();
    
    // Ack interrupt manually since we disabled IRQs
    uint32_t int_status = reg_read32(VIRTIO_INTERRUPT_STATUS);
    if (int_status) {
        reg_write32(VIRTIO_INTERRUPT_ACK, int_status);
    }

    int res = (blk_status == 0 ? 0 : -1);
    spinlock_release_irqrestore(&blk_lock, flags);
    return res;
}

/**
 * Reads one or more sectors from the block device.
 */
volatile int blk_in_use = 0;

int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count) {
    uint64_t flags;
    while (1) {
        flags = spinlock_acquire_irqsave(&blk_request_lock);
        if (!blk_in_use) {
            blk_in_use = 1;
            spinlock_release_irqrestore(&blk_request_lock, flags);
            break;
        }
        spinlock_release_irqrestore(&blk_request_lock, flags);
        safe_wfi();
    }

    int res = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (virtio_blk_do_op(sector + i, (uint8_t*)buf + (i * 512), VIRTIO_BLK_T_IN) != 0) {
            res = -1;
            break;
        }
    }

    flags = spinlock_acquire_irqsave(&blk_request_lock);
    blk_in_use = 0;
    spinlock_release_irqrestore(&blk_request_lock, flags);
    process_wake_all();
    return res;
}

int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count) {
    uint64_t flags;
    while (1) {
        flags = spinlock_acquire_irqsave(&blk_request_lock);
        if (!blk_in_use) {
            blk_in_use = 1;
            spinlock_release_irqrestore(&blk_request_lock, flags);
            break;
        }
        spinlock_release_irqrestore(&blk_request_lock, flags);
        safe_wfi();
    }

    int res = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (virtio_blk_do_op(sector + i, (void*)((uint8_t*)buf + (i * 512)), VIRTIO_BLK_T_OUT) != 0) {
            res = -1;
            break;
        }
    }

    flags = spinlock_acquire_irqsave(&blk_request_lock);
    blk_in_use = 0;
    spinlock_release_irqrestore(&blk_request_lock, flags);
    process_wake_all();
    return res;
}

#endif

