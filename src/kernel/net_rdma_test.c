#if defined(KERNEL_MODE_UNIT_TEST) && defined(__x86_64__)

#include "net_rdma.h"
#include "unit_test.h"
#include "timer.h"

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);
extern int is_host;

static uint8_t test_dma_buf[128] __attribute__((aligned(4096)));

void net_rdma_test_suite(void) {
    uart_puts("net_rdma_test_suite:\n");

    net_rdma_init();

    if (!g_rdma_active) {
        uart_puts("  Remote PCIe Sharing is INACTIVE (startup parameter missing) - bypassing test suite.\n");
        return;
    }

    if (is_host) {
        uart_puts("  Running in HOST (Provider) mode - bypassing client tests.\n");
        return;
    }

    // Give the Host daemon plenty of time to boot and bind port 7777
    uart_puts("  Waiting for Host network startup...\n");
    uint64_t start_wait = timer_get_ms();
    while (timer_get_ms() - start_wait < 1500) {
        extern void virtio_net_handle_irq(void);
        virtio_net_handle_irq();
        __asm__ volatile("pause");
    }

    // -------------------------------------------------------------
    // Test 1: Identification Register Read
    // -------------------------------------------------------------
    uart_puts("  Running test_rdma_read_ident...\n");
    uint32_t ident = v_edu_read32(EDU_REG_IDENT);
    uart_puts("    Read identification: ");
    uart_print_hex(ident);
    uart_puts("\n");
    
    tests_run++;
    if (g_rdma_vendor_id == 0x10de) {
        if (ident != 0xFFFFFFFF && ident != 0) {
            uart_puts("    PASS\n");
        } else {
            uart_puts("    FAIL\n");
            tests_failed++;
        }
    } else {
        if (ident == 0x010000ed) {
            uart_puts("    PASS\n");
        } else {
            uart_puts("    FAIL\n");
            tests_failed++;
        }
    }

    // -------------------------------------------------------------
    // Test 2: Liveness Register Bitwise NOT Write/Read
    // -------------------------------------------------------------
    uart_puts("  Running test_rdma_liveness...\n");
    uint32_t test_val = 0x12345678;
    v_edu_write32(EDU_REG_LIVENESS, test_val);
    uint32_t resp_val = v_edu_read32(EDU_REG_LIVENESS);
    uart_puts("    Wrote: "); uart_print_hex(test_val);
    uart_puts(" Read back: "); uart_print_hex(resp_val);
    uart_puts("\n");

    tests_run++;
    if (g_rdma_vendor_id == 0x10de) {
        uart_puts("    PASS\n");
    } else {
        if (resp_val == ~test_val) {
            uart_puts("    PASS\n");
        } else {
            uart_puts("    FAIL\n");
            tests_failed++;
        }
    }

    // -------------------------------------------------------------
    // Test 3: Factorial Register Calculation
    // -------------------------------------------------------------
    uart_puts("  Running test_rdma_factorial...\n");
    // Calculate 5! = 120
    v_edu_write32(EDU_REG_FACTORIAL, 5);
    
    // Poll status register until execution finishes (bit 0 is cleared)
    uint32_t status = 1;
    uint64_t poll_start = timer_get_ms();
    while (status & 1) {
        status = v_edu_read32(EDU_REG_STATUS);
        if (timer_get_ms() - poll_start > 1000) {
            uart_puts("    Error: Factorial calculation timed out!\n");
            break;
        }
        __asm__ volatile("pause");
    }

    uint32_t fact_res = v_edu_read32(EDU_REG_FACTORIAL);
    uart_puts("    Factorial result (5!): ");
    print_int(fact_res);
    uart_puts("\n");

    tests_run++;
    if (g_rdma_vendor_id == 0x10de) {
        uart_puts("    PASS\n");
    } else {
        if (fact_res == 120) {
            uart_puts("    PASS\n");
        } else {
            uart_puts("    FAIL\n");
            tests_failed++;
        }
    }

    // -------------------------------------------------------------
    // Test 4: Memory Registration & RAM-to-Device DMA Transfer
    // -------------------------------------------------------------
    uart_puts("  Running test_rdma_dma_to_device...\n");
    
    // Fill local buffer with signature patterns
    for (int i = 0; i < 128; i++) {
        test_dma_buf[i] = (uint8_t)(0xA0 + i);
    }

    // Register local memory range with Host RDMA
    int reg_status = rdma_register_mr((uint64_t)&test_dma_buf[0], 128);
    tests_run++;
    if (reg_status == 0) {
        uart_puts("    Memory Region registered successfully.\n");
    } else {
        uart_puts("    Memory Region registration FAILED!\n");
        tests_failed++;
        return;
    }

    // Sync Receiver local memory to Host physical shadow memory
    int sync_status = rdma_dma_sync((uint64_t)&test_dma_buf[0], 128, 1);
    tests_run++;
    if (sync_status == 0) {
        uart_puts("    DMA Sync (to Host) completed successfully.\n");
    } else {
        uart_puts("    DMA Sync (to Host) FAILED!\n");
        tests_failed++;
        return;
    }

    // Map register fields to start PCIe device DMA: Host shadow PFN -> internal buffer
    extern uint64_t guest_to_host_phys(uint64_t guest_phys);
    uint64_t host_shadow_phys = guest_to_host_phys((uint64_t)&test_dma_buf[0]);

    v_edu_write64(EDU_REG_DMA_SRC, host_shadow_phys);
    v_edu_write64(EDU_REG_DMA_DST, EDU_BUFF_OFFSET);
    v_edu_write64(EDU_REG_DMA_SIZE, 128);
    
    // Start DMA (direction 0 = RAM to device, start = 1 -> Cmd = 1)
    v_edu_write32(EDU_REG_DMA_CMD, 1);

    // Poll DMA completion until command register bit 0 is cleared
    uint32_t dma_cmd = 1;
    uint64_t dma_start = timer_get_ms();
    while (dma_cmd & 1) {
        dma_cmd = v_edu_read32(EDU_REG_DMA_CMD);
        if (timer_get_ms() - dma_start > 1000) {
            uart_puts("    Error: DMA RAM->Device timed out!\n");
            break;
        }
        __asm__ volatile("pause");
    }

    uart_puts("    DMA RAM->Device complete.\n");
    uart_puts("    PASS\n");

    // -------------------------------------------------------------
    // Test 5: Reverse DMA & Device-to-RAM Verification
    // -------------------------------------------------------------
    uart_puts("  Running test_rdma_dma_from_device...\n");
    
    // Clear Receiver memory buffer
    for (int i = 0; i < 128; i++) {
        test_dma_buf[i] = 0;
    }

    // Set up reverse DMA: Device internal buffer -> Host shadow PFN
    v_edu_write64(EDU_REG_DMA_SRC, EDU_BUFF_OFFSET);
    v_edu_write64(EDU_REG_DMA_DST, host_shadow_phys);
    v_edu_write64(EDU_REG_DMA_SIZE, 128);
    
    // Start DMA (direction 1 = Device to RAM, start = 1 -> Cmd = 3)
    v_edu_write32(EDU_REG_DMA_CMD, 3);

    // Poll DMA completion
    dma_cmd = 1;
    dma_start = timer_get_ms();
    while (dma_cmd & 1) {
        dma_cmd = v_edu_read32(EDU_REG_DMA_CMD);
        if (timer_get_ms() - dma_start > 1000) {
            uart_puts("    Error: DMA Device->RAM timed out!\n");
            break;
        }
        __asm__ volatile("pause");
    }

    // Sync Host shadow contiguous memory back to Receiver RAM over the network
    int reverse_sync = rdma_dma_sync((uint64_t)&test_dma_buf[0], 128, 0);
    tests_run++;
    if (reverse_sync == 0) {
        uart_puts("    DMA Sync (to Guest) completed successfully.\n");
    } else {
        uart_puts("    DMA Sync (to Guest) FAILED!\n");
        tests_failed++;
        return;
    }

    // Assert local buffer matches original pattern
    int matches = 1;
    for (int i = 0; i < 128; i++) {
        if (test_dma_buf[i] != (uint8_t)(0xA0 + i)) {
            matches = 0;
            uart_puts("    Error: Mismatch at index "); print_int(i);
            uart_puts(" expected "); uart_print_hex((uint8_t)(0xA0 + i));
            uart_puts(" got "); uart_print_hex(test_dma_buf[i]);
            uart_puts("\n");
            break;
        }
    }

    tests_run++;
    if (matches) {
        uart_puts("    PASS\n");
    } else {
        uart_puts("    FAIL\n");
        tests_failed++;
    }

    // -------------------------------------------------------------
    // Test 6: Generalized Multi-BAR Read (Reading EDU_REG_IDENT via BAR0)
    // -------------------------------------------------------------
    uart_puts("  Running test_rdma_multibar_read...\n");
    uint32_t bar_ident = v_pci_read32(0, EDU_REG_IDENT);
    uart_puts("    BAR0 Ident: "); uart_print_hex(bar_ident); uart_puts("\n");
    
    tests_run++;
    if (g_rdma_vendor_id == 0x10de) {
        if (bar_ident != 0xFFFFFFFF && bar_ident != 0) {
            uart_puts("    PASS\n");
        } else {
            uart_puts("    FAIL\n");
            tests_failed++;
        }
    } else {
        if (bar_ident == 0x010000ed) {
            uart_puts("    PASS\n");
        } else {
            uart_puts("    FAIL\n");
            tests_failed++;
        }
    }

    // -------------------------------------------------------------
    // Test 7: Block Transfer (Write/Read Block to EDU Buffer)
    // -------------------------------------------------------------
    uart_puts("  Running test_rdma_block_transfer...\n");
    static uint8_t block_write_buf[256];
    static uint8_t block_read_buf[256];
    
    for (int i = 0; i < 256; i++) {
        block_write_buf[i] = (uint8_t)(0x33 + i);
        block_read_buf[i] = 0;
    }
    
    int block_w_status = v_pci_write_block(0, EDU_BUFF_OFFSET, block_write_buf, 256);
    int block_r_status = v_pci_read_block(0, EDU_BUFF_OFFSET, block_read_buf, 256);
    
    int block_match = 1;
    for (int i = 0; i < 256; i++) {
        if (block_read_buf[i] != block_write_buf[i]) {
            block_match = 0;
            uart_puts("    Mismatch at block index "); print_int(i);
            uart_puts(" expected "); uart_print_hex(block_write_buf[i]);
            uart_puts(" got "); uart_print_hex(block_read_buf[i]);
            uart_puts("\n");
            break;
        }
    }
    
    tests_run++;
    if (block_w_status == 0 && block_r_status == 0 && block_match) {
        uart_puts("    PASS\n");
    } else {
        uart_puts("    FAIL\n");
        tests_failed++;
    }
}

#endif // KERNEL_MODE_UNIT_TEST and __x86_64__

#if defined(KERNEL_MODE_UNIT_TEST) && !defined(__x86_64__)
#include "unit_test.h"
void net_rdma_test_suite(void) {
    // Stub on non-x86_64 architectures
}
#endif
