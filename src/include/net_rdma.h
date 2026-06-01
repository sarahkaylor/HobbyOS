#ifndef NET_RDMA_H
#define NET_RDMA_H

#include <stdint.h>
#include <stddef.h>

// QEMU Educational PCIe Device Constants
#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11E8

// NVIDIA GPU Constants
#define NVIDIA_VENDOR_ID 0x10DE

// Register offsets within the 'edu' BAR0 (MMIO space)
#define EDU_REG_IDENT      0x00  // 4 bytes, R
#define EDU_REG_LIVENESS   0x04  // 4 bytes, RW
#define EDU_REG_FACTORIAL  0x08  // 4 bytes, RW
#define EDU_REG_STATUS     0x20  // 4 bytes, R
#define EDU_REG_DMA_SRC    0x80  // 8 bytes, RW
#define EDU_REG_DMA_DST    0x88  // 8 bytes, RW
#define EDU_REG_DMA_SIZE   0x90  // 8 bytes, RW
#define EDU_REG_DMA_CMD    0x98  // 4 bytes, RW

// The offset of the internal buffer of the 'edu' device
#define EDU_BUFF_OFFSET    0x40000

// RDMA Protocol Port
#define RDMA_PORT          7777

// IP Configurations for the instances
#define RDMA_HOST_IP       0x0A000210  // 10.0.2.16 (Provider/Host)
#define RDMA_GUEST_IP      0x0A00020F  // 10.0.2.15 (Consumer/Receiver)

// Custom RDMA OPCodes
enum rdma_op {
    RDMA_OP_REG_MR = 1,
    RDMA_OP_UNREG_MR = 2,
    RDMA_OP_READ_REQ = 3,
    RDMA_OP_READ_RESP = 4,
    RDMA_OP_WRITE_REQ = 5,
    RDMA_OP_WRITE_RESP = 6,
    RDMA_OP_DMA_SYNC_TO_HOST = 7,  // Copy Consumer RAM -> Host Shadow Contiguous RAM
    RDMA_OP_DMA_SYNC_TO_GUEST = 8, // Copy Host Shadow Contiguous RAM -> Consumer RAM
    RDMA_OP_DMA_SYNC_RESP = 9,
    RDMA_OP_READ_BLOCK_REQ = 10,
    RDMA_OP_READ_BLOCK_RESP = 11,
    RDMA_OP_WRITE_BLOCK_REQ = 12,
    RDMA_OP_WRITE_BLOCK_RESP = 13,
};

#define RDMA_DATA_LEN 1024

// RDMA Packet Structure over UDP/IP (Generalized for Multi-BAR and dynamic routing)
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

// Dynamic RDMA Configuration (Startup Parameters)
extern uint16_t g_rdma_vendor_id;
extern uint16_t g_rdma_device_id;
extern int g_rdma_active;

// API Declarations
void net_rdma_init(void);
void net_rdma_poll(void);

// Generalized Consumer Virtual PCI Driver API
uint32_t v_pci_read32(uint8_t bar, uint64_t offset);
void v_pci_write32(uint8_t bar, uint64_t offset, uint32_t val);
uint64_t v_pci_read64(uint8_t bar, uint64_t offset);
void v_pci_write64(uint8_t bar, uint64_t offset, uint64_t val);
int v_pci_read_block(uint8_t bar, uint64_t offset, void* buf, uint32_t len);
int v_pci_write_block(uint8_t bar, uint64_t offset, const void* buf, uint32_t len);

// Consumer Virtual Driver API (Backward Compatibility wrappers)
uint32_t v_edu_read32(uint32_t offset);
void v_edu_write32(uint32_t offset, uint32_t val);
uint64_t v_edu_read64(uint32_t offset);
void v_edu_write64(uint32_t offset, uint64_t val);

// Memory Registration APIs
int rdma_register_mr(uint64_t guest_phys, uint32_t size);
int rdma_dma_sync(uint64_t guest_phys, uint32_t size, int to_device);

// Unit Test Suite
void net_rdma_test_suite(void);

#endif // NET_RDMA_H
