#include "virtio_blk.h"

// VirtIO MMIO offsets
#define VIRTIO_MAGIC        0x000
#define VIRTIO_VERSION      0x004
#define VIRTIO_DEVICE_ID    0x008
#define VIRTIO_VENDOR_ID    0x00C
#define VIRTIO_DEVICE_FEAT  0x010
#define VIRTIO_DEVICE_FEAT_SEL 0x014
#define VIRTIO_DRIVER_FEAT  0x020
#define VIRTIO_DRIVER_FEAT_SEL 0x024
#define VIRTIO_QUEUE_SEL    0x030
#define VIRTIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_QUEUE_NUM    0x038
#define VIRTIO_QUEUE_READY  0x044
#define VIRTIO_QUEUE_NOTIFY 0x050
#define VIRTIO_STATUS       0x070
#define VIRTIO_QUEUE_DESC_L 0x080
#define VIRTIO_QUEUE_DESC_H 0x084
#define VIRTIO_QUEUE_DRV_L  0x090
#define VIRTIO_QUEUE_DRV_H  0x094
#define VIRTIO_QUEUE_DEV_L  0x0A0
#define VIRTIO_QUEUE_DEV_H  0x0A4

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

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

// Statically allocate virtqueue and request buffers to keep it simple
static struct virtq_desc desc[8] __attribute__((aligned(16)));
static struct virtq_avail avail __attribute__((aligned(2)));
static struct virtq_used used __attribute__((aligned(4)));

static struct virtio_blk_req blk_req;
static uint8_t blk_status;

static uint8_t* blk_mmio = 0;
static uint16_t ack_used_idx = 0;

static inline void reg_write32(uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(blk_mmio + offset) = val;
}

static inline uint32_t reg_read32(uint32_t offset) {
    return *(volatile uint32_t*)(blk_mmio + offset);
}

int virtio_blk_init(void) {
    // Scan for virtio block device (ID 2)
    for (int i = 0; i < 32; i++) {
        uint8_t* mmio = MMIO_BASE(i);
        uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
        uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
        if (magic == 0x74726976 && devid == 2) {
            blk_mmio = mmio;
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
    reg_write32(VIRTIO_QUEUE_SEL, 0);
    uint32_t max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
    if (max_size == 0) return -1;
    
    reg_write32(VIRTIO_QUEUE_NUM, 8);

    uint64_t desc_addr = (uint64_t)&desc;
    uint64_t avail_addr = (uint64_t)&avail;
    uint64_t used_addr = (uint64_t)&used;

    reg_write32(VIRTIO_QUEUE_DESC_L, (uint32_t)desc_addr);
    reg_write32(VIRTIO_QUEUE_DESC_H, (uint32_t)(desc_addr >> 32));
    reg_write32(VIRTIO_QUEUE_DRV_L, (uint32_t)avail_addr);
    reg_write32(VIRTIO_QUEUE_DRV_H, (uint32_t)(avail_addr >> 32));
    reg_write32(VIRTIO_QUEUE_DEV_L, (uint32_t)used_addr);
    reg_write32(VIRTIO_QUEUE_DEV_H, (uint32_t)(used_addr >> 32));

    reg_write32(VIRTIO_QUEUE_READY, 1);

    // Driver OK
    status |= 4; reg_write32(VIRTIO_STATUS, status);

    ack_used_idx = 0;
    return 0;
}

extern void uart_puts(const char* s);
extern void print_int(int val);

static int virtio_blk_do_op(uint64_t sector, void* buf, uint32_t type) {
    if (!blk_mmio) return -1;

    blk_req.type = type;
    blk_req.reserved = 0;
    blk_req.sector = sector;

    // Descriptor 0: The request header
    desc[0].addr = (uint64_t)&blk_req;
    desc[0].len = sizeof(struct virtio_blk_req);
    desc[0].flags = 1; // VIRTQ_DESC_F_NEXT
    desc[0].next = 1;

    // Descriptor 1: The data buffer
    desc[1].addr = (uint64_t)buf;
    desc[1].len = 512; 
    desc[1].flags = 1 | (type == VIRTIO_BLK_T_IN ? 2 : 0); // VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE
    desc[1].next = 2;

    // Descriptor 2: The status byte
    desc[2].addr = (uint64_t)&blk_status;
    desc[2].len = 1;
    desc[2].flags = 2; // VIRTQ_DESC_F_WRITE
    desc[2].next = 0;

    // Publish to available ring
    uint16_t head_idx = avail.idx;
    avail.ring[head_idx % 8] = 0; // head descriptor is 0
    
    // Memory barrier
    __asm__ volatile("dmb sy" ::: "memory");
    
    avail.idx++;
    
    __asm__ volatile("dmb sy" ::: "memory");

    uart_puts("Sending notify to QEMU.\n");

    // Notify device
    reg_write32(VIRTIO_QUEUE_NOTIFY, 0);

    // Poll for completion with timeout
    uint32_t timeout = 10000000;
    while (*(volatile uint16_t*)&used.idx == ack_used_idx && timeout > 0) {
        timeout--;
        __asm__ volatile("nop");
    }

    if (timeout == 0) {
        uart_puts("QEMU virtio timeout!\n");
        return -1;
    }

    uart_puts("QEMU responded.\n");

    ack_used_idx = used.idx;
    
    // Memory barrier before reading status
    __asm__ volatile("dmb sy" ::: "memory");

    return blk_status == 0 ? 0 : -1;
}

int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (virtio_blk_do_op(sector + i, (uint8_t*)buf + (i * 512), VIRTIO_BLK_T_IN) != 0) {
            return -1;
        }
    }
    return 0;
}

int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        // Warning: virtio_blk_do_op incorrectly strips const mathematically, but it's okay for virtio output
        if (virtio_blk_do_op(sector + i, (void*)((uint8_t*)buf + (i * 512)), VIRTIO_BLK_T_OUT) != 0) {
            return -1;
        }
    }
    return 0;
}
