#include "virtio_input.h"
#include "lock.h"
#include "arch/cpu.h"


#ifdef __x86_64__

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);

#define EVENT_RING_SIZE 256
static struct virtio_input_event event_ring[EVENT_RING_SIZE];
static int ring_head = 0;
static int ring_tail = 0;
static spinlock_t input_lock;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void push_event(uint16_t type, uint16_t code, uint32_t value) {
    int next_head = (ring_head + 1) % EVENT_RING_SIZE;
    if (next_head != ring_tail) {
        event_ring[ring_head].type = type;
        event_ring[ring_head].code = code;
        event_ring[ring_head].value = value;
        ring_head = next_head;
    }
}

static int kbd_escaped = 0;

static void handle_keyboard_byte(uint8_t data) {
    if (data == 0xE0) {
        kbd_escaped = 1;
        return;
    }
    
    uint16_t code = 0;
    uint32_t value = 0;
    
    if (kbd_escaped) {
        kbd_escaped = 0;
        uint8_t base = data & 0x7F;
        int is_release = (data & 0x80) != 0;
        
        if (base == 0x48) code = 103;      // KEY_UP
        else if (base == 0x4B) code = 105; // KEY_LEFT
        else if (base == 0x4D) code = 106; // KEY_RIGHT
        else if (base == 0x50) code = 108; // KEY_DOWN
        
        if (code != 0) {
            value = is_release ? 0 : 1;
        }
    } else {
        int is_release = (data & 0x80) != 0;
        code = data & 0x7F;
        value = is_release ? 0 : 1;
    }
    
    if (code != 0) {
        push_event(EV_KEY, code, value);
        push_event(EV_SYN, 0, 0);
    }
}

static int mouse_cycle = 0;
static uint8_t mouse_packet[3];
static int mouse_x = 16384;
static int mouse_y = 16384;
static int prev_left = 0;
static int prev_right = 0;

static void handle_mouse_byte(uint8_t data) {
    if (mouse_cycle == 0 && !(data & 0x08)) {
        return;
    }
    
    mouse_packet[mouse_cycle++] = data;
    
    if (mouse_cycle == 3) {
        mouse_cycle = 0;
        uint8_t flags = mouse_packet[0];
        int dx = (int)mouse_packet[1];
        int dy = (int)mouse_packet[2];
        
        if (flags & 0x10) dx -= 256;
        if (flags & 0x20) dy -= 256;
        
        mouse_x += dx * 40;
        mouse_y -= dy * 40;
        
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x > 32767) mouse_x = 32767;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y > 32767) mouse_y = 32767;
        
        push_event(EV_ABS, ABS_X, mouse_x);
        push_event(EV_ABS, ABS_Y, mouse_y);
        
        int left = (flags & 0x01) != 0;
        int right = (flags & 0x02) != 0;
        
        if (left != prev_left) {
            push_event(EV_KEY, 0x110, left);
            prev_left = left;
        }
        if (right != prev_right) {
            push_event(EV_KEY, 0x111, right);
            prev_right = right;
        }
        
        push_event(EV_SYN, 0, 0);
    }
}

void virtio_input_handle_irq(int irq) {
    (void)irq;
    uint64_t flags = spinlock_acquire_irqsave(&input_lock);
    
    while (1) {
        uint8_t status = inb(0x64);
        if (!(status & 1)) {
            break;
        }
        uint8_t data = inb(0x60);
        if (status & 0x20) {
            handle_mouse_byte(data);
        } else {
            handle_keyboard_byte(data);
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

static void ps2_wait_write(void) {
    while (inb(0x64) & 2);
}

static void ps2_wait_read(void) {
    while (!(inb(0x64) & 1));
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(0x64, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(0x60, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(0x60);
}

static void ps2_write_mouse(uint8_t data) {
    ps2_write_cmd(0xD4);
    ps2_write_data(data);
    uint8_t ack = ps2_read_data();
    (void)ack;
}

int virtio_input_init(void) {
    spinlock_init(&input_lock);
    
    // Enable mouse
    ps2_write_cmd(0xA8);
    // Enable keyboard
    ps2_write_cmd(0xAE);
    
    // Read Controller Command Byte
    ps2_write_cmd(0x20);
    uint8_t ccb = ps2_read_data();
    uart_puts("Original CCB: ");
    uart_print_hex(ccb);
    uart_puts("\n");
    
    // Enable interrupts for keyboard (bit 0) and mouse (bit 1)
    ccb |= 0x03;
    // Clear disable flags for keyboard (bit 4) and mouse (bit 5)
    ccb &= ~0x30;
    
    uart_puts("Modified CCB: ");
    uart_print_hex(ccb);
    uart_puts("\n");
    
    ps2_write_cmd(0x60);
    ps2_write_data(ccb);
    
    // Configure mouse
    ps2_write_mouse(0xF6); // Set defaults
    ps2_write_mouse(0xF4); // Enable data reporting
    
    extern void gic_enable_interrupt(uint32_t intid);
    gic_enable_interrupt(33); // Keyboard IRQ 1
    gic_enable_interrupt(44); // Mouse IRQ 12
    
    return 0;
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
                    arch_memory_barrier();
                    
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
                    
                    arch_memory_barrier();
                    dev->vq.avail.idx++;
                    arch_memory_barrier();
                    
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
            arch_memory_barrier();
            dev->vq.avail.idx = 64;
            arch_memory_barrier();
            
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

#endif

