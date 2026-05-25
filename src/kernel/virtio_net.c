#include "virtio_net.h"
#include "lock.h"
#include "process.h"
#include "arch/cpu.h"

#define NUM_RX_BUFFERS 8
#define RX_BUFFER_SIZE 2048

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

#ifdef __x86_64__

#include "net.h"

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);

// Legacy VirtIO PCI Offsets
#define VIRTIO_PCI_HOST_FEATURES  0x00 // 32-bit R
#define VIRTIO_PCI_GUEST_FEATURES 0x04 // 32-bit RW
#define VIRTIO_PCI_QUEUE_PFN      0x08 // 32-bit RW
#define VIRTIO_PCI_QUEUE_NUM      0x0C // 16-bit R
#define VIRTIO_PCI_QUEUE_SEL      0x0E // 16-bit RW
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10 // 16-bit RW
#define VIRTIO_PCI_STATUS         0x12 // 8-bit RW
#define VIRTIO_PCI_ISR            0x13 // 8-bit R
#define VIRTIO_PCI_CONFIG_OFF     0x14 // Config space (MAC)

// PCI specific VirtIO 256-Descriptor rings
struct virtq_avail_pci {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_pci {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[256];
    uint16_t avail_event;
} __attribute__((packed));

struct virtq_pci {
    struct virtq_desc desc[256];
    struct virtq_avail_pci avail;
    uint8_t padding[8192 - (4096 + sizeof(struct virtq_avail_pci))];
    struct virtq_used_pci used;
} __attribute__((aligned(4096)));

struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

static spinlock_t net_lock;
static spinlock_t net_tx_lock;
int virtio_net_irq = -1;
static uint8_t local_mac[6];
static uint16_t io_base = 0;

static struct virtq_pci rx_vq __attribute__((aligned(4096)));
static struct virtq_pci tx_vq __attribute__((aligned(4096)));

static uint8_t rx_buffers[NUM_RX_BUFFERS][RX_BUFFER_SIZE] __attribute__((aligned(4096)));
static struct virtio_net_hdr rx_hdrs[NUM_RX_BUFFERS] __attribute__((aligned(4096)));
static struct virtio_net_hdr tx_hdr __attribute__((aligned(4096)));

static uint16_t rx_ack_used_idx = 0;
static uint16_t tx_ack_used_idx = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
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

void virtio_net_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++) mac[i] = local_mac[i];
}

