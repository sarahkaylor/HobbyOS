#include "gic.h"
#include "process.h"

// Distributor Registers (shared layout on QEMU virt for v2 and v3)
#define GICD_CTLR         0x000
#define GICD_TYPER        0x004
#define GICD_ISENABLER(n) (0x100 + (n) * 4)
#define GICD_ITARGETSR(n) (0x800 + (n) * 4)   // v2 only (per-SPI routing)
#define GICD_IPRIORITYR(n) (0x400 + (n) * 4)
#define GICD_PIDR2        0xFE8               // ArchRev nibble: 3 = GICv3

// CPU Interface Registers (v2, MMIO at GICC_BASE)
#define GICC_CTLR         0x0000
#define GICC_PMR          0x0004
#define GICC_IAR          0x000C
#define GICC_EOIR         0x0010

// v3 redistributor: each PE owns two consecutive 64KB frames starting at
// GICR_BASE.  Frame 0 = SGI/PPI registers, frame 1 = control (CTLR/TYPER/WAKER).
#define GICR_CTLR         0x0000
#define GICR_TYPER        0x0008
#define GICR_WAKER        0x0014
#define GICR_ISENABLER0   0x0100

static int gic_is_v3 = 0;

static inline void gicd_write32(uint32_t offset, uint32_t val) {
  *(volatile uint32_t*)((uint8_t*)GICD_BASE + offset) = val;
}

static inline uint32_t gicd_read32(uint32_t offset) {
  return *(volatile uint32_t*)((uint8_t*)GICD_BASE + offset);
}

static inline void gicc_write32(uint32_t offset, uint32_t val) {
  *(volatile uint32_t*)((uint8_t*)GICC_BASE + offset) = val;
}

static inline uint32_t gicc_read32(uint32_t offset) {
  return *(volatile uint32_t*)((uint8_t*)GICC_BASE + offset);
}

/* GICv3 CPU-interface system registers (group 1).  Opcode-encoded forms so
   the assembler does not need sysreg aliases. */
static inline uint32_t icc_iar1(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, s3_0_c12_c12_0" : "=r"(v)); // ICC_IAR1_EL1
  return (uint32_t)(v & 0xFFFFFF);
}

static inline void icc_eoir1(uint32_t intid) {
  __asm__ volatile("msr s3_0_c12_c12_1, %0" : : "r"((uint64_t)intid)); // ICC_EOIR1_EL1
}

static inline void icc_pmr(uint32_t priority) {
  __asm__ volatile("msr s3_0_c4_c6_0, %0" : : "r"((uint64_t)priority)); // ICC_PMR_EL1
}

static inline void icc_grp1_enable(void) {
  __asm__ volatile("msr s3_0_c12_c12_7, %0" : : "r"(1ULL)); // ICC_IGRPEN1_EL1
}

/* Find this PE's redistributor pair base by walking GICR_TYPER.ProcessorNumber. */
static uint64_t gicr_base_for_cpu(void) {
  uint32_t cpu = get_cpuid();
  for (uint64_t base = GICR_BASE; base < GICR_BASE + 0x200000; base += 0x20000) {
    uint32_t typer = *(volatile uint32_t*)(base + 0x10000 + GICR_TYPER);
    uint32_t pn = (typer >> 20) & 0xFF;      /* ProcessorNumber (aff0 on QEMU virt) */
    if (pn == cpu)
      return base;
  }
  return GICR_BASE;
}

/**
 * Detects and initializes the GIC Distributor.
 * GICv2 and GICv3 share the distributor MMIO window but differ in routing:
 * v2 uses ITARGETSR, v3 uses per-PE redistributors and IROUTER (defaults to
 * PE 0, which is fine for the SPI devices this kernel uses).
 */
