#ifndef KMEM_H
#define KMEM_H

/*
 * P1 / AD-3 (docs/cp-address-space.md section 3.3): kernel address
 * translation helpers.  Every place the kernel turns a physical address into
 * a pointer, or a kernel pointer into a physical address, goes through one of
 * these.  The AST cast scan (tools/cp/mmu/castscan_check.sh) keeps it that
 * way: an int<->pointer cast outside this header and outside the reviewed
 * allowlist fails the check.
 *
 *   phys_to_virt(pa)   RAM (and sub-4 GiB legacy MMIO such as the LAPIC and
 *                      the BGA LFB, whose attributes the shared PDs give it)
 *                      -> a kernel pointer.
 *   virt_to_phys(va)   a KERNEL pointer -> its physical address.  A user VA
 *                      is a bug (user leaves are translated by vm_arch_walk).
 *   kimg_pa(sym)       a kernel-image object (.text/.data/.bss) -> its
 *                      physical address (K0: the image runs at VA == PA).
 *   ioremap(pa, len)   a device window above the direct map's RAM (64-bit
 *                      BARs, BAR windows) -> an uncached kernel mapping.
 *
 * x86_64 (F1): the direct map (DMAP) is PML4[256], 512 GiB at
 * 0xffff800000000000, shared by every root; 0-8 GiB reuses the identity PDs
 * (identical attributes), RAM above 8 GiB gets its own 2 MiB WB leaves.  The
 * ioremap area is PML4[257] at 0xffff808000000000 (PCD|PWT, 2 MiB pages,
 * bump-allocated, never re-pointed).  2 MiB pages only: TCG's default CPU
 * model has no 1 GiB pages.
 *
 * ARM keeps its identity map (docs/cp-address-space.md section 1.9): every
 * helper is the identity there, so converted shared code compiles to the
 * same object code.
 */

#include <stdint.h>

#ifdef __x86_64__

#define DMAP_BASE 0xffff800000000000ULL
#define DMAP_SIZE 0x0000008000000000ULL /* 512 GiB (one PML4 slot) */
#define DMAP_PML4_IDX 256
#define IOREMAP_BASE 0xffff808000000000ULL
#define IOREMAP_SIZE 0x0000008000000000ULL
#define IOREMAP_PML4_IDX 257
/* RAM the direct map can cover (static PDs; 64 GiB of pool at most). */
#define DMAP_RAM_MAX 0x1000000000ULL

/* K0 (docs/cp-address-space.md section 3.1): the kernel image keeps its
 * 1:1 low address; F1's compat layout leaves [KIMG_BASE, CP_KHOLE_HI) as a
 * supervisor-only hole.  CP_KHOLE_HI is a fixed F1 constant (the image's
 * ceiling, enforced by a linker ASSERT), not the image's current end, so
 * the user-visible layout does not move when .bss grows. */
#define KIMG_BASE 0x70000000ULL
#define CP_KHOLE_HI 0x78000000ULL

/* Tripwire for virt_to_phys on a pointer that is neither DMAP nor image:
 * prints one [KMEM-BADVA] row (bounded) and returns va unchanged, which is
 * today's identity answer -- loud, never a new crash. */
uint64_t kmem_bad_va(uint64_t va, void *caller);

static inline void *phys_to_virt(uint64_t pa) {
  return (void *)(uintptr_t)(DMAP_BASE + pa);
}

static inline uint64_t virt_to_phys(const volatile void *va) {
  uint64_t v = (uint64_t)(uintptr_t)va;
  if (v - DMAP_BASE < DMAP_SIZE)
    return v - DMAP_BASE;
  if (v - KIMG_BASE < CP_KHOLE_HI - KIMG_BASE)
    return v; /* K0: image VA == PA */
  return kmem_bad_va(v, __builtin_return_address(0));
}

static inline uint64_t kimg_pa(const volatile void *sym) {
  return (uint64_t)(uintptr_t)sym;
}

/* The direct-map kernel VA of `pa` as an integer (saved stack pointers,
 * register images): phys_to_virt without a pointer type. */
static inline uint64_t phys_to_kva(uint64_t pa) { return DMAP_BASE + pa; }

/* Is `va` a direct-map pointer? (diagnostics, tests) */
static inline int kmem_is_dmap(const volatile void *va) {
  return (uint64_t)(uintptr_t)va - DMAP_BASE < DMAP_SIZE;
}

/* Map [pa, pa+len) uncached (PCD|PWT) in the ioremap area; returns the
 * kernel pointer for `pa`, or 0 when the area or its static tables are
 * exhausted.  Never unmapped (boot/probe-time windows). */
volatile void *ioremap(uint64_t pa, uint64_t len);

#else /* ARM: the identity map stays */

static inline void *phys_to_virt(uint64_t pa) { return (void *)(uintptr_t)pa; }
static inline uint64_t virt_to_phys(const volatile void *va) {
  return (uint64_t)(uintptr_t)va;
}
static inline uint64_t kimg_pa(const volatile void *sym) {
  return (uint64_t)(uintptr_t)sym;
}
static inline uint64_t phys_to_kva(uint64_t pa) { return pa; }
static inline int kmem_is_dmap(const volatile void *va) { (void)va; return 1; }
static inline volatile void *ioremap(uint64_t pa, uint64_t len) {
  (void)len;
  return (volatile void *)(uintptr_t)pa;
}

#endif

#endif /* KMEM_H */
