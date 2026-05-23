#include "net_rdma.h"
#include "net.h"
#include "lock.h"
#include "process.h"
#include "timer.h"
#include "arch/cpu.h"

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);

// Global State
int is_host = 0;
struct socket_pcb* rdma_socket = NULL;
static spinlock_t rdma_lock;

// Dynamic RDMA Configuration (Startup Parameters)
uint16_t g_rdma_vendor_id = 0;
uint16_t g_rdma_device_id = 0;
int g_rdma_active = 0;


// Host specific hardware pointers
static volatile uint32_t* p_edu_regs = NULL;
static uint64_t edu_regs_phys = 0;

// Host Memory Registration Table
#define MAX_MRS 8
struct host_mr_entry {
    uint64_t guest_phys;
    uint64_t host_phys;
    uint8_t* host_virt;
    uint32_t size;
    int in_use;
};
static struct host_mr_entry host_mrs[MAX_MRS];
static uint8_t host_dma_buffers[MAX_MRS][4096] __attribute__((aligned(4096)));

// Guest specific state
struct guest_mr_entry {
    uint64_t guest_phys;
    uint64_t host_phys;
    uint32_t size;
    int in_use;
};
static struct guest_mr_entry guest_mrs[MAX_MRS];

static volatile struct rdma_packet guest_rx_packet;
static volatile int guest_rx_ready = 0;
static uint32_t next_tx_id = 1;

// Standard PCI access
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

// Host packet handler
static void handle_host_rdma(struct rdma_packet* pkt) {
    struct rdma_packet resp;
    resp.op = pkt->op + 1; // Standard response Opcode (Op + 1)
    resp.tx_id = pkt->tx_id;
    resp.addr = pkt->addr;
    resp.len = pkt->len;
    resp.status = 0;

    if (pkt->op == RDMA_OP_READ_REQ) {
        if (p_edu_regs) {
            if (pkt->len == 4) {
                uint32_t val = p_edu_regs[pkt->addr / 4];
                *(uint32_t*)resp.data = val;
            } else if (pkt->len == 8) {
                uint64_t val = *(volatile uint64_t*)((uint8_t*)p_edu_regs + pkt->addr);
                *(uint64_t*)resp.data = val;
            } else {
                resp.status = 1;
            }
        } else {
            resp.status = 2; // Device not mapped
        }
    } 
    else if (pkt->op == RDMA_OP_WRITE_REQ) {
        if (p_edu_regs) {
            if (pkt->len == 4) {
                uint32_t val = *(uint32_t*)pkt->data;
                p_edu_regs[pkt->addr / 4] = val;
            } else if (pkt->len == 8) {
                uint64_t val = *(uint64_t*)pkt->data;
                *(volatile uint64_t*)((uint8_t*)p_edu_regs + pkt->addr) = val;
            } else {
                resp.status = 1;
            }
        } else {
            resp.status = 2;
        }
    } 
    else if (pkt->op == RDMA_OP_REG_MR) {
        int idx = -1;
        for (int i = 0; i < MAX_MRS; i++) {
            if (host_mrs[i].in_use && host_mrs[i].guest_phys == pkt->addr) {
                idx = i;
                break;
            }
        }
        if (idx == -1) {
            for (int i = 0; i < MAX_MRS; i++) {
                if (!host_mrs[i].in_use) {
                    host_mrs[i].guest_phys = pkt->addr;
                    host_mrs[i].host_phys = (uint64_t)&host_dma_buffers[i][0];
                    host_mrs[i].host_virt = &host_dma_buffers[i][0];
                    host_mrs[i].size = pkt->len;
                    host_mrs[i].in_use = 1;
                    idx = i;
                    break;
                }
            }
        }
        if (idx != -1) {
            resp.addr = host_mrs[idx].host_phys; // Return Host shadow physical address
            resp.status = 0;
        } else {
            resp.status = 3; // Out of MRs
        }
    } 
    else if (pkt->op == RDMA_OP_DMA_SYNC_TO_HOST) {
        int found = 0;
        for (int i = 0; i < MAX_MRS; i++) {
            if (host_mrs[i].in_use && host_mrs[i].guest_phys == pkt->addr) {
                for (uint32_t j = 0; j < pkt->len; j++) {
                    host_mrs[i].host_virt[j] = pkt->data[j];
                }
                found = 1;
                break;
            }
        }
        resp.op = RDMA_OP_DMA_SYNC_RESP;
        resp.status = found ? 0 : 4;
    } 
    else if (pkt->op == RDMA_OP_DMA_SYNC_TO_GUEST) {
        int found = 0;
        for (int i = 0; i < MAX_MRS; i++) {
            if (host_mrs[i].in_use && host_mrs[i].guest_phys == pkt->addr) {
                for (uint32_t j = 0; j < pkt->len; j++) {
                    resp.data[j] = host_mrs[i].host_virt[j];
                }
                found = 1;
                break;
            }
        }
        resp.op = RDMA_OP_DMA_SYNC_RESP;
        resp.status = found ? 0 : 4;
    }

    net_socket_send(rdma_socket, &resp, sizeof(struct rdma_packet));
}

