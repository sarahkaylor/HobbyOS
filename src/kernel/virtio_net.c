#include "virtio_net.h"
#include "lock.h"
#include "process.h"

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
#define VIRTIO_CONFIG       0x100

#define MMIO_BASE(slot) ((uint8_t*)0x0A000000 + (slot) * 0x200)

// VirtIO Features
#define VIRTIO_NET_F_MAC (1 << 5)

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

struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

#define NUM_RX_BUFFERS 8
#define RX_BUFFER_SIZE 2048

static struct virtq rx_vq __attribute__((aligned(4096)));
static struct virtq tx_vq __attribute__((aligned(4096)));

static uint8_t rx_buffers[NUM_RX_BUFFERS][RX_BUFFER_SIZE] __attribute__((aligned(4096)));
static struct virtio_net_hdr rx_hdrs[NUM_RX_BUFFERS] __attribute__((aligned(4096)));

static struct virtio_net_hdr tx_hdr __attribute__((aligned(4096)));

static uint8_t* net_mmio = 0;
int virtio_net_irq = -1;
static spinlock_t net_lock;
static spinlock_t net_tx_lock;

static uint16_t rx_ack_used_idx = 0;
static uint16_t tx_ack_used_idx = 0;

static uint8_t local_mac[6];

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);

// Forward declaration for network stack integration
extern void net_rx_packet(uint8_t* packet, uint32_t len);

static inline void reg_write32(uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(net_mmio + offset) = val;
}

static inline uint32_t reg_read32(uint32_t offset) {
    return *(volatile uint32_t*)(net_mmio + offset);
}

static inline void reg_write8(uint32_t offset, uint8_t val) {
    *(volatile uint8_t*)(net_mmio + offset) = val;
}

static inline uint8_t reg_read8(uint32_t offset) {
    return *(volatile uint8_t*)(net_mmio + offset);
}

void virtio_net_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        mac[i] = local_mac[i];
    }
}

static void fill_rx_descriptors(void) {
    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
        // Descriptor 0: header
        rx_vq.desc[i * 2].addr = (uint64_t)&rx_hdrs[i];
        rx_vq.desc[i * 2].len = sizeof(struct virtio_net_hdr);
        rx_vq.desc[i * 2].flags = 3; // NEXT | WRITE
        rx_vq.desc[i * 2].next = i * 2 + 1;

        // Descriptor 1: data buffer
        rx_vq.desc[i * 2 + 1].addr = (uint64_t)rx_buffers[i];
        rx_vq.desc[i * 2 + 1].len = RX_BUFFER_SIZE;
        rx_vq.desc[i * 2 + 1].flags = 2; // WRITE
        rx_vq.desc[i * 2 + 1].next = 0;

        rx_vq.avail.ring[i] = i * 2;
    }
    
    __asm__ volatile("dmb sy" ::: "memory");
    rx_vq.avail.idx = NUM_RX_BUFFERS;
    __asm__ volatile("dmb sy" ::: "memory");
    
    // Notify queue 0 (RX)
    reg_write32(VIRTIO_QUEUE_SEL, 0);
    reg_write32(VIRTIO_QUEUE_NOTIFY, 0);
}

int virtio_net_init(void) {
    // Initialize required spinlocks for queue protection
    spinlock_init(&net_lock);
    spinlock_init(&net_tx_lock);

    // Probe MMIO slots for a valid VirtIO Network device
    for (int i = 0; i < 32; i++) {
        uint8_t* mmio = MMIO_BASE(i);
        uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
        uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
        if (magic == 0x74726976 && devid == 1) { // 1 is network device
            net_mmio = mmio;
            virtio_net_irq = 48 + i;
            break;
        }
    }

    if (!net_mmio) {
        uart_puts("virtio-net device not found!\n");
        return -1;
    }

    uint32_t status = 0;
    
    // Reset device
    reg_write32(VIRTIO_STATUS, status);
    
    // Acknowledge
    status |= 1; reg_write32(VIRTIO_STATUS, status);
    
    // Driver
    status |= 2; reg_write32(VIRTIO_STATUS, status);
    
    // Read features and set VIRTIO_F_VERSION_1 (bit 32) + VIRTIO_NET_F_MAC
    reg_write32(VIRTIO_DRIVER_FEAT_SEL, 1);
    reg_write32(VIRTIO_DRIVER_FEAT, 1); // Set bit 32
    
    reg_write32(VIRTIO_DRIVER_FEAT_SEL, 0);
    reg_write32(VIRTIO_DRIVER_FEAT, VIRTIO_NET_F_MAC);

    // Features OK
    status |= 8; reg_write32(VIRTIO_STATUS, status);
    if (!(reg_read32(VIRTIO_STATUS) & 8)) return -1;

    // Read MAC
    for (int i = 0; i < 6; i++) {
        local_mac[i] = reg_read8(VIRTIO_CONFIG + i);
    }
    
    uart_puts("virtio-net MAC: ");
    for (int i=0; i<6; i++) {
        uart_print_hex(local_mac[i]);
        if (i < 5) uart_puts(":");
    }
    uart_puts("\n");

    // Setup RX queue (0)
    reg_write32(VIRTIO_GUEST_PAGE_SIZE, 4096);
    reg_write32(VIRTIO_QUEUE_SEL, 0);
    uint32_t max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
    if (max_size == 0 || max_size < 16) return -1;
    
    reg_write32(VIRTIO_QUEUE_NUM, 16);
    reg_write32(VIRTIO_QUEUE_ALIGN, 4096);
    reg_write32(VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&rx_vq / 4096));

    // Setup TX queue (1)
    reg_write32(VIRTIO_QUEUE_SEL, 1);
    max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
    if (max_size == 0 || max_size < 16) return -1;
    
    reg_write32(VIRTIO_QUEUE_NUM, 16);
    reg_write32(VIRTIO_QUEUE_ALIGN, 4096);
    reg_write32(VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&tx_vq / 4096));

    // Driver OK
    status |= 4; reg_write32(VIRTIO_STATUS, status);

    fill_rx_descriptors();

    return 0;
}