void gic_init_distributor(void) {
  /* Detect the GIC version.  GICD_PIDR2.ArchRev is 2 for v2 and 3/4 for
     v3/v4 on real hardware, but QEMU's GICv3 returns 0 for the whole PIDR
     block.  Fall back to GICD_TYPER for that degenerate case: a GICv3
     distributor reports its TYPER with the upper fields populated
     (0x037A0008 on QEMU virt), while GICv2's is a small value (0xE8). */
  uint32_t pidr2 = gicd_read32(GICD_PIDR2);
  uint32_t rev = (pidr2 >> 4) & 0xF;
  // Disable Distributor initially; GICD_TYPER below is sampled after this so
  // QEMU's GIC reports its real (v3) personality rather than the legacy value
  // it serves for the first few distributor reads of the boot.
  gicd_write32(GICD_CTLR, 0);
  uint32_t typer_det = gicd_read32(GICD_TYPER);
  if (rev == 2) {
    gic_is_v3 = 0;
  } else if (rev == 3 || rev == 4) {
    gic_is_v3 = 1;
  } else {
    gic_is_v3 = (typer_det >> 24) != 0;
  }

  uint32_t typer = gicd_read32(GICD_TYPER);
  uint32_t lines = 32 * ((typer & 0x1F) + 1);

  if (!gic_is_v3) {
    // Route all SPIs to CPU 0 (v2 only)
    for (uint32_t i = 32; i < lines; i += 4) {
      gicd_write32(GICD_ITARGETSR(i / 4), 0x01010101);
    }
  }

  // Assign generic priority 0xA0 to all SPIs (v3 SGIs/PPIs are fixed)
  for (uint32_t i = gic_is_v3 ? 32 : 0; i < lines; i += 4) {
    gicd_write32(GICD_IPRIORITYR(i / 4), 0xA0A0A0A0);
  }

  // Enable Distributor: Grp0 | Grp1 (v3 needs bit 1; v2 ignores bit 1)
  gicd_write32(GICD_CTLR, gic_is_v3 ? 3 : 1);
}

/**
 * Initializes the GIC CPU Interface for the current core.
 * v2: MMIO GICC.  v3: per-PE redistributor + ICC_* system registers.
 */
void gic_init_cpu(void) {
  if (gic_is_v3) {
    uint64_t rb = gicr_base_for_cpu();
    uint64_t ctl = rb + 0x10000;

    // Wake this redistributor and enable both groups
    uint32_t waker = *(volatile uint32_t*)(ctl + GICR_WAKER);
    waker &= ~(1U << 1);                 // ProcessorSleep = 0
    *(volatile uint32_t*)(ctl + GICR_WAKER) = waker;
    for (int i = 0; i < 1000000; i++) {
      if (!(*(volatile uint32_t*)(ctl + GICR_WAKER) & (1U << 2)))
        break;                            // ChildrenAsleep clear
    }
    *(volatile uint32_t*)(ctl + GICR_CTLR) = 3; // EnableGrp0 | EnableGrp1

    // CPU interface via system registers
    icc_pmr(0xF0);
    icc_grp1_enable();

    /* Enable this core's private timer PPI (30) in its own redistributor.
       (GICD_ISENABLER is banked in v2 and covers every core at once; in v3
       that is per-redistributor, so do it here and let timer_init's call to
       gic_enable_interrupt be a no-op for PPIs on v3.) */
    uint32_t en = *(volatile uint32_t*)(rb + GICR_ISENABLER0);
    en |= (1U << 30);
    *(volatile uint32_t*)(rb + GICR_ISENABLER0) = en;
    return;
  }

  // Allow all priority levels higher than 0xF0
  gicc_write32(GICC_PMR, 0xF0);

  // Enable CPU Interface
  gicc_write32(GICC_CTLR, 1);
}

/**
 * Global GIC initialization. Configures both distributor and the primary CPU's interface.
 */
void gic_init(void) {
  gicd_write32(GICD_CTLR, 0); // Reset distributor

  gic_init_distributor();
  gic_init_cpu();
}

/**
 * Unmasks a specific interrupt ID in the GIC.
 * v3: SGIs/PPIs live in each PE's redistributor; SPIs in the distributor.
 */
void gic_enable_interrupt(uint32_t intid) {
  if (gic_is_v3 && intid < 32) {
    uint64_t rb = gicr_base_for_cpu();
    uint32_t en = *(volatile uint32_t*)(rb + GICR_ISENABLER0);
    en |= (1U << intid);
    *(volatile uint32_t*)(rb + GICR_ISENABLER0) = en;
    return;
  }
  gicd_write32(GICD_ISENABLER(intid / 32), 1 << (intid % 32));
}

/**
 * Acknowledges a pending interrupt and returns its ID.
 */
uint32_t gic_acknowledge_interrupt(void) {
  // Reading IAR also acknowledges the interrupt in the GIC hardware
  if (gic_is_v3)
    return icc_iar1();
  return gicc_read32(GICC_IAR) & 0x3FF;
}

/**
 * Signals End Of Interrupt (EOI) to the GIC.
 */
void gic_end_interrupt(uint32_t intid) {
  if (gic_is_v3)
    icc_eoir1(intid);
  else
    gicc_write32(GICC_EOIR, intid);
}