// Guest response handler
static void handle_guest_rdma(struct rdma_packet* pkt) {
    guest_rx_packet = *pkt;
    guest_rx_ready = 1;
}

// Shared network polling loop
void net_rdma_poll(void) {
    if (!rdma_socket) return;
    if (rdma_socket->rx_head == rdma_socket->rx_tail) return;

    uint32_t avail = rdma_socket->rx_tail - rdma_socket->rx_head;
    if (avail < sizeof(struct rdma_packet)) return;

    struct rdma_packet pkt;
    uint64_t flags = spinlock_acquire_irqsave(&rdma_lock);
    uint8_t* dest = (uint8_t*)&pkt;
    for (uint32_t i = 0; i < sizeof(struct rdma_packet); i++) {
        dest[i] = rdma_socket->rx_buf[rdma_socket->rx_head % 2048];
        rdma_socket->rx_head++;
    }
    spinlock_release_irqrestore(&rdma_lock, flags);

    // Save remote parameters for Host reply routing
    if (is_host) {
        rdma_socket->remote_ip = htonl(RDMA_GUEST_IP);
        rdma_socket->remote_port = 49152;
        handle_host_rdma(&pkt);
    } else {
        handle_guest_rdma(&pkt);
    }
}

// Host Provider Listening Loop
static void provider_loop(void *arg) {
    (void)arg;
    uart_puts("[RDMA] Host Provider Thread started!\n");
    while (1) {
        net_rdma_poll();
        extern void virtio_net_handle_irq(void);
        virtio_net_handle_irq();
        __asm__ volatile("pause");
    }
}

// QEMU fw_cfg Port I/O Helpers
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Reads a file from QEMU fw_cfg by name via Port I/O
static int fw_cfg_read_file(const char* target_name, void* buf, int max_len) {
    // 1. Verify signature
    outw(0x510, 0x0000);
    char sig[4];
    sig[0] = inb(0x511);
    sig[1] = inb(0x511);
    sig[2] = inb(0x511);
    sig[3] = inb(0x511);
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        return -1;
    }

    // 2. Read number of directory entries (big-endian)
    outw(0x510, 0x0019);
    uint32_t count = 0;
    count |= ((uint32_t)inb(0x511)) << 24;
    count |= ((uint32_t)inb(0x511)) << 16;
    count |= ((uint32_t)inb(0x511)) << 8;
    count |= ((uint32_t)inb(0x511));

    // 3. Scan directory entries
    for (uint32_t f = 0; f < count; f++) {
        uint32_t size = 0;
        size |= ((uint32_t)inb(0x511)) << 24;
        size |= ((uint32_t)inb(0x511)) << 16;
        size |= ((uint32_t)inb(0x511)) << 8;
        size |= ((uint32_t)inb(0x511));

        uint16_t select = 0;
        select |= ((uint16_t)inb(0x511)) << 8;
        select |= ((uint16_t)inb(0x511));

        inb(0x511); // reserved
        inb(0x511); // reserved

        char name[56];
        for (int i = 0; i < 56; i++) {
            name[i] = inb(0x511);
        }
        name[55] = '\0';

        // Match name
        int match = 1;
        for (int i = 0; target_name[i] != '\0' || name[i] != '\0'; i++) {
            if (target_name[i] != name[i]) {
                match = 0;
                break;
            }
        }

        if (match) {
            // Write select key to selector port
            outw(0x510, select);
            int read_len = (size < (uint32_t)max_len) ? (int)size : max_len;
            uint8_t* ptr = (uint8_t*)buf;
            for (int i = 0; i < read_len; i++) {
                ptr[i] = inb(0x511);
            }
            return read_len;
        }
    }
    return -1;
}

// Parses hexadecimal strings like "0x1234"
static uint32_t parse_hex(const char* s, int* chars_read) {
    uint32_t val = 0;
    int i = 0;
    if (s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) {
        i += 2;
    }
    while (1) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            val = (val << 4) + (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val = (val << 4) + (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val = (val << 4) + (c - 'A' + 10);
        } else {
            break;
        }
        i++;
    }
    if (chars_read) *chars_read = i;
    return val;
}