int virtio_net_init(void) {
    spinlock_init(&net_lock);
    spinlock_init(&net_tx_lock);

    // Scan PCI bus for Legacy VirtIO Network device (Vendor 0x1AF4, Device 0x1000)
    int found = 0;
    uint8_t net_bus = 0, net_slot = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read_config(bus, slot, 0, 0);
            if ((id & 0xFFFF) != 0xFFFF) {
                uint16_t vendor = id & 0xFFFF;
                uint16_t device = (id >> 16) & 0xFFFF;
                if (vendor == 0x1AF4 && device == 0x1000) {
                    net_bus = bus;
                    net_slot = slot;
                    found = 1;
                    break;
                }
            }
        }
        if (found) break;
    }

    if (!found) {
        uart_puts("[VIRTIO_NET] Legacy VirtIO network device not found!\n");
        return -1;
    }

    // Read BAR0 (I/O Port Base)
    uint32_t bar0 = pci_read_config(net_bus, net_slot, 0, 0x10);
    if (!(bar0 & 1)) {
        uart_puts("[VIRTIO_NET] Error: BAR0 is not I/O space!\n");
        return -1;
    }
    io_base = bar0 & ~1;

    // Enable PCI Bus Mastering and I/O Space
    uint32_t cmd = pci_read_config(net_bus, net_slot, 0, 0x04);
    pci_write_config(net_bus, net_slot, 0, 0x04, cmd | 0x05); // Enable I/O Space (0x01) and Bus Master (0x04)

    // Reset VirtIO device
    outb(io_base + VIRTIO_PCI_STATUS, 0);
    
    // Set ACK & DRIVER status
    outb(io_base + VIRTIO_PCI_STATUS, 1 | 2);

    // Read host features and negotiate MAC (bit 5)
    uint32_t host_features = inl(io_base + VIRTIO_PCI_HOST_FEATURES);
    outl(io_base + VIRTIO_PCI_GUEST_FEATURES, host_features & (1 << 5));

    // Read MAC address from CONFIG space
    for (int i = 0; i < 6; i++) {
        local_mac[i] = inb(io_base + VIRTIO_PCI_CONFIG_OFF + i);
    }

    uart_puts("[VIRTIO_NET] MAC Address: ");
    for (int i = 0; i < 6; i++) {
        uart_print_hex(local_mac[i]);
        if (i < 5) uart_puts(":");
    }
    uart_puts("\n");

    // Setup RX queue (0)
    outw(io_base + VIRTIO_PCI_QUEUE_SEL, 0);
    uint16_t rx_qsize = inw(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (rx_qsize == 0) return -1;
    outl(io_base + VIRTIO_PCI_QUEUE_PFN, (uint32_t)((uint64_t)&rx_vq / 4096));

    // Setup TX queue (1)
    outw(io_base + VIRTIO_PCI_QUEUE_SEL, 1);
    uint16_t tx_qsize = inw(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (tx_qsize == 0) return -1;
    outl(io_base + VIRTIO_PCI_QUEUE_PFN, (uint32_t)((uint64_t)&tx_vq / 4096));

    // Set DRIVER_OK status
    outb(io_base + VIRTIO_PCI_STATUS, 1 | 2 | 4);

    // Populate RX descriptors
    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
        // Descriptor 0: header
        rx_vq.desc[i * 2].addr = (uint64_t)&rx_hdrs[i];
        rx_vq.desc[i * 2].len = sizeof(struct virtio_net_hdr);
        rx_vq.desc[i * 2].flags = 3; // NEXT | WRITE
        rx_vq.desc[i * 2].next = i * 2 + 1;

        // Descriptor 1: buffer
        rx_vq.desc[i * 2 + 1].addr = (uint64_t)rx_buffers[i];
        rx_vq.desc[i * 2 + 1].len = RX_BUFFER_SIZE;
        rx_vq.desc[i * 2 + 1].flags = 2; // WRITE
        rx_vq.desc[i * 2 + 1].next = 0;

        rx_vq.avail.ring[i] = i * 2;
    }

    arch_memory_barrier();
    rx_vq.avail.idx = NUM_RX_BUFFERS;
    arch_memory_barrier();

    // Notify queue 0 (RX)
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    return 0;
}

int virtio_net_send(const void *buf, uint32_t len) {
    uart_puts("[VIRTIO_NET] Sending packet, len=");
    print_int(len);
    uart_puts("...\n");

    uint64_t flags = spinlock_acquire_irqsave(&net_tx_lock);
    if (io_base == 0) {
        uart_puts("[VIRTIO_NET] Error: io_base is 0!\n");
        spinlock_release_irqrestore(&net_tx_lock, flags);
        return -1;
    }

    // Format legacy TX header
    for (int i = 0; i < (int)sizeof(struct virtio_net_hdr); i++) {
        ((uint8_t*)&tx_hdr)[i] = 0;
    }

    // Wrap at 256 (TX queue capacity)
    uint16_t desc_idx = (tx_vq.avail.idx % 128) * 2; 

    // Header descriptor
    tx_vq.desc[desc_idx].addr = (uint64_t)&tx_hdr;
    tx_vq.desc[desc_idx].len = sizeof(struct virtio_net_hdr);
    tx_vq.desc[desc_idx].flags = 1; // NEXT
    tx_vq.desc[desc_idx].next = desc_idx + 1;

    // Payload descriptor
    tx_vq.desc[desc_idx + 1].addr = (uint64_t)buf;
    tx_vq.desc[desc_idx + 1].len = len;
    tx_vq.desc[desc_idx + 1].flags = 0; // End of chain
    tx_vq.desc[desc_idx + 1].next = 0;

    tx_vq.avail.ring[tx_vq.avail.idx % 256] = desc_idx;

    arch_memory_barrier();
    tx_vq.avail.idx++;
    arch_memory_barrier();

    uart_puts("[VIRTIO_NET] Notifying device of TX...\n");
    // Trigger Queue Notify for TX (Queue 1)
    outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, 1);

    uart_puts("[VIRTIO_NET] Waiting for TX ACK...\n");
    // Poll synchronously for completion
    while (*(volatile uint16_t*)&tx_vq.used.idx == tx_ack_used_idx) {
        __asm__ volatile("pause");
    }
    tx_ack_used_idx = tx_vq.used.idx;
    uart_puts("[VIRTIO_NET] TX ACK received!\n");

    arch_memory_barrier();
    spinlock_release_irqrestore(&net_tx_lock, flags);
    return 0;
}

