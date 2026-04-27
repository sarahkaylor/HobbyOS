#include "gic.h"
#include "process.h"

// Distributor Registers
#define GICD_CTLR         0x000
#define GICD_TYPER        0x004
#define GICD_ISENABLER(n) (0x100 + (n) * 4)
#define GICD_ITARGETSR(n) (0x800 + (n) * 4)
#define GICD_IPRIORITYR(n) (0x400 + (n) * 4)

// CPU Interface Registers
#define GICC_CTLR         0x0000
#define GICC_PMR          0x0004
#define GICC_IAR          0x000C
#define GICC_EOIR         0x0010

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

void gic_init_distributor(void) {
    // Disable Distributor initially
    gicd_write32(GICD_CTLR, 0);

    uint32_t typer = gicd_read32(GICD_TYPER);
    uint32_t lines = 32 * ((typer & 0x1F) + 1);

    // Route all SPIs to CPU 0
    for (uint32_t i = 32; i < lines; i += 4) {
        gicd_write32(GICD_ITARGETSR(i / 4), 0x01010101);
    }

    // Assign generic priority 0xA0 to all interrupts
    for (uint32_t i = 0; i < lines; i += 4) {
        gicd_write32(GICD_IPRIORITYR(i / 4), 0xA0A0A0A0);
    }

    // Enable Distributor
    gicd_write32(GICD_CTLR, 1);
}

void gic_init_cpu(void) {
    // Allow all priority levels higher than 0xF0
    gicc_write32(GICC_PMR, 0xF0);

    // Enable CPU Interface
    gicc_write32(GICC_CTLR, 1);
}

void gic_init(void) {
    gic_init_distributor();
    gic_init_cpu();
}

void gic_enable_interrupt(uint32_t intid) {
    gicd_write32(GICD_ISENABLER(intid / 32), 1 << (intid % 32));
}

uint32_t gic_acknowledge_interrupt(void) {
    // Reading IAR also acknowledges the interrupt in the GIC hardware
    return gicc_read32(GICC_IAR) & 0x3FF;
}

void gic_end_interrupt(uint32_t intid) {
    // Signal EOI
    gicc_write32(GICC_EOIR, intid);
}
