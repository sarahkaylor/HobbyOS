#ifndef IOMMU_VTD_H
#define IOMMU_VTD_H

#ifdef __x86_64__

#include <stdint.h>

/* ============================================================================
 *  Intel VT-d (DMA Remapping) IOMMU Driver for HobbyOS
 *
 *  References:
 *    - Intel VT-d Specification, Rev 3.0+
 *    - ACPI Specification (DMAR table)
 *
 *  This driver targets the QEMU emulated intel-iommu device launched with:
 *    -device intel-iommu,intremap=on,caching-mode=on
 *
 *  All code is guarded by __x86_64__ since HobbyOS also targets AArch64.
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 *  DMAR MMIO Register Offsets
 *
 *  These are byte offsets from the DMAR register base address discovered
 *  via the ACPI DMAR table's DRHD structure.
 * ------------------------------------------------------------------------ */
#define VTD_VER_REG         0x00    /* Version Register (32-bit, RO)         */
#define VTD_CAP_REG         0x08    /* Capability Register (64-bit, RO)      */
#define VTD_ECAP_REG        0x10    /* Extended Capability Register (64-bit) */
#define VTD_GCMD_REG        0x18    /* Global Command Register (32-bit, WO)  */
#define VTD_GSTS_REG        0x1C    /* Global Status Register (32-bit, RO)   */
#define VTD_RTADDR_REG      0x20    /* Root Table Address Register (64-bit)  */
#define VTD_CCMD_REG        0x28    /* Context Command Register (64-bit)     */
#define VTD_FSTS_REG        0x34    /* Fault Status Register (32-bit)        */
#define VTD_FECTL_REG       0x38    /* Fault Event Control Register (32-bit) */
#define VTD_FEDATA_REG      0x3C    /* Fault Event Data Register (32-bit)    */
#define VTD_FEADDR_REG      0x40    /* Fault Event Address Register (32-bit) */
#define VTD_IQH_REG         0x80    /* Invalidation Queue Head (64-bit)      */
#define VTD_IQT_REG         0x88    /* Invalidation Queue Tail (64-bit)      */
#define VTD_IQA_REG         0x90    /* Invalidation Queue Address (64-bit)   */
#define VTD_IRTA_REG        0xB8    /* Interrupt Remap Table Address (64-bit)*/

/* ---------------------------------------------------------------------------
 *  IOTLB Register Offsets (relative to ECAP.IRO * 16)
 *
 *  The IOTLB register is NOT at a fixed offset. It is calculated as:
 *    iotlb_base = ecap_iro * 16
 *    IOTLB_REG  = iotlb_base + 0x08
 *    IVA_REG    = iotlb_base + 0x00
 * ------------------------------------------------------------------------ */
#define VTD_IOTLB_REG_OFF   0x08    /* IOTLB Invalidation Register offset   */
#define VTD_IVA_REG_OFF     0x00    /* Invalidation Address Register offset  */

/* ---------------------------------------------------------------------------
 *  Global Command Register (GCMD) Bits
 * ------------------------------------------------------------------------ */
#define VTD_GCMD_TE         (1U << 31)  /* Translation Enable                */
#define VTD_GCMD_SRTP       (1U << 30)  /* Set Root Table Pointer            */
#define VTD_GCMD_SFL        (1U << 29)  /* Set Fault Log                     */
#define VTD_GCMD_EAFL       (1U << 28)  /* Enable Advanced Fault Logging     */
#define VTD_GCMD_WBF        (1U << 27)  /* Write Buffer Flush                */
#define VTD_GCMD_QIE        (1U << 26)  /* Queued Invalidation Enable        */
#define VTD_GCMD_IRE        (1U << 25)  /* Interrupt Remapping Enable        */
#define VTD_GCMD_SIRTP      (1U << 24)  /* Set Interrupt Remap Table Pointer */
#define VTD_GCMD_CFI        (1U << 23)  /* Compatibility Format Interrupt    */