void virtio_net_handle_irq(void) {
    if (io_base == 0) return;
    
    // Read and clear legacy ISR status
    uint8_t isr = inb(io_base + VIRTIO_PCI_ISR);
    if (!isr) return;
    
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    
    // Process all pending packets in the RX used ring
    while (rx_ack_used_idx != rx_vq.used.idx) {
        arch_memory_barrier();
        
        uint16_t used_idx = rx_ack_used_idx % 256;
        uint32_t id = rx_vq.used.ring[used_idx].id;
        uint32_t len = rx_vq.used.ring[used_idx].len;
        
        uint32_t buffer_idx = id / 2;
        
        if (len > sizeof(struct virtio_net_hdr)) {
            uint32_t packet_len = len - sizeof(struct virtio_net_hdr);
            spinlock_release_irqrestore(&net_lock, flags);
            net_rx_packet(rx_buffers[buffer_idx], packet_len);
            flags = spinlock_acquire_irqsave(&net_lock);
        }

        // Re-queue the descriptor
        uint16_t avail_idx = rx_vq.avail.idx % 256;
        rx_vq.avail.ring[avail_idx] = id;
        
        arch_memory_barrier();
        rx_vq.avail.idx++;
        arch_memory_barrier();
        
        rx_ack_used_idx++;
        
        // Notify device RX queue is filled
        outw(io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);
    }
    
    spinlock_release_irqrestore(&net_lock, flags);
}

int virtio_net_is_active(void) {
    return io_base != 0;
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
#define VIRTIO_CONFIG       0x100

#define MMIO_BASE(slot) ((uint8_t*)0x0A000000 + (slot) * 0x200)

// VirtIO Features
#define VIRTIO_NET_F_MAC (1 << 5)

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[16];
    uint16_t used_event;
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
        rx_vq.desc[i * 2].addr = (uint64_t)&rx_hdrs[i];
        rx_vq.desc[i * 2].len = sizeof(struct virtio_net_hdr);
        rx_vq.desc[i * 2].flags = 3; // NEXT | WRITE
        rx_vq.desc[i * 2].next = i * 2 + 1;

        rx_vq.desc[i * 2 + 1].addr = (uint64_t)rx_buffers[i];
        rx_vq.desc[i * 2 + 1].len = RX_BUFFER_SIZE;
        rx_vq.desc[i * 2 + 1].flags = 2; // WRITE
        rx_vq.desc[i * 2 + 1].next = 0;

        rx_vq.avail.ring[i] = i * 2;
    }
    
    arch_memory_barrier();
    rx_vq.avail.idx = NUM_RX_BUFFERS;
    arch_memory_barrier();
    
    reg_write32(VIRTIO_QUEUE_SEL, 0);
    reg_write32(VIRTIO_QUEUE_NOTIFY, 0);
}