// Parses string like "host:0x1234:0x11e8"
static int parse_rdma_config(const char* str) {
    int idx = 0;
    if (str[0] == 'h' && str[1] == 'o' && str[2] == 's' && str[3] == 't') {
        is_host = 1;
        idx = 4;
    } else if (str[0] == 'g' && str[1] == 'u' && str[2] == 'e' && str[3] == 's' && str[4] == 't') {
        is_host = 0;
        idx = 5;
    } else {
        return -1;
    }

    if (str[idx] != ':') return -1;
    idx++;

    int chars_read = 0;
    g_rdma_vendor_id = (uint16_t)parse_hex(&str[idx], &chars_read);
    if (chars_read == 0) return -1;
    idx += chars_read;

    if (str[idx] != ':') return -1;
    idx++;

    g_rdma_device_id = (uint16_t)parse_hex(&str[idx], &chars_read);
    if (chars_read == 0) return -1;

    return 0;
}

// Core Subsystem Initialization
void net_rdma_init(void) {
    spinlock_init(&rdma_lock);
    is_host = 0;

    // Check for startup configuration parameter via QEMU fw_cfg
    char config_buf[64];
    int len = fw_cfg_read_file("opt/pcishare", config_buf, sizeof(config_buf) - 1);
    if (len <= 0) {
        uart_puts("[RDMA] Remote PCIe sharing is INACTIVE (no startup parameter).\n");
        g_rdma_active = 0;
        return;
    }
    config_buf[len] = '\0';

    // Parse configuration string
    if (parse_rdma_config(config_buf) != 0) {
        uart_puts("[RDMA] Failed to parse startup configuration parameter: ");
        uart_puts(config_buf);
        uart_puts("\n");
        g_rdma_active = 0;
        return;
    }

    g_rdma_active = 1;
    uart_puts("[RDMA] Dynamic configuration active: role=");
    if (is_host) {
        uart_puts("host, targeting device ");
    } else {
        uart_puts("guest, targeting device ");
    }
    uart_print_hex(g_rdma_vendor_id);
    uart_puts(":");
    uart_print_hex(g_rdma_device_id);
    uart_puts("\n");

    if (is_host) {
        // --- Run as Host (Provider) ---
        // Scan PCI bus for the target device
        int found = 0;
        uint8_t edu_bus = 0, edu_slot = 0;
        for (uint32_t bus = 0; bus < 256; bus++) {
            for (uint32_t slot = 0; slot < 32; slot++) {
                uint32_t id = pci_read_config(bus, slot, 0, 0);
                if ((id & 0xFFFF) != 0xFFFF) {
                    uint16_t vendor = id & 0xFFFF;
                    uint16_t device = (id >> 16) & 0xFFFF;
                    if (vendor == g_rdma_vendor_id && device == g_rdma_device_id) {
                        edu_bus = bus;
                        edu_slot = slot;
                        found = 1;
                        break;
                    }
                }
            }
            if (found) break;
        }

        if (!found) {
            uart_puts("[RDMA] Host target PCIe device NOT found on physical bus!\n");
            g_rdma_active = 0;
            return;
        }

        uart_puts("[RDMA] Host target PCIe device found on physical bus.\n");

        // Read physical BAR0
        uint32_t bar0 = pci_read_config(edu_bus, edu_slot, 0, 0x10);
        edu_regs_phys = bar0 & 0xFFFFFFF0;
        p_edu_regs = (volatile uint32_t*)(uint64_t)edu_regs_phys;

        // Enable memory space and Bus Mastering
        uint32_t cmd = pci_read_config(edu_bus, edu_slot, 0, 0x04);
        pci_write_config(edu_bus, edu_slot, 0, 0x04, cmd | 0x06);

        // Bind UDP socket to Host IP
        net_set_ip(htonl(RDMA_HOST_IP), htonl(0xFFFFFF00), htonl(0x0A000202));
        rdma_socket = net_socket_create(IP_PROTO_UDP);
        rdma_socket->local_port = RDMA_PORT;
        rdma_socket->local_ip = htonl(RDMA_HOST_IP);
        rdma_socket->state = SOCKET_ESTABLISHED;

        // Initialize Host Memory Registration Table
        for (int i = 0; i < MAX_MRS; i++) {
            host_mrs[i].in_use = 0;
        }

        // Enter the provider loop directly on CPU 0 to run synchronously during unit tests
        provider_loop(NULL);
    } 
    else {
        // --- Run as Receiver (Consumer) ---
        uart_puts("[RDMA] Receiver role initialized.\n");

        // Bind UDP socket to Guest IP and connect to Host
        net_set_ip(htonl(RDMA_GUEST_IP), htonl(0xFFFFFF00), htonl(0x0A000202));
        rdma_socket = net_socket_create(IP_PROTO_UDP);
        rdma_socket->local_port = 49152;
        rdma_socket->local_ip = htonl(RDMA_GUEST_IP);
        rdma_socket->remote_ip = htonl(RDMA_HOST_IP);
        rdma_socket->remote_port = RDMA_PORT;
        rdma_socket->state = SOCKET_ESTABLISHED;

        // Initialize Guest Memory Registration Table
        for (int i = 0; i < MAX_MRS; i++) {
            guest_mrs[i].in_use = 0;
        }
    }
}