int virtio_net_send(const void *buf, uint32_t len) {
    uint64_t flags = spinlock_acquire_irqsave(&net_tx_lock);
    if (!net_mmio) {
        spinlock_release_irqrestore(&net_tx_lock, flags);
        return -1;
    }

    for (int i = 0; i < sizeof(struct virtio_net_hdr); i++) {
        ((uint8_t*)&tx_hdr)[i] = 0;
    }

    uint16_t desc_idx = tx_vq.avail.idx % 8 * 2; 

    // Format header descriptor
    tx_vq.desc[desc_idx].addr = (uint64_t)&tx_hdr;
    tx_vq.desc[desc_idx].len = sizeof(struct virtio_net_hdr);
    tx_vq.desc[desc_idx].flags = 1; // NEXT flag: chained to payload
    tx_vq.desc[desc_idx].next = desc_idx + 1;

    // Format payload descriptor
    tx_vq.desc[desc_idx + 1].addr = (uint64_t)buf;
    tx_vq.desc[desc_idx + 1].len = len;
    tx_vq.desc[desc_idx + 1].flags = 0; // End of chain
    tx_vq.desc[desc_idx + 1].next = 0;

    tx_vq.avail.ring[tx_vq.avail.idx % 16] = desc_idx;

    __asm__ volatile("dmb sy" ::: "memory");
    tx_vq.avail.idx++;
    __asm__ volatile("dmb sy" ::: "memory");

    // Dispatch TX queue notification to the device
    reg_write32(VIRTIO_QUEUE_SEL, 1);
    reg_write32(VIRTIO_QUEUE_NOTIFY, 1);

    // Wait synchronously for the device to acknowledge the TX completion.
    // By using WFI (Wait For Interrupt), we avoid busy-polling and save CPU cycles.
    while (*(volatile uint16_t*)&tx_vq.used.idx == tx_ack_used_idx) {
        spinlock_release_irqrestore(&net_tx_lock, flags);
        safe_wfi();
        flags = spinlock_acquire_irqsave(&net_tx_lock);
    }
    tx_ack_used_idx = tx_vq.used.idx;

    __asm__ volatile("dmb sy" ::: "memory");
    spinlock_release_irqrestore(&net_tx_lock, flags);
    return 0;
}

void virtio_net_handle_irq(void) {
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    if (!net_mmio) {
        spinlock_release_irqrestore(&net_lock, flags);
        return;
    }

    uint32_t status = reg_read32(VIRTIO_INTERRUPT_STATUS);
    if (status) {
        reg_write32(VIRTIO_INTERRUPT_ACK, status);
    }

    // Process all pending packets in the RX used ring
    while (rx_ack_used_idx != rx_vq.used.idx) {
        __asm__ volatile("dmb sy" ::: "memory");
        
        uint16_t used_idx = rx_ack_used_idx % 16;
        uint32_t id = rx_vq.used.ring[used_idx].id;
        uint32_t len = rx_vq.used.ring[used_idx].len;
        
        uint32_t buffer_idx = id / 2;
        
        // Push payload to network stack, skipping the virtio_net_hdr
        if (len > sizeof(struct virtio_net_hdr)) {
            uint32_t packet_len = len - sizeof(struct virtio_net_hdr);
            spinlock_release_irqrestore(&net_lock, flags);
            net_rx_packet(rx_buffers[buffer_idx], packet_len);
            flags = spinlock_acquire_irqsave(&net_lock);
        }

        // Re-queue the descriptor
        uint16_t avail_idx = rx_vq.avail.idx % 16;
        rx_vq.avail.ring[avail_idx] = id;
        
        __asm__ volatile("dmb sy" ::: "memory");
        rx_vq.avail.idx++;
        __asm__ volatile("dmb sy" ::: "memory");
        
        rx_ack_used_idx++;
        
        reg_write32(VIRTIO_QUEUE_SEL, 0);
        reg_write32(VIRTIO_QUEUE_NOTIFY, 0);
    }

    spinlock_release_irqrestore(&net_lock, flags);
    process_wake_all();
}
