#ifdef __x86_64__

#include "net_rdma.h"
#include "net.h"
#include "lock.h"
#include "process.h"
#include "timer.h"
#include "mmu.h"
#include "arch/cpu.h"

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);

// Global State
int is_host = 0;
struct socket_pcb* rdma_socket = NULL;
static struct socket_pcb* irq_notify_socket = NULL;  // Dedicated socket for IRQ notifications
static spinlock_t rdma_lock;

// Lock-free SPSC ring buffer for handing RDMA packets from CPU 0 → CPU 1.
// CPU 0 (producer) enqueues via rdma_wq_head.
// CPU 1 (consumer) dequeues via rdma_wq_tail.
#define RDMA_WQ_SIZE 256
static struct rdma_packet rdma_work_queue[RDMA_WQ_SIZE];
static volatile uint32_t rdma_wq_head = 0;  // Written by producer (CPU 0)
static volatile uint32_t rdma_wq_tail = 0;  // Written by consumer (CPU 1)
static volatile int rdma_worker_running = 0;

// Dynamic RDMA Configuration (Startup Parameters)
uint16_t g_rdma_vendor_id = 0;
uint16_t g_rdma_device_id = 0;
int g_rdma_active = 0;


// Host specific hardware pointers
static volatile uint8_t* p_pci_bars[6] = {NULL};
static uint64_t pci_bars_phys[6] = {0};
static uint64_t pci_bars_size[6] = {0};

static volatile uint8_t* p_pci_rom = NULL;
static uint64_t pci_rom_phys = 0;
static uint64_t pci_rom_size = 0;

// Host Memory Registration Table
#define MAX_MRS 64
struct host_mr_entry {
    uint64_t guest_phys;
    uint64_t host_phys;
    uint8_t* host_virt;
    uint32_t size;
    int in_use;
};
static struct host_mr_entry host_mrs[MAX_MRS];
static uint8_t host_dma_buffers[MAX_MRS][4096] __attribute__((aligned(4096)));

// IOMMU Shadow Buffer Pool (bump allocator for IOVA-mapped DMA regions)
// This pool provides physically contiguous shadow buffers that the host IOMMU
// maps IOVAs to. When the GPU reads an IOVA from a descriptor, the IOMMU
// translates it to a shadow buffer in this pool.
#define IOMMU_SHADOW_POOL_SIZE (32 * 1024 * 1024)  // 32MB
static uint8_t iommu_shadow_pool[IOMMU_SHADOW_POOL_SIZE] __attribute__((aligned(4096)));
static uint64_t iommu_shadow_pool_offset = 0;

// IOMMU Mapping Table: tracks IOVA → shadow buffer associations
#define MAX_IOMMU_MAPS 1024
struct iommu_map_entry {
    uint64_t iova;        // Guest IOVA (I/O Virtual Address)
    uint64_t host_phys;   // Host shadow buffer physical address
    uint8_t* host_virt;   // Host shadow buffer virtual pointer
    uint64_t size;        // Mapping size in bytes
    int in_use;
    int is_identity;      // 1 if identity-mapped (no VT-d programming needed)
};
static struct iommu_map_entry iommu_maps[MAX_IOMMU_MAPS];

// GPU PCI bus/device/function (detected during init, used for IOMMU context)
static uint8_t gpu_pci_bus = 0;
static uint8_t gpu_pci_devfn = 0;

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

static uint8_t mock_edu_buffer[4096];

static volatile uint32_t dbg_read_req_count = 0;
static volatile uint32_t dbg_read_resp_count = 0;
static volatile uint32_t dbg_map_req_count = 0;