/* ---------------------------------------------------------------------------
 *  Global Status Register (GSTS) Bits
 * ------------------------------------------------------------------------ */
#define VTD_GSTS_TES        (1U << 31)  /* Translation Enable Status         */
#define VTD_GSTS_RTPS       (1U << 30)  /* Root Table Pointer Status         */
#define VTD_GSTS_FLS        (1U << 29)  /* Fault Log Status                  */
#define VTD_GSTS_AFLS       (1U << 28)  /* Advanced Fault Logging Status     */
#define VTD_GSTS_WBFS       (1U << 27)  /* Write Buffer Flush Status         */
#define VTD_GSTS_QIES       (1U << 26)  /* Queued Invalidation Enable Status */
#define VTD_GSTS_IRES       (1U << 25)  /* Interrupt Remapping Enable Status */
#define VTD_GSTS_SIRTPS     (1U << 24)  /* Set IRTP Status                   */
#define VTD_GSTS_CFIS       (1U << 23)  /* Compatibility Format Int Status   */

/* ---------------------------------------------------------------------------
 *  Context Command Register (CCMD) Bits
 * ------------------------------------------------------------------------ */
#define VTD_CCMD_ICC        (1ULL << 63) /* Invalidate Context Cache          */
#define VTD_CCMD_CIRG_GLOBAL (1ULL << 61) /* Global invalidation granularity */

/* ---------------------------------------------------------------------------
 *  IOTLB Invalidation Register Bits
 * ------------------------------------------------------------------------ */
#define VTD_IOTLB_IVT       (1ULL << 63) /* Invalidate IOTLB                */
#define VTD_IOTLB_IIRG_GLOBAL (1ULL << 60) /* Global invalidation           */
#define VTD_IOTLB_DR        (1ULL << 49) /* Drain Reads                      */
#define VTD_IOTLB_DW        (1ULL << 48) /* Drain Writes                     */

/* ---------------------------------------------------------------------------
 *  Capability Register (CAP) Field Extraction Macros
 * ------------------------------------------------------------------------ */
#define VTD_CAP_SAGAW(cap)      (((cap) >> 8) & 0x1F)   /* Supported AGAW  */
#define VTD_CAP_MGAW(cap)       (((cap) >> 16) & 0x3F)  /* Max Guest Addr W*/
#define VTD_CAP_NDOMS(cap)      ((cap) & 0x7)            /* Num Domains     */
#define VTD_CAP_SLLPS(cap)      (((cap) >> 34) & 0xF)   /* Super Large Pg  */
#define VTD_CAP_FRO(cap)        (((cap) >> 24) & 0x3FF)  /* Fault Rec Offs  */
#define VTD_CAP_NFR(cap)        (((cap) >> 40) & 0xFF)   /* Num Fault Recs  */

/* ---------------------------------------------------------------------------
 *  Extended Capability Register (ECAP) Field Extraction Macros
 * ------------------------------------------------------------------------ */
#define VTD_ECAP_IRO(ecap)      (((ecap) >> 8) & 0x3FF)  /* IOTLB Reg Offs */
#define VTD_ECAP_QI(ecap)       ((ecap) & 0x2)            /* Queued Inval   */
#define VTD_ECAP_IR(ecap)       (((ecap) >> 3) & 0x1)     /* Interrupt Remap*/
#define VTD_ECAP_C(ecap)        (((ecap) >> 0) & 0x1)     /* Coherent       */

/* ---------------------------------------------------------------------------
 *  Page Table Entry (PTE) Bit Definitions
 *
 *  These apply to both L2 (PGD) and L1 (PTE) page table entries.
 *  64-bit entries; physical address in bits [51:12], flags in low bits.
 * ------------------------------------------------------------------------ */
#define VTD_PTE_PRESENT     (1ULL << 0)   /* Read access / Present           */
#define VTD_PTE_WRITE       (1ULL << 1)   /* Write access                    */
#define VTD_PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL  /* Phys addr bits [51:12] */

