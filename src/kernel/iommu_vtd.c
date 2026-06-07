/* ============================================================================
 *  Intel VT-d (DMA Remapping) IOMMU Driver for HobbyOS
 *
 *  This driver discovers the IOMMU via ACPI DMAR table, programs the root and
 *  context tables, manages 3-level (39-bit IOVA) page tables for DMA address
 *  translation, and handles IOTLB invalidation.
 *
 *  Targets the QEMU emulated intel-iommu device:
 *    -device intel-iommu,intremap=on,caching-mode=on
 *
 *  All page tables are statically allocated — no dynamic heap is used.
 * ========================================================================= */

#ifdef __x86_64__

#include "iommu_vtd.h"
#include "arch/cpu.h"

extern void uart_puts(const char *s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);
extern void mmu_map_mmio_range(uint64_t phys_addr, uint64_t size);
extern void arch_memory_barrier(void);

/* ---------------------------------------------------------------------------
 *  Static Page Table Allocations
 *
 *  IOMMU page tables must be 4KB-aligned and physically contiguous.
 *  Since HobbyOS uses identity mapping (phys == virt), we can use the
 *  C address of these arrays directly as physical addresses.
 *
 *  Root Table:     256 entries × 16 bytes = 4KB (one per PCI bus)
 *  Context Table:  256 entries × 16 bytes = 4KB (one per devfn on a bus)
 *  L2 Table (PGD): 512 entries × 8 bytes  = 4KB (each covers 1GB of IOVA)
 *  L1 Table (PTE): 512 entries × 8 bytes  = 4KB (each covers 2MB of IOVA)
 *
 *  Root and context table entries are 128-bit (two uint64_t per entry), so
 *  we store them as pairs: [lo, hi] at indices [2*i] and [2*i+1].
 * ------------------------------------------------------------------------ */

/* Root table: 256 entries × 2 qwords = 512 qwords */
static uint64_t root_table[512] __attribute__((aligned(4096)));

/* Context tables: one per supported bus, 256 entries × 2 qwords each */
static uint64_t context_tables[VTD_MAX_CONTEXT_TABLES][512]
    __attribute__((aligned(4096)));

/* Level-2 page tables (PGD): each has 512 × 8-byte entries */
static uint64_t l2_tables[VTD_MAX_L2_TABLES][512]
    __attribute__((aligned(4096)));
static int l2_table_used[VTD_MAX_L2_TABLES];

/* Level-1 page tables (PTE): each has 512 × 8-byte entries */
static uint64_t l1_tables[VTD_MAX_L1_TABLES][512]
    __attribute__((aligned(4096)));
static int l1_table_used[VTD_MAX_L1_TABLES];

/* ---------------------------------------------------------------------------
 *  Driver State
 * ------------------------------------------------------------------------ */

/* DMAR MMIO base address (volatile for hardware register access) */
static volatile uint8_t *dmar_base = 0;
static uint64_t dmar_phys_base = 0;

/* Cached capability values */
static uint64_t vtd_cap = 0;
static uint64_t vtd_ecap = 0;
static uint32_t vtd_iotlb_offset = 0;  /* Calculated from ECAP.IRO */

/* Initialization status */
static int vtd_initialized = 0;

/* Track which context table index is assigned to which bus */
static int context_bus_map[VTD_MAX_CONTEXT_TABLES];
static int context_bus_count = 0;

/* ---------------------------------------------------------------------------
 *  MMIO Register Access Helpers
 *
 *  All reads/writes go through volatile pointers to prevent the compiler
 *  from reordering or caching hardware register accesses.
 * ------------------------------------------------------------------------ */

static inline uint32_t vtd_read32(uint32_t offset) {
    return *(volatile uint32_t *)(dmar_base + offset);
}

static inline uint64_t vtd_read64(uint32_t offset) {
    return *(volatile uint64_t *)(dmar_base + offset);
}

static inline void vtd_write32(uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(dmar_base + offset) = val;
}

static inline void vtd_write64(uint32_t offset, uint64_t val) {
    *(volatile uint64_t *)(dmar_base + offset) = val;
}

/* ---------------------------------------------------------------------------
 *  ACPI RSDP Discovery
 *
 *  Scans the BIOS read-only memory area (0xE0000 - 0xFFFFF) on 16-byte
 *  boundaries for the "RSD PTR " signature. This is the standard ACPI
 *  method for bare-metal OS to locate the Root System Description Pointer.
 * ------------------------------------------------------------------------ */

