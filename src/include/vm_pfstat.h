/* vm_pfstat.h -- H7-fault: what a demand fault costs (docs/cp-fault.md §2).
 *
 * The [PF*] rows: faults by kind, a latency histogram, the time a fault spends
 * waiting for its page's I/O, for vm_lock and holding it, zeroing frames, TLB
 * shootdowns, and the interrupts that land on user threads.  Diagnostic only:
 * no hook changes what the fault path does.  x86_64 (the Linux ABI, where the
 * game runs); every hook is an empty inline elsewhere, so ARM builds the same
 * call sites. */
#ifndef VM_PFSTAT_H
#define VM_PFSTAT_H

#include <stdint.h>

enum pf_kind {
  PFK_ANON = 0, /* private anonymous zero-fill */
  PFK_SHOBJ,    /* a shared object's page (shm, memfd, tmpfs MAP_SHARED) */
  PFK_NFS,      /* file-backed, NFS (the game copy, Wine's tree) */
  PFK_EXT2,     /* file-backed, ext2 (the prefix) */
  PFK_FAT,      /* file-backed, FAT16 (disk.img) */
  PFK_IMG,      /* a native v2 IMAGE page */
  PFK_COW,      /* write to a COW leaf (fork share, or a private file page) */
  PFK_PRIV,     /* MAP_PRIVATE over memfd/tmpfs: read-through or copy */
  PFK_RETRY,    /* the leaf was already present: TLB race / prot-up / refault */
  PFK_ERR,      /* no page: hole, protection, EIO, OOM, BUS */
  PFK_N
};

#define PF_HB 17 /* histogram buckets: < 1,2,4,...,32768 us, and the rest */

#if defined(__x86_64__)
static inline uint64_t pf_tsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
/* vm_handle_fault: open and close one fault on this CPU (nested
   opens are folded into the outermost). */
void pfstat_begin(int kmode, uint64_t va);
void pfstat_end(void);
void pfstat_kind(int kind);    /* the path that resolved it (last call wins) */
/* I/O inside the open fault: the page's source read (cache or device). */
void pfstat_io(uint64_t tsc);
void pfstat_rpc(void);         /* nfs.c: one READ/RPC went on the wire */
/* vm_lock, every site in vm.c (vm_lock_acq/rel wrappers). */
void pfstat_lock_acquired(uint64_t t_before, int site);
void pfstat_lock_releasing(void);
/* frame.c */
void pfstat_frame_alloc(uint64_t phys);
void pfstat_zeroed(uint64_t phys, uint64_t tsc);
/* mmu.c */
void pfstat_shoot(uint64_t tsc, int targets);
/* trap.c: a trap from user mode that returned to the same thread. */
void pfstat_trap(uint64_t vector, uint64_t tsc);
/* per-thread totals (tsc), for [AWAY] */
uint64_t pfstat_thr_pf(int slot);
uint64_t pfstat_thr_irq(int slot);
uint64_t pfstat_tsc_to_us(uint64_t tsc);
void pfstat_emit_now(void);    /* test syscall / probe: one row set now */
#else
static inline uint64_t pf_tsc(void) { return 0; }
static inline void pfstat_begin(int kmode, uint64_t va) { (void)kmode; (void)va; }
static inline void pfstat_end(void) {}
static inline void pfstat_kind(int kind) { (void)kind; }
static inline void pfstat_io(uint64_t tsc) { (void)tsc; }
static inline void pfstat_rpc(void) {}
static inline void pfstat_lock_acquired(uint64_t t, int site) { (void)t; (void)site; }
static inline void pfstat_lock_releasing(void) {}
static inline void pfstat_frame_alloc(uint64_t phys) { (void)phys; }
static inline void pfstat_zeroed(uint64_t phys, uint64_t tsc) { (void)phys; (void)tsc; }
static inline void pfstat_shoot(uint64_t tsc, int targets) { (void)tsc; (void)targets; }
static inline void pfstat_trap(uint64_t vector, uint64_t tsc) { (void)vector; (void)tsc; }
static inline uint64_t pfstat_thr_pf(int slot) { (void)slot; return 0; }
static inline uint64_t pfstat_thr_irq(int slot) { (void)slot; return 0; }
static inline uint64_t pfstat_tsc_to_us(uint64_t tsc) { return tsc; }
static inline void pfstat_emit_now(void) {}
#endif

#endif
