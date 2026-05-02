#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "mmu.h"
#include "process.h"

extern void uart_puts(const char* s);
extern uint32_t get_cpuid(void);

// Expose the internal table from mmu.c for testing
extern uint64_t l2_table_1[MAX_CPUS][512];

#define PT_MEM_NORMAL       1
#define PT_USER_RW          (0b01 << 6)

static void test_mmu_make_user_block_desc(void) {
    tests_run++;
    uart_puts("  Running test_mmu_make_user_block_desc...\n");

    uint64_t phys_addr = 0x40000000;
    uint64_t desc = mmu_make_user_block_desc(phys_addr);

    // Verify address portion
    EXPECT_EQ((desc & 0x0000FFFFFFFFF000ULL) == phys_addr, 1);

    // Verify attributes
    uint64_t attr = (PT_MEM_NORMAL << 2) | (1 << 10) | 0b01;
    attr |= PT_USER_RW;
    attr |= (1ULL << 53);

    EXPECT_EQ((desc & ~0x0000FFFFFFFFF000ULL), attr);
}

static void test_mmu_switch_user_mapping(void) {
    tests_run++;
    uart_puts("  Running test_mmu_switch_user_mapping...\n");

    uint32_t cpu = get_cpuid();
    uint64_t phys_base = 0x60000000;

    // Call the function
    mmu_switch_user_mapping(phys_base);

    // Verify that the table was updated correctly
    // The mapping starts at index 32 for USER_VIRT_BASE (0x40000000)
    int num_blocks = USER_REGION_SIZE / 0x200000;
    if (USER_REGION_SIZE % 0x200000) num_blocks++;

    for (int i = 0; i < num_blocks; i++) {
        uint64_t expected_desc = mmu_make_user_block_desc(phys_base + (uint64_t)i * 0x200000);
        EXPECT_EQ(l2_table_1[cpu][32 + i], expected_desc);
    }
}

static void test_mmu_map_user_framebuffer(void) {
    tests_run++;
    uart_puts("  Running test_mmu_map_user_framebuffer...\n");

    uint64_t phys_addr = 0x80000000;
    
    // Call the function
    mmu_map_user_framebuffer(phys_addr);

    // Verify mapping for all CPUs
    for (int c = 0; c < MAX_CPUS; c++) {
        EXPECT_EQ(l2_table_1[c][128], mmu_make_user_block_desc(phys_addr));
        EXPECT_EQ(l2_table_1[c][129], mmu_make_user_block_desc(phys_addr + 0x200000));
    }
}

void mmu_test_suite(void) {
    uart_puts("mmu_test_suite:\n");
    test_mmu_make_user_block_desc();
    test_mmu_switch_user_mapping();
    test_mmu_map_user_framebuffer();
}

#endif // KERNEL_MODE_UNIT_TEST