/* ---------------------------------------------------------------------------
 *  Root Table Entry Format (128 bits / 16 bytes per entry)
 *
 *  Low 64 bits:
 *    Bit  0     : Present
 *    Bits [63:12]: Context Table Pointer (4KB aligned physical address)
 *  High 64 bits: Reserved (must be zero)
 * ------------------------------------------------------------------------ */
#define VTD_ROOT_PRESENT    (1ULL << 0)

/* ---------------------------------------------------------------------------
 *  Context Table Entry Format (128 bits / 16 bytes per entry)
 *
 *  Low 64 bits:
 *    Bit  0     : Present
 *    Bit  1     : Fault Processing Disable (FPD)
 *    Bits [3:2] : Translation Type (00 = untranslated only)
 *    Bits [63:12]: Second-Level Page Table Pointer (4KB aligned)
 *  High 64 bits:
 *    Bits [2:0] : Address Width (AW): 1 = 39-bit (3-level), 2 = 48-bit
 *    Bits [23:8]: Domain ID
 * ------------------------------------------------------------------------ */
#define VTD_CTX_PRESENT         (1ULL << 0)
#define VTD_CTX_FPD             (1ULL << 1)
#define VTD_CTX_TT_TRANSLATED   (0ULL << 2) /* Multi-level page table (TT=0) */
#define VTD_CTX_TT_PASS         (0ULL << 2) /* Alias — kept for compatibility  */
#define VTD_CTX_TT_PASSTHROUGH  (2ULL << 2) /* Pass-through mode (TT=2 = 0x8) — IOVA=GPA, no IOMMU translate */

/* Address Width for context entry high qword bits [2:0] */
#define VTD_CTX_AW_39BIT    1   /* 3-level page table, 39-bit IOVA space     */
#define VTD_CTX_AW_48BIT    2   /* 4-level page table, 48-bit IOVA space     */

/* ---------------------------------------------------------------------------
 *  ACPI RSDP (Root System Description Pointer) Signature
 * ------------------------------------------------------------------------ */
#define RSDP_SIGNATURE      "RSD PTR "
#define RSDP_SCAN_START     0x000E0000
#define RSDP_SCAN_END       0x000FFFFF

/* ---------------------------------------------------------------------------
 *  ACPI Table Structures
 *
 *  We define these as simple structs for parsing. Per HobbyOS design rules,
 *  multi-byte fields at potentially unaligned offsets must be read byte-by-byte.
 *  However, ACPI tables are well-aligned in practice (guaranteed by firmware),
 *  so we use volatile reads with packed structs here.
 * ------------------------------------------------------------------------ */

/* ACPI RSDP (revision 2.0, XSDT variant) */
struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR "                                */
    uint8_t  checksum;          /* Checksum over first 20 bytes              */
    char     oem_id[6];         /* OEM identification string                 */
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = ACPI 2.0+              */
    uint32_t rsdt_address;      /* 32-bit physical address of RSDT           */
    uint32_t length;            /* Length of this table (revision 2+)        */
    uint64_t xsdt_address;      /* 64-bit physical address of XSDT           */
    uint8_t  extended_checksum; /* Checksum over entire table                */
    uint8_t  reserved[3];
} __attribute__((packed));