static int str_match(const char *a, const char *b, int len) {
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static struct acpi_rsdp *find_rsdp(void) {
    uart_puts("[VT-d] Scanning BIOS area for ACPI RSDP...\n");

    /* Map the BIOS ROM area so we can read it */
    mmu_map_mmio_range(RSDP_SCAN_START,
                       RSDP_SCAN_END - RSDP_SCAN_START + 1);

    /* RSDP must be aligned on a 16-byte boundary */
    for (uint64_t addr = RSDP_SCAN_START; addr < RSDP_SCAN_END; addr += 16) {
        volatile uint8_t *ptr = (volatile uint8_t *)addr;

        if (str_match((const char *)ptr, RSDP_SIGNATURE, 8)) {
            /* Validate checksum over first 20 bytes */
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) {
                sum += ptr[i];
            }
            if (sum == 0) {
                uart_puts("[VT-d] RSDP found at ");
                uart_print_hex(addr);
                uart_puts("\n");
                return (struct acpi_rsdp *)addr;
            }
        }
    }

    uart_puts("[VT-d] ERROR: ACPI RSDP not found in BIOS area!\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 *  ACPI Table Search
 *
 *  Given the RSDP, follows the XSDT (64-bit) or RSDT (32-bit) pointer
 *  to enumerate all ACPI tables, searching for one matching the given
 *  4-character signature (e.g., "DMAR").
 * ------------------------------------------------------------------------ */

static struct acpi_sdt_header *find_acpi_table(struct acpi_rsdp *rsdp,
                                                const char *sig) {
    if (!rsdp) return 0;

    /* Prefer XSDT (revision >= 2) over RSDT for 64-bit address support */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        uint64_t xsdt_phys = rsdp->xsdt_address;
        uart_puts("[VT-d] Using XSDT at ");
        uart_print_hex(xsdt_phys);
        uart_puts("\n");

        /* Map the XSDT region */
        mmu_map_mmio_range(xsdt_phys, 0x1000);

        struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)xsdt_phys;
        uint32_t num_entries = (xsdt->length - sizeof(struct acpi_sdt_header))
                               / sizeof(uint64_t);

        uart_puts("[VT-d] XSDT has ");
        print_int(num_entries);
        uart_puts(" entries\n");

        uint64_t *entries = (uint64_t *)((uint8_t *)xsdt
                            + sizeof(struct acpi_sdt_header));

        for (uint32_t i = 0; i < num_entries; i++) {
            uint64_t table_phys = entries[i];
            if (table_phys == 0) continue;

            /* Map this table's header so we can read signature */
            mmu_map_mmio_range(table_phys, 0x1000);

            struct acpi_sdt_header *hdr =
                (struct acpi_sdt_header *)table_phys;

            if (str_match(hdr->signature, sig, 4)) {
                uart_puts("[VT-d] Found '");
                uart_puts(sig);
                uart_puts("' table at ");
                uart_print_hex(table_phys);
                uart_puts("\n");

                /* Map entire table if it's larger than one page */
                if (hdr->length > 0x1000) {
                    mmu_map_mmio_range(table_phys, hdr->length);
                }
                return hdr;
            }
        }
    } else {
        /* Fallback to RSDT (32-bit addresses) */
        uint64_t rsdt_phys = (uint64_t)rsdp->rsdt_address;
        uart_puts("[VT-d] Using RSDT at ");
        uart_print_hex(rsdt_phys);
        uart_puts("\n");

        mmu_map_mmio_range(rsdt_phys, 0x1000);

        struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)rsdt_phys;
        uint32_t num_entries = (rsdt->length - sizeof(struct acpi_sdt_header))
                               / sizeof(uint32_t);

        uart_puts("[VT-d] RSDT has ");
        print_int(num_entries);
        uart_puts(" entries\n");

        uint32_t *entries = (uint32_t *)((uint8_t *)rsdt
                            + sizeof(struct acpi_sdt_header));

        for (uint32_t i = 0; i < num_entries; i++) {
            uint64_t table_phys = (uint64_t)entries[i];
            if (table_phys == 0) continue;

            mmu_map_mmio_range(table_phys, 0x1000);

            struct acpi_sdt_header *hdr =
                (struct acpi_sdt_header *)table_phys;

            if (str_match(hdr->signature, sig, 4)) {
                uart_puts("[VT-d] Found '");
                uart_puts(sig);
                uart_puts("' table at ");
                uart_print_hex(table_phys);
                uart_puts("\n");

                if (hdr->length > 0x1000) {
                    mmu_map_mmio_range(table_phys, hdr->length);
                }
                return hdr;
            }
        }
    }

    uart_puts("[VT-d] ERROR: ACPI table '");
    uart_puts(sig);
    uart_puts("' not found!\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 *  DMAR Table Parsing
 *
 *  Walks the DMAR table's remapping structure list to find the first DRHD
 *  (DMA Remapping Hardware Unit Definition) and extracts its MMIO base.
 * ------------------------------------------------------------------------ */

static int parse_dmar_table(struct acpi_sdt_header *dmar_hdr) {
    if (!dmar_hdr) return -1;

    struct acpi_dmar_header *dmar = (struct acpi_dmar_header *)dmar_hdr;

    uart_puts("[VT-d] DMAR table: host_address_width=");
    print_int(dmar->host_address_width);
    uart_puts(", flags=");
    uart_print_hex(dmar->flags);
    uart_puts("\n");

    /* Walk variable-length remapping structures after the fixed header */
    uint8_t *ptr = (uint8_t *)dmar + sizeof(struct acpi_dmar_header);
    uint8_t *end = (uint8_t *)dmar + dmar->header.length;

    while (ptr < end) {
        struct dmar_remap_header *rh = (struct dmar_remap_header *)ptr;

        if (rh->length == 0) {
            uart_puts("[VT-d] ERROR: Zero-length DMAR remapping structure!\n");
            return -1;
        }

        if (rh->type == DMAR_DRHD_TYPE) {
            struct dmar_drhd *drhd = (struct dmar_drhd *)ptr;

            uart_puts("[VT-d] DRHD found: flags=");
            uart_print_hex(drhd->flags);
            uart_puts(", segment=");
            print_int(drhd->segment);
            uart_puts(", register_base=");
            uart_print_hex(drhd->register_base);
            uart_puts("\n");

            /* Use the first DRHD we find */
            dmar_phys_base = drhd->register_base;
            return 0;
        }

        ptr += rh->length;
    }

    uart_puts("[VT-d] ERROR: No DRHD structure found in DMAR table!\n");
    return -1;
}

/* ---------------------------------------------------------------------------
 *  Page Table Pool Allocators
 *
 *  Since we can't dynamically allocate memory, these functions hand out
 *  pre-allocated tables from the static pools.
 * ------------------------------------------------------------------------ */

static uint64_t *alloc_l2_table(void) {
    for (int i = 0; i < VTD_MAX_L2_TABLES; i++) {
        if (!l2_table_used[i]) {
            l2_table_used[i] = 1;
            /* Zero the table */
            for (int j = 0; j < 512; j++) {
                l2_tables[i][j] = 0;
            }
            return l2_tables[i];
        }
    }
    uart_puts("[VT-d] ERROR: L2 page table pool exhausted!\n");
    return 0;
}

static uint64_t *alloc_l1_table(void) {
    for (int i = 0; i < VTD_MAX_L1_TABLES; i++) {
        if (!l1_table_used[i]) {
            l1_table_used[i] = 1;
            /* Zero the table */
            for (int j = 0; j < 512; j++) {
                l1_tables[i][j] = 0;
            }
            return l1_tables[i];
        }
    }
    uart_puts("[VT-d] ERROR: L1 page table pool exhausted!\n");
    return 0;
}

/**
 * Gets or allocates a context table for the given PCI bus number.
 * Returns a pointer to the context table, or NULL if the pool is exhausted.
 */
static uint64_t *get_context_table(uint8_t bus) {
    /* Check if we already have a context table for this bus */
    for (int i = 0; i < context_bus_count; i++) {
        if (context_bus_map[i] == (int)bus) {
            return context_tables[i];
        }
    }

    /* Allocate a new one */
    if (context_bus_count >= VTD_MAX_CONTEXT_TABLES) {
        uart_puts("[VT-d] ERROR: Context table pool exhausted (bus=");
        print_int(bus);
        uart_puts(")!\n");
        return 0;
    }

    int idx = context_bus_count;
    context_bus_map[idx] = (int)bus;
    context_bus_count++;

    /* Zero the context table */
    for (int j = 0; j < 512; j++) {
        context_tables[idx][j] = 0;
    }

    uart_puts("[VT-d] Allocated context table ");
    print_int(idx);
    uart_puts(" for bus ");
    print_int(bus);
    uart_puts("\n");

    return context_tables[idx];
}

/* ---------------------------------------------------------------------------
 *  Context Cache Invalidation
 *
 *  After modifying context table entries, we must invalidate the context
 *  cache so the IOMMU re-reads the updated entries.
 * ------------------------------------------------------------------------ */

void iommu_vtd_invalidate_context(void) {
    if (!vtd_initialized) return;

    /* Write global invalidation request:
     *   Bit 63: ICC (Invalidate Context Cache) — set to 1 to start
     *   Bits [62:61]: CIRG = 01 (Global) */
    uint64_t cmd = VTD_CCMD_ICC | VTD_CCMD_CIRG_GLOBAL;
    vtd_write64(VTD_CCMD_REG, cmd);

    /* Wait for ICC to clear (hardware clears it when done) */
    int timeout = 100000;
    while (timeout > 0) {
        uint64_t val = vtd_read64(VTD_CCMD_REG);
        if (!(val & VTD_CCMD_ICC)) {
            return;
        }
        timeout--;
        cpu_relax();
    }

    uart_puts("[VT-d] WARNING: Context cache invalidation timed out!\n");
}

/* ---------------------------------------------------------------------------
 *  IOTLB Invalidation
 *
 *  After modifying page table entries, we must invalidate the IOTLB so
 *  the IOMMU picks up the new translations. We use global invalidation
 *  for simplicity.
 *
 *  The IOTLB register offset is computed from ECAP.IRO (bits [17:8]).
 *  Actual register address = dmar_base + (IRO * 16) + 0x08
 * ------------------------------------------------------------------------ */

void iommu_vtd_invalidate_iotlb(void) {
    if (!vtd_initialized) return;

    uint32_t iotlb_reg = vtd_iotlb_offset + VTD_IOTLB_REG_OFF;

    /* Global invalidation with drain reads + writes:
     *   Bit 63:    IVT (start invalidation)
     *   Bits [60]: IIRG = 01 (Global invalidation granularity)
     *   Bit 49:    DR (Drain Reads)
     *   Bit 48:    DW (Drain Writes) */
    uint64_t cmd = VTD_IOTLB_IVT | VTD_IOTLB_IIRG_GLOBAL
                 | VTD_IOTLB_DR | VTD_IOTLB_DW;

    vtd_write64(iotlb_reg, cmd);
    arch_memory_barrier();

    /* Wait for IVT bit to clear (hardware clears when invalidation done) */
    int timeout = 100000;
    while (timeout > 0) {
        uint64_t val = vtd_read64(iotlb_reg);
        if (!(val & VTD_IOTLB_IVT)) {
            return;
        }
        timeout--;
        cpu_relax();
    }

    uart_puts("[VT-d] WARNING: IOTLB invalidation timed out!\n");
}

/* ---------------------------------------------------------------------------
 *  IOMMU Initialization
 *
 *  1. Discover RSDP → XSDT/RSDT → DMAR table
 *  2. Parse DMAR to find DRHD register base
 *  3. Map DMAR MMIO region
 *  4. Read capability registers
 *  5. Set up root table
 *  6. Enable DMA remapping translation
 * ------------------------------------------------------------------------ */

int iommu_vtd_init(void) {
    uart_puts("\n========================================\n");
    uart_puts("[VT-d] Intel IOMMU Driver Initializing\n");
    uart_puts("========================================\n");

    /* Zero all static tables */
    for (int i = 0; i < 512; i++) root_table[i] = 0;
    for (int i = 0; i < VTD_MAX_CONTEXT_TABLES; i++) {
        for (int j = 0; j < 512; j++) context_tables[i][j] = 0;
    }
    for (int i = 0; i < VTD_MAX_L2_TABLES; i++) {
        l2_table_used[i] = 0;
        for (int j = 0; j < 512; j++) l2_tables[i][j] = 0;
    }
    for (int i = 0; i < VTD_MAX_L1_TABLES; i++) {
        l1_table_used[i] = 0;
        for (int j = 0; j < 512; j++) l1_tables[i][j] = 0;
    }
    for (int i = 0; i < VTD_MAX_CONTEXT_TABLES; i++) {
        context_bus_map[i] = -1;
    }
    context_bus_count = 0;

    /* --- Step 1: ACPI Discovery --- */
    struct acpi_rsdp *rsdp = find_rsdp();
    if (!rsdp) {
        uart_puts("[VT-d] INIT FAILED: No RSDP\n");
        return -1;
    }

    struct acpi_sdt_header *dmar_hdr = find_acpi_table(rsdp, "DMAR");
    if (!dmar_hdr) {
        uart_puts("[VT-d] INIT FAILED: No DMAR table (intel-iommu not present?)\n");
        return -1;
    }

    /* --- Step 2: Parse DMAR for DRHD base --- */
    if (parse_dmar_table(dmar_hdr) != 0) {
        uart_puts("[VT-d] INIT FAILED: Could not parse DMAR\n");
        return -1;
    }

    /* --- Step 3: Map DMAR MMIO region --- */
    uart_puts("[VT-d] Mapping DMAR MMIO at ");
    uart_print_hex(dmar_phys_base);
    uart_puts(" (4KB)\n");

    mmu_map_mmio_range(dmar_phys_base, 0x1000);
    dmar_base = (volatile uint8_t *)dmar_phys_base;

    /* --- Step 4: Read and log capability registers --- */
    uint32_t ver = vtd_read32(VTD_VER_REG);
    vtd_cap  = vtd_read64(VTD_CAP_REG);
    vtd_ecap = vtd_read64(VTD_ECAP_REG);

    uart_puts("[VT-d] Version: ");
    uart_print_hex(ver);
    uart_puts(" (major=");
    print_int((ver >> 4) & 0xF);
    uart_puts(", minor=");
    print_int(ver & 0xF);
    uart_puts(")\n");

    uart_puts("[VT-d] CAP:  ");
    uart_print_hex(vtd_cap);
    uart_puts("\n");
    uart_puts("[VT-d]   SAGAW (Supported AGAW): ");
    uart_print_hex(VTD_CAP_SAGAW(vtd_cap));
    uart_puts("\n");
    uart_puts("[VT-d]   MGAW (Max Guest Addr Width): ");
    print_int(VTD_CAP_MGAW(vtd_cap));
    uart_puts("\n");
    uart_puts("[VT-d]   NFR (Num Fault Records): ");
    print_int(VTD_CAP_NFR(vtd_cap) + 1);
    uart_puts("\n");
    uart_puts("[VT-d]   FRO (Fault Record Offset): ");
    uart_print_hex(VTD_CAP_FRO(vtd_cap));
    uart_puts("\n");

    uart_puts("[VT-d] ECAP: ");
    uart_print_hex(vtd_ecap);
    uart_puts("\n");

    /* Calculate IOTLB register offset from ECAP.IRO */
    uint32_t ecap_iro = VTD_ECAP_IRO(vtd_ecap);
    vtd_iotlb_offset = ecap_iro * 16;
    uart_puts("[VT-d]   IRO (IOTLB Register Offset): ");
    print_int(ecap_iro);
    uart_puts(" → base offset ");
    uart_print_hex(vtd_iotlb_offset);
    uart_puts("\n");
    uart_puts("[VT-d]   IR (Interrupt Remapping): ");
    print_int(VTD_ECAP_IR(vtd_ecap));
    uart_puts("\n");

    /* Verify 39-bit AGAW support (3-level page tables) */
    uint32_t sagaw = VTD_CAP_SAGAW(vtd_cap);
    if (!(sagaw & (1 << 1))) {
        uart_puts("[VT-d] ERROR: Hardware does not support 39-bit AGAW! SAGAW=");
        uart_print_hex(sagaw);
        uart_puts("\n");
        return -1;
    }
    uart_puts("[VT-d] 39-bit AGAW (3-level) supported ✓\n");

    /* --- Step 5: Set up Root Table --- */
    uint64_t root_table_phys = (uint64_t)&root_table[0];
    uart_puts("[VT-d] Root table at phys ");
    uart_print_hex(root_table_phys);
    uart_puts("\n");

    /* Write root table address to RTADDR register */
    vtd_write64(VTD_RTADDR_REG, root_table_phys);
    arch_memory_barrier();

    /* Issue GCMD.SRTP (Set Root Table Pointer) command */
    uint32_t gsts = vtd_read32(VTD_GSTS_REG);
    vtd_write32(VTD_GCMD_REG, gsts | VTD_GCMD_SRTP);

    /* Wait for GSTS.RTPS (Root Table Pointer Status) to be set */
    int timeout = 100000;
    while (timeout > 0) {
        gsts = vtd_read32(VTD_GSTS_REG);
        if (gsts & VTD_GSTS_RTPS) break;
        timeout--;
        cpu_relax();
    }

    if (!(gsts & VTD_GSTS_RTPS)) {
        uart_puts("[VT-d] ERROR: Timed out waiting for GSTS.RTPS!\n");
        return -1;
    }
    uart_puts("[VT-d] Root Table Pointer set ✓ (GSTS=");
    uart_print_hex(gsts);
    uart_puts(")\n");

    /* --- Step 5b: Invalidate caches before enabling translation --- */

    /* Invalidate context cache (global) */
    uint64_t ccmd = VTD_CCMD_ICC | VTD_CCMD_CIRG_GLOBAL;
    vtd_write64(VTD_CCMD_REG, ccmd);
    timeout = 100000;
    while (timeout > 0) {
        if (!(vtd_read64(VTD_CCMD_REG) & VTD_CCMD_ICC)) break;
        timeout--;
        cpu_relax();
    }
    if (timeout == 0) {
        uart_puts("[VT-d] WARNING: Context cache pre-invalidation timed out\n");
    } else {
        uart_puts("[VT-d] Context cache pre-invalidated ✓\n");
    }

    /* Invalidate IOTLB (global) */
    uint32_t iotlb_reg = vtd_iotlb_offset + VTD_IOTLB_REG_OFF;
    uint64_t iotlb_cmd = VTD_IOTLB_IVT | VTD_IOTLB_IIRG_GLOBAL
                        | VTD_IOTLB_DR | VTD_IOTLB_DW;
    vtd_write64(iotlb_reg, iotlb_cmd);
    arch_memory_barrier();
    timeout = 100000;
    while (timeout > 0) {
        if (!(vtd_read64(iotlb_reg) & VTD_IOTLB_IVT)) break;
        timeout--;
        cpu_relax();
    }
    if (timeout == 0) {
        uart_puts("[VT-d] WARNING: IOTLB pre-invalidation timed out\n");
    } else {
        uart_puts("[VT-d] IOTLB pre-invalidated ✓\n");
    }

    /* --- Step 6: Mark hardware ready but do NOT yet enable translation ---
     * Translation will be enabled AFTER the GPU passthrough context entry
     * is installed by net_rdma.c via iommu_vtd_set_passthrough() +
     * iommu_vtd_enable_translation(). Enabling translation with an empty
     * root table would block ALL device DMA (including GPU firmware init),
     * causing the GPU to hang before it can respond to MMIO reads. */
    vtd_initialized = 1;

    uart_puts("[VT-d] Hardware ready (translation NOT YET enabled)\n");
    uart_puts("[VT-d] Call iommu_vtd_set_passthrough() + iommu_vtd_enable_translation() to activate\n");

    /* Log fault status for diagnostics */
    uint32_t fsts = vtd_read32(VTD_FSTS_REG);
    uart_puts("[VT-d] Fault Status Register: ");
    uart_print_hex(fsts);
    uart_puts("\n");

    uart_puts("========================================\n");
    uart_puts("[VT-d] Intel IOMMU initialized successfully!\n");
    uart_puts("========================================\n\n");

    return 0;
}

/* ---------------------------------------------------------------------------
 *  Set Pass-Through Context Entry
 *
 *  Installs a Translation Type = 2 (pass-through) context entry for the
 *  specified PCI device. In pass-through mode, the IOMMU forwards all DMA
 *  from the device to the same physical address (IOVA == GPA). This must
 *  be called before enabling translation so the GPU firmware can DMA freely.
 * ------------------------------------------------------------------------ */

int iommu_vtd_set_passthrough(uint8_t bus, uint8_t devfn) {
    if (!vtd_initialized) {
        uart_puts("[VT-d] ERROR: set_passthrough called before init!\n");
        return -1;
    }

    uart_puts("[VT-d] Installing passthrough context entry: bus=");
    print_int(bus);
    uart_puts(" devfn=");
    uart_print_hex(devfn);
    uart_puts("\n");

    /* Get or allocate context table for this bus */
    uint64_t *ctx_table = get_context_table(bus);
    if (!ctx_table) {
        uart_puts("[VT-d] ERROR: Failed to allocate context table for passthrough!\n");
        return -1;
    }

    /* Install root table entry for this bus */
    if (!(root_table[bus * 2] & VTD_ROOT_PRESENT)) {
        uint64_t ctx_phys = (uint64_t)ctx_table;
        root_table[bus * 2] = ctx_phys | VTD_ROOT_PRESENT;
        root_table[bus * 2 + 1] = 0;
    }

    /* Install passthrough context entry for this device.
     * Translation Type = 10b (0x8) = pass-through.
     * SLPTPTR is zero (ignored in pass-through mode).
     * AW = 0 (ignored in pass-through mode). */
    ctx_table[devfn * 2]     = VTD_CTX_PRESENT | VTD_CTX_TT_PASSTHROUGH;
    ctx_table[devfn * 2 + 1] = 0;

    arch_memory_barrier();

    uart_puts("[VT-d] Passthrough context entry installed\n");

    /* Invalidate context cache so IOMMU picks up the new entry */
    iommu_vtd_invalidate_context();
    iommu_vtd_invalidate_iotlb();

    return 0;
}

/* ---------------------------------------------------------------------------
 *  Enable DMA Remapping Translation
 *
 *  Activates the VT-d IOMMU globally. Must be called AFTER all required
 *  passthrough context entries are installed (e.g., for the GPU).
 * ------------------------------------------------------------------------ */

int iommu_vtd_enable_translation(void) {
    if (!vtd_initialized) {
        uart_puts("[VT-d] ERROR: enable_translation called before init!\n");
        return -1;
    }

    uart_puts("[VT-d] Enabling DMA remapping translation...\n");

    uint32_t gsts = vtd_read32(VTD_GSTS_REG);
    vtd_write32(VTD_GCMD_REG, gsts | VTD_GCMD_TE);

    int timeout = 100000;
    while (timeout > 0) {
        gsts = vtd_read32(VTD_GSTS_REG);
        if (gsts & VTD_GSTS_TES) break;
        timeout--;
        cpu_relax();
    }

    if (!(gsts & VTD_GSTS_TES)) {
        uart_puts("[VT-d] ERROR: Timed out waiting for GSTS.TES!\n");
        return -1;
    }

    uart_puts("[VT-d] Translation ENABLED ✓ (GSTS=");
    uart_print_hex(gsts);
    uart_puts(")\n");

    uint32_t fsts = vtd_read32(VTD_FSTS_REG);
    uart_puts("[VT-d] Fault Status after enable: ");
    uart_print_hex(fsts);
    uart_puts("\n");

    return 0;
}

/* ---------------------------------------------------------------------------
 *  IOMMU DMA Address Mapping
 *
 *  Programs the root table, context table, and 3-level page tables to
 *  create an IOVA → physical address mapping for a specific PCI device.
 *
 *  Page Table Structure (39-bit / 3-level):
 *
 *    IOVA bits [38:30] → L2 (PGD) index  (512 entries, each covers 1GB)
 *    IOVA bits [29:21] → L1 (PMD) index  (512 entries, each covers 2MB)
 *    IOVA bits [20:12] → L0 (PTE) index  (512 entries, each covers 4KB)
 *    IOVA bits [11:0]  → page offset
 *
 *  Wait — VT-d spec uses different naming. Let me clarify:
 *    Level 3 (SLPTPTR from context entry): 512 entries → bits [38:30]
 *    Level 2: 512 entries → bits [29:21]
 *    Level 1: 512 entries → bits [20:12] → final 4KB page mapping
 *
 *  Our static pools:
 *    l2_tables = Level 3 tables (top-level, pointed to by context entry)
 *    l1_tables = Level 2 tables (mid-level, pointed to by l2 entries)
 *    For 4KB pages we need a Level 1 table too — but we can collapse
 *    the last two levels. Actually, let's implement all three correctly.
 *
 *  Correction: With 39-bit AGAW (3-level), the hierarchy is:
 *    Context Entry → points to Level-3 table (SLPTE L3)
 *    L3 entry → points to Level-2 table
 *    L2 entry → points to Level-1 table
 *    L1 entry → final 4KB page
 *
 *  We reuse our pools:
 *    l2_tables pool → used as Level-3 tables (top of hierarchy)
 *    l1_tables pool → used as Level-2 tables (middle)
 *
 *  But we'd need a third pool for Level-1 tables. Since we have limited
 *  static space, we implement 2-level mapping using 2MB large pages
 *  (if hardware supports it), or we split the l1_tables pool to serve
 *  double duty.
 *
 *  For simplicity and correctness in the QEMU environment, we use a
 *  3-level walk with our two pools:
 *    - l2_tables → top-level (Level 3, a.k.a. "PGD")
 *    - l1_tables → bottom-level (Level 1 final PTEs for 4KB pages)
 *  And we allocate Level 2 tables from the l1_tables pool as well
 *  (they have the same format and size).
 * ------------------------------------------------------------------------ */

/**
 * Helper: get or create the L2 (mid-level) page table for a given
 * L3 (top-level) entry. Returns pointer to the L2 table.
 */
static uint64_t *get_or_alloc_next_table(uint64_t *parent, int index) {
    if (parent[index] & VTD_PTE_PRESENT) {
        /* Table already exists; extract its physical address */
        uint64_t phys = parent[index] & VTD_PTE_ADDR_MASK;
        return (uint64_t *)phys;
    }

    /* Allocate a new table from the L1 pool (same size/alignment) */
    uint64_t *tbl = alloc_l1_table();
    if (!tbl) return 0;

    uint64_t phys = (uint64_t)tbl;
    parent[index] = phys | VTD_PTE_PRESENT | VTD_PTE_WRITE;
    return tbl;
}

int iommu_vtd_map(uint8_t bus, uint8_t devfn,
                  uint64_t iova, uint64_t phys, uint64_t size) {
    if (!vtd_initialized) {
        uart_puts("[VT-d] ERROR: map called before init!\n");
        return -1;
    }

    if ((iova & 0xFFF) || (phys & 0xFFF) || (size & 0xFFF)) {
        uart_puts("[VT-d] ERROR: iova/phys/size must be 4KB aligned!\n");
        return -1;
    }

    if (size == 0) {
        uart_puts("[VT-d] WARNING: map called with size=0\n");
        return 0;
    }


    /* --- 1. Set up Root Table Entry --- */
    uint64_t *ctx_table = get_context_table(bus);
    if (!ctx_table) return -1;

    /* Root table entry: two uint64_t per bus.
     * root_table[bus*2]   = low qword: present + context table pointer
     * root_table[bus*2+1] = high qword: reserved (0) */
    if (!(root_table[bus * 2] & VTD_ROOT_PRESENT)) {
        uint64_t ctx_phys = (uint64_t)ctx_table;
        root_table[bus * 2] = ctx_phys | VTD_ROOT_PRESENT;
        root_table[bus * 2 + 1] = 0;

    }

    /* --- 2. Set up Context Table Entry --- */
    /* Context table entry: two uint64_t per devfn.
     * ctx[devfn*2]   = low: present + translation type + SLPTPTR
     * ctx[devfn*2+1] = high: address width + domain ID */
    uint64_t l3_phys = ctx_table[devfn * 2] & VTD_PTE_ADDR_MASK;
    
    if (!(ctx_table[devfn * 2] & VTD_CTX_PRESENT) || l3_phys == 0) {
        /* Allocate a top-level (L3) page table for this device */
        uint64_t *l3_table = alloc_l2_table();
        if (!l3_table) return -1;

        l3_phys = (uint64_t)l3_table;
        ctx_table[devfn * 2] = l3_phys | VTD_CTX_PRESENT | VTD_CTX_TT_TRANSLATED;

        /* High qword: AW=1 (39-bit, 3-level), Domain ID = devfn for now */
        uint64_t domain_id = ((uint64_t)bus << 8) | (uint64_t)devfn;
        ctx_table[devfn * 2 + 1] = (uint64_t)VTD_CTX_AW_39BIT
                                  | (domain_id << 8);

        /* If we transitioned from passthrough, invalidate context cache immediately */
        iommu_vtd_invalidate_context();
    }

    /* Get L3 table pointer from context entry */
    uint64_t *l3_table = (uint64_t *)l3_phys;

    /* --- 3. Fill page table entries for each 4KB page --- */
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t va = iova + offset;
        uint64_t pa = phys + offset;

        /* Decompose IOVA into table indices:
         *   L3 index: bits [38:30]  (top-level)
         *   L2 index: bits [29:21]  (mid-level)
         *   L1 index: bits [20:12]  (bottom / final PTE) */
        uint32_t l3_idx = (va >> 30) & 0x1FF;
        uint32_t l2_idx = (va >> 21) & 0x1FF;
        uint32_t l1_idx = (va >> 12) & 0x1FF;

        /* Walk/create L3 → L2 table */
        uint64_t *l2_table = get_or_alloc_next_table(l3_table, l3_idx);
        if (!l2_table) {
            uart_puts("[VT-d] ERROR: Failed to allocate L2 table!\n");
            return -1;
        }

        /* Walk/create L2 → L1 table */
        uint64_t *l1_table = get_or_alloc_next_table(l2_table, l2_idx);
        if (!l1_table) {
            uart_puts("[VT-d] ERROR: Failed to allocate L1 table!\n");
            return -1;
        }

        /* Set the final L1 PTE (4KB page mapping) */
        l1_table[l1_idx] = (pa & VTD_PTE_ADDR_MASK)
                          | VTD_PTE_PRESENT | VTD_PTE_WRITE;

        offset += 0x1000;  /* 4KB */
    }

    /* Ensure all page table writes are visible before invalidation */
    arch_memory_barrier();

    /* Invalidate context cache and IOTLB */
    iommu_vtd_invalidate_context();
    iommu_vtd_invalidate_iotlb();

    return 0;
}

