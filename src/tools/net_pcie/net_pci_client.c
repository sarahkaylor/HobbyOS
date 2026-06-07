#define _GNU_SOURCE
/*
 * net_pci_client.c
 *
 * A virtual PCIe device daemon using libvfio-user. It exposes a virtual PCIe device
 * to a QEMU guest VM, and forwards all BAR accesses over UDP/IP RDMA to a remote
 * HobbyOS instance acting as the physical PCIe host.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <poll.h>
#include <err.h>
#include <sys/time.h>

#include "libvfio-user.h"

// -------------------------------------------------------------
// RDMA Protocol Definitions (network compatible with HobbyOS)
// -------------------------------------------------------------

#define RDMA_PORT          7777

enum rdma_op {
    RDMA_OP_REG_MR = 1,
    RDMA_OP_UNREG_MR = 2,
    RDMA_OP_READ_REQ = 3,
    RDMA_OP_READ_RESP = 4,
    RDMA_OP_WRITE_REQ = 5,
    RDMA_OP_WRITE_RESP = 6,
    RDMA_OP_DMA_SYNC_TO_HOST = 7,
    RDMA_OP_DMA_SYNC_TO_GUEST = 8,
    RDMA_OP_DMA_SYNC_RESP = 9,
    RDMA_OP_READ_BLOCK_REQ = 10,
    RDMA_OP_READ_BLOCK_RESP = 11,
    RDMA_OP_WRITE_BLOCK_REQ = 12,
    RDMA_OP_WRITE_BLOCK_RESP = 13,
    RDMA_OP_IOMMU_MAP = 14,
    RDMA_OP_IOMMU_MAP_RESP = 15,
    RDMA_OP_IOMMU_UNMAP = 16,
    RDMA_OP_IOMMU_UNMAP_RESP = 17,
    RDMA_OP_DMA_SYNC_RELIABLE = 18,
    RDMA_OP_DMA_SYNC_RELIABLE_RESP = 19,
};

#define RDMA_DATA_LEN 1024

struct rdma_packet {
    uint32_t op;               // enum rdma_op
    uint32_t tx_id;            // Transaction ID to match req/resp
    uint64_t addr;             // Target offset / physical address
    uint32_t len;              // Data length
    uint32_t status;           // 0 on success, non-zero on failure
    uint8_t  bar_index;        // PCI BAR index (0-5) to target
    uint8_t  reserved[3];      // Alignment/padding
    uint8_t  data[RDMA_DATA_LEN]; // Data payload
} __attribute__((packed));

// -------------------------------------------------------------
// Global Network & Context State
// -------------------------------------------------------------

static int sock_fd = -1;
static int blast_sock_fd = -1;  // Dedicated socket for fire-and-forget DMA blast sync
static int rdma_rpc_sock_fd = -1; // Dedicated socket for request-response RDMA transactions
static struct sockaddr_in host_addr;
static uint32_t next_tx_id = 1;
static volatile bool running = true;
static volatile bool g_connected = false;
static volatile bool irq_thread_running = false;

// Blast sync thread state
struct blast_sync_request {
    uint64_t gpa;
    size_t len;
    uint8_t *data;       // Snapshot of firmware data to send
    bool done;
    vfu_ctx_t *vfu_ctx;
    uint32_t write_val;
    uint64_t bar_off;
    struct blast_sync_request *next;
};

static pthread_t blast_thread;
static volatile bool blast_thread_active = false;
static struct blast_sync_request *blast_queue_head = NULL;
static struct blast_sync_request *blast_queue_tail = NULL;
static pthread_mutex_t blast_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blast_queue_cond = PTHREAD_COND_INITIALIZER;

static volatile int pending_blast_count = 0;
static pthread_mutex_t blast_complete_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blast_complete_cond = PTHREAD_COND_INITIALIZER;

// Pending GPA registration queue — bar_access_cb queues GPAs here,
// irq_thread processes them (registers MR, does blast sync).
#define MAX_PENDING_GPA 16
static struct {
    uint64_t gpa;
    size_t   len;
    uint64_t bar_offset;   // Which BAR register triggered this (0x88080, 0x100cb8, etc)
    bool     is_pfn;       // true=PFN format (val<<12), false=direct address
    bool     pending;
    bool     mr_registered;
    uint64_t host_phys;
} g_pending_gpa[MAX_PENDING_GPA];
static pthread_mutex_t pending_gpa_mutex = PTHREAD_MUTEX_INITIALIZER;

static void queue_gpa_registration(uint64_t gpa, size_t len, uint64_t bar_offset, bool is_pfn) {
    pthread_mutex_lock(&pending_gpa_mutex);
    for (int i = 0; i < MAX_PENDING_GPA; i++) {
        if (g_pending_gpa[i].pending && g_pending_gpa[i].gpa == gpa) {
            pthread_mutex_unlock(&pending_gpa_mutex);
            return; // Already queued
        }
    }
    for (int i = 0; i < MAX_PENDING_GPA; i++) {
        if (!g_pending_gpa[i].pending) {
            g_pending_gpa[i].gpa = gpa;
            g_pending_gpa[i].len = len;
            g_pending_gpa[i].bar_offset = bar_offset;
            g_pending_gpa[i].is_pfn = is_pfn;
            g_pending_gpa[i].pending = true;
            g_pending_gpa[i].mr_registered = false;
            g_pending_gpa[i].host_phys = 0;
            printf("QUEUED GPA REG: gpa=%#llx len=%zu bar_offset=%#llx is_pfn=%d\n",
                   (unsigned long long)gpa, len, (unsigned long long)bar_offset, is_pfn);
            pthread_mutex_unlock(&pending_gpa_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&pending_gpa_mutex);
    printf("WARNING: Pending GPA queue full, cannot queue gpa=%#llx\n", (unsigned long long)gpa);
}

// GPA-to-Host-Physical translation table
// When the guest sets up GPU DMA addresses, we need to replace the guest's GPA
// with the host's shadow buffer address so the GPU's DMA goes to the right place.
#define MAX_GPA_XLAT 32
static struct {
    uint64_t guest_gpa;    // Guest physical address
    uint64_t host_phys;    // Host shadow buffer physical address
    size_t   size;
    bool     in_use;
} g_gpa_xlat[MAX_GPA_XLAT];
static pthread_mutex_t gpa_xlat_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t gpa_xlat_lookup(uint64_t guest_gpa) {
    pthread_mutex_lock(&gpa_xlat_mutex);
    for (int i = 0; i < MAX_GPA_XLAT; i++) {
        if (g_gpa_xlat[i].in_use && g_gpa_xlat[i].guest_gpa == guest_gpa) {
            uint64_t host = g_gpa_xlat[i].host_phys;
            pthread_mutex_unlock(&gpa_xlat_mutex);
            return host;
        }
    }
    pthread_mutex_unlock(&gpa_xlat_mutex);
    return 0;
}

static void gpa_xlat_register(uint64_t guest_gpa, uint64_t host_phys, size_t size) {
    pthread_mutex_lock(&gpa_xlat_mutex);
    for (int i = 0; i < MAX_GPA_XLAT; i++) {
        if (g_gpa_xlat[i].in_use && g_gpa_xlat[i].guest_gpa == guest_gpa) {
            g_gpa_xlat[i].host_phys = host_phys;
            g_gpa_xlat[i].size = size;
            pthread_mutex_unlock(&gpa_xlat_mutex);
            return;
        }
    }
    for (int i = 0; i < MAX_GPA_XLAT; i++) {
        if (!g_gpa_xlat[i].in_use) {
            g_gpa_xlat[i].guest_gpa = guest_gpa;
            g_gpa_xlat[i].host_phys = host_phys;
            g_gpa_xlat[i].size = size;
            g_gpa_xlat[i].in_use = true;
            pthread_mutex_unlock(&gpa_xlat_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&gpa_xlat_mutex);
}

// IOMMU mode flag — when enabled, DMA callbacks forward IOVA mappings to host
static bool g_iommu_mode = false;

// IOVA Tracking Table: maps guest IOVAs to host shadow buffer addresses
#define MAX_IOVA_MAPS 4096
static struct {
    uint64_t iova;       // Guest IOVA (from vIOMMU DMA MAP)
    uint64_t host_phys;  // Host shadow buffer physical address (from RDMA response)
    void    *vaddr;      // Guest-side virtual address for reading DMA data
    size_t   size;
    bool     active;
    bool     synced;     // true if initial data has been synced to host
} g_iova_maps[MAX_IOVA_MAPS];
static pthread_mutex_t iova_maps_mutex = PTHREAD_MUTEX_INITIALIZER;


// Signal & Log Helpers
// -------------------------------------------------------------

static void _log(vfu_ctx_t *vfu_ctx, int level, char const *msg) {
    fprintf(stderr, "net_pci_client[%d]: %s\n", getpid(), msg);
}

static void sig_handler(int sig) {
    running = false;
}

static pthread_mutex_t rdma_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t dma_sync_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_DMA_REGIONS 64

struct dma_region {
    uint64_t iova;       // Guest physical address
    void *vaddr;         // Local virtual address in net_pci_client
    size_t len;          // Length of the region
    uint8_t *cache;      // Local cache copy of the data
    bool in_use;
};

static struct dma_region g_dma_regions[MAX_DMA_REGIONS];

static void* map_dma_region(vfu_ctx_t *vfu_ctx, uint64_t iova, size_t len, dma_sg_t **out_sg, struct iovec *out_iov) {
    dma_sg_t *sg = malloc(dma_sg_size());
    if (sg == NULL) return NULL;
    
    int ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)iova, len, sg, 1, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        free(sg);
        return NULL;
    }
    
    ret = vfu_sgl_get(vfu_ctx, sg, out_iov, 1, 0);
    if (ret < 0) {
        free(sg);
        return NULL;
    }
    
    *out_sg = sg;
    return out_iov->iov_base;
}

static void unmap_dma_region(vfu_ctx_t *vfu_ctx, dma_sg_t *sg, struct iovec *iov) {
    if (sg != NULL) {
        vfu_sgl_put(vfu_ctx, sg, iov, 1);
        free(sg);
    }
}

static void register_active_dma(uint64_t gpa, size_t len, bool is_write);

static void dma_register_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info) {
    if (info == NULL) return;
    
    uint64_t iova = (uint64_t)info->iova.iov_base;
    size_t len = info->iova.iov_len;

    // In IOMMU mode, accept ALL DMA mappings (these are IOVA->GPA from the vIOMMU)
    if (g_iommu_mode) {
        printf("IOMMU DMA REGISTER: iova=%#llx, vaddr=%p, len=%zu\n",
               (unsigned long long)iova, info->vaddr, len);

        // Record in IOVA tracking table
        pthread_mutex_lock(&iova_maps_mutex);
        for (int i = 0; i < MAX_IOVA_MAPS; i++) {
            if (!g_iova_maps[i].active) {
                g_iova_maps[i].iova = iova;
                g_iova_maps[i].host_phys = 0;  // Will be filled by RDMA response
                g_iova_maps[i].vaddr = info->vaddr;
                g_iova_maps[i].size = len;
                g_iova_maps[i].active = true;
                g_iova_maps[i].synced = false;
                break;
            }
        }
        pthread_mutex_unlock(&iova_maps_mutex);

        // Also add to legacy DMA regions table (for vfu_addr_to_sgl lookups)
        pthread_mutex_lock(&dma_sync_mutex);
        for (int i = 0; i < MAX_DMA_REGIONS; i++) {
            if (!g_dma_regions[i].in_use) {
                g_dma_regions[i].iova = iova;
                g_dma_regions[i].vaddr = info->vaddr;
                g_dma_regions[i].len = len;
                g_dma_regions[i].cache = NULL;
                g_dma_regions[i].in_use = true;
                break;
            }
        }
        pthread_mutex_unlock(&dma_sync_mutex);

        // Auto-register small/medium IOMMU regions as active DMAs for sync-back.
        // These are typically GSP command/response queues, event buffers, etc.
        // Skip very large regions (RAM, BAR space) — they're too big for continuous sync.
        if (len > 0 && len <= 4 * 1024 * 1024 && info->vaddr != NULL) {
            register_active_dma(iova, len, true);
            printf("IOMMU AUTO-ACTIVE: iova=%#llx len=%zu (auto-registered for sync-back)\n",
                   (unsigned long long)iova, len);
        }
        return;
    }

    // Legacy (non-IOMMU) path below:
    // Ignore any small DMA regions below 4 KB (NULL pointer guard)
    if (iova < 0x1000 && len < 128 * 1024 * 1024) {
        printf("DMA REGISTER: Ignore low BIOS/ROM mapping: iova=%p, len=%zu\n",
               info->iova.iov_base, len);
        return;
    }
    
    // Ignore any DMA regions starting at 4 GB and above (upper 64-bit RAM zone)
    if (iova >= 4096ULL * 1024 * 1024) {
        printf("DMA REGISTER: Ignore 64-bit RAM mapping: iova=%p, len=%zu\n",
               info->iova.iov_base, len);
        return;
    }
    
    printf("DMA REGISTER: iova=%p, vaddr=%p, len=%zu\n", 
           info->iova.iov_base, info->vaddr, len);
           
    pthread_mutex_lock(&dma_sync_mutex);
    // Add to table
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (!g_dma_regions[i].in_use) {
            g_dma_regions[i].iova = iova;
            g_dma_regions[i].vaddr = info->vaddr;
            g_dma_regions[i].len = len;
            // Only allocate cache for small regions (<= 256KB) to save memory and avoid giant allocs
            if (len > 0 && len <= 262144) {
                g_dma_regions[i].cache = malloc(len);
                if (g_dma_regions[i].cache != NULL) {
                    if (info->vaddr != NULL) {
                        memcpy(g_dma_regions[i].cache, info->vaddr, len);
                    } else {
                        dma_sg_t *sg = NULL;
                        struct iovec iov;
                        void *temp_vaddr = map_dma_region(vfu_ctx, iova, len, &sg, &iov);
                        if (temp_vaddr != NULL) {
                            memcpy(g_dma_regions[i].cache, temp_vaddr, len);
                            unmap_dma_region(vfu_ctx, sg, &iov);
                        } else {
                            memset(g_dma_regions[i].cache, 0, len);
                        }
                    }
                }
            } else {
                g_dma_regions[i].cache = NULL;
            }
            g_dma_regions[i].in_use = true;
            break;
        }
    }
    pthread_mutex_unlock(&dma_sync_mutex);
}

// Forward declaration (do_rdma_transaction is defined later in the file)
static int do_rdma_transaction(vfu_ctx_t *vfu_ctx, struct rdma_packet *req, struct rdma_packet *resp);

static void dma_unregister_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info) {
    if (info == NULL) return;
    
    uint64_t iova = (uint64_t)info->iova.iov_base;
    size_t len = info->iova.iov_len;

    if (g_iommu_mode) {
        printf("IOMMU DMA UNREGISTER: iova=%#llx, len=%zu\n",
               (unsigned long long)iova, len);

        // Fire-and-forget IOMMU UNMAP — don't block callback thread with rdma_mutex
        struct rdma_packet req = {0};
        req.op = RDMA_OP_IOMMU_UNMAP;
        req.addr = iova;
        req.len = (uint32_t)len;
        sendto(blast_sock_fd >= 0 ? blast_sock_fd : sock_fd,
               &req, sizeof(req), 0,
               (struct sockaddr*)&host_addr, sizeof(host_addr));

        // Remove from IOVA tracking table
        pthread_mutex_lock(&iova_maps_mutex);
        for (int i = 0; i < MAX_IOVA_MAPS; i++) {
            if (g_iova_maps[i].active && g_iova_maps[i].iova == iova) {
                g_iova_maps[i].active = false;
                break;
            }
        }
        pthread_mutex_unlock(&iova_maps_mutex);

        // Also remove from legacy DMA regions table
        pthread_mutex_lock(&dma_sync_mutex);
        for (int i = 0; i < MAX_DMA_REGIONS; i++) {
            if (g_dma_regions[i].in_use && g_dma_regions[i].iova == iova) {
                if (g_dma_regions[i].cache != NULL) {
                    free(g_dma_regions[i].cache);
                    g_dma_regions[i].cache = NULL;
                }
                g_dma_regions[i].in_use = false;
                break;
            }
        }
        pthread_mutex_unlock(&dma_sync_mutex);
        return;
    }

    // Legacy (non-IOMMU) path:
    if (iova < 0x1000 && len < 128 * 1024 * 1024) {
        return;
    }
    if (iova >= 4096ULL * 1024 * 1024) {
        return;
    }
    
    printf("DMA UNREGISTER: iova=%p, vaddr=%p, len=%zu\n", 
           info->iova.iov_base, info->vaddr, len);
           
    pthread_mutex_lock(&dma_sync_mutex);
    // Remove from table
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use && g_dma_regions[i].iova == iova) {
            if (g_dma_regions[i].cache != NULL) {
                free(g_dma_regions[i].cache);
                g_dma_regions[i].cache = NULL;
            }
            g_dma_regions[i].in_use = false;
            break;
        }
    }
    pthread_mutex_unlock(&dma_sync_mutex);
}

static bool is_valid_gpa(uint64_t gpa) {
    bool valid = false;
    pthread_mutex_lock(&dma_sync_mutex);
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use) {
            if (gpa >= g_dma_regions[i].iova && gpa < (g_dma_regions[i].iova + g_dma_regions[i].len)) {
                valid = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&dma_sync_mutex);
    return valid;
}

static size_t get_dma_region_remaining_len(uint64_t gpa) {
    size_t len = 0;
    pthread_mutex_lock(&dma_sync_mutex);
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use) {
            if (gpa >= g_dma_regions[i].iova && gpa < (g_dma_regions[i].iova + g_dma_regions[i].len)) {
                len = (g_dma_regions[i].iova + g_dma_regions[i].len) - gpa;
                break;
            }
        }
    }
    pthread_mutex_unlock(&dma_sync_mutex);
    return len;
}

// Core BAR RDMA Network Callback forward declaration
static int do_rdma_transaction(vfu_ctx_t *vfu_ctx, struct rdma_packet *req, struct rdma_packet *resp);

#define MAX_ACTIVE_DMAS 256
struct active_dma {
    uint64_t gpa;
    size_t len;
    bool is_write;
    uint8_t *cache;
    bool first_sync_done; // Optimization: tracking flag for large buffers
};
static struct active_dma g_active_dmas[MAX_ACTIVE_DMAS];
static int g_active_dmas_count = 0;

static void register_active_dma(uint64_t gpa, size_t len, bool is_write) {
    pthread_mutex_lock(&dma_sync_mutex);
    size_t alloc_len = (len > 96 * 1024 * 1024) ? (96 * 1024 * 1024) : len;
    for (int i = 0; i < g_active_dmas_count; i++) {
        if (g_active_dmas[i].gpa == gpa) {
            if (alloc_len > g_active_dmas[i].len) {
                uint8_t *new_cache = realloc(g_active_dmas[i].cache, alloc_len);
                if (new_cache != NULL) {
                    memset(new_cache + g_active_dmas[i].len, 0, alloc_len - g_active_dmas[i].len);
                    g_active_dmas[i].cache = new_cache;
                }
                g_active_dmas[i].len = alloc_len;
                g_active_dmas[i].first_sync_done = false; // Reset sync status since the buffer grew!
            }
            g_active_dmas[i].is_write |= is_write;
            pthread_mutex_unlock(&dma_sync_mutex);
            return;
        }
    }
    if (g_active_dmas_count < MAX_ACTIVE_DMAS) {
        g_active_dmas[g_active_dmas_count].gpa = gpa;
        g_active_dmas[g_active_dmas_count].len = alloc_len;
        g_active_dmas[g_active_dmas_count].is_write = is_write;
        g_active_dmas[g_active_dmas_count].first_sync_done = false; // Initialize to false
        g_active_dmas[g_active_dmas_count].cache = malloc(alloc_len);
        if (g_active_dmas[g_active_dmas_count].cache != NULL) {
            memset(g_active_dmas[g_active_dmas_count].cache, 0, alloc_len);
        }
        g_active_dmas_count++;
        printf("REGISTER ACTIVE DMA: gpa=%#llx, len=%zu (original=%zu), is_write=%d\n",
               (unsigned long long)gpa, alloc_len, len, is_write);
    }
    pthread_mutex_unlock(&dma_sync_mutex);
}

// -------------------------------------------------------------------------
// Blast Sync Thread: Fire-and-forget DMA sync for large firmware buffers.
// Uses the main socket (sock_fd) — safe because the host sends NO response
// for DMA_SYNC_TO_HOST, so the main socket's receive path stays clean.
// -------------------------------------------------------------------------
static void *blast_sync_worker(void *arg) {
    (void)arg;
    uint32_t blast_tx_id = 1000000; // Separate tx_id range for blast
    
    while (running) {
        pthread_mutex_lock(&blast_queue_mutex);
        while (blast_queue_head == NULL && running) {
            pthread_cond_wait(&blast_queue_cond, &blast_queue_mutex);
        }
        if (!running) {
            pthread_mutex_unlock(&blast_queue_mutex);
            break;
        }
        struct blast_sync_request *req = blast_queue_head;
        blast_queue_head = req->next;
        if (blast_queue_head == NULL) blast_queue_tail = NULL;
        pthread_mutex_unlock(&blast_queue_mutex);
        
        printf("BLAST SYNC START: gpa=%#llx len=%zu\n",
               (unsigned long long)req->gpa, req->len);
        
        int tx_fd = (blast_sock_fd >= 0) ? blast_sock_fd : sock_fd;
        int verified = 0;
        int attempt_num = 0;
        
        while (!verified && attempt_num < 3 && running) {
            attempt_num++;
            size_t remaining = req->len;
            size_t offset = 0;
            int pkt_count = 0;
            // Per-packet delay: 200μs for first attempt (~20s for 98K pkts),
            // 500μs for retries (~49s) to ensure zero packet loss.
            int pkt_delay_us = (attempt_num > 1) ? 500 : 200;
            
            // Phase 1: Fire-and-forget blast with per-packet rate limiting
            while (remaining > 0 && running) {
                size_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
                struct rdma_packet pkt = {0};
                pkt.op = RDMA_OP_DMA_SYNC_TO_HOST;
                pkt.tx_id = blast_tx_id++;
                pkt.addr = req->gpa + offset;
                pkt.len = chunk;
                memcpy(pkt.data, &req->data[offset], chunk);
                
                sendto(tx_fd, &pkt, sizeof(pkt), 0,
                       (struct sockaddr*)&host_addr, sizeof(host_addr));
                pkt_count++;
                usleep(pkt_delay_us);
                offset += chunk;
                remaining -= chunk;
            }
            
            printf("BLAST SYNC SENT: attempt=%d gpa=%#llx sent %d packets (%zu bytes)\n",
                   attempt_num, (unsigned long long)req->gpa, pkt_count, req->len);
            
            // Phase 2: Compute local CRC32 and send VERIFY request
            // Simple CRC32 over the data we sent
            uint32_t crc = 0xFFFFFFFF;
            for (size_t i = 0; i < req->len; i++) {
                crc ^= req->data[i];
                for (int b = 0; b < 8; b++) {
                    crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
                }
            }
            crc ^= 0xFFFFFFFF;
            
            // Allow host time to process ALL remaining queued packets
            // before computing CRC. The host rx_buf may still contain
            // thousands of packets being drained by the provider loop.
            usleep(3000000); // 3s drain time
            
            // Send verify request using the reliable opcode
            // Pack: addr = target GPA, len = total size, data[0..3] = expected CRC32
            struct rdma_packet verify = {0};
            struct rdma_packet verify_resp = {0};
            verify.op = RDMA_OP_DMA_SYNC_RELIABLE;
            verify.tx_id = next_tx_id++;
            verify.addr = req->gpa;
            verify.len = req->len;
            *(uint32_t*)verify.data = crc;
            
            // Use do_rdma_transaction for proper serialization with irq_thread.
            // It handles rdma_mutex, tx_id matching, retries, and timeouts.
            // NOTE: do_rdma_transaction returns -1 for non-zero status (CRC mismatch),
            // so we check verify_resp.op to differentiate from real errors.
            int verify_ret = do_rdma_transaction(req->vfu_ctx, &verify, &verify_resp);
            if (verify_ret == 0) {
                // status == 0 means CRC match
                verified = 1;
                printf("BLAST SYNC VERIFIED: gpa=%#llx CRC=%#x match!\n",
                       (unsigned long long)req->gpa, crc);
            } else if (verify_resp.op == RDMA_OP_DMA_SYNC_RELIABLE_RESP && verify_resp.status == 1) {
                // CRC mismatch — host returned its computed CRC
                printf("BLAST SYNC CRC MISMATCH: gpa=%#llx expected=%#x host=%#x — retrying\n",
                       (unsigned long long)req->gpa, crc, *(uint32_t*)verify_resp.data);
            } else {
                printf("BLAST SYNC VERIFY TRANSACTION FAILED: gpa=%#llx ret=%d\n",
                       (unsigned long long)req->gpa, verify_ret);
            }
            
            if (!verified) {
                printf("BLAST SYNC VERIFY TIMEOUT: attempt=%d — retrying full blast\n", attempt_num);
            }
        }
        
        if (!verified) {
            printf("BLAST SYNC FAILED: gpa=%#llx after %d attempts — sending doorbell anyway\n",
                   (unsigned long long)req->gpa, attempt_num);
        }
        
        printf("BLAST SYNC DONE: gpa=%#llx (%s)\n",
               (unsigned long long)req->gpa, verified ? "VERIFIED" : "UNVERIFIED");
        
        // Now that ALL DMA data is on the host, send the deferred BAR write
        // (doorbell) to the GPU. This was suppressed in bar_access_cb to
        // prevent the GPU from reading DMA data before it was transferred.
        if (req->bar_off != 0) {
            struct rdma_packet doorbell = {0};
            doorbell.op = RDMA_OP_WRITE_REQ;
            doorbell.bar_index = 0;
            doorbell.addr = req->bar_off;
            doorbell.len = 4;
            *(uint32_t*)doorbell.data = req->write_val;
            sendto(tx_fd, &doorbell, sizeof(doorbell), 0,
                   (struct sockaddr*)&host_addr, sizeof(host_addr));
            printf("BLAST SYNC: Sent deferred BAR write: offset=%#llx val=%#x\n",
                   (unsigned long long)req->bar_off, req->write_val);
        }
        printf("BLAST SYNC COMPLETE: gpa=%#llx\n",
               (unsigned long long)req->gpa);
        
        pthread_mutex_lock(&blast_complete_mutex);
        pending_blast_count--;
        pthread_cond_broadcast(&blast_complete_cond);
        pthread_mutex_unlock(&blast_complete_mutex);
        
        free(req->data);
        free(req);
    }
    return NULL;
}

static void trigger_blast_sync(vfu_ctx_t *vfu_ctx, uint64_t gpa, size_t len, uint32_t write_val, uint64_t bar_off) {
    if (sock_fd < 0) return;
    
    // Snapshot the guest's DMA data using pre-mapped vaddr only.
    // We MUST NOT call map_dma_region() here because this runs inside bar_access_cb,
    // and calling vfu_sgl_get/vfu_addr_to_sgl from within a callback deadlocks libvfio-user.
    uint8_t *snapshot = malloc(len);
    if (!snapshot) return;
    
    bool found = false;
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use &&
            gpa >= g_dma_regions[i].iova &&
            gpa < (g_dma_regions[i].iova + g_dma_regions[i].len)) {
            size_t start_offset = gpa - g_dma_regions[i].iova;
            uint8_t *vaddr = (uint8_t*)g_dma_regions[i].vaddr;
            
            if (vaddr == NULL) {
                // Cannot safely map from within bar_access_cb — skip blast sync.
                // The irq_thread's sync_dma_to_host will handle this safely.
                printf("BLAST SYNC DEFERRED: gpa=%#llx vaddr=NULL (will sync via irq_thread)\n",
                       (unsigned long long)gpa);
                free(snapshot);
                return;
            }
            memcpy(snapshot, vaddr + start_offset, len);
            found = true;
            break;
        }
    }
    if (!found) {
        free(snapshot);
        return;
    }
    
    struct blast_sync_request *req = malloc(sizeof(struct blast_sync_request));
    if (!req) { free(snapshot); return; }
    req->gpa = gpa;
    req->len = len;
    req->data = snapshot;
    req->done = false;
    req->vfu_ctx = vfu_ctx;
    req->write_val = write_val;
    req->bar_off = bar_off;
    req->next = NULL;
    
    pthread_mutex_lock(&blast_complete_mutex);
    pending_blast_count++;
    pthread_mutex_unlock(&blast_complete_mutex);
    
    pthread_mutex_lock(&blast_queue_mutex);
    if (blast_queue_tail) {
        blast_queue_tail->next = req;
    } else {
        blast_queue_head = req;
    }
    blast_queue_tail = req;
    pthread_cond_signal(&blast_queue_cond);
    pthread_mutex_unlock(&blast_queue_mutex);
}

static void sync_dma_to_host(vfu_ctx_t *vfu_ctx, bool sync_large) {
    pthread_mutex_lock(&dma_sync_mutex);
    
    // 1. Sync small regions that have a local cache
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use && g_dma_regions[i].len > 0 && g_dma_regions[i].cache != NULL) {
            uint64_t iova = g_dma_regions[i].iova;
            size_t len = g_dma_regions[i].len;
            uint8_t *cache = g_dma_regions[i].cache;
            
            uint8_t *vaddr = (uint8_t*)g_dma_regions[i].vaddr;
            dma_sg_t *sg = NULL;
            struct iovec iov;
            bool was_mapped = false;
            
            if (vaddr == NULL) {
                vaddr = (uint8_t*)map_dma_region(vfu_ctx, iova, len, &sg, &iov);
                if (vaddr == NULL) {
                    continue;
                }
                was_mapped = true;
            }
            
            if (memcmp(vaddr, cache, len) != 0) {
                size_t remaining = len;
                size_t offset = 0;
                while (remaining > 0) {
                    size_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
                    if (memcmp(&vaddr[offset], &cache[offset], chunk) != 0) {
                        memcpy(&cache[offset], &vaddr[offset], chunk);
                        struct rdma_packet req = {0};
                        req.op = RDMA_OP_DMA_SYNC_TO_HOST;
                        req.tx_id = next_tx_id++;
                        req.addr = iova + offset;
                        req.len = chunk;
                        memcpy(req.data, &vaddr[offset], chunk);
                        // Fire-and-forget: host processes silently, no response
                        sendto(sock_fd, &req, sizeof(req), 0,
                               (struct sockaddr*)&host_addr, sizeof(host_addr));
                    }
                    offset += chunk;
                    remaining -= chunk;
                }
            }
            
            if (was_mapped) {
                unmap_dma_region(vfu_ctx, sg, &iov);
            }
        }
    }
    
    // 2. Sync active sub-ranges within uncached large regions
    for (int k = 0; k < g_active_dmas_count; k++) {
        uint64_t gpa = g_active_dmas[k].gpa;
        size_t len = g_active_dmas[k].len;
        uint8_t *cache = g_active_dmas[k].cache;
        
        if (cache == NULL) continue;
        
        // Skip large regions if they have already been synced once or if sync_large is false
        if (len > 256 * 1024) {
            if (!sync_large || g_active_dmas[k].first_sync_done) {
                continue;
            }
        }
        
        // Find which DMA region contains this active GPA
        for (int i = 0; i < MAX_DMA_REGIONS; i++) {
            if (g_dma_regions[i].in_use && g_dma_regions[i].cache == NULL) {
                if (gpa >= g_dma_regions[i].iova && (gpa + len) <= (g_dma_regions[i].iova + g_dma_regions[i].len)) {
                    uint8_t *vaddr = (uint8_t*)g_dma_regions[i].vaddr;
                    dma_sg_t *sg = NULL;
                    struct iovec iov;
                    bool was_mapped = false;
                    
                    if (vaddr == NULL) {
                        vaddr = (uint8_t*)map_dma_region(vfu_ctx, g_dma_regions[i].iova, g_dma_regions[i].len, &sg, &iov);
                        if (vaddr == NULL) {
                            continue;
                        }
                        was_mapped = true;
                    }
                    
                    size_t start_offset = gpa - g_dma_regions[i].iova;
                    uint8_t *gpa_vaddr = vaddr + start_offset;
                    
                    if (memcmp(gpa_vaddr, cache, len) != 0) {
                        size_t remaining = len;
                        size_t offset = 0;
                        int pkt_count = 0;
                        while (remaining > 0) {
                            size_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
                            if (memcmp(&gpa_vaddr[offset], &cache[offset], chunk) != 0) {
                                memcpy(&cache[offset], &gpa_vaddr[offset], chunk);
                                struct rdma_packet req = {0};
                                req.op = RDMA_OP_DMA_SYNC_TO_HOST;
                                req.tx_id = next_tx_id++;
                                req.addr = gpa + offset;
                                req.len = chunk;
                                memcpy(req.data, &gpa_vaddr[offset], chunk);
                                // Fire-and-forget: no response expected, no mutex dance needed
                                sendto(sock_fd, &req, sizeof(req), 0,
                                       (struct sockaddr*)&host_addr, sizeof(host_addr));
                                pkt_count++;
                            }
                            offset += chunk;
                            remaining -= chunk;
                        }
                        if (pkt_count > 0) {
                            printf("DMA SYNC TO HOST: gpa=%#llx sent %d chunks (%zu bytes)\n",
                                   (unsigned long long)gpa, pkt_count, len);
                        }
                    }
                    
                    g_active_dmas[k].first_sync_done = true; // Mark large region sync as completed!
                    
                    if (was_mapped) {
                        unmap_dma_region(vfu_ctx, sg, &iov);
                    }
                    break;
                }
            }
        }
    }
    
    pthread_mutex_unlock(&dma_sync_mutex);
}

static void sync_dma_from_host(vfu_ctx_t *vfu_ctx, bool sync_large) {
    pthread_mutex_lock(&dma_sync_mutex);
    
    // 1. Sync all cached regions that have a local cache
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use && g_dma_regions[i].len > 0 && g_dma_regions[i].cache != NULL) {
            uint64_t iova = g_dma_regions[i].iova;
            size_t len = g_dma_regions[i].len;
            
            // Only sync this cached region if it overlaps with an ACTIVE GPU DMA region.
            // This prevents syncing read-only ROM regions (like 0xc0000) and segfaulting on memcpy.
            bool is_active = false;
            for (int k = 0; k < g_active_dmas_count; k++) {
                uint64_t active_start = g_active_dmas[k].gpa;
                uint64_t active_end = active_start + g_active_dmas[k].len;
                if (iova < active_end && (iova + len) > active_start) {
                    is_active = true;
                    break;
                }
            }
            if (!is_active) {
                continue;
            }
            
            uint8_t *vaddr = (uint8_t*)g_dma_regions[i].vaddr;
            dma_sg_t *sg = NULL;
            struct iovec iov;
            bool was_mapped = false;
            
            if (vaddr == NULL) {
                vaddr = (uint8_t*)map_dma_region(vfu_ctx, iova, len, &sg, &iov);
                if (vaddr == NULL) {
                    continue;
                }
                was_mapped = true;
            }
            
            size_t remaining = len;
            uint64_t curr_iova = iova;
            size_t offset = 0;
            
            while (remaining > 0) {
                size_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
                struct rdma_packet req = {0};
                struct rdma_packet resp = {0};
                req.op = RDMA_OP_DMA_SYNC_TO_GUEST;
                req.tx_id = next_tx_id++;
                req.addr = curr_iova;
                req.len = chunk;
                
                if (do_rdma_transaction(vfu_ctx, &req, &resp) == 0) {
                    if (memcmp(&vaddr[offset], resp.data, chunk) != 0) {
                        memcpy(&vaddr[offset], resp.data, chunk);
                        memcpy(&g_dma_regions[i].cache[offset], resp.data, chunk);
                    }
                }
                curr_iova += chunk;
                offset += chunk;
                remaining -= chunk;
            }
            
            if (was_mapped) {
                unmap_dma_region(vfu_ctx, sg, &iov);
            }
        }
    }
    
    // 2. Sync active sub-ranges within uncached large regions (e.g. event/status queues or GSP status)
    if (sync_large) {
        for (int k = 0; k < g_active_dmas_count; k++) {
            uint64_t gpa = g_active_dmas[k].gpa;
            size_t len = g_active_dmas[k].len;
            uint8_t *cache = g_active_dmas[k].cache;
            
            if (cache == NULL) continue;
            
            // Pull the full active DMA buffer (like GSP or scrubber buffer) to capture all status queues & boot bits
            size_t pull_len = (len > 256 * 1024) ? (256 * 1024) : len;
            
            // Find which DMA region contains this active GPA
            for (int i = 0; i < MAX_DMA_REGIONS; i++) {
                if (g_dma_regions[i].in_use && g_dma_regions[i].cache == NULL) {
                    if (gpa >= g_dma_regions[i].iova && (gpa + len) <= (g_dma_regions[i].iova + g_dma_regions[i].len)) {
                        uint8_t *vaddr = (uint8_t*)g_dma_regions[i].vaddr;
                        dma_sg_t *sg = NULL;
                        struct iovec iov;
                        bool was_mapped = false;
                        
                        if (vaddr == NULL) {
                            vaddr = (uint8_t*)map_dma_region(vfu_ctx, g_dma_regions[i].iova, g_dma_regions[i].len, &sg, &iov);
                            if (vaddr == NULL) {
                                continue;
                            }
                            was_mapped = true;
                        }
                        
                        size_t start_offset = gpa - g_dma_regions[i].iova;
                        uint8_t *gpa_vaddr = vaddr + start_offset;
                        
                        size_t remaining = pull_len;
                        size_t offset = 0;
                        
                        while (remaining > 0 && running) {
                            size_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
                            struct rdma_packet req = {0};
                            struct rdma_packet resp = {0};
                            req.op = RDMA_OP_DMA_SYNC_TO_GUEST;
                            req.tx_id = next_tx_id++;
                            req.addr = gpa + offset;
                            req.len = chunk;
                            
                            if (do_rdma_transaction(vfu_ctx, &req, &resp) == 0) {
                                if (memcmp(&gpa_vaddr[offset], resp.data, chunk) != 0) {
                                    memcpy(&gpa_vaddr[offset], resp.data, chunk);
                                    memcpy(&cache[offset], resp.data, chunk);
                                }
                            }
                            offset += chunk;
                            remaining -= chunk;
                        }
                        
                        if (was_mapped) {
                            unmap_dma_region(vfu_ctx, sg, &iov);
                        }
                        break;
                    }
                }
            }
        }
    }
    
    pthread_mutex_unlock(&dma_sync_mutex);
}
 
static void* irq_thread(void *arg) {
    vfu_ctx_t *vfu_ctx = (vfu_ctx_t*)arg;
    int last_ret = 0;
    int err_count = 0;
    uint32_t loop_counter = 0;
    while (irq_thread_running) {
        usleep(10000); // 10ms interval (100Hz)
        if (!g_connected || !irq_thread_running) {
            continue;
        }
        loop_counter++;
        
        // 0. Process pending GPA registrations (deferred from bar_access_cb)
        pthread_mutex_lock(&pending_gpa_mutex);
        for (int i = 0; i < MAX_PENDING_GPA; i++) {
            if (g_pending_gpa[i].pending && !g_pending_gpa[i].mr_registered) {
                uint64_t gpa = g_pending_gpa[i].gpa;
                size_t len = g_pending_gpa[i].len;
                pthread_mutex_unlock(&pending_gpa_mutex);
                
                // Register MR with host
                struct rdma_packet mr_req = {0}, mr_resp = {0};
                mr_req.op = RDMA_OP_REG_MR;
                mr_req.tx_id = next_tx_id++;
                mr_req.addr = gpa;
                mr_req.len = len;
                if (do_rdma_transaction(vfu_ctx, &mr_req, &mr_resp) == 0 && mr_resp.status == 0) {
                    uint64_t host_phys = mr_resp.addr;
                    gpa_xlat_register(gpa, host_phys, len);
                    printf("GPA XLAT [irq_thread]: guest=%#llx -> host=%#llx (size=%zu)\n",
                           (unsigned long long)gpa, (unsigned long long)host_phys, len);
                    
                    pthread_mutex_lock(&pending_gpa_mutex);
                    g_pending_gpa[i].mr_registered = true;
                    g_pending_gpa[i].host_phys = host_phys;
                    pthread_mutex_unlock(&pending_gpa_mutex);
                    
                    // Determine the value to write to the register
                    uint64_t bar_off = g_pending_gpa[i].bar_offset;
                    bool is_pfn = g_pending_gpa[i].is_pfn;
                    uint32_t write_val = is_pfn ? (uint32_t)(host_phys >> 12) : (uint32_t)host_phys;

                    // Blast sync firmware data to host (will execute the BAR0 write at the end)
                    printf("GPA XLAT [irq_thread]: Triggering async blast sync gpa=%#llx len=%zu\n",
                           (unsigned long long)gpa, len);
                    trigger_blast_sync(vfu_ctx, gpa, len, write_val, bar_off);
                } else {
                    printf("GPA XLAT [irq_thread]: MR registration failed for gpa=%#llx\n",
                           (unsigned long long)gpa);
                }
                
                pthread_mutex_lock(&pending_gpa_mutex);
            }
        }
        pthread_mutex_unlock(&pending_gpa_mutex);

        // 0.5. Process pending IOVA mappings (IOMMU mode)
        if (g_iommu_mode) {
            pthread_mutex_lock(&iova_maps_mutex);
            for (int i = 0; i < MAX_IOVA_MAPS; i++) {
                if (g_iova_maps[i].active && !g_iova_maps[i].synced) {
                    uint64_t iova = g_iova_maps[i].iova;
                    size_t map_size = g_iova_maps[i].size;
                    void *vaddr = g_iova_maps[i].vaddr;

                    // Fire-and-forget IOMMU MAP — don't hold rdma_mutex here.
                    // bar_access_cb needs rdma_mutex for BAR reads; blocking here
                    // would starve the GPU driver's register accesses.
                    // The host programs VT-d and sends a response; we'll pick it
                    // up on the next poll iteration via the async receive path.
                    struct rdma_packet map_req = {0};
                    map_req.op = RDMA_OP_IOMMU_MAP;
                    map_req.addr = iova;
                    map_req.len = 8;
                    *(uint64_t*)map_req.data = map_size;
                    sendto(blast_sock_fd >= 0 ? blast_sock_fd : sock_fd,
                           &map_req, sizeof(map_req), 0,
                           (struct sockaddr*)&host_addr, sizeof(host_addr));

                    // Mark synced optimistically; host will program VT-d async
                    g_iova_maps[i].synced = true;
                    printf("IOMMU MAP SENT (async): iova=%#llx size=%zu\n",
                           (unsigned long long)iova, map_size);

                    // NOTE: Do NOT sync initial data here. Sending up to 1GB of UDP packets
                    // would overflow the network bridge and cause ARP to go INCOMPLETE.
                    // Data sync is handled separately when the GPU actually performs DMA.
                    if (vaddr != NULL && map_size > 0 && map_size <= 64*1024) {
                        // Only sync very small regions (e.g., VGA BIOS at 0xa0000, 64KB)
                        size_t sent = 0;
                        while (sent < map_size) {
                            size_t chunk = (map_size - sent > RDMA_DATA_LEN) ? RDMA_DATA_LEN : (map_size - sent);
                            struct rdma_packet sync_pkt = {0};
                            sync_pkt.op = RDMA_OP_DMA_SYNC_TO_HOST;
                            sync_pkt.addr = iova + sent;
                            sync_pkt.len = (uint32_t)chunk;
                            memcpy(sync_pkt.data, (uint8_t*)vaddr + sent, chunk);
                            sendto(blast_sock_fd >= 0 ? blast_sock_fd : sock_fd,
                                   &sync_pkt, sizeof(sync_pkt), 0,
                                   (struct sockaddr*)&host_addr, sizeof(host_addr));
                            sent += chunk;
                        }
                        printf("IOMMU SYNC: iova=%#llx synced %zu bytes\n",
                               (unsigned long long)iova, map_size);
                    }
                }
            }
            pthread_mutex_unlock(&iova_maps_mutex);
        }
        
        // 1. Sync Guest DMA changes to Host
        sync_dma_to_host(vfu_ctx, true);
        
        // 2. Trigger interrupt in Guest VM
        int ret = vfu_irq_trigger(vfu_ctx, 0);
        if (ret < 0) {
            if (ret != last_ret && err_count < 20) {
                fprintf(stderr, "net_pci_client: vfu_irq_trigger failed: %s (ret=%d)\n", strerror(errno), ret);
                last_ret = ret;
                err_count++;
            }
        } else {
            if (last_ret < 0) {
                fprintf(stderr, "net_pci_client: vfu_irq_trigger succeeded!\n");
                last_ret = 0;
            }
        }
        
        // 3. Sync Host DMA changes to Guest
        // Sync large buffers (> 8KB) every 250ms (25 loop iterations) to avoid starving main thread BAR/MMIO accesses
        bool sync_large = (loop_counter % 25 == 0);
        sync_dma_from_host(vfu_ctx, sync_large);
    }
    return NULL;
}

// -------------------------------------------------------------
// Core BAR RDMA Network Callback
#define CACHE_SIZE 4096

struct bar_cache {
    uint8_t bar_index;
    uint64_t start_addr;
    uint8_t data[CACHE_SIZE];
    uint32_t valid_len;
    bool is_valid;
};

static struct bar_cache g_cache = { .is_valid = false };

static int do_rdma_transaction(vfu_ctx_t *vfu_ctx, struct rdma_packet *req, struct rdma_packet *resp) {
    pthread_mutex_lock(&rdma_mutex);
    int retries = (pending_blast_count > 0) ? 5 : 3;
    bool success = false;
    int final_ret = 0;

    while (retries > 0) {
        // Use blast_sock_fd for transactions — it's the first socket that contacts
        // the host, so the host's remote_port is set to blast_sock_fd's ephemeral port.
        // All responses from the host go to this port.
        int tx_fd = (blast_sock_fd >= 0) ? blast_sock_fd : sock_fd;

        ssize_t sent = sendto(tx_fd, req, sizeof(*req), 0,
                              (struct sockaddr*)&host_addr, sizeof(host_addr));
        if (req->op == RDMA_OP_READ_REQ || req->op == RDMA_OP_READ_BLOCK_REQ) {
            printf("[RDMA_TX] fd=%d op=%d tx_id=%u dst=%s:%d sent=%zd errno=%d\n",
                   tx_fd, req->op, req->tx_id,
                   inet_ntoa(host_addr.sin_addr), ntohs(host_addr.sin_port),
                   sent, sent < 0 ? errno : 0);
        }
        if (sent < 0) {
            vfu_log(vfu_ctx, LOG_ERR, "sendto failed: %s", strerror(errno));
            final_ret = -1;
            goto out;
        }

        // During blast sync the host is processing DMA packets and may be
        // slower to respond to BAR reads. Use a longer timeout.
        // CRC verify needs even more time (host computes CRC32 over 100MB).
        uint64_t timeout_us;
        if (req->op == RDMA_OP_DMA_SYNC_RELIABLE) {
            timeout_us = 10000000; // 10s for CRC verify
        } else if (pending_blast_count > 0) {
            timeout_us = 2000000;  // 2s during blast
        } else {
            timeout_us = 500000;   // 500ms normal
        }
        struct timeval timeout = { .tv_sec = timeout_us / 1000000, .tv_usec = timeout_us % 1000000 };
        uint64_t total_timeout_us = timeout_us;
        uint64_t start_us;
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        start_us = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
        
        while (1) {
            // Try non-blocking receive first to completely bypass select() context switches
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t recvd = recvfrom(tx_fd, resp, sizeof(*resp), MSG_DONTWAIT,
                                     (struct sockaddr*)&from_addr, &from_len);
            if (recvd >= 0) {
                if (resp->tx_id == req->tx_id) {
                    if (resp->status == 0) {
                        success = true;
                        break;
                    } else {
                        vfu_log(vfu_ctx, LOG_ERR, "RDMA Host returned error status: %u", resp->status);
                        errno = EIO;
                        final_ret = -1;
                        goto out;
                    }
                } else {
                    // Stale packet from previous transaction, ignore it and try again
                    gettimeofday(&tv_now, NULL);
                    uint64_t curr_us = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
                    if (curr_us >= start_us + total_timeout_us) {
                        break; // Timeout expired
                    }
                    continue;
                }
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    break;
                }
            }
            
            // Calculate remaining timeout
            gettimeofday(&tv_now, NULL);
            uint64_t curr_us = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
            if (curr_us >= start_us + total_timeout_us) {
                break; // Timeout expired
            }
            
            uint64_t remaining_us = (start_us + total_timeout_us) - curr_us;
            
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(tx_fd, &read_fds);
            struct timeval tv = {
                .tv_sec = remaining_us / 1000000,
                .tv_usec = remaining_us % 1000000
            };
            int sel = select(tx_fd + 1, &read_fds, NULL, NULL, &tv);
            if (sel <= 0) {
                break; // Timeout or error
            }
        }

        if (success) {
            break;
        }

        retries--;
    }

    if (!success) {
        vfu_log(vfu_ctx, LOG_ERR, "RDMA response timeout after retries (tx_id=%u, BAR=%d, offset=%#llx)", 
                req->tx_id, req->bar_index, (unsigned long long)req->addr);
        errno = ETIMEDOUT;
        final_ret = -1;
        goto out;
    }

out:
    pthread_mutex_unlock(&rdma_mutex);
    return final_ret;
}

static ssize_t bar_access_cb(vfu_ctx_t *vfu_ctx, char * const buf,
                             size_t count, loff_t offset,
                             const bool is_write, uint8_t bar_index) {
    bool use_cache = (bar_index == 0 && offset >= 0x300000 && offset < 0x400000);

    // NOTE: We intentionally do NOT suppress BAR0 reads during blast sync.
    // Returning zeros causes the driver to think the GPU is dead (Xid 79).
    // The blast thread uses raw sendto() without the RDMA mutex, so BAR reads
    // via do_rdma_transaction() proceed concurrently. Reads may be slower 
    // (~2s timeout) but return real hardware values.

    if (is_write) {
        if (use_cache) {
            g_cache.is_valid = false;
        }
    } else {
        // Check cache hit
        if (use_cache && g_cache.is_valid && g_cache.bar_index == bar_index &&
            offset >= g_cache.start_addr &&
            (offset + count) <= (g_cache.start_addr + g_cache.valid_len)) {
            memcpy(buf, &g_cache.data[offset - g_cache.start_addr], count);
            return count;
        }

        // Cache miss: prefetch RDMA_DATA_LEN bytes for small sequential memory reads
        if (use_cache && count <= 8) {
            struct rdma_packet req = {0};
            struct rdma_packet resp = {0};
            req.op = RDMA_OP_READ_BLOCK_REQ;
            req.tx_id = next_tx_id++;
            req.addr = offset;
            req.len = RDMA_DATA_LEN;
            req.bar_index = bar_index;

            if (do_rdma_transaction(vfu_ctx, &req, &resp) == 0) {
                g_cache.bar_index = bar_index;
                g_cache.start_addr = offset;
                g_cache.valid_len = resp.len;
                if (g_cache.valid_len > CACHE_SIZE) {
                    g_cache.valid_len = CACHE_SIZE;
                }
                memcpy(g_cache.data, resp.data, g_cache.valid_len);
                g_cache.is_valid = true;

                if (count <= g_cache.valid_len) {
                    memcpy(buf, g_cache.data, count);
                    return count;
                }
            }
        }
    }

    // Fallback: standard single read/write transaction
    struct rdma_packet req = {0};
    struct rdma_packet resp = {0};
    bool skip_host_write = false;

    req.op = is_write ? RDMA_OP_WRITE_REQ : RDMA_OP_READ_REQ;
    req.tx_id = next_tx_id++;
    req.addr = offset;
    req.len = count;
    req.bar_index = bar_index;

    if (is_write) {
        if (count > sizeof(req.data)) {
            errno = EINVAL;
            return -1;
        }
        memcpy(req.data, buf, count);

        if (bar_index == 0) {
            if (count == 4) {
                uint32_t val = *(uint32_t*)buf;
                if (offset == 0x88080 || offset == 0x88084) {
                    // Try two GPA interpretations:
                    // 1) PFN (page frame number): val << 12 — used by 0x88080/0x88084
                    uint64_t gpa_pfn = (uint64_t)val << 12;
                    uint64_t gpa_direct = (uint64_t)val;
                    // Also try page-aligned mask for large values
                    uint64_t gpa_masked = (uint64_t)val & ~0xFFFULL;
                    
                    uint64_t true_gpa = 0;
                    const char *method = "none";
                    
                    if (gpa_pfn >= 0x1000 && gpa_pfn < 0xC0000000ULL && is_valid_gpa(gpa_pfn)) {
                        true_gpa = gpa_pfn;
                        method = "pfn";
                    } else if (gpa_direct >= 0x1000 && gpa_direct < 0xC0000000ULL && is_valid_gpa(gpa_direct)) {
                        true_gpa = gpa_direct;
                        method = "direct";
                    } else if (gpa_masked >= 0x1000 && gpa_masked < 0xC0000000ULL && is_valid_gpa(gpa_masked)) {
                        true_gpa = gpa_masked;
                        method = "masked";
                    }
                    
                    printf("GPA SNIFF: offset=%#llx val=%#x pfn=%#llx direct=%#llx method=%s true_gpa=%#llx\n",
                           (unsigned long long)offset, val, (unsigned long long)gpa_pfn,
                           (unsigned long long)gpa_direct, method, (unsigned long long)true_gpa);
                    
                    if (true_gpa != 0) {
                        static int blast_sync_count_32 = 0;
                        size_t max_len = (blast_sync_count_32 == 0) ? (96 * 1024 * 1024) : (2 * 1024 * 1024);
                        
                        size_t dma_len = get_dma_region_remaining_len(true_gpa);
                        if (dma_len > max_len) dma_len = max_len;
                        if (dma_len > 0) {
                            if (offset == 0x88080 || offset == 0x88084) {
                                blast_sync_count_32++;
                            }
                            printf("GPA SNIFF: dma_len=%zu (capped) for gpa=%#llx\n", dma_len, (unsigned long long)true_gpa);
                            register_active_dma(true_gpa, dma_len, true);
                            // Queue for deferred processing by irq_thread.
                            // We MUST NOT call do_rdma_transaction here (inside bar_access_cb)
                            // as it deadlocks the vfio-user event loop.
                            if (g_iommu_mode) {
                                if (offset == 0x88080 || offset == 0x88084) {
                                    printf("GPA SNIFF (IOMMU): Queueing blast sync for IOVA=%#llx len=%zu\n",
                                           (unsigned long long)true_gpa, dma_len);
                                    trigger_blast_sync(vfu_ctx, true_gpa, dma_len, val, offset);
                                    // Suppress the BAR write — blast thread will send it AFTER
                                    // data sync completes (~10s). BAR reads still go through
                                    // normally via do_rdma_transaction (no fast-path zeros).
                                    // The 120s GSP timeout gives plenty of headroom.
                                    skip_host_write = true;
                                }
                            } else {
                                if (offset == 0x88080 || offset == 0x88084) {
                                    printf("GSP CRITICAL: Queueing GPA registration for gpa=%#llx len=%zu\n",
                                           (unsigned long long)true_gpa, dma_len);
                                    queue_gpa_registration(true_gpa, dma_len, offset, strcmp(method, "pfn") == 0);
                                }
                                // Check if we already have a translation from a previous cycle
                                uint64_t host_phys = gpa_xlat_lookup(true_gpa);
                                if (host_phys != 0) {
                                    if (strcmp(method, "pfn") == 0) {
                                        uint32_t host_pfn = (uint32_t)(host_phys >> 12);
                                        *(uint32_t*)buf = host_pfn;
                                        memcpy(req.data, buf, count);
                                        printf("GPA XLAT INLINE: offset=%#llx guest_pfn=%#x -> host_pfn=%#x\n",
                                               (unsigned long long)offset, val, host_pfn);
                                    } else {
                                        *(uint32_t*)buf = (uint32_t)host_phys;
                                        memcpy(req.data, buf, count);
                                        printf("GPA XLAT INLINE: offset=%#llx guest_addr=%#x -> host_addr=%#x\n",
                                               (unsigned long long)offset, val, (uint32_t)host_phys);
                                    }
                                } else {
                                    printf("GPA PENDING: Dropping host write for offset=%#llx until translation completes.\n", (unsigned long long)offset);
                                    skip_host_write = true;
                                }
                            }
                        }
                    }
                }
            } else if (count == 8) {
                uint64_t val = *(uint64_t*)buf;
                if (offset == 0x88080 || offset == 0x88084) {
                    uint64_t gpa_pfn = val << 12;
                    uint64_t gpa_direct = val;
                    uint64_t gpa_masked = val & ~0xFFFULL;
                    
                    uint64_t true_gpa = 0;
                    if (gpa_pfn >= 0x1000 && gpa_pfn < 0xC0000000ULL && is_valid_gpa(gpa_pfn)) {
                        true_gpa = gpa_pfn;
                    } else if (gpa_direct >= 0x1000 && gpa_direct < 0xC0000000ULL && is_valid_gpa(gpa_direct)) {
                        true_gpa = gpa_direct;
                    } else if (gpa_masked >= 0x1000 && gpa_masked < 0xC0000000ULL && is_valid_gpa(gpa_masked)) {
                        true_gpa = gpa_masked;
                    }
                    
                    if (true_gpa != 0) {
                        static int blast_sync_count = 0;
                        size_t max_len = (blast_sync_count == 0) ? (96 * 1024 * 1024) : (2 * 1024 * 1024);
                        
                        size_t dma_len = get_dma_region_remaining_len(true_gpa);
                        if (dma_len > max_len) dma_len = max_len;
                        if (dma_len > 0) {
                            if (offset == 0x88080 || offset == 0x88084) {
                                blast_sync_count++;
                            }
                            register_active_dma(true_gpa, dma_len, true);
                            if (g_iommu_mode) {
                                if (offset == 0x88080 || offset == 0x88084) {
                                    printf("GPA SNIFF (IOMMU 64-bit): Queueing blast sync for IOVA=%#llx len=%zu\n",
                                           (unsigned long long)true_gpa, dma_len);
                                    trigger_blast_sync(vfu_ctx, true_gpa, dma_len, (uint32_t)val, offset);
                                    skip_host_write = true;
                                }
                            } else {
                                if (offset == 0x88080 || offset == 0x88084) {
                                    printf("GSP CRITICAL (64-bit): Queueing GPA registration for gpa=%#llx len=%zu\n",
                                           (unsigned long long)true_gpa, dma_len);
                                    queue_gpa_registration(true_gpa, dma_len, offset, false);
                                }
                                
                                uint64_t host_phys = gpa_xlat_lookup(true_gpa);
                                if (host_phys != 0) {
                                    *(uint64_t*)buf = host_phys;
                                    memcpy(req.data, buf, count);
                                } else {
                                    skip_host_write = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (skip_host_write) {
        return count;
    }

    if (is_write) {
        // BAR writes must be reliable — use do_rdma_transaction for ACK.
        // Fire-and-forget writes get dropped when competing with DMA sync
        // packets on blast_sock_fd, causing the GPU to miss critical register
        // writes (Falcon CPUCTL, DMEM programming, etc.).
        // do_rdma_transaction has a 500ms timeout, well within QEMU's
        // vfio-user protocol timeout (~10s).
        struct rdma_packet write_resp = {0};
        int write_ret = do_rdma_transaction(vfu_ctx, &req, &write_resp);
        if (bar_index == 0) {
            uint32_t val = (count == 4) ? *(uint32_t*)buf : 0;
            printf("BAR0 WRITE: offset=%#llx count=%zu val=%#x (ret=%d)\n",
                   (unsigned long long)offset, count, val, write_ret);
        }
        return count;
    }

    // BAR reads need a blocking round-trip to get the response value
    if (do_rdma_transaction(vfu_ctx, &req, &resp) != 0) {
        // On timeout, return all-ones (PCI convention for device errors)
        memset(buf, 0xFF, count);
        return count;
    }

    memcpy(buf, resp.data, count);
    uint32_t raw_val = (count == 4) ? *(uint32_t*)resp.data : 0;

    if (bar_index == 0) {
        uint32_t val = (count == 4) ? *(uint32_t*)buf : 0;
        printf("BAR0 READ: offset=%#llx count=%zu val=%#x (raw=%#x)\n", 
               (unsigned long long)offset, count, val, raw_val);
    }

    return count;
}


// -------------------------------------------------------------
// Individual BAR Callbacks
// -------------------------------------------------------------

static ssize_t bar0_access(vfu_ctx_t *vfu_ctx, char * const buf,
                           size_t count, loff_t offset,
                           const bool is_write) {
    return bar_access_cb(vfu_ctx, buf, count, offset, is_write, 0);
}

static ssize_t bar1_access(vfu_ctx_t *vfu_ctx, char * const buf,
                           size_t count, loff_t offset,
                           const bool is_write) {
    return bar_access_cb(vfu_ctx, buf, count, offset, is_write, 1);
}

static ssize_t bar3_access(vfu_ctx_t *vfu_ctx, char * const buf,
                           size_t count, loff_t offset,
                           const bool is_write) {
    return bar_access_cb(vfu_ctx, buf, count, offset, is_write, 3);
}

static ssize_t bar5_access(vfu_ctx_t *vfu_ctx, char * const buf,
                           size_t count, loff_t offset,
                           const bool is_write) {
    return bar_access_cb(vfu_ctx, buf, count, offset, is_write, 5);
}

static ssize_t bar6_access(vfu_ctx_t *vfu_ctx, char * const buf,
                           size_t count, loff_t offset,
                           const bool is_write) {
    return bar_access_cb(vfu_ctx, buf, count, offset, is_write, 6);
}

// -------------------------------------------------------------
// Main Event Loop Entry Point
// -------------------------------------------------------------

ssize_t cfg_access_cb(vfu_ctx_t *vfu_ctx, char *buf, size_t count,
                      loff_t offset, bool is_write) {
    if (!is_write) {
        memset(buf, 0, count);
        return count;
    }
    printf("net_pci_client: ignoring write to config space offset 0x%lx size %ld\n", (unsigned long)offset, (long)count);
    return count;
}


int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        g_dma_regions[i].in_use = false;
        g_dma_regions[i].cache = NULL;
    }
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <socket_path> <host_ip> <vendor_id> <device_id>\n", argv[0]);
        fprintf(stderr, "Example: %s /tmp/net_pcie.sock 10.0.2.16 0x10de 0x2684\n", argv[0]);
        return EXIT_FAILURE;
    }

    char* socket_path = argv[1];
    char* host_ip_str = argv[2];
    uint16_t vendor_id = (uint16_t)strtol(argv[3], NULL, 16);
    uint16_t device_id = (uint16_t)strtol(argv[4], NULL, 16);

    // Check for optional --iommu flag
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--iommu") == 0) {
            g_iommu_mode = true;
            printf("IOMMU mode ENABLED: DMA callbacks will forward IOVA mappings to host\n");
        }
    }

    printf("======================================================\n");
    printf(" Starting Remote PCIe Client Daemon (vfio-user Server)\n");
    printf("======================================================\n");
    printf(" Socket Path: %s\n", socket_path);
    printf(" Host IP:     %s\n", host_ip_str);
    printf(" Vendor ID:   %#06x\n", vendor_id);
    printf(" Device ID:   %#06x\n", device_id);
    printf(" IOMMU Mode:  %s\n", g_iommu_mode ? "ENABLED" : "disabled");
    printf("======================================================\n");

    // Initialize UDP Socket for RDMA Network Protocol
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        err(EXIT_FAILURE, "failed to create UDP socket");
    }

    // Configure large socket send/receive buffers to avoid UDP packet loss during high-speed bursts
    int buf_size = 16 * 1024 * 1024; // 16MB buffer
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    // Initialize dedicated blast sync socket (fire-and-forget DMA)
    blast_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (blast_sock_fd < 0) {
        warn("failed to create blast sync socket, blast sync disabled");
    } else {
        int blast_buf = 32 * 1024 * 1024; // 32MB send buffer for burst writes
        setsockopt(blast_sock_fd, SOL_SOCKET, SO_SNDBUF, &blast_buf, sizeof(blast_buf));
        printf("Blast sync socket initialized (fd=%d)\n", blast_sock_fd);
    }

    // Initialize dedicated RPC socket for request-response RDMA transactions
    // This MUST be separate from blast_sock_fd to avoid response packets being
    // lost amid the flood of fire-and-forget DMA sync packets.
    rdma_rpc_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rdma_rpc_sock_fd < 0) {
        warn("failed to create RDMA RPC socket");
    } else {
        int rpc_buf = 16 * 1024 * 1024;
        setsockopt(rdma_rpc_sock_fd, SOL_SOCKET, SO_SNDBUF, &rpc_buf, sizeof(rpc_buf));
        setsockopt(rdma_rpc_sock_fd, SOL_SOCKET, SO_RCVBUF, &rpc_buf, sizeof(rpc_buf));
        printf("RDMA RPC socket initialized (fd=%d)\n", rdma_rpc_sock_fd);
    }

    memset(&host_addr, 0, sizeof(host_addr));
    host_addr.sin_family = AF_INET;
    host_addr.sin_port = htons(RDMA_PORT);
    if (inet_pton(AF_INET, host_ip_str, &host_addr.sin_addr) <= 0) {
        err(EXIT_FAILURE, "invalid Host IP address");
    }

    // Register Signal Handlers
    struct sigaction sa = { .sa_handler = sig_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Start background Blast Sync worker
    pthread_create(&blast_thread, NULL, blast_sync_worker, NULL);
    blast_thread_active = true;

    // ---------------------------------------------------------------
    // Pre-flight RDMA Connectivity Check
    // Verify the HobbyOS host is alive and responding before setting
    // up the vfio-user device. This prevents QEMU from sending BAR
    // accesses that will all timeout.
    // ---------------------------------------------------------------
    printf("Pre-flight: Verifying RDMA connectivity to %s:%d...\n", host_ip_str, RDMA_PORT);
    {
        struct rdma_packet ping = {0};
        ping.op = RDMA_OP_READ_REQ;
        ping.tx_id = 0xFFFF; // Reserved pre-flight tx_id
        ping.addr = 0;       // Read BAR0 offset 0 (PCI vendor/device ID)
        ping.len = 4;
        ping.bar_index = 0;

        bool host_alive = false;
        for (int attempt = 0; attempt < 30 && running; attempt++) {
            int tx_fd = (blast_sock_fd >= 0) ? blast_sock_fd : sock_fd;
            ssize_t sent = sendto(tx_fd, &ping, sizeof(ping), 0,
                                  (struct sockaddr*)&host_addr, sizeof(host_addr));
            if (sent < 0) {
                printf("Pre-flight: sendto failed: %s\n", strerror(errno));
                sleep(2);
                continue;
            }

            // Wait up to 2 seconds for response
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(tx_fd, &fds);
            int sel = select(tx_fd + 1, &fds, NULL, NULL, &tv);
            if (sel > 0) {
                struct rdma_packet resp = {0};
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                ssize_t recvd = recvfrom(tx_fd, &resp, sizeof(resp), 0,
                                          (struct sockaddr*)&from, &from_len);
                if (recvd > 0 && resp.tx_id == 0xFFFF && resp.op == RDMA_OP_READ_RESP) {
                    uint32_t val = *(uint32_t*)resp.data;
                    printf("Pre-flight: Host alive! BAR0[0x0] = %#x (attempt %d)\n", val, attempt + 1);
                    host_alive = true;
                    break;
                }
            }
            printf("Pre-flight: No response (attempt %d/30), retrying...\n", attempt + 1);
        }

        if (!host_alive) {
            fprintf(stderr, "Pre-flight: FATAL - HobbyOS host is not responding. Aborting.\n");
            close(sock_fd);
            if (blast_sock_fd >= 0) close(blast_sock_fd);
            if (rdma_rpc_sock_fd >= 0) close(rdma_rpc_sock_fd);
            return EXIT_FAILURE;
        }
    }
    printf("Pre-flight: RDMA link verified. Proceeding with device setup.\n");

    // Initialize libvfio-user context
    vfu_ctx_t *vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, socket_path, 0, NULL, VFU_DEV_TYPE_PCI);
    if (vfu_ctx == NULL) {
        err(EXIT_FAILURE, "failed to create vfio-user context");
    }

    vfu_setup_log(vfu_ctx, _log, LOG_ERR);

    int ret = vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0xa1);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_pci_init() failed");
    }

    ret = vfu_setup_device_dma(vfu_ctx, 128, dma_register_cb, dma_unregister_cb);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup device DMA");
    }

    // Set PCI Identification Registers to match MSI RTX 4090 Subsystem (1462:5104)
    vfu_pci_set_id(vfu_ctx, vendor_id, device_id, 0x1462, 0x5104);

    // Set Class Code to display controller (VGA compatible controller: 0x03, 0x00, 0x00)
    vfu_pci_set_class(vfu_ctx, 0x03, 0x00, 0x00);

    // Setup capabilities (Power Management, MSI, Express)
    struct pmcap pm = { .hdr.id = PCI_CAP_ID_PM, .pmcs.nsfrst = 0x1 };
    if (vfu_pci_add_capability(vfu_ctx, 0, 0, &pm) < 0) {
        err(EXIT_FAILURE, "failed to add PM capability");
    }

    struct msicap msi = {
        .hdr.id = PCI_CAP_ID_MSI,
        .mc.msie = 0,
        .mc.mmc = 0,
        .mc.mme = 0,
        .mc.c64 = 1,
        .mc.pvm = 0,
    };
    if (vfu_pci_add_capability(vfu_ctx, 0, 0, &msi) < 0) {
        err(EXIT_FAILURE, "failed to add MSI capability");
    }

    struct pxcap px = {
        .hdr.id = PCI_CAP_ID_EXP,
        .pxdcap = {.flrc = 0x1}
    };
    if (vfu_pci_add_capability(vfu_ctx, 0, 0, &px) < 0) {
        err(EXIT_FAILURE, "failed to add Express capability");
    }

    // Setup IRQ counts
    // INTx IRQ
    ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_INTX_IRQ, 1);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup INTX IRQs");
    }

    // MSI IRQ
    ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSI_IRQ, 1);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup MSI IRQs");
    }

    // Set PCI Interrupt Pin to INTA (1)
    vfu_pci_config_space_t *config_space = vfu_pci_get_config_space(vfu_ctx);
    if (config_space != NULL) {
        config_space->hdr.intr.ipin = 1;
    }

    // Setup BAR regions
    // BAR 0: 16 MB Memory BAR (32-bit, non-prefetchable)
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, 16 * 1024 * 1024,
                           &bar0_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR0 region");
    }

    // BAR1 is 32GB. Use a sparse memfd to avoid KVM page faults and to allow local QEMU PCI probing.
    int bar1_fd = memfd_create("bar1", 0);
    ftruncate(bar1_fd, 32ULL * 1024 * 1024 * 1024);
    struct iovec bar1_mmap[1] = {{ .iov_base = 0, .iov_len = 32ULL * 1024 * 1024 * 1024 }};
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR1_REGION_IDX, 32ULL * 1024 * 1024 * 1024,
                           NULL, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM | VFU_REGION_FLAG_64_BITS | VFU_REGION_FLAG_PREFETCH, bar1_mmap, 1, bar1_fd, 0);
    if (ret < 0) {
        fprintf(stderr, "net_pci_client: failed to setup BAR1\n");
        exit(EXIT_FAILURE);
    }
    
    // BAR3 is 32MB. Use a sparse memfd as well.
    int bar3_fd = memfd_create("bar3", 0);
    ftruncate(bar3_fd, 32 * 1024 * 1024);
    struct iovec bar3_mmap[1] = {{ .iov_base = 0, .iov_len = 32 * 1024 * 1024 }};
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR3_REGION_IDX, 32 * 1024 * 1024,
                           NULL, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM | VFU_REGION_FLAG_64_BITS | VFU_REGION_FLAG_PREFETCH, bar3_mmap, 1, bar3_fd, 0);

    // BAR 5: 128 Bytes I/O BAR
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR5_REGION_IDX, 128,
                           &bar5_access, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR5 region");
    }

    // Expansion ROM: 512 KB
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_ROM_REGION_IDX, 512 * 1024,
                           &bar6_access, VFU_REGION_FLAG_READ | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup ROM region");
    }

    // Config Space Callback
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_CFG_REGION_IDX, 4096,
                           &cfg_access_cb, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup config space callback");
    }

    // Realize Context
    ret = vfu_realize_ctx(vfu_ctx);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to realize vfio-user context");
    }

    printf("Waiting for QEMU client connection on socket: %s...\n", socket_path);
    ret = vfu_attach_ctx(vfu_ctx);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to attach/listen on vfio-user socket");
    }
    g_connected = true;
    printf("Connection established! Emulating PCIe device...\n");
 
    pthread_t thread_id = 0;
    bool thread_active = false;
 
    // Processing Loop
    while (running) {
        if (g_connected && !thread_active) {
            irq_thread_running = true;
            if (pthread_create(&thread_id, NULL, irq_thread, vfu_ctx) == 0) {
                thread_active = true;
                printf("IRQ polling thread started.\n");
            } else {
                vfu_log(vfu_ctx, LOG_ERR, "failed to create IRQ polling thread");
            }
        }

        ret = vfu_run_ctx(vfu_ctx);
        if (ret < 0) {
            if (errno == EINTR) {
                if (!running) break;
                continue;
            }
            if (errno == ENOTCONN || errno == ESHUTDOWN) {
                g_connected = false;
                
                // Stop and join IRQ thread immediately upon disconnect
                if (thread_active) {
                    irq_thread_running = false;
                    pthread_join(thread_id, NULL);
                    thread_active = false;
                    printf("IRQ polling thread stopped.\n");
                }

                // Clear all DMA regions and caches
                pthread_mutex_lock(&dma_sync_mutex);
                for (int i = 0; i < MAX_DMA_REGIONS; i++) {
                    if (g_dma_regions[i].cache != NULL) {
                        free(g_dma_regions[i].cache);
                        g_dma_regions[i].cache = NULL;
                    }
                    g_dma_regions[i].in_use = false;
                }
                for (int i = 0; i < g_active_dmas_count; i++) {
                    if (g_active_dmas[i].cache != NULL) {
                        free(g_active_dmas[i].cache);
                        g_active_dmas[i].cache = NULL;
                    }
                }
                g_active_dmas_count = 0;
                pthread_mutex_unlock(&dma_sync_mutex);

                printf("Client disconnected. Re-listening...\n");
                ret = vfu_attach_ctx(vfu_ctx);
                if (ret < 0) {
                    vfu_log(vfu_ctx, LOG_ERR, "failed to re-attach context");
                    break;
                }
                g_connected = true;
                printf("Connection established! Emulating PCIe device...\n");
            } else {
                vfu_log(vfu_ctx, LOG_ERR, "vfu_run_ctx() error: %s", strerror(errno));
                break;
            }
        }
    }
 
    printf("Shutting down net_pci_client...\n");
    if (thread_active) {
        irq_thread_running = false;
        pthread_join(thread_id, NULL);
    }
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].cache != NULL) {
            free(g_dma_regions[i].cache);
            g_dma_regions[i].cache = NULL;
        }
    }
    for (int i = 0; i < g_active_dmas_count; i++) {
        if (g_active_dmas[i].cache != NULL) {
            free(g_active_dmas[i].cache);
            g_active_dmas[i].cache = NULL;
        }
    }
    vfu_destroy_ctx(vfu_ctx);
    close(sock_fd);
    if (blast_sock_fd >= 0) close(blast_sock_fd);
    if (rdma_rpc_sock_fd >= 0) close(rdma_rpc_sock_fd);
    unlink(socket_path);
 
    return EXIT_SUCCESS;
}