// Host packet handler
static void handle_host_rdma(struct rdma_packet* pkt) {
    // Clamp data payload length — but NOT for DMA_SYNC_RELIABLE which uses
    // the len field to carry the total verify size (up to 100MB+).
    if (pkt->op != RDMA_OP_DMA_SYNC_RELIABLE && pkt->len > RDMA_DATA_LEN) {
        pkt->len = RDMA_DATA_LEN;
    }
    struct rdma_packet resp = {0};
    resp.op = pkt->op + 1; // Standard response Opcode (Op + 1)
    resp.tx_id = pkt->tx_id;
    resp.bar_index = pkt->bar_index;
    resp.addr = pkt->addr;
    resp.len = pkt->len;
    resp.status = 0;

    if (pkt->op == RDMA_OP_READ_REQ) dbg_read_req_count++;
    if (pkt->op == RDMA_OP_IOMMU_MAP) dbg_map_req_count++;

    // Copy client's IP to the IRQ notification socket on first contact.
    // The main rdma_socket learns the client's IP via handle_udp() routing,
    // so we mirror it to irq_notify_socket to enable IRQ forwarding.
    if (irq_notify_socket && irq_notify_socket->remote_ip == 0 && rdma_socket && rdma_socket->remote_ip != 0) {
        irq_notify_socket->remote_ip = rdma_socket->remote_ip;
        irq_notify_socket->mac_cached = 0;  // Force ARP for the new IP
        uart_puts("[RDMA] IRQ notify socket: client IP set to ");
        uart_print_hex(rdma_socket->remote_ip);
        uart_puts("\n");
    }

    static volatile uint32_t op_dbg = 0;
    op_dbg++;
    if (op_dbg <= 10 || op_dbg % 100 == 1) {
        uart_puts("[RDMA Host] op=");
        print_int(pkt->op);
        uart_puts(" bar=");
        print_int(pkt->bar_index);
        uart_puts(" addr=");
        uart_print_hex(pkt->addr);
        uart_puts(" len=");
        print_int(pkt->len);
        uart_puts(" site=");
        print_int(pkt->status);  // Repurpose status as call_site tag before handler clears it
        uart_puts(" #");
        print_int(op_dbg);
        uart_puts("\n");
    }
    pkt->status = 0;  // Reset status for handler use

    int use_mock = (pkt->bar_index == 0 && pkt->addr >= 0x40000 && pkt->addr < 0x40000 + 4096);

    if (pkt->op == RDMA_OP_READ_REQ) {
        uint8_t bar = pkt->bar_index;
        if (bar == 6) {
            if (p_pci_rom) {
                if (pkt->len <= RDMA_DATA_LEN) {
                    if (pkt->addr + pkt->len <= pci_rom_size) {
                        uint32_t words = pkt->len / 4;
                        for (uint32_t i = 0; i < words; i++) {
                            ((uint32_t*)resp.data)[i] = *(volatile uint32_t*)(p_pci_rom + pkt->addr + i * 4);
                        }
                        for (uint32_t i = words * 4; i < pkt->len; i++) {
                            resp.data[i] = *(volatile uint8_t*)(p_pci_rom + pkt->addr + i);
                        }
                    }
                    // Out of range: return zeros (safe default for probing)
                } else {
                    resp.status = 1;
                }
            } else {
                resp.status = 2; // ROM not mapped
            }
        }
        else if (bar < 6 && p_pci_bars[bar]) {
            // Bounds check: reject accesses beyond the physical BAR size
            if (pkt->addr + pkt->len > pci_bars_size[bar]) {
                // Return zeros — safe default for out-of-range probing
                resp.status = 0;
            } else if (use_mock) {
                if (pkt->len == 1) {
                    *(uint8_t*)resp.data = mock_edu_buffer[pkt->addr - 0x40000];
                } else if (pkt->len == 2) {
                    *(uint16_t*)resp.data = *(uint16_t*)&mock_edu_buffer[pkt->addr - 0x40000];
                } else if (pkt->len == 4) {
                    *(uint32_t*)resp.data = *(uint32_t*)&mock_edu_buffer[pkt->addr - 0x40000];
                } else if (pkt->len == 8) {
                    *(uint64_t*)resp.data = *(uint64_t*)&mock_edu_buffer[pkt->addr - 0x40000];
                } else {
                    resp.status = 1;
                }
            } else {
                if (pkt->len == 1) {
                    uint8_t val = *(volatile uint8_t*)(p_pci_bars[bar] + pkt->addr);
                    *(uint8_t*)resp.data = val;
                } else if (pkt->len == 2) {
                    uint16_t val = *(volatile uint16_t*)(p_pci_bars[bar] + pkt->addr);
                    *(uint16_t*)resp.data = val;
                } else if (pkt->len == 4) {
                    uint32_t val = *(volatile uint32_t*)(p_pci_bars[bar] + pkt->addr);
                    *(uint32_t*)resp.data = val;

                } else if (pkt->len == 8) {
                    uint64_t val = *(volatile uint64_t*)(p_pci_bars[bar] + pkt->addr);
                    *(uint64_t*)resp.data = val;
                } else {
                    resp.status = 1;
                }
            }
        } else {
            resp.status = 2; // Device not mapped
        }
    } 
    else if (pkt->op == RDMA_OP_WRITE_REQ) {
        uint8_t bar = pkt->bar_index;
        if (bar < 6 && p_pci_bars[bar]) {
            // Bounds check: silently ignore writes beyond the physical BAR size
            if (pkt->addr + pkt->len > pci_bars_size[bar]) {
                // Silently ignore — safe for out-of-range probing
            } else if (use_mock) {
                if (pkt->len == 1) {
                    mock_edu_buffer[pkt->addr - 0x40000] = *(uint8_t*)pkt->data;
                } else if (pkt->len == 2) {
                    *(uint16_t*)&mock_edu_buffer[pkt->addr - 0x40000] = *(uint16_t*)pkt->data;
                } else if (pkt->len == 4) {
                    *(uint32_t*)&mock_edu_buffer[pkt->addr - 0x40000] = *(uint32_t*)pkt->data;
                } else if (pkt->len == 8) {
                    *(uint64_t*)&mock_edu_buffer[pkt->addr - 0x40000] = *(uint64_t*)pkt->data;
                }
            } else {
                if (pkt->len == 1) {
                    uint8_t val = *(uint8_t*)pkt->data;
                    *(volatile uint8_t*)(p_pci_bars[bar] + pkt->addr) = val;
                } else if (pkt->len == 2) {
                    uint16_t val = *(uint16_t*)pkt->data;
                    *(volatile uint16_t*)(p_pci_bars[bar] + pkt->addr) = val;
                } else if (pkt->len == 4) {
                    uint32_t val = *(uint32_t*)pkt->data;
                    *(volatile uint32_t*)(p_pci_bars[bar] + pkt->addr) = val;

                } else if (pkt->len == 8) {
                    uint64_t val = *(uint64_t*)pkt->data;
                    *(volatile uint64_t*)(p_pci_bars[bar] + pkt->addr) = val;
                }
                arch_memory_barrier();
            }
        }
        return; // Fire-and-forget: BAR writes don't need a response
    } 
    else if (pkt->op == RDMA_OP_READ_BLOCK_REQ) {
        uint8_t bar = pkt->bar_index;
        if (bar < 6 && p_pci_bars[bar]) {
            uint32_t block_len = pkt->len;
            if (block_len <= RDMA_DATA_LEN) {
                // Bounds check
                if (pkt->addr + block_len > pci_bars_size[bar]) {
                    // Return zeros — safe for out-of-range probing
                    resp.status = 0;
                } else if (use_mock) {
                    for (uint32_t i = 0; i < block_len; i++) {
                        resp.data[i] = mock_edu_buffer[pkt->addr - 0x40000 + i];
                    }
                } else {
                    uint32_t words = block_len / 4;
                    for (uint32_t i = 0; i < words; i++) {
                        uint32_t val = *(volatile uint32_t*)(p_pci_bars[bar] + pkt->addr + i * 4);
                        ((uint32_t*)resp.data)[i] = val;
                    }
                    // Trailing bytes (if any)
                    for (uint32_t i = words * 4; i < block_len; i++) {
                        resp.data[i] = *(volatile uint8_t*)(p_pci_bars[bar] + pkt->addr + i);
                    }
                }
            } else {
                resp.status = 1; // Length too large
            }
        } else {
            resp.status = 2;
        }
    }
    else if (pkt->op == RDMA_OP_WRITE_BLOCK_REQ) {
        uint8_t bar = pkt->bar_index;
        if (bar < 6 && p_pci_bars[bar]) {
            uint32_t block_len = pkt->len;
            if (block_len <= RDMA_DATA_LEN) {
                // Bounds check
                if (pkt->addr + block_len > pci_bars_size[bar]) {
                    // Silently ignore — safe for out-of-range probing
                } else if (use_mock) {
                    for (uint32_t i = 0; i < block_len; i++) {
                        mock_edu_buffer[pkt->addr - 0x40000 + i] = pkt->data[i];
                    }
                } else {
                    uint32_t words = block_len / 4;
                    for (uint32_t i = 0; i < words; i++) {
                        uint32_t val = ((uint32_t*)pkt->data)[i];
                        *(volatile uint32_t*)(p_pci_bars[bar] + pkt->addr + i * 4) = val;
                        arch_memory_barrier();
                    }
                    // Trailing bytes (if any)
                    for (uint32_t i = words * 4; i < block_len; i++) {
                        *(volatile uint8_t*)(p_pci_bars[bar] + pkt->addr + i) = pkt->data[i];
                        arch_memory_barrier();
                    }
                }
            }
        }
        return; // Fire-and-forget: block writes don't need a response
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
        // Fire-and-forget: write data directly to host physical memory.
        // The GPU's DMA engine reads from real physical memory. Shadow buffers
        // (IOMMU/MR) are irrelevant for GPU DMA — they're only used for the
        // client's SYNC_FROM_HOST path. Skip them for maximum throughput.
        if (pkt->addr < 0x70000000ULL || pkt->addr >= 0x71300000ULL) {
            uint8_t* ptr = (uint8_t*)pkt->addr;
            for (uint32_t j = 0; j < pkt->len; j++) {
                ptr[j] = pkt->data[j];
            }
        }
        return; // No response — fire-and-forget
    } 
    else if (pkt->op == RDMA_OP_DMA_SYNC_RELIABLE) {
        // CRC32 Verification: client sends expected CRC32 in data[0..3],
        // addr = target GPA, len = total size of DMA region to verify.
        // Host computes CRC32 over the memory at addr..addr+len and compares.
        uint32_t expected_crc = *(uint32_t*)pkt->data;
        uint64_t verify_addr = pkt->addr;
        uint64_t verify_len = pkt->len;
        
        // Compute CRC32 over host memory
        uint32_t crc = 0xFFFFFFFF;
        if (verify_addr < 0x70000000ULL || verify_addr >= 0x71300000ULL) {
            uint8_t* ptr = (uint8_t*)(uintptr_t)verify_addr;
            for (uint64_t j = 0; j < verify_len; j++) {
                crc ^= ptr[j];
                for (int b = 0; b < 8; b++) {
                    crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1));
                }
            }
        }
        crc ^= 0xFFFFFFFF;
        
        resp.op = RDMA_OP_DMA_SYNC_RELIABLE_RESP;
        if (crc == expected_crc) {
            resp.status = 0; // Match!
            uart_puts("[RDMA] DMA VERIFY OK: addr=");
            uart_print_hex(verify_addr);
            uart_puts(" len=");
            uart_print_hex(verify_len);
            uart_puts(" crc=");
            uart_print_hex(crc);
            uart_puts("\n");
        } else {
            resp.status = 1; // Mismatch
            *(uint32_t*)resp.data = crc; // Return host's CRC for diagnostics
            uart_puts("[RDMA] DMA VERIFY FAIL: addr=");
            uart_print_hex(verify_addr);
            uart_puts(" expected=");
            uart_print_hex(expected_crc);
            uart_puts(" got=");
            uart_print_hex(crc);
            uart_puts("\n");
        }
        // Fall through to send_resp to send the ACK
    }
    else if (pkt->op == RDMA_OP_DMA_SYNC_TO_GUEST) {
        int found = 0;
        
        // 1. Check IOMMU shadow buffers first (IOVA-based mappings from vIOMMU).
        for (int i = 0; i < MAX_IOMMU_MAPS; i++) {
            if (iommu_maps[i].in_use &&
                pkt->addr >= iommu_maps[i].iova &&
                (pkt->addr + pkt->len) <= (iommu_maps[i].iova + iommu_maps[i].size)) {
                uint64_t offset = pkt->addr - iommu_maps[i].iova;
                for (uint32_t j = 0; j < pkt->len; j++) {
                    resp.data[j] = iommu_maps[i].host_virt[offset + j];
                }
                found = 1;
                break;
            }
        }
        
        if (!found) {
            // Read from identity-mapped physical address (where GPU DMA writes responses),
            // not just the shadow buffer. The GPU writes directly to physical memory.
            if (pkt->addr < 0x70000000ULL || pkt->addr >= 0x71300000ULL) {
                uint8_t* ptr = (uint8_t*)pkt->addr;
                for (uint32_t j = 0; j < pkt->len; j++) {
                    resp.data[j] = ptr[j];
                }
                found = 1;
            } else {
                // Blocked range — try shadow buffer as fallback
                for (int i = 0; i < MAX_MRS; i++) {
                    if (host_mrs[i].in_use &&
                        pkt->addr >= host_mrs[i].guest_phys &&
                        (pkt->addr + pkt->len) <= (host_mrs[i].guest_phys + host_mrs[i].size)) {
                        uint64_t mr_offset = pkt->addr - host_mrs[i].guest_phys;
                        for (uint32_t j = 0; j < pkt->len; j++) {
                            resp.data[j] = host_mrs[i].host_virt[mr_offset + j];
                        }
                        found = 1;
                        break;
                    }
                }
            }
        }
        resp.op = RDMA_OP_DMA_SYNC_RESP;
        resp.status = found ? 0 : 4;
    }
    else if (pkt->op == RDMA_OP_IOMMU_MAP) {
        // Map an IOVA to a shadow buffer on the host, program the IOMMU.
        // pkt->addr = IOVA, pkt->data contains the 64-bit size
        uint64_t iova = pkt->addr;
        uint64_t map_size = *(uint64_t*)pkt->data;

        // Handle zero-size mappings gracefully
        if (map_size == 0) {
            // Zero-size IOMMU_MAP is a no-op — fire-and-forget, no response needed
            uart_puts("[IOMMU] MAP: iova=");
            uart_print_hex(iova);
            uart_puts(" size=0 (no-op)\n");
            return; // Fire-and-forget: no response
        }

        // Page-align size
        if (map_size & 0xFFF) {
            map_size = (map_size + 0xFFF) & ~0xFFFULL;
        }

        // Check for existing mapping
        int existing = -1;
        for (int i = 0; i < MAX_IOMMU_MAPS; i++) {
            if (iommu_maps[i].in_use && iommu_maps[i].iova == iova) {
                existing = i;
                break;
            }
        }

        if (existing >= 0) {
            // Already mapped — return existing mapping
            resp.op = RDMA_OP_IOMMU_MAP_RESP;
            resp.addr = iommu_maps[existing].host_phys;
            resp.status = 0;
            uart_puts("[IOMMU] MAP reuse: iova=");
            uart_print_hex(iova);
            uart_puts(" -> host=");
            uart_print_hex(iommu_maps[existing].host_phys);
            uart_puts("\n");
        } else {
            // ALL mappings use identity mode (iova == phys) to stay compatible
            // with passthrough context entry. Shadow buffers would require
            // transitioning to translated mode, which breaks all DMA for
            // large identity-mapped regions (2GB RAM, 32GB BAR1) since our
            // VT-d page table pool can't hold millions of 4KB PTEs.
            int use_identity = 1;

            uint64_t shadow_phys;
            uint8_t* shadow_virt;

            if (use_identity) {
                // Identity: host_phys = iova (guest RAM is physically accessible on host)
                shadow_phys = iova;
                shadow_virt = (uint8_t*)(uintptr_t)iova;
            } else {
                // Allocate shadow buffer from pool (bump allocator)
                uint64_t aligned_offset = (iommu_shadow_pool_offset + 0xFFF) & ~0xFFFULL;
                if (aligned_offset + map_size > IOMMU_SHADOW_POOL_SIZE) {
                    uart_puts("[IOMMU] ERROR: Shadow pool exhausted!\n");
                    resp.op = RDMA_OP_IOMMU_MAP_RESP;
                    resp.status = 3;
                    goto send_resp;
                }
                shadow_virt = &iommu_shadow_pool[aligned_offset];
                shadow_phys = (uint64_t)shadow_virt;
                iommu_shadow_pool_offset = aligned_offset + map_size;

                // Zero the shadow buffer
                for (uint64_t j = 0; j < map_size; j++) {
                    shadow_virt[j] = 0;
                }
            }

            // Find free slot in mapping table
            int slot = -1;
            for (int i = 0; i < MAX_IOMMU_MAPS; i++) {
                if (!iommu_maps[i].in_use) {
                    slot = i;
                    break;
                }
            }

            if (slot < 0) {
                uart_puts("[IOMMU] ERROR: Mapping table full!\n");
                resp.op = RDMA_OP_IOMMU_MAP_RESP;
                resp.status = 4;
            } else {
                iommu_maps[slot].iova = iova;
                iommu_maps[slot].host_phys = shadow_phys;
                iommu_maps[slot].host_virt = shadow_virt;
                iommu_maps[slot].size = map_size;
                iommu_maps[slot].in_use = 1;
                iommu_maps[slot].is_identity = use_identity;

                // Program the host IOMMU: IOVA → shadow buffer phys
                // Identity mappings (iova == phys) rely on passthrough mode at the
                // context entry level. Only shadow-buffered (non-identity) regions
                // need individual page table entries.
                //
                // IMPORTANT: if this is the FIRST non-identity map, we must transition
                // the context entry from passthrough to translated mode. But we ALSO
                // need to pre-populate the page tables with identity maps for all
                // existing identity-mapped regions, otherwise their DMA will break.
                int rc = 0;
                if (!use_identity) {
                    extern int iommu_vtd_map(uint8_t bus, uint8_t devfn,
                                             uint64_t iova, uint64_t phys, uint64_t size);
                    rc = iommu_vtd_map(gpu_pci_bus, gpu_pci_devfn, iova, shadow_phys, map_size);
                }

                resp.op = RDMA_OP_IOMMU_MAP_RESP;
                resp.addr = shadow_phys;
                resp.status = (rc == 0) ? 0 : 5;

                uart_puts("[IOMMU] MAP: iova=");
                uart_print_hex(iova);
                uart_puts(" -> host=");
                uart_print_hex(shadow_phys);
                uart_puts(" size=");
                uart_print_hex(map_size);
                uart_puts(use_identity ? " (identity)\n" : " (shadow)\n");
            }
        }
        // IOMMU_MAP is fire-and-forget from the host side. The client marks
        // mappings synced optimistically and never waits for a response.
        // Sending IOMMU_MAP_RESP floods sock_fd and causes READ_REQ timeouts.
        return;
    }

    else if (pkt->op == RDMA_OP_IOMMU_UNMAP) {
        // Unmap an IOVA from the host IOMMU.
        // pkt->addr = IOVA, pkt->len = size
        uint64_t iova = pkt->addr;

        int found_slot = -1;
        for (int i = 0; i < MAX_IOMMU_MAPS; i++) {
            if (iommu_maps[i].in_use && iommu_maps[i].iova == iova) {
                found_slot = i;
                break;
            }
        }

        if (found_slot >= 0) {
            // Only call VT-d unmap if this was a shadow-mapped (non-identity) region
            if (!iommu_maps[found_slot].is_identity) {
                extern int iommu_vtd_unmap(uint8_t bus, uint8_t devfn,
                                           uint64_t iova, uint64_t size);
                extern void iommu_vtd_invalidate_iotlb(void);
                iommu_vtd_unmap(gpu_pci_bus, gpu_pci_devfn, iova, iommu_maps[found_slot].size);
                iommu_vtd_invalidate_iotlb();
            }

            iommu_maps[found_slot].in_use = 0;

            uart_puts("[IOMMU] UNMAP: iova=");
            uart_print_hex(iova);
            uart_puts("\n");
        }

        resp.op = RDMA_OP_IOMMU_UNMAP_RESP;
        resp.status = 0;  // Always OK — guest drives unmap lifecycle
        // IOMMU_UNMAP is fire-and-forget: no response sent.
        // The client sends IOMMU_UNMAP via blast_sock_fd and never waits.
        return;
    }

    send_resp:
    dbg_read_resp_count++;
    net_socket_send(rdma_socket, &resp, sizeof(struct rdma_packet));
}