int virtio_net_init(void) {
    spinlock_init(&net_lock);
    spinlock_init(&net_tx_lock);

    for (int i = 0; i < 32; i++) {
        uint8_t* mmio = MMIO_BASE(i);
        uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
        uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
        if (magic == 0x74726976 && devid == 1) { 
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
    reg_write32(VIRTIO_STATUS, status);
    
    status |= 1; reg_write32(VIRTIO_STATUS, status);
    status |= 2; reg_write32(VIRTIO_STATUS, status);
    
    reg_write32(VIRTIO_DRIVER_FEAT_SEL, 1);
    reg_write32(VIRTIO_DRIVER_FEAT, 1); 
    
    reg_write32(VIRTIO_DRIVER_FEAT_SEL, 0);
    reg_write32(VIRTIO_DRIVER_FEAT, VIRTIO_NET_F_MAC);

    status |= 8; reg_write32(VIRTIO_STATUS, status);
    if (!(reg_read32(VIRTIO_STATUS) & 8)) return -1;

    for (int i = 0; i < 6; i++) {
        local_mac[i] = reg_read8(VIRTIO_CONFIG + i);
    }
    
    uart_puts("virtio-net MAC: ");
    for (int i=0; i<6; i++) {
        uart_print_hex(local_mac[i]);
        if (i < 5) uart_puts(":");
    }
    uart_puts("\n");

    reg_write32(VIRTIO_GUEST_PAGE_SIZE, 4096);
    reg_write32(VIRTIO_QUEUE_SEL, 0);
    uint32_t max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
    if (max_size == 0 || max_size < 16) return -1;
    
    reg_write32(VIRTIO_QUEUE_NUM, 16);
    reg_write32(VIRTIO_QUEUE_ALIGN, 4096);
    reg_write32(VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&rx_vq / 4096));

    reg_write32(VIRTIO_QUEUE_SEL, 1);
    max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
    if (max_size == 0 || max_size < 16) return -1;
    
    reg_write32(VIRTIO_QUEUE_NUM, 16);
    reg_write32(VIRTIO_QUEUE_ALIGN, 4096);
    reg_write32(VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&tx_vq / 4096));

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

    for (int i = 0; i < (int)sizeof(struct virtio_net_hdr); i++) {
        ((uint8_t*)&tx_hdr)[i] = 0;
    }

    uint16_t desc_idx = (tx_vq.avail.idx % 8) * 2; 

    tx_vq.desc[desc_idx].addr = (uint64_t)&tx_hdr;
    tx_vq.desc[desc_idx].len = sizeof(struct virtio_net_hdr);
    tx_vq.desc[desc_idx].flags = 1; 
    tx_vq.desc[desc_idx].next = desc_idx + 1;

    tx_vq.desc[desc_idx + 1].addr = (uint64_t)buf;
    tx_vq.desc[desc_idx + 1].len = len;
    tx_vq.desc[desc_idx + 1].flags = 0; 
    tx_vq.desc[desc_idx + 1].next = 0;

    tx_vq.avail.ring[tx_vq.avail.idx % 16] = desc_idx;

    arch_memory_barrier();
    tx_vq.avail.idx++;
    arch_memory_barrier();

    uart_puts("[VIRTIO_NET] TX: tx_vq=");
    uart_print_hex((uint64_t)&tx_vq);
    uart_puts(" tx_hdr=");
    uart_print_hex((uint64_t)&tx_hdr);
    uart_puts(" buf=");
    uart_print_hex((uint64_t)buf);
    uart_puts(" idx=");
    print_int(tx_vq.avail.idx);
    uart_puts(" ack=");
    print_int(tx_ack_used_idx);
    uart_puts("\n");

    reg_write32(VIRTIO_QUEUE_SEL, 1);
    reg_write32(VIRTIO_QUEUE_NOTIFY, 1);

    while (*(volatile uint16_t*)&tx_vq.used.idx == tx_ack_used_idx) {
        spinlock_release_irqrestore(&net_tx_lock, flags);
        safe_wfi();
        flags = spinlock_acquire_irqsave(&net_tx_lock);
    }
    tx_ack_used_idx = tx_vq.used.idx;

    arch_memory_barrier();
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

    while (rx_ack_used_idx != rx_vq.used.idx) {
        arch_memory_barrier();
        
        uint16_t used_idx = rx_ack_used_idx % 16;
        uint32_t id = rx_vq.used.ring[used_idx].id;
        uint32_t len = rx_vq.used.ring[used_idx].len;
        
        uint32_t buffer_idx = id / 2;
        
        if (len > sizeof(struct virtio_net_hdr)) {
            uint32_t packet_len = len - sizeof(struct virtio_net_hdr);
            spinlock_release_irqrestore(&net_lock, flags);
            net_rx_packet(rx_buffers[buffer_idx], packet_len);
            flags = spinlock_acquire_irqsave(&net_lock);
        }

        uint16_t avail_idx = rx_vq.avail.idx % 16;
        rx_vq.avail.ring[avail_idx] = id;
        
        arch_memory_barrier();
        rx_vq.avail.idx++;
        arch_memory_barrier();
        
        rx_ack_used_idx++;
        
        reg_write32(VIRTIO_QUEUE_SEL, 0);
        reg_write32(VIRTIO_QUEUE_NOTIFY, 0);
    }

    spinlock_release_irqrestore(&net_lock, flags);
    process_wake_all();
}

int virtio_net_is_active(void) {
    return net_mmio != NULL;
}

#endif
