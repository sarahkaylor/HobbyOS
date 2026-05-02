#include "virtio_blk.h"
#include "lock.h"

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
    __asm__ volatile("dmb sy" ::: "memory");
    
    vq.avail.idx++;
    
    __asm__ volatile("dmb sy" ::: "memory");

    // Notify device
    reg_write32(VIRTIO_QUEUE_NOTIFY, 0);

    // Hardware WFI Mechanism:
    // To strictly eliminate busy-polling queues which aggressively spin the CPU,
    // the Wait For Interrupt (`wfi`) instruction is evoked. Because PSTATE.I is cleared
    // down in EL1, the processor enters a genuine low-power sleep.
    // It wakes autonomously only when the GIC validates the designated interrupt line 
    // raised by the virtio device once IO completes and edits `used.idx`.
    while (*(volatile uint16_t*)&vq.used.idx == ack_used_idx) {
        // Release lock while waiting to allow other CPUs to process interrupts
        spinlock_release_irqrestore(&blk_lock, flags);
        __asm__ volatile("wfi");
        flags = spinlock_acquire_irqsave(&blk_lock);
    }

    ack_used_idx = vq.used.idx;
    
    // Memory barrier before reading status
    __asm__ volatile("dmb sy" ::: "memory");

    int res = (blk_status == 0 ? 0 : -1);
    spinlock_release_irqrestore(&blk_lock, flags);
    return res;
}

/**
 * Reads one or more sectors from the block device.
 */
int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_request_lock);
    for (uint32_t i = 0; i < count; i++) {
        if (virtio_blk_do_op(sector + i, (uint8_t*)buf + (i * 512), VIRTIO_BLK_T_IN) != 0) {
            spinlock_release_irqrestore(&blk_request_lock, flags);
            return -1;
        }
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    return 0;
}

/**
 * Writes one or more sectors to the block device.
 */
int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_request_lock);
    for (uint32_t i = 0; i < count; i++) {
        // Warning: virtio_blk_do_op incorrectly strips const mathematically, but it's okay for virtio output
        if (virtio_blk_do_op(sector + i, (void*)((uint8_t*)buf + (i * 512)), VIRTIO_BLK_T_OUT) != 0) {
            spinlock_release_irqrestore(&blk_request_lock, flags);
            return -1;
        }
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    return 0;
}