// Guest response handler
static void handle_guest_rdma(struct rdma_packet* pkt) {
    guest_rx_packet = *pkt;
    guest_rx_ready = 1;
}

// Fast-path BAR write handler: called directly from handle_udp() in ISR context.
// BAR writes are fire-and-forget MMIO operations that MUST NOT go through the
// rx_buf ring buffer. The blast sync floods the 4MB buffer with 100MB of data,
// wrapping it ~25 times and silently overwriting any BAR write packets before
// net_rdma_poll() can consume them. By handling writes inline in the UDP handler,
// every write reaches the physical hardware reliably.
void net_rdma_fast_write(const struct rdma_packet *pkt) {
    if (!is_host) return;
    if (pkt->op != RDMA_OP_WRITE_REQ) return;

    uint8_t bar = pkt->bar_index;
    if (bar >= 6 || !p_pci_bars[bar]) return;
    if (pkt->addr + pkt->len > pci_bars_size[bar]) return;

    if (pkt->len == 1) {
        *(volatile uint8_t*)(p_pci_bars[bar] + pkt->addr) = *(uint8_t*)pkt->data;
    } else if (pkt->len == 2) {
        *(volatile uint16_t*)(p_pci_bars[bar] + pkt->addr) = *(uint16_t*)pkt->data;
    } else if (pkt->len == 4) {
        *(volatile uint32_t*)(p_pci_bars[bar] + pkt->addr) = *(uint32_t*)pkt->data;
    } else if (pkt->len == 8) {
        *(volatile uint64_t*)(p_pci_bars[bar] + pkt->addr) = *(uint64_t*)pkt->data;
    }
    extern void arch_memory_barrier(void);
    arch_memory_barrier();
}