// Synchronous Transaction helper for the Consumer
static int rdma_transaction(struct rdma_packet* req, struct rdma_packet* resp) {
    req->tx_id = next_tx_id++;
    guest_rx_ready = 0;

    uart_puts("[RDMA] Sending transaction request...\n");
    net_socket_send(rdma_socket, req, sizeof(struct rdma_packet));
    uart_puts("[RDMA] Transaction request sent. Polling for response...\n");

    // Wait and poll for response
    uint64_t start = timer_get_ms();
    while (1) {
        net_rdma_poll();
        extern void virtio_net_handle_irq(void);
        virtio_net_handle_irq();

        if (guest_rx_ready && guest_rx_packet.tx_id == req->tx_id) {
            *resp = *(struct rdma_packet*)&guest_rx_packet;
            guest_rx_ready = 0;
            uart_puts("[RDMA] Transaction response received successfully!\n");
            return 0;
        }

        if (timer_get_ms() - start > 2000) {
            uart_puts("[RDMA] Transaction Timeout! No response after 2 seconds.\n");
            return -1;
        }
        __asm__ volatile("pause");
    }
}

// Consumer APIs to access emulated registers
uint32_t v_edu_read32(uint32_t offset) {
    struct rdma_packet req, resp;
    req.op = RDMA_OP_READ_REQ;
    req.addr = offset;
    req.len = 4;
    req.status = 0;

    if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
        return 0xFFFFFFFF;
    }
    return *(uint32_t*)resp.data;
}

void v_edu_write32(uint32_t offset, uint32_t val) {
    struct rdma_packet req, resp;
    req.op = RDMA_OP_WRITE_REQ;
    req.addr = offset;
    req.len = 4;
    req.status = 0;
    *(uint32_t*)req.data = val;

    rdma_transaction(&req, &resp);
}

uint64_t v_edu_read64(uint32_t offset) {
    struct rdma_packet req, resp;
    req.op = RDMA_OP_READ_REQ;
    req.addr = offset;
    req.len = 8;
    req.status = 0;

    if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    return *(uint64_t*)resp.data;
}

void v_edu_write64(uint32_t offset, uint64_t val) {
    struct rdma_packet req, resp;
    req.op = RDMA_OP_WRITE_REQ;
    req.addr = offset;
    req.len = 8;
    req.status = 0;
    *(uint64_t*)req.data = val;

    rdma_transaction(&req, &resp);
}

// Memory Region Registration APIs
int rdma_register_mr(uint64_t guest_phys, uint32_t size) {
    struct rdma_packet req, resp;
    req.op = RDMA_OP_REG_MR;
    req.addr = guest_phys;
    req.len = size;
    req.status = 0;

    if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
        return -1;
    }

    // Save in Guest MR table
    for (int i = 0; i < MAX_MRS; i++) {
        if (!guest_mrs[i].in_use) {
            guest_mrs[i].guest_phys = guest_phys;
            guest_mrs[i].host_phys = resp.addr; // Host Physical returned
            guest_mrs[i].size = size;
            guest_mrs[i].in_use = 1;
            return 0;
        }
    }
    return -1;
}

// Translates a Guest physical address to the registered Host physical shadow address
uint64_t guest_to_host_phys(uint64_t guest_phys) {
    for (int i = 0; i < MAX_MRS; i++) {
        if (guest_mrs[i].in_use && guest_phys >= guest_mrs[i].guest_phys &&
            guest_phys < guest_mrs[i].guest_phys + guest_mrs[i].size) {
            return guest_mrs[i].host_phys + (guest_phys - guest_mrs[i].guest_phys);
        }
    }
    return guest_phys; // Fallback
}

// Syncs Guest local buffer data back and forth to Host physical shadow buffer
int rdma_dma_sync(uint64_t guest_phys, uint32_t size, int to_device) {
    if (to_device) {
        struct rdma_packet req, resp;
        req.op = RDMA_OP_DMA_SYNC_TO_HOST;
        req.addr = guest_phys;
        req.len = size;
        
        // Copy Guest RAM (identity mapped) into payload
        for (uint32_t i = 0; i < size; i++) {
            req.data[i] = ((uint8_t*)guest_phys)[i];
        }

        if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
            return -1;
        }
    } 
    else {
        struct rdma_packet req, resp;
        req.op = RDMA_OP_DMA_SYNC_TO_GUEST;
        req.addr = guest_phys;
        req.len = size;

        if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
            return -1;
        }

        // Copy shadow data back into Guest RAM
        for (uint32_t i = 0; i < size; i++) {
            ((uint8_t*)guest_phys)[i] = resp.data[i];
        }
    }
    return 0;
}