/* ---------------------------------------------------------------------------
 *  IOMMU DMA Address Unmapping
 *
 *  Clears page table entries for the given IOVA range. Does NOT free the
 *  page table structures themselves (they remain allocated for reuse).
 * ------------------------------------------------------------------------ */

int iommu_vtd_unmap(uint8_t bus, uint8_t devfn,
                    uint64_t iova, uint64_t size) {
    if (!vtd_initialized) {
        uart_puts("[VT-d] ERROR: unmap called before init!\n");
        return -1;
    }

    if ((iova & 0xFFF) || (size & 0xFFF)) {
        uart_puts("[VT-d] ERROR: iova/size must be 4KB aligned!\n");
        return -1;
    }

    if (size == 0) return 0;


    /* Verify root table entry exists for this bus */
    if (!(root_table[bus * 2] & VTD_ROOT_PRESENT)) {
        uart_puts("[VT-d] WARNING: No root entry for bus ");
        print_int(bus);
        uart_puts("\n");
        return -1;
    }

    /* Get context table */
    uint64_t ctx_phys = root_table[bus * 2] & VTD_PTE_ADDR_MASK;
    uint64_t *ctx_table = (uint64_t *)ctx_phys;

    /* Verify context entry exists */
    if (!(ctx_table[devfn * 2] & VTD_CTX_PRESENT)) {
        uart_puts("[VT-d] WARNING: No context entry for devfn ");
        print_int(devfn);
        uart_puts("\n");
        return -1;
    }

    /* Get L3 table */
    uint64_t l3_phys = ctx_table[devfn * 2] & VTD_PTE_ADDR_MASK;
    if (l3_phys == 0) return 0; // Nothing to unmap
    
    uint64_t *l3_table = (uint64_t *)l3_phys;

    /* Clear page table entries */
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t va = iova + offset;

        uint32_t l3_idx = (va >> 30) & 0x1FF;
        uint32_t l2_idx = (va >> 21) & 0x1FF;
        uint32_t l1_idx = (va >> 12) & 0x1FF;

        /* Walk L3 → L2 (don't allocate, just follow existing entries) */
        if (l3_table[l3_idx] & VTD_PTE_PRESENT) {
            uint64_t *l2_table = (uint64_t *)(l3_table[l3_idx]
                                              & VTD_PTE_ADDR_MASK);
            if (l2_table[l2_idx] & VTD_PTE_PRESENT) {
                uint64_t *l1_table = (uint64_t *)(l2_table[l2_idx]
                                                  & VTD_PTE_ADDR_MASK);
                /* Clear the L1 PTE */
                l1_table[l1_idx] = 0;
            }
        }

        offset += 0x1000;
    }

    arch_memory_barrier();

    /* Invalidate after clearing entries */
    iommu_vtd_invalidate_iotlb();

    return 0;
}

/* ---------------------------------------------------------------------------
 *  Status Query
 * ------------------------------------------------------------------------ */

int iommu_vtd_is_active(void) {
    return vtd_initialized;
}

#endif /* __x86_64__ */
