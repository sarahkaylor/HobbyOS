#include "virtio_input.h"
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

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[64];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[64];
    uint16_t avail_event;
} __attribute__((packed));

struct virtq {
    struct virtq_desc desc[64];
    struct virtq_avail avail;
    uint8_t padding[4096 - (1024 + sizeof(struct virtq_avail))];
    struct virtq_used used;
} __attribute__((aligned(4096)));

#define MAX_INPUT_DEVS 4

struct virtio_input_dev {
    uint8_t* mmio;
    int irq;
    struct virtq vq __attribute__((aligned(4096)));
    struct virtio_input_event events[64];
    uint16_t ack_used_idx;
};

static struct virtio_input_dev input_devs[MAX_INPUT_DEVS];
static int num_input_devs = 0;

// Kernel event ring buffer
#define EVENT_RING_SIZE 256
static struct virtio_input_event event_ring[EVENT_RING_SIZE];
static int ring_head = 0;
static int ring_tail = 0;
static spinlock_t input_lock;

static inline void reg_write32(uint8_t* mmio, uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(mmio + offset) = val;
}

static inline uint32_t reg_read32(uint8_t* mmio, uint32_t offset) {
    return *(volatile uint32_t*)(mmio + offset);
}

void virtio_input_handle_irq(int irq) {
    uint64_t flags = spinlock_acquire_irqsave(&input_lock);
    
    for (int d = 0; d < num_input_devs; d++) {
        struct virtio_input_dev* dev = &input_devs[d];
        if (dev->irq == irq && dev->mmio) {
            uint32_t status = reg_read32(dev->mmio, VIRTIO_INTERRUPT_STATUS);
            if (status) {
                reg_write32(dev->mmio, VIRTIO_INTERRUPT_ACK, status);
                
                // Process the used ring
                while (dev->ack_used_idx != *(volatile uint16_t*)&dev->vq.used.idx) {
                    __asm__ volatile("dmb sy" ::: "memory");
                    
                    uint16_t idx = dev->ack_used_idx % 64;
                    uint32_t id = dev->vq.used.ring[idx].id;
                    
                    // Copy event to the global ring
                    int next_head = (ring_head + 1) % EVENT_RING_SIZE;
                    if (next_head != ring_tail) {
                        event_ring[ring_head] = dev->events[id];
                        ring_head = next_head;
                    }
                    
                    // Re-enqueue the descriptor to receive more events
                    dev->vq.desc[id].addr = (uint64_t)&dev->events[id];
                    dev->vq.desc[id].len = sizeof(struct virtio_input_event);
                    dev->vq.desc[id].flags = 2; // VIRTQ_DESC_F_WRITE
                    
                    uint16_t avail_idx = dev->vq.avail.idx;
                    dev->vq.avail.ring[avail_idx % 64] = id;
                    
                    __asm__ volatile("dmb sy" ::: "memory");
                    dev->vq.avail.idx++;
                    __asm__ volatile("dmb sy" ::: "memory");
                    
                    dev->ack_used_idx++;
                }
                
                reg_write32(dev->mmio, VIRTIO_QUEUE_NOTIFY, 0); // queue 0 is event queue
            }
        }
    }
    
    spinlock_release_irqrestore(&input_lock, flags);
}

int virtio_input_get_events(struct virtio_input_event *buf, int max_events) {
    uint64_t flags = spinlock_acquire_irqsave(&input_lock);
    int count = 0;
    while (ring_tail != ring_head && count < max_events) {
        buf[count] = event_ring[ring_tail];
        ring_tail = (ring_tail + 1) % EVENT_RING_SIZE;
        count++;
    }
    spinlock_release_irqrestore(&input_lock, flags);
    return count;
}

int virtio_input_init(void) {
    spinlock_init(&input_lock);
    num_input_devs = 0;
    
    // Device ID 18 is VirtIO Input
    for (int i = 0; i < 32 && num_input_devs < MAX_INPUT_DEVS; i++) {
        uint8_t* mmio = MMIO_BASE(i);
        uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
        uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
        
        if (magic == 0x74726976 && devid == 18) {
            struct virtio_input_dev* dev = &input_devs[num_input_devs];
            dev->mmio = mmio;
            dev->irq = 48 + i;
            dev->ack_used_idx = 0;
            
            uint32_t status = 0;
            reg_write32(mmio, VIRTIO_STATUS, status);
            status |= 1; reg_write32(mmio, VIRTIO_STATUS, status); // ACKNOWLEDGE
            status |= 2; reg_write32(mmio, VIRTIO_STATUS, status); // DRIVER
            
            reg_write32(mmio, VIRTIO_DRIVER_FEAT_SEL, 1);
            reg_write32(mmio, VIRTIO_DRIVER_FEAT, 1);
            reg_write32(mmio, VIRTIO_DRIVER_FEAT_SEL, 0);
            reg_write32(mmio, VIRTIO_DRIVER_FEAT, 0);
            
            status |= 8; reg_write32(mmio, VIRTIO_STATUS, status); // FEATURES_OK
            if (!(reg_read32(mmio, VIRTIO_STATUS) & 8)) continue;
            
            // Queue 0: Event queue
            reg_write32(mmio, VIRTIO_GUEST_PAGE_SIZE, 4096);
            reg_write32(mmio, VIRTIO_QUEUE_SEL, 0);
            uint32_t max_size = reg_read32(mmio, VIRTIO_QUEUE_NUM_MAX);
            if (max_size == 0) continue;
            
            reg_write32(mmio, VIRTIO_QUEUE_NUM, 64);
            reg_write32(mmio, VIRTIO_QUEUE_ALIGN, 4096);
            reg_write32(mmio, VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&dev->vq / 4096));
            
            // Populate the event queue
            for (int j = 0; j < 64; j++) {
                dev->vq.desc[j].addr = (uint64_t)&dev->events[j];
                dev->vq.desc[j].len = sizeof(struct virtio_input_event);
                dev->vq.desc[j].flags = 2; // VIRTQ_DESC_F_WRITE
                dev->vq.desc[j].next = 0;
                
                dev->vq.avail.ring[j] = j;
            }
            __asm__ volatile("dmb sy" ::: "memory");
            dev->vq.avail.idx = 64;
            __asm__ volatile("dmb sy" ::: "memory");
            
            status |= 4; reg_write32(mmio, VIRTIO_STATUS, status); // DRIVER_OK
            
            // Notify to start receiving events
            reg_write32(mmio, VIRTIO_QUEUE_NOTIFY, 0);
            
            extern void gic_enable_interrupt(uint32_t intid);
            gic_enable_interrupt(dev->irq);
            
            num_input_devs++;
        }
    }
    
    return num_input_devs > 0 ? 0 : -1;
}