/* Standard ACPI SDT header (shared by RSDT, XSDT, DMAR, etc.) */
struct acpi_sdt_header {
    char     signature[4];      /* Table signature (e.g. "DMAR")             */
    uint32_t length;            /* Total table length including header       */
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* ACPI DMAR table header (DMA Remapping Reporting) */
struct acpi_dmar_header {
    struct acpi_sdt_header header;
    uint8_t  host_address_width; /* Max DMA physical address width - 1       */
    uint8_t  flags;              /* Bit 0: INTR_REMAP, Bit 1: X2APIC_OPT_OUT*/
    uint8_t  reserved[10];
    /* Followed by variable-length remapping structures                      */
} __attribute__((packed));

/* DMAR Remapping Structure Header (shared prefix for DRHD, RMRR, etc.) */
struct dmar_remap_header {
    uint16_t type;               /* 0=DRHD, 1=RMRR, 2=ATSR, etc.            */
    uint16_t length;             /* Length of this structure                  */
} __attribute__((packed));

/* DRHD (DMA Remapping Hardware Unit Definition) */
#define DMAR_DRHD_TYPE       0
struct dmar_drhd {
    struct dmar_remap_header header;
    uint8_t  flags;              /* Bit 0: INCLUDE_PCI_ALL                    */
    uint8_t  reserved;
    uint16_t segment;            /* PCI segment number                        */
    uint64_t register_base;      /* Base address of DMAR MMIO registers       */
    /* Followed by Device Scope structures                                    */
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 *  Static Allocation Pool Sizes
 *
 *  All IOMMU page tables are statically allocated since HobbyOS has no
 *  dynamic heap. These sizes are chosen to provide reasonable coverage
 *  for the QEMU emulation environment.
 * ------------------------------------------------------------------------ */
#define VTD_MAX_CONTEXT_TABLES  4     /* Context tables for PCI buses 0-3    */
#define VTD_MAX_L2_TABLES       64    /* Level-2 (PGD) page tables           */
#define VTD_MAX_L1_TABLES       512   /* Level-1 (PTE) page tables           */

/* ---------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------ */

/**
 * Discovers the IOMMU via ACPI DMAR table, initializes capability registers,
 * sets up the root table, and enables DMA remapping translation.
 *
 * @return 0 on success, -1 on failure (no DMAR found, HW error, etc.)
 */
int iommu_vtd_init(void);

/**
 * Maps a contiguous IOVA range to a physical address range in the IOMMU
 * page tables for a specific PCI device.
 *
 * @param bus    PCI bus number (0-255)
 * @param devfn  PCI device/function (device << 3 | function)
 * @param iova   I/O Virtual Address (must be 4KB aligned)
 * @param phys   Physical address to map to (must be 4KB aligned)
 * @param size   Size of the mapping in bytes (must be 4KB aligned)
 * @return 0 on success, -1 on failure
 */
int iommu_vtd_map(uint8_t bus, uint8_t devfn,
                  uint64_t iova, uint64_t phys, uint64_t size);

/**
 * Installs a pass-through context entry for a device, allowing its DMA
 * to bypass IOMMU translation (IOVA == GPA). Must be called BEFORE
 * enabling VT-d translation to prevent GPU firmware stalls.
 *
 * @param bus    PCI bus number
 * @param devfn  PCI device/function
 * @return 0 on success, -1 on failure
 */
int iommu_vtd_set_passthrough(uint8_t bus, uint8_t devfn);

/**
 * Unmaps a contiguous IOVA range from the IOMMU page tables for a specific
 * PCI device. Clears the page table entries and invalidates the IOTLB.
 *
 * @param bus    PCI bus number (0-255)
 * @param devfn  PCI device/function
 * @param iova   I/O Virtual Address (must be 4KB aligned)
 * @param size   Size of the region to unmap in bytes (must be 4KB aligned)
 * @return 0 on success, -1 on failure
 */
int iommu_vtd_unmap(uint8_t bus, uint8_t devfn,
                    uint64_t iova, uint64_t size);

/**
 * Performs a global IOTLB invalidation. Must be called after any page table
 * modifications to ensure the IOMMU sees the updated mappings.
 */
void iommu_vtd_invalidate_iotlb(void);

/**
 * Performs a global context cache invalidation.
 */
void iommu_vtd_invalidate_context(void);

/**
 * Returns 1 if the IOMMU was successfully initialized, 0 otherwise.
 */
int iommu_vtd_is_active(void);

/**
 * Enables global DMA remapping translation. Must be called AFTER
 * iommu_vtd_set_passthrough() for all required devices.
 *
 * @return 0 on success, -1 on hardware timeout
 */
int iommu_vtd_enable_translation(void);

#endif /* __x86_64__ */

#endif /* IOMMU_VTD_H */
