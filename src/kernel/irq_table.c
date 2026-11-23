/* ============================================================================
 *  Interrupt-vector registry (x86_64 only)
 *
 *  One handler per vector in 0xA0..0xBF, the range the IDT reserves for
 *  MSI/MSI-X.  The trap handler in arch/x64/trap.c installs gates for the
 *  whole range and forwards the vector here; anything unregistered is
 *  swallowed after EOI.
 *
 *  On ARM this translation unit is empty: irq.h's declarations still exist so
 *  shared headers compile, but nothing calls them.
 * ========================================================================= */

#ifdef __x86_64__

#include "irq.h"
#include "kmem.h"
#include "errno.h"

/* Local APIC registers used only for the id fallback. */
#define LAPIC_ID_REG 0xFEE00020

struct irq_slot {
  void (*fn)(void *ctx);
  void *ctx;
};

/* One slot per vector in the reserved range.  Written only from driver probe
 * and from the dispatch path's read side, so no lock is needed: a handler is
 * bound once, before the vector is ever delivered. */
static struct irq_slot g_slots[IRQ_VEC_COUNT];

static int vec_ok(int vec) {
  return vec >= IRQ_VEC_FIRST && vec <= IRQ_VEC_LAST;
}

int irq_alloc_vector(void) {
  for (int v = IRQ_VEC_FIRST; v <= IRQ_VEC_LAST; v++) {
    if (g_slots[v - IRQ_VEC_FIRST].fn == 0) return v;
  }
  return -ENOSPC;
}

int irq_register_vector(int vec, void (*fn)(void *ctx), void *ctx) {
  if (!vec_ok(vec) || !fn) return -EINVAL;
  struct irq_slot *s = &g_slots[vec - IRQ_VEC_FIRST];
  if (s->fn && s->fn != fn) return -EBUSY;
  s->fn = fn;
  s->ctx = ctx;
  return 0;
}

void irq_unregister_vector(int vec) {
  if (!vec_ok(vec)) return;
  g_slots[vec - IRQ_VEC_FIRST].fn = 0;
  g_slots[vec - IRQ_VEC_FIRST].ctx = 0;
}

void irq_dispatch(int vec) {
  if (!vec_ok(vec)) return;
  void (*fn)(void *) = g_slots[vec - IRQ_VEC_FIRST].fn;
  if (!fn) return; /* no owner (or released): EOI already sent by the caller */
  fn(g_slots[vec - IRQ_VEC_FIRST].ctx);
}

uint32_t irq_lapic_id(void) {
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t c = 0;
  uint32_t d = 0;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                            : "a"(1u), "c"(0u));
  uint32_t id = (b >> 24) & 0xFFu;
  if (id != 0) return id;
  /* CPUID reports no x2APIC id in EBX: read the LAPIC id register directly
   * (identity/direct mapped below 4 GiB). */
  volatile uint32_t *id_reg = (volatile uint32_t *)phys_to_virt(LAPIC_ID_REG);
  return (*id_reg >> 24) & 0xFFu;
}

int irq_registered_count(void) {
  int n = 0;
  for (int i = 0; i < IRQ_VEC_COUNT; i++) {
    if (g_slots[i].fn) n++;
  }
  return n;
}

#endif /* __x86_64__ */
