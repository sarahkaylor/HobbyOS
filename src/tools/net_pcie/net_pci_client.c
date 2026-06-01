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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <err.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/mman.h>

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
    volatile bool done;
};
static pthread_t blast_thread;
static volatile bool blast_thread_active = false;

// -------------------------------------------------------------
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

static void dma_register_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info) {
    if (info == NULL) return;
    
    // Ignore any small DMA regions below 1 MB (BIOS/ROM reserved space)
    if ((uint64_t)info->iova.iov_base < 0x100000 && info->iova.iov_len < 128 * 1024 * 1024) {
        printf("DMA REGISTER: Ignore low BIOS/ROM mapping: iova=%p, len=%zu\n",
               info->iova.iov_base, info->iova.iov_len);
        return;
    }
    
    // Ignore any DMA regions starting at 4 GB and above (upper 64-bit RAM zone)
    if ((uint64_t)info->iova.iov_base >= 4096ULL * 1024 * 1024) {
        printf("DMA REGISTER: Ignore 64-bit RAM mapping: iova=%p, len=%zu\n",
               info->iova.iov_base, info->iova.iov_len);
        return;
    }
    
    size_t actual_len = info->iova.iov_len;
    printf("DMA REGISTER: iova=%p, vaddr=%p, len=%zu\n", 
           info->iova.iov_base, info->vaddr, info->iova.iov_len);
           
    pthread_mutex_lock(&dma_sync_mutex);
    // Add to table
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (!g_dma_regions[i].in_use) {
            g_dma_regions[i].iova = (uint64_t)info->iova.iov_base;
            g_dma_regions[i].vaddr = info->vaddr;
            g_dma_regions[i].len = actual_len;
            // Only allocate cache for small regions (<= 256KB) to save memory and avoid giant allocs
            if (actual_len > 0 && actual_len <= 262144) {
                g_dma_regions[i].cache = malloc(actual_len);
                if (g_dma_regions[i].cache != NULL) {
                    if (info->vaddr != NULL) {
                        memcpy(g_dma_regions[i].cache, info->vaddr, actual_len);
                    } else {
                        dma_sg_t *sg = NULL;
                        struct iovec iov;
                        void *temp_vaddr = map_dma_region(vfu_ctx, (uint64_t)info->iova.iov_base, actual_len, &sg, &iov);
                        if (temp_vaddr != NULL) {
                            memcpy(g_dma_regions[i].cache, temp_vaddr, actual_len);
                            unmap_dma_region(vfu_ctx, sg, &iov);
                        } else {
                            memset(g_dma_regions[i].cache, 0, actual_len);
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

static void dma_unregister_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info) {
    if (info == NULL) return;
    if ((uint64_t)info->iova.iov_base < 0x100000 && info->iova.iov_len < 128 * 1024 * 1024) {
        return;
    }
    if ((uint64_t)info->iova.iov_base >= 4096ULL * 1024 * 1024) {
        return;
    }
    
    printf("DMA UNREGISTER: iova=%p, vaddr=%p, len=%zu\n", 
           info->iova.iov_base, info->vaddr, info->iova.iov_len);
           
    pthread_mutex_lock(&dma_sync_mutex);
    // Remove from table
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use && g_dma_regions[i].iova == (uint64_t)info->iova.iov_base) {
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
    struct blast_sync_request *req = (struct blast_sync_request *)arg;
    printf("BLAST SYNC START: gpa=%#llx len=%zu\n",
           (unsigned long long)req->gpa, req->len);
    
    size_t remaining = req->len;
    size_t offset = 0;
    int pkt_count = 0;
    uint32_t blast_tx_id = 1000000; // Separate tx_id range for blast
    
    while (remaining > 0 && running) {
        size_t chunk = (remaining > RDMA_DATA_LEN) ? RDMA_DATA_LEN : remaining;
        struct rdma_packet pkt = {0};
        pkt.op = RDMA_OP_DMA_SYNC_TO_HOST;
        pkt.tx_id = blast_tx_id++;
        pkt.addr = req->gpa + offset;
        pkt.len = chunk;
        memcpy(pkt.data, &req->data[offset], chunk);
        
        sendto(sock_fd, &pkt, sizeof(pkt), 0,
               (struct sockaddr*)&host_addr, sizeof(host_addr));
        pkt_count++;
        
        // Pace: every 256 packets, yield briefly to let host drain
        if (pkt_count % 256 == 0) {
            usleep(100);
        }
        offset += chunk;
        remaining -= chunk;
    }
    
    printf("BLAST SYNC DONE: gpa=%#llx sent %d packets (%zu bytes)\n",
           (unsigned long long)req->gpa, pkt_count, req->len);
    
    free(req->data);
    req->done = true;
    free(req);
    return NULL;
}

static void trigger_blast_sync(vfu_ctx_t *vfu_ctx, uint64_t gpa, size_t len) {
    if (sock_fd < 0) return;
    
    // Snapshot the guest's DMA data
    uint8_t *snapshot = malloc(len);
    if (!snapshot) return;
    
    // Map the guest memory and copy
    for (int i = 0; i < MAX_DMA_REGIONS; i++) {
        if (g_dma_regions[i].in_use &&
            gpa >= g_dma_regions[i].iova &&
            gpa < (g_dma_regions[i].iova + g_dma_regions[i].len)) {
            size_t start_offset = gpa - g_dma_regions[i].iova;
            uint8_t *vaddr = (uint8_t*)g_dma_regions[i].vaddr;
            
            if (vaddr == NULL) {
                dma_sg_t *sg = NULL;
                struct iovec iov;
                vaddr = (uint8_t*)map_dma_region(vfu_ctx, g_dma_regions[i].iova,
                                                  g_dma_regions[i].len, &sg, &iov);
                if (vaddr) {
                    memcpy(snapshot, vaddr + start_offset, len);
                    unmap_dma_region(vfu_ctx, sg, &iov);
                } else {
                    free(snapshot);
                    return;
                }
            } else {
                memcpy(snapshot, vaddr + start_offset, len);
            }
            break;
        }
    }
    
    struct blast_sync_request *req = malloc(sizeof(struct blast_sync_request));
    if (!req) { free(snapshot); return; }
    req->gpa = gpa;
    req->len = len;
    req->data = snapshot;
    req->done = false;
    
    pthread_t t;
    pthread_create(&t, NULL, blast_sync_worker, req);
    pthread_detach(t);
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
    int retries = 5;
    bool success = false;
    int final_ret = 0;

    while (retries > 0) {
        // Drain any stale packets first
        struct rdma_packet junk;
        struct sockaddr_in junk_addr;
        socklen_t junk_len = sizeof(junk_addr);
        while (recvfrom(sock_fd, &junk, sizeof(junk), MSG_DONTWAIT, 
                        (struct sockaddr*)&junk_addr, &junk_len) >= 0) {
            // Discard stale late packets from previous transactions
        }

        ssize_t sent = sendto(sock_fd, req, sizeof(*req), 0,
                              (struct sockaddr*)&host_addr, sizeof(host_addr));
        if (sent < 0) {
            vfu_log(vfu_ctx, LOG_ERR, "sendto failed: %s", strerror(errno));
            final_ret = -1;
            goto out;
        }

        struct timeval timeout = { .tv_sec = 0, .tv_usec = 20000 }; // 20ms per try
        uint64_t total_timeout_us = 20000;
        uint64_t start_us;
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        start_us = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
        
        while (1) {
            // Try non-blocking receive first to completely bypass select() context switches
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t recvd = recvfrom(sock_fd, resp, sizeof(*resp), MSG_DONTWAIT,
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
            FD_SET(sock_fd, &read_fds);
            struct timeval tv = {
                .tv_sec = remaining_us / 1000000,
                .tv_usec = remaining_us % 1000000
            };
            int sel = select(sock_fd + 1, &read_fds, NULL, NULL, &tv);
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
                if (offset == 0x88080 || offset == 0x88084 || offset == 0x100c10) {
                    // Try two GPA interpretations:
                    // 1) PFN (page frame number): val << 12 — used by 0x88080/0x88084
                    // 2) Direct byte address: val as-is — used by 0x100c10
                    uint64_t gpa_pfn = (uint64_t)val << 12;
                    uint64_t gpa_direct = (uint64_t)val;
                    // Also try page-aligned mask for large values
                    uint64_t gpa_masked = (uint64_t)val & ~0xFFFULL;
                    
                    uint64_t true_gpa = 0;
                    const char *method = "none";
                    
                    if (gpa_pfn >= 0x100000 && gpa_pfn < 0xC0000000ULL && is_valid_gpa(gpa_pfn)) {
                        true_gpa = gpa_pfn;
                        method = "pfn";
                    } else if (gpa_direct >= 0x100000 && gpa_direct < 0xC0000000ULL && is_valid_gpa(gpa_direct)) {
                        true_gpa = gpa_direct;
                        method = "direct";
                    } else if (gpa_masked >= 0x100000 && gpa_masked < 0xC0000000ULL && is_valid_gpa(gpa_masked)) {
                        true_gpa = gpa_masked;
                        method = "masked";
                    }
                    
                    printf("GPA SNIFF: offset=%#llx val=%#x pfn=%#llx direct=%#llx method=%s true_gpa=%#llx\n",
                           (unsigned long long)offset, val, (unsigned long long)gpa_pfn,
                           (unsigned long long)gpa_direct, method, (unsigned long long)true_gpa);
                    
                    if (true_gpa != 0) {
                        size_t dma_len = get_dma_region_remaining_len(true_gpa);
                        // Cap DMA size based on register type:
                        // 0x100c10 = GSP boot arguments (small struct, cap to 1MB)
                        // 0x88080/0x88084 = GSP firmware (large, use default 96MB cap)
                        size_t max_len = (offset == 0x100c10) ? (1 * 1024 * 1024) : (96 * 1024 * 1024);
                        if (dma_len > max_len) dma_len = max_len;
                        printf("GPA SNIFF: dma_len=%zu (capped) for gpa=%#llx\n", dma_len, (unsigned long long)true_gpa);
                        if (dma_len > 0) {
                            register_active_dma(true_gpa, dma_len, true);
                            // Immediately blast firmware data to host for GSP-critical registers
                            if (offset == 0x88080 || offset == 0x88084 || offset == 0x100c10) {
                                printf("GSP CRITICAL: Triggering blast sync for gpa=%#llx len=%zu\n",
                                       (unsigned long long)true_gpa, dma_len);
                                trigger_blast_sync(vfu_ctx, true_gpa, dma_len);
                            }
                        }
                    }
                }
            } else if (count == 8) {
                uint64_t val = *(uint64_t*)buf;
                if (offset == 0x88080 || offset == 0x88084 || offset == 0x100c10) {
                    uint64_t gpa_pfn = val << 12;
                    uint64_t gpa_direct = val;
                    uint64_t gpa_masked = val & ~0xFFFULL;
                    
                    uint64_t true_gpa = 0;
                    if (gpa_pfn >= 0x100000 && gpa_pfn < 0xC0000000ULL && is_valid_gpa(gpa_pfn)) {
                        true_gpa = gpa_pfn;
                    } else if (gpa_direct >= 0x100000 && gpa_direct < 0xC0000000ULL && is_valid_gpa(gpa_direct)) {
                        true_gpa = gpa_direct;
                    } else if (gpa_masked >= 0x100000 && gpa_masked < 0xC0000000ULL && is_valid_gpa(gpa_masked)) {
                        true_gpa = gpa_masked;
                    }
                    
                    if (true_gpa != 0) {
                        size_t dma_len = get_dma_region_remaining_len(true_gpa);
                        size_t max_len = (offset == 0x100c10) ? (1 * 1024 * 1024) : (96 * 1024 * 1024);
                        if (dma_len > max_len) dma_len = max_len;
                        if (dma_len > 0) {
                            register_active_dma(true_gpa, dma_len, true);
                            if (offset == 0x88080 || offset == 0x88084 || offset == 0x100c10) {
                                printf("GSP CRITICAL (64-bit): Triggering blast sync for gpa=%#llx len=%zu\n",
                                       (unsigned long long)true_gpa, dma_len);
                                trigger_blast_sync(vfu_ctx, true_gpa, dma_len);
                            }
                        }
                    }
                }
            }
        }
    }

    if (do_rdma_transaction(vfu_ctx, &req, &resp) != 0) {
        return -1;
    }

    if (is_write) {
        if (bar_index == 0) {
            uint32_t val = (count == 4) ? *(uint32_t*)buf : 0;
            printf("BAR0 WRITE: offset=%#llx count=%zu val=%#x\n", (unsigned long long)offset, count, val);
        }
        // Sync small DMA regions to Host on BAR writes (e.g. doorbells).
        // Large firmware syncs are handled by the irq_thread which releases dma_sync_mutex
        // during network I/O so BAR reads can be served concurrently.
        sync_dma_to_host(vfu_ctx, false);
    } else {
        memcpy(buf, resp.data, count);
        uint32_t raw_val = (count == 4) ? *(uint32_t*)resp.data : 0;
        

        
        if (bar_index == 0) {
            uint32_t val = (count == 4) ? *(uint32_t*)buf : 0;
            printf("BAR0 READ: offset=%#llx count=%zu val=%#x (raw=%#x)\n", 
                   (unsigned long long)offset, count, val, raw_val);
        }
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

    printf("======================================================\n");
    printf(" Starting Remote PCIe Client Daemon (vfio-user Server)\n");
    printf("======================================================\n");
    printf(" Socket Path: %s\n", socket_path);
    printf(" Host IP:     %s\n", host_ip_str);
    printf(" Vendor ID:   %#06x\n", vendor_id);
    printf(" Device ID:   %#06x\n", device_id);
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
            ssize_t sent = sendto(sock_fd, &ping, sizeof(ping), 0,
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
            FD_SET(sock_fd, &fds);
            int sel = select(sock_fd + 1, &fds, NULL, NULL, &tv);
            if (sel > 0) {
                struct rdma_packet resp = {0};
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                ssize_t recvd = recvfrom(sock_fd, &resp, sizeof(resp), 0,
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

    // BAR 1: 32 GB Memory BAR (64-bit, prefetchable)
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR1_REGION_IDX, 32ULL * 1024 * 1024 * 1024,
                           &bar1_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM | VFU_REGION_FLAG_64_BITS | VFU_REGION_FLAG_PREFETCH, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR1 region");
    }

    // BAR 3: 32 MB Memory BAR (64-bit, prefetchable)
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR3_REGION_IDX, 32 * 1024 * 1024,
                           &bar3_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM | VFU_REGION_FLAG_64_BITS | VFU_REGION_FLAG_PREFETCH, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR3 region");
    }

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
    unlink(socket_path);
 
    return EXIT_SUCCESS;
}
