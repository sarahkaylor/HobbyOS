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

struct rdma_packet {
    uint32_t op;               // enum rdma_op
    uint32_t tx_id;            // Transaction ID to match req/resp
    uint64_t addr;             // Target offset / physical address
    uint32_t len;              // Data length
    uint32_t status;           // 0 on success, non-zero on failure
    uint8_t  bar_index;        // PCI BAR index (0-5) to target
    uint8_t  reserved[3];      // Alignment/padding
    uint8_t  data[512];        // Data payload
} __attribute__((packed));

// -------------------------------------------------------------
// Global Network & Context State
// -------------------------------------------------------------

static int sock_fd = -1;
static struct sockaddr_in host_addr;
static uint32_t next_tx_id = 1;
static volatile bool running = true;

// -------------------------------------------------------------
// Signal & Log Helpers
// -------------------------------------------------------------

static void _log(vfu_ctx_t *vfu_ctx, int level, char const *msg) {
    fprintf(stderr, "net_pci_client[%d]: %s\n", getpid(), msg);
}

static void sig_handler(int sig) {
    running = false;
}

// -------------------------------------------------------------
// Core BAR RDMA Network Callback
// -------------------------------------------------------------

static ssize_t bar_access_cb(vfu_ctx_t *vfu_ctx, char * const buf,
                             size_t count, loff_t offset,
                             const bool is_write, uint8_t bar_index) {
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
    }

    // Transmit request to HobbyOS RDMA host over UDP
    ssize_t sent = sendto(sock_fd, &req, sizeof(req), 0,
                          (struct sockaddr*)&host_addr, sizeof(host_addr));
    if (sent < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "sendto failed: %s", strerror(errno));
        return -1;
    }

    // Await response with a strict 2-second timeout
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock_fd, &read_fds);

    int sel = select(sock_fd + 1, &read_fds, NULL, NULL, &tv);
    if (sel <= 0) {
        vfu_log(vfu_ctx, LOG_ERR, "RDMA response timeout (tx_id=%u, BAR=%d, offset=%#llx)", 
                req.tx_id, bar_index, (unsigned long long)offset);
        errno = ETIMEDOUT;
        return -1;
    }

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    ssize_t recvd = recvfrom(sock_fd, &resp, sizeof(resp), 0,
                             (struct sockaddr*)&from_addr, &from_len);
    if (recvd < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "recvfrom failed: %s", strerror(errno));
        return -1;
    }

    if (resp.tx_id != req.tx_id) {
        vfu_log(vfu_ctx, LOG_ERR, "RDMA response tx_id mismatch: got %u, expected %u", 
                resp.tx_id, req.tx_id);
        errno = EIO;
        return -1;
    }

    if (resp.status != 0) {
        vfu_log(vfu_ctx, LOG_ERR, "RDMA Host returned error status: %u", resp.status);
        errno = EIO;
        return -1;
    }

    if (!is_write) {
        memcpy(buf, resp.data, count);
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

// -------------------------------------------------------------
// Main Event Loop Entry Point
// -------------------------------------------------------------

int main(int argc, char *argv[]) {
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

    // Initialize libvfio-user context
    vfu_ctx_t *vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, socket_path, 0, NULL, VFU_DEV_TYPE_PCI);
    if (vfu_ctx == NULL) {
        err(EXIT_FAILURE, "failed to create vfio-user context");
    }

    vfu_setup_log(vfu_ctx, _log, LOG_DEBUG);

    int ret = vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_CONVENTIONAL, PCI_HEADER_TYPE_NORMAL, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_pci_init() failed");
    }

    // Set PCI Identification Registers
    vfu_pci_set_id(vfu_ctx, vendor_id, device_id, 0xcafe, 0xbabe);

    // Setup BAR regions
    // BAR 0: 16 MB Memory BAR
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, 16 * 1024 * 1024,
                           &bar0_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR0 region");
    }

    // BAR 3: 32 MB Memory BAR
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR3_REGION_IDX, 32 * 1024 * 1024,
                           &bar3_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR3 region");
    }

    // BAR 5: 128 Bytes I/O BAR
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR5_REGION_IDX, 128,
                           &bar5_access, VFU_REGION_FLAG_RW, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR5 region");
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

    printf("Connection established! Emulating PCIe device...\n");

    // Processing Loop
    while (running) {
        ret = vfu_run_ctx(vfu_ctx);
        if (ret < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOTCONN || errno == ESHUTDOWN) {
                printf("Client disconnected. Re-listening...\n");
                ret = vfu_attach_ctx(vfu_ctx);
                if (ret < 0) {
                    vfu_log(vfu_ctx, LOG_ERR, "failed to re-attach context");
                    break;
                }
                printf("Connection established! Emulating PCIe device...\n");
            } else {
                vfu_log(vfu_ctx, LOG_ERR, "vfu_run_ctx() error: %s", strerror(errno));
                break;
            }
        }
    }

    printf("Shutting down net_pci_client...\n");
    vfu_destroy_ctx(vfu_ctx);
    close(sock_fd);
    unlink(socket_path);

    return EXIT_SUCCESS;
}