// Shared network polling loop
static volatile int rdma_poll_active = 0; // Re-entrancy guard for timer interrupt

void net_rdma_poll(void) {
    if (!rdma_socket) return;
    if (rdma_poll_active) return; // Prevent re-entrant calls from timer interrupt
    rdma_poll_active = 1;

    // Process ALL available RDMA packets in the rx_buf, not just one.
    // This is critical because DMA blast syncs can flood the buffer with
    // thousands of packets, and BAR read/write requests need timely processing.
    int processed = 0;
    while (processed < 4096) {
        if (rdma_socket->rx_head == rdma_socket->rx_tail) break;

        uint32_t avail = rdma_socket->rx_tail - rdma_socket->rx_head;
        if (avail < sizeof(struct rdma_packet)) break;

        struct rdma_packet pkt = {0};
        uint64_t flags = spinlock_acquire_irqsave(&rdma_lock);
        uint8_t* dest = (uint8_t*)&pkt;
        extern void arch_memory_barrier(void);
        arch_memory_barrier();
        for (uint32_t i = 0; i < sizeof(struct rdma_packet); i++) {
            dest[i] = rdma_socket->rx_buf[rdma_socket->rx_head % SOCKET_RX_BUF_SIZE];
            rdma_socket->rx_head++;
        }
        spinlock_release_irqrestore(&rdma_lock, flags);

        if (is_host) {
            // READ_REQ and WRITE_REQ are the hot path (GPU BAR reads/writes).
            // Handle them INLINE on CPU 0 for minimum latency — bypassing the
            // worker queue entirely. The worker queue is only for heavy ops like
            // IOMMU_MAP that can tolerate queuing without causing client timeouts.
            int is_fast_path = (pkt.op == RDMA_OP_READ_REQ ||
                                pkt.op == RDMA_OP_WRITE_REQ ||
                                pkt.op == RDMA_OP_DMA_SYNC_TO_HOST ||
                                pkt.op == RDMA_OP_DMA_SYNC_RELIABLE ||
                                pkt.op == RDMA_OP_DMA_SYNC_TO_GUEST ||
                                pkt.op == RDMA_OP_DMA_SYNC_RESP ||
                                pkt.op == RDMA_OP_READ_BLOCK_REQ ||
                                pkt.op == RDMA_OP_WRITE_BLOCK_REQ ||
                                pkt.op == RDMA_OP_REG_MR);
            if (is_fast_path) {
                pkt.status = 6; // call site: fast path inline (READ/WRITE/DMA)
                handle_host_rdma(&pkt);
            } else if (rdma_worker_running) {
                // Heavy ops (IOMMU_MAP, IOMMU_UNMAP): enqueue to worker on CPU 1
                uint32_t next_head = (rdma_wq_head + 1) % RDMA_WQ_SIZE;
                if (next_head != rdma_wq_tail) {
                    pkt.status = 4; // call site: worker queue
                    rdma_work_queue[rdma_wq_head] = pkt;
                    arch_memory_barrier();
                    rdma_wq_head = next_head;
                } else {
                    // Queue full — process inline as fallback
                    pkt.status = 2; // call site: queue full inline
                    handle_host_rdma(&pkt);
                }
            } else {
                // No worker yet — process inline (during boot)
                pkt.status = 3; // call site: no worker inline
                handle_host_rdma(&pkt);
            }
        } else {
            handle_guest_rdma(&pkt);
        }
        processed++;
    }

    rdma_poll_active = 0;
}

