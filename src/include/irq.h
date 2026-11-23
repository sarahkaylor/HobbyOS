#ifndef IRQ_H
#define IRQ_H

/*
 * Small interrupt-vector registry (x86_64).
 *
 * PCIe MSI/MSI-X delivers a device interrupt as a vector handed to the LAPIC,
 * so the kernel needs one place that says "vector 0xA3 belongs to this
 * device" and one entry point the trap handler can call for a vector it does
 * not otherwise know.  Vectors 0xA0..0xBF (160..191) are reserved for that:
 * they collide with nothing the IDT already installs (32-47 PIC/LAPIC timer,
 * 0x80 syscall, 0x81 reschedule IPI, 0x82 TLB shootdown, 0x83 test-only panic
 * IPI, 0x84 high-resolution timer).
 *
 * The trap handler EOIs the LAPIC before calling irq_dispatch(), so a vector
 * with no registered handler is already safe to swallow.
 *
 * x86_64 only: the ARM build compiles irq_table.c to an empty translation
 * unit, and the declarations here stay available for shared headers.
 */

#include <stdint.h>

#define IRQ_VEC_FIRST 0xA0
#define IRQ_VEC_LAST  0xBF
#define IRQ_VEC_COUNT (IRQ_VEC_LAST - IRQ_VEC_FIRST + 1)

/* First vector in IRQ_VEC_FIRST..IRQ_VEC_LAST with no handler registered, or
 * -ENOSPC when all 32 are taken. */
int irq_alloc_vector(void);

/* Bind `vec` to fn(ctx).  0 on success, -EINVAL outside the vector range,
 * -EBUSY when another handler already owns it. */
int irq_register_vector(int vec, void (*fn)(void *ctx), void *ctx);

/* Release `vec` (its handler and its allocation).  Safe on a free vector. */
void irq_unregister_vector(int vec);

/* Call the handler bound to `vec`.  A no-op for an unregistered or
 * out-of-range vector; the caller has already sent EOI. */
void irq_dispatch(int vec);

/* This CPU's local APIC id (CPUID leaf 1 EBX[31:24], falling back to the
 * LAPIC id register).  The MSI/MSI-X address field is built from it. */
uint32_t irq_lapic_id(void);

/* Vectors currently bound (diagnostics/tests). */
int irq_registered_count(void);

#endif /* IRQ_H */