// RDMA Worker thread — runs on a dedicated CPU core.
// Dequeues RDMA requests from the SPSC ring buffer and processes them.
// This decouples the slow PCI BAR read + TX response from the RX polling loop.
static void rdma_worker_thread(void *arg) {
    (void)arg;
    uart_puts("[RDMA] Worker thread started on dedicated CPU core\n");
    rdma_worker_running = 1;

    while (1) {
        // Spin-check the work queue
        if (rdma_wq_tail != rdma_wq_head) {
            extern void arch_memory_barrier(void);
            arch_memory_barrier();
            struct rdma_packet pkt = rdma_work_queue[rdma_wq_tail];
            arch_memory_barrier();
            rdma_wq_tail = (rdma_wq_tail + 1) % RDMA_WQ_SIZE;
            pkt.status = 5; // call site: worker thread dequeue
            handle_host_rdma(&pkt);
        } else {
            __asm__ volatile("pause");
        }
    }
}

// Host Provider Listening Loop
// Polls virtio_net and RDMA continuously on CPU 0.
// Uses virtio_net_poll_rx instead of virtio_net_handle_irq to avoid the ISR
// clearing race: the hardware IRQ handler clears the ISR, but the provider_loop's
// call to virtio_net_handle_irq sees ISR=0 and returns without processing.
static void provider_loop(void *arg) {
    (void)arg;
    uart_puts("[RDMA] Host Provider Thread started!\n");
    
    // Tell handle_irq to only clear ISR and not process packets.
    // We handle all RX via poll_rx, which avoids the net_lock deadlock.
    extern volatile int virtio_net_provider_active;
    virtio_net_provider_active = 1;
    
    extern uint64_t timer_get_ms(void);
    uint64_t last_report = timer_get_ms();
    uint32_t rx_count = 0;
    uint32_t rdma_count = 0;
    uint32_t irq_fwd_count = 0;
    uint64_t last_irq_ms = 0;
    while (1) {
        extern void virtio_net_poll_rx(void);
        virtio_net_poll_rx();
        
        // Count RDMA packets processed
        if (rdma_socket && rdma_socket->rx_head != rdma_socket->rx_tail) {
            rdma_count++;
        }
        net_rdma_poll();

        // GPU Interrupt Polling & Forwarding:
        // Read the GPU's PMC INTR register (BAR0 + 0x100) to detect pending interrupts.
        // Only forward when the status CHANGES (edge-triggered) to avoid flooding
        // the network with 100K+ packets/sec that drown out BAR read responses.
        // Rate-limited to max 100 notifications/sec (10ms minimum gap).
        // NOTE: Do NOT clear/acknowledge interrupts here — the guest driver reads
        // the same register via BAR0 MMIO and needs to see the pending bits.
        // CRITICAL (task #8): only poll the GPU interrupt register at most every 50ms.
        // On this Ada/GSP GPU, reading PMC_INTR (BAR0+0x100) after GSP locks it returns
        // NVIDIA's PRI poison (0xbadfXXXX) AFTER a slow PRI timeout. Reading it EVERY
        // provider-loop iteration (thousands/sec) throttled this single-threaded loop, so
        // RDMA read responses were delayed past the client's 500ms timeout → ~785ms/read,
        // ~59% retries, and the guest soft-locked. Polling it rarely frees the loop to
        // service RDMA reads fast. (0x100 yields no usable interrupts here anyway — real
        // GSP interrupts are MSI-X; see task notes.)
        if (p_pci_bars[0] && irq_notify_socket && irq_notify_socket->remote_port != 0) {
            uint64_t now_irq = timer_get_ms();
            if (now_irq - last_irq_ms >= 50) {
                last_irq_ms = now_irq;
                uint32_t intr = *(volatile uint32_t*)(p_pci_bars[0] + 0x100);
                // Poison filter: drop 0xbadfXXXX (GSP-locked PRI poison) so we don't spam
                // the guest with fake IRQs.
                int is_poison = ((intr & 0xFFFF0000U) == 0xBADF0000U);
                if (intr != 0 && intr != 0xFFFFFFFF && !is_poison) {
                    uint32_t vector_mask = 0x1;
                    struct rdma_packet irq_pkt = {0};
                    irq_pkt.op = RDMA_OP_IRQ_NOTIFY;
                    irq_pkt.tx_id = 0;
                    irq_pkt.addr = intr;
                    irq_pkt.len = 4;
                    *(uint32_t*)irq_pkt.data = vector_mask;
                    net_socket_send(irq_notify_socket, &irq_pkt, sizeof(struct rdma_packet));
                    irq_fwd_count++;
                }
            }
        }
        
        // Periodic debug report
        uint64_t now = timer_get_ms();
        if (now - last_report >= 2000) {
            extern volatile uint32_t dbg_handle_irq_calls;
            extern volatile uint32_t dbg_handle_irq_isr_nonzero;
            extern volatile uint32_t dbg_handle_irq_pkts;
            extern volatile uint32_t dbg_poll_rx_pkts;
            extern volatile uint16_t dbg_rx_ack_used_idx;
            extern volatile uint16_t dbg_rx_used_idx;
            extern volatile uint32_t rdma_ring_dbg;
            uart_puts("[RDMA STATS] rdma=");
            extern void print_int(int val);
            print_int(rdma_count);
            uart_puts(" irq_calls=");
            print_int(dbg_handle_irq_calls);
            uart_puts(" poll_pkts=");
            print_int(dbg_poll_rx_pkts);
            uart_puts(" read_req=");
            print_int(dbg_read_req_count);
            uart_puts(" resp_sent=");
            print_int(dbg_read_resp_count);
            uart_puts(" rdma_rx_seen=");
            print_int(rdma_ring_dbg);
            uart_puts(" rx_ack=");
            print_int(dbg_rx_ack_used_idx);
            uart_puts(" rx_used=");
            print_int(dbg_rx_used_idx);
            uart_puts(" irq_fwd=");
            print_int(irq_fwd_count);
            uart_puts(" rx_head=");
            extern void uart_print_hex(uint64_t val);
            uart_print_hex(rdma_socket ? rdma_socket->rx_head : 0);
            uart_puts(" rx_tail=");
            uart_print_hex(rdma_socket ? rdma_socket->rx_tail : 0);
            uart_puts("\n");
            rdma_count = 0;
            last_report = now;
        }
        
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
    static int initialized = 0;
    if (initialized) {
        return;
    }
    initialized = 1;

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
        // Enable PCI decoding, Bus Mastering, and D0 power state on all devices and bridges on the bus
        for (uint32_t bus = 0; bus < 256; bus++) {
            for (uint32_t slot = 0; slot < 32; slot++) {
                uint32_t id = pci_read_config(bus, slot, 0, 0);
                if ((id & 0xFFFF) != 0xFFFF) {
                    // Enable Memory Space, I/O Space, and Bus Mastering
                    uint32_t cmd = pci_read_config(bus, slot, 0, 0x04);
                    pci_write_config(bus, slot, 0, 0x04, cmd | 0x07);
                    
                    // Force D0 power state for any device with Power Management Capability
                    // Check standard PCI capabilities list bit in Status register
                    if (cmd & 0x00100000) {
                        uint32_t cap_ptr = pci_read_config(bus, slot, 0, 0x34) & 0xFF;
                        while (cap_ptr != 0) {
                            uint32_t cap_header = pci_read_config(bus, slot, 0, cap_ptr);
                            uint8_t cap_id = cap_header & 0xFF;
                            if (cap_id == 0x01) { // Power Management Capability
                                uint32_t pmcsr = pci_read_config(bus, slot, 0, cap_ptr + 4);
                                pci_write_config(bus, slot, 0, cap_ptr + 4, pmcsr & ~0x3); // Set PowerState = D0
                                break;
                            }
                            cap_ptr = (cap_header >> 8) & 0xFF;
                        }
                    }
                }
            }
        }

        // Scan PCI bus for the target device
        int found = 0;
        uint8_t dev_bus = 0, dev_slot = 0;
        for (uint32_t bus = 0; bus < 256; bus++) {
            for (uint32_t slot = 0; slot < 32; slot++) {
                uint32_t id = pci_read_config(bus, slot, 0, 0);
                if ((id & 0xFFFF) != 0xFFFF) {
                    uint16_t vendor = id & 0xFFFF;
                    uint16_t device = (id >> 16) & 0xFFFF;
                    if (vendor == g_rdma_vendor_id && device == g_rdma_device_id) {
                        dev_bus = bus;
                        dev_slot = slot;
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

        // Clear all BAR mappings first
        for (int i = 0; i < 6; i++) {
            p_pci_bars[i] = NULL;
            pci_bars_phys[i] = 0;
            pci_bars_size[i] = 0;
        }

        // Scan and map all 6 BARs dynamically
        for (int i = 0; i < 6; i++) {
            uint8_t offset = 0x10 + i * 4;
            uint32_t bar_val = pci_read_config(dev_bus, dev_slot, 0, offset);
            if (bar_val == 0) continue; // Unused or zero base

            // Inspect type and sizing
            // 1. Save original value
            pci_write_config(dev_bus, dev_slot, 0, offset, 0xFFFFFFFF);
            uint32_t size_mask = pci_read_config(dev_bus, dev_slot, 0, offset);
            pci_write_config(dev_bus, dev_slot, 0, offset, bar_val); // Restore

            if (size_mask == 0 || size_mask == 0xFFFFFFFF) continue;

            // Check if Memory Space (bit 0 = 0)
            int is_io = (bar_val & 0x1);
            if (is_io) {
                // I/O space BAR, not standard MMIO, but let's size it anyway
                uint32_t size = ~(size_mask & 0xFFFFFFFC) + 1;
                pci_bars_size[i] = size;
                pci_bars_phys[i] = bar_val & 0xFFFFFFFC;
                uart_puts("[RDMA] Detected I/O BAR ");
                print_int(i);
                uart_puts(" size ");
                uart_print_hex(size);
                uart_puts("\n");
                continue;
            }

            // Check if 64-bit Memory space (bits 2-1 = 2 -> 0x4)
            int is_64bit = ((bar_val & 0x6) == 0x4);
            uint64_t phys_addr = bar_val & 0xFFFFFFF0;
            uint64_t size_mask_64 = size_mask & 0xFFFFFFF0;

            if (is_64bit && i < 5) {
                uint8_t next_offset = offset + 4;
                uint32_t bar_val_hi = pci_read_config(dev_bus, dev_slot, 0, next_offset);
                pci_write_config(dev_bus, dev_slot, 0, next_offset, 0xFFFFFFFF);
                uint32_t size_mask_hi = pci_read_config(dev_bus, dev_slot, 0, next_offset);
                pci_write_config(dev_bus, dev_slot, 0, next_offset, bar_val_hi); // Restore

                phys_addr |= ((uint64_t)bar_val_hi) << 32;
                size_mask_64 |= ((uint64_t)size_mask_hi) << 32;
            }

            uint64_t size;
            if (is_64bit) {
                size = ~size_mask_64 + 1;
            } else {
                size = (uint32_t)(~((uint32_t)size_mask_64) + 1);
            }
            mmu_map_mmio_range(phys_addr, size);
            pci_bars_phys[i] = phys_addr;
            pci_bars_size[i] = size;
            p_pci_bars[i] = (volatile uint8_t*)phys_addr;

            uart_puts("[RDMA] Mapped Memory BAR ");
            print_int(i);
            uart_puts(" at ");
            uart_print_hex(phys_addr);
            uart_puts(" size ");
            uart_print_hex(size);
            uart_puts("\n");

            if (is_64bit) {
                i++; // Skip the next index since 64-bit BAR consumes two register slots
            }
        }

        // Enable memory space and Bus Mastering
        uint32_t cmd = pci_read_config(dev_bus, dev_slot, 0, 0x04);
        pci_write_config(dev_bus, dev_slot, 0, 0x04, cmd | 0x06);

        // Map and Enable physical expansion ROM BAR (offset 0x30)
        uint32_t rom_val = pci_read_config(dev_bus, dev_slot, 0, 0x30);
        if (rom_val != 0 && rom_val != 0xFFFFFFFF) {
            uint64_t rom_phys = rom_val & 0xFFFFF800ULL; // 2KB or larger alignment
            uint64_t rom_size = 1024 * 1024; // Standard 1MB size for GPU Expansion ROM
            pci_write_config(dev_bus, dev_slot, 0, 0x30, rom_val | 0x1); // Enable ROM
            
            mmu_map_mmio_range(rom_phys, rom_size);
            pci_rom_phys = rom_phys;
            pci_rom_size = rom_size;
            p_pci_rom = (volatile uint8_t*)rom_phys;
            
            uart_puts("[RDMA] Mapped and Enabled Physical Expansion ROM at ");
            uart_print_hex(rom_phys);
            uart_puts("\n");
        }

        // Bind UDP socket to Host IP dynamically using DHCP
        extern void dhcp_init(void);
        dhcp_init();
        uint32_t my_ip = net_get_ip();

        rdma_socket = net_socket_create(IP_PROTO_UDP);
        rdma_socket->local_port = RDMA_PORT;
        rdma_socket->local_ip = my_ip;
        rdma_socket->state = SOCKET_ESTABLISHED;

        // Create a dedicated socket for sending IRQ notifications to the client.
        // The client listens on RDMA_PORT+1 (7778) for these fire-and-forget packets.
        // Using a separate socket prevents IRQ traffic from interfering with the
        // main RDMA request-response channel.
        irq_notify_socket = net_socket_create(IP_PROTO_UDP);
        if (irq_notify_socket) {
            irq_notify_socket->local_port = RDMA_PORT + 1;
            irq_notify_socket->local_ip = my_ip;
            irq_notify_socket->remote_port = RDMA_PORT + 1;  // Client listens on 7778
            irq_notify_socket->state = SOCKET_ESTABLISHED;
            uart_puts("[RDMA] IRQ notification socket created (port ");
            print_int(RDMA_PORT + 1);
            uart_puts(")\n");
        } else {
            uart_puts("[RDMA] WARNING: Failed to create IRQ notification socket\n");
        }

        // Initialize Host Memory Registration Table
        for (int i = 0; i < MAX_MRS; i++) {
            host_mrs[i].in_use = 0;
        }

        // Initialize IOMMU mapping table
        for (int i = 0; i < MAX_IOMMU_MAPS; i++) {
            iommu_maps[i].in_use = 0;
        }
        iommu_shadow_pool_offset = 0;

        // Store the GPU's PCI bus/devfn for IOMMU context programming
        gpu_pci_bus = dev_bus;
        gpu_pci_devfn = (dev_slot << 3);  // devfn = slot << 3 | func (func=0)
        uart_puts("[RDMA] GPU PCI location: bus=");
        print_int(gpu_pci_bus);
        uart_puts(" devfn=");
        uart_print_hex(gpu_pci_devfn);
        uart_puts("\n");

        // Initialize the Intel VT-d IOMMU driver
        extern int iommu_vtd_init(void);
        int iommu_rc = iommu_vtd_init();
        if (iommu_rc == 0) {
            uart_puts("[RDMA] IOMMU VT-d initialized successfully\n");

            // Install passthrough context entry for the GPU so that if translation
            // is ever enabled, GPU firmware DMA is not blocked.
            // NOTE: We do NOT call iommu_vtd_enable_translation() here because
            // enabling global translation on the HOST would block NVMe and VirtIO
            // DMA. With VFIO passthrough and translation disabled, QEMU allows
            // all GPU DMA to guest memory (caching-mode only restricts when
            // translation IS enabled).
            extern int iommu_vtd_set_passthrough(uint8_t bus, uint8_t devfn);
            int pt_rc = iommu_vtd_set_passthrough((uint8_t)gpu_pci_bus,
                                                   (uint8_t)gpu_pci_devfn);
            if (pt_rc == 0) {
                uart_puts("[RDMA] GPU passthrough context entry installed (translation not enabled)\n");
            } else {
                uart_puts("[RDMA] WARNING: GPU passthrough install failed\n");
            }
        } else {
            uart_puts("[RDMA] WARNING: IOMMU VT-d init failed (rc=");
            print_int(iommu_rc);
            uart_puts("), IOMMU-based DMA translation unavailable\n");
        }

        // Spawn RDMA worker thread on a dedicated CPU core.
        // This thread dequeues from the SPSC work queue and handles
        // PCI BAR reads + TX responses independently of the RX poller.
        extern int process_create_kernel(void (*entry)(void*), void *arg);
        int worker_pid = process_create_kernel(rdma_worker_thread, NULL);
        if (worker_pid >= 0) {
            uart_puts("[RDMA] Spawned RDMA worker thread (PID ");
            print_int(worker_pid);
            uart_puts(")\n");
        } else {
            uart_puts("[RDMA] WARNING: Failed to spawn worker thread, running single-threaded\n");
        }

        // Spawn the provider loop as a kernel process so net_rdma_init() returns
        // immediately and the desktop (or other kernel modes) can continue.
        // The provider loop runs concurrently on a secondary core.
        int prov_pid = process_create_kernel(provider_loop, NULL);
        if (prov_pid >= 0) {
            uart_puts("[RDMA] Host Provider Loop spawned (PID ");
            print_int(prov_pid);
            uart_puts(")\n");
        } else {
            uart_puts("[RDMA] WARNING: Failed to spawn provider loop, falling back to synchronous\n");
            // Fallback: run synchronously (blocks — only reached if process creation fails)
            provider_loop(NULL);
        }
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

    uart_puts("[RDMA Guest] Sending request: op=");
    print_int(req->op);
    uart_puts(" tx_id=");
    print_int(req->tx_id);
    uart_puts(" size=");
    print_int(sizeof(struct rdma_packet));
    uart_puts("\n");
    
    net_socket_send(rdma_socket, req, sizeof(struct rdma_packet));
    uart_puts("[RDMA Guest] Request sent. Polling rx_head=");
    print_int(rdma_socket->rx_head);
    uart_puts(" rx_tail=");
    print_int(rdma_socket->rx_tail);
    uart_puts("\n");

    // Wait and poll for response
    uint64_t start = timer_get_ms();
    while (1) {
        net_rdma_poll();
        extern void virtio_net_handle_irq(void);
        virtio_net_handle_irq();

        if (guest_rx_ready) {
            uart_puts("[RDMA Guest] Ready packet tx_id=");
            print_int(guest_rx_packet.tx_id);
            uart_puts(" expected=");
            print_int(req->tx_id);
            uart_puts("\n");
            
            if (guest_rx_packet.tx_id == req->tx_id) {
                *resp = *(struct rdma_packet*)&guest_rx_packet;
                guest_rx_ready = 0;
                uart_puts("[RDMA] Transaction response received successfully!\n");
                return 0;
            }
            guest_rx_ready = 0; // Stale or different tx_id
        }

        if (timer_get_ms() - start > 2000) {
            uart_puts("[RDMA] Transaction Timeout! No response after 2 seconds.\n");
            return -1;
        }
        __asm__ volatile("pause");
    }
}

// Generalized Consumer Virtual PCI Driver API
uint32_t v_pci_read32(uint8_t bar, uint64_t offset) {
    struct rdma_packet req = {0}, resp = {0};
    req.op = RDMA_OP_READ_REQ;
    req.bar_index = bar;
    req.addr = offset;
    req.len = 4;
    req.status = 0;

    if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
        return 0xFFFFFFFF;
    }
    return *(uint32_t*)resp.data;
}

void v_pci_write32(uint8_t bar, uint64_t offset, uint32_t val) {
    struct rdma_packet req = {0}, resp = {0};
    req.op = RDMA_OP_WRITE_REQ;
    req.bar_index = bar;
    req.addr = offset;
    req.len = 4;
    req.status = 0;
    *(uint32_t*)req.data = val;

    rdma_transaction(&req, &resp);
}

uint64_t v_pci_read64(uint8_t bar, uint64_t offset) {
    struct rdma_packet req = {0}, resp = {0};
    req.op = RDMA_OP_READ_REQ;
    req.bar_index = bar;
    req.addr = offset;
    req.len = 8;
    req.status = 0;

    if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    return *(uint64_t*)resp.data;
}

void v_pci_write64(uint8_t bar, uint64_t offset, uint64_t val) {
    struct rdma_packet req = {0}, resp = {0};
    req.op = RDMA_OP_WRITE_REQ;
    req.bar_index = bar;
    req.addr = offset;
    req.len = 8;
    req.status = 0;
    *(uint64_t*)req.data = val;

    rdma_transaction(&req, &resp);
}

int v_pci_read_block(uint8_t bar, uint64_t offset, void* buf, uint32_t len) {
    uint8_t* ptr = (uint8_t*)buf;
    uint32_t remaining = len;
    uint64_t curr_offset = offset;

    while (remaining > 0) {
        uint32_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
        struct rdma_packet req = {0}, resp = {0};
        req.op = RDMA_OP_READ_BLOCK_REQ;
        req.bar_index = bar;
        req.addr = curr_offset;
        req.len = chunk;
        req.status = 0;

        if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
            return -1;
        }

        for (uint32_t i = 0; i < chunk; i++) {
            ptr[i] = resp.data[i];
        }

        ptr += chunk;
        curr_offset += chunk;
        remaining -= chunk;
    }
    return 0;
}

int v_pci_write_block(uint8_t bar, uint64_t offset, const void* buf, uint32_t len) {
    const uint8_t* ptr = (const uint8_t*)buf;
    uint32_t remaining = len;
    uint64_t curr_offset = offset;

    while (remaining > 0) {
        uint32_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
        struct rdma_packet req = {0}, resp = {0};
        req.op = RDMA_OP_WRITE_BLOCK_REQ;
        req.bar_index = bar;
        req.addr = curr_offset;
        req.len = chunk;
        req.status = 0;

        for (uint32_t i = 0; i < chunk; i++) {
            req.data[i] = ptr[i];
        }

        if (rdma_transaction(&req, &resp) != 0 || resp.status != 0) {
            return -1;
        }

        ptr += chunk;
        curr_offset += chunk;
        remaining -= chunk;
    }
    return 0;
}

// Consumer APIs to access emulated registers (EDU backward compatibility wrappers)
uint32_t v_edu_read32(uint32_t offset) {
    return v_pci_read32(0, offset);
}

void v_edu_write32(uint32_t offset, uint32_t val) {
    v_pci_write32(0, offset, val);
}

uint64_t v_edu_read64(uint32_t offset) {
    return v_pci_read64(0, offset);
}

void v_edu_write64(uint32_t offset, uint64_t val) {
    v_pci_write64(0, offset, val);
}

// Memory Region Registration APIs
int rdma_register_mr(uint64_t guest_phys, uint32_t size) {
    struct rdma_packet req = {0}, resp = {0};
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
        struct rdma_packet req = {0}, resp = {0};
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
        struct rdma_packet req = {0}, resp = {0};
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

// Reflected CRC32 (poly 0xEDB88320, init/final 0xFFFFFFFF) — matches the host's
// RDMA_OP_DMA_SYNC_RELIABLE handler and the net_pci_client crc32_buf(). All three
// implementations MUST stay byte-identical or DMA verification breaks silently; the
// golden-vector unit test (net_rdma_test.c) guards this.
uint32_t net_rdma_crc32(const uint8_t* p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

// Verify that the host's copy of a DMA region matches the local buffer byte-for-byte,
// using the host's CRC32 (RDMA_OP_DMA_SYNC_RELIABLE). The host stores DMA_SYNC_TO_HOST
// data at physical address == guest_phys (identity), so we verify the same address.
// Returns 0 if the host matches, 1 on CRC mismatch, -1 on transport error.
int rdma_dma_verify(uint64_t guest_phys, uint32_t size) {
    uint32_t want = net_rdma_crc32((const uint8_t*)guest_phys, size);
    struct rdma_packet req = {0}, resp = {0};
    req.op = RDMA_OP_DMA_SYNC_RELIABLE;
    req.addr = guest_phys;
    req.len = size;
    *(uint32_t*)req.data = want;
    if (rdma_transaction(&req, &resp) != 0) {
        return -1;
    }
    return (resp.status == 0) ? 0 : 1;
}

#else

#include "net_rdma.h"
void net_rdma_init(void) {}
void net_rdma_poll(void) {}
uint32_t v_pci_read32(uint8_t bar, uint64_t offset) { (void)bar; (void)offset; return 0xFFFFFFFF; }
void v_pci_write32(uint8_t bar, uint64_t offset, uint32_t val) { (void)bar; (void)offset; (void)val; }
uint64_t v_pci_read64(uint8_t bar, uint64_t offset) { (void)bar; (void)offset; return 0xFFFFFFFFFFFFFFFFULL; }
void v_pci_write64(uint8_t bar, uint64_t offset, uint64_t val) { (void)bar; (void)offset; (void)val; }
int v_pci_read_block(uint8_t bar, uint64_t offset, void* buf, uint32_t len) { (void)bar; (void)offset; (void)buf; (void)len; return -1; }
int v_pci_write_block(uint8_t bar, uint64_t offset, const void* buf, uint32_t len) { (void)bar; (void)offset; (void)buf; (void)len; return -1; }
uint32_t v_edu_read32(uint32_t offset) { (void)offset; return 0xFFFFFFFF; }
void v_edu_write32(uint32_t offset, uint32_t val) { (void)offset; (void)val; }
uint64_t v_edu_read64(uint32_t offset) { (void)offset; return 0xFFFFFFFFFFFFFFFFULL; }
void v_edu_write64(uint32_t offset, uint64_t val) { (void)offset; (void)val; }
int rdma_register_mr(uint64_t guest_phys, uint32_t size) { (void)guest_phys; (void)size; return -1; }
uint64_t guest_to_host_phys(uint64_t guest_phys) { return guest_phys; }
int rdma_dma_sync(uint64_t guest_phys, uint32_t size, int to_device) { (void)guest_phys; (void)size; (void)to_device; return -1; }
int rdma_dma_verify(uint64_t guest_phys, uint32_t size) { (void)guest_phys; (void)size; return -1; }
uint32_t net_rdma_crc32(const uint8_t* p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

#endif
