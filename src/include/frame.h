#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

/*
 * P2.2 (docs/browser/p2-vm-design.md section 2): the 4 KiB frame
 * allocator.
 *
 * One bitmap over the physical pool extents; fixed-size static (bss),
 * no per-frame metadata.  The legacy 32 MiB process block layer is a
 * thin layer on the same bitmap (block = 8192 contiguous, 8192-aligned
 * frames) so v1 process semantics survive the P2 transition
 * bit-identically (design D3).
 *
 * Pool extents (design section 2.1, validated by the frame_init boot
 * log line):
 *   ARM  [0x70000000, 0x240000000)                        1,900,544 frames
 *   x64  [0x20000000, 0x70000000) + [0x100000000, 0x200000000)
 *   (L75: extent 1 starts at 4 GiB, not 2 GiB -- 2-4 GiB is the 32-bit PCI
 *   MMIO hole on this machine and the NVMe BAR lives at its base.  The
 *   2 MiB identity shadow of each kernel MMIO window is reserved too.)
 * Both extents are identity-mapped kernel-side on both arches (ARM
 * L1[1..8], x64 PD0/PD1..PD7), so a frame's kernel VA equals its
 * physical address and table frames are usable with ordinary stores.
 */
#define FRAME_SIZE  0x1000
#define FRAME_SHIFT 12

/* Pool extents (physical).  A frame's kernel VA equals its physical address
 * on both arches, so these are also the identity-mapped VA ranges.  L75: x64
 * extent 1 starts at 4 GiB -- 2-4 GiB is the 32-bit PCI MMIO hole on this
 * machine and the NVMe BAR is placed at its base, so the old extent
 * (0x80000000) handed out the controller's own register file as RAM. */
#ifdef __x86_64__
#define FRAME_EXT0_BASE 0x20000000ULL
#define FRAME_EXT0_TOP  0x70000000ULL
#define FRAME_EXT1_BASE 0x100000000ULL
#define FRAME_EXT1_TOP  0x200000000ULL
/* The two 2 MiB kernel MMIO windows sit at VA 0x1FC000000 (NVMe, PD7[480])
 * and 0x1FE000000 (virtio-gpu, PD7[496..497]).  PD7 is an identity map, so
 * installing a window there removes the identity mapping of the same
 * physical 2 MiB: a frame allocated in that range would be reachable only
 * through the window, i.e. it would BE the device.  Reserved by frame_init. */
#define FRAME_MMIO_SHADOW_LO 0x1FC000000ULL
#define FRAME_MMIO_SHADOW_HI 0x200000000ULL
#else
#define FRAME_EXT0_BASE 0x70000000ULL
#define FRAME_EXT0_TOP  0x240000000ULL
#endif

#ifdef __x86_64__
#define FRAME_POOL_EXTENTS 2
#else
#define FRAME_POOL_EXTENTS 1
#endif

/* Initialize the allocator: zero the bitmap, seed the boot log line.
 * Must run before any block/frame allocation (called from process_init). */
void frame_init(void);

/* Allocate one frame; returns its physical address, or 0 when the pool
 * is exhausted (0 is never a valid in-pool frame address). */
uint64_t frame_alloc(void);

/* frame_alloc() plus a zero-fill (the demand-paging guarantee: a frame
 * must read as zeroes before anything else can touch it). */
uint64_t frame_alloc_zeroed(void);

/* Release a frame returned by frame_alloc().  Idempotent for an aligned
 * in-pool address: clearing an already-clear bit is a no-op. */
void frame_free(uint64_t phys);

/* Is this physical frame currently owned by someone?  Diagnostic only (the
 * fault dump asks whether the frame a live leaf points at has already gone
 * back to the allocator, which is what a use-after-free looks like). */
int frame_is_alloced(uint64_t phys);

/* Watch one frame: every frame_free() naming it prints [FREEW] with the
 * caller's return address, and frame_watch_count() counts the hits. */
void frame_watch(uint64_t phys);
int frame_watch_count(void);

/* Reserve the physical range [lo, hi) (rounded outward to frames) so the
 * allocator never hands out frames over it.  Boot-time use: the kernel
 * image (text/rodata/data/bss + boot stacks) must be reserved, else the
 * forward-walking hint eventually allocates over live kernel pages.
 * Idempotent; call once at boot before any allocation. */
void frame_reserve_range(uint64_t lo, uint64_t hi);

/* Allocate `n` contiguous frames (n >= 1); returns the physical base
 * address, or 0 on failure/exhaustion.  First-fit from the rotating
 * hint; used for multi-frame kernel structures (region-array growth,
 * contiguous table runs) and by the loader for multi-page images.
 * The whole search + mark happens under one frame_lock hold. */
uint64_t frame_alloc_contig(int n);

int frame_total_count(void);
int frame_free_count(void);
int frame_used_count(void);
int frame_high_water(void);
/* L75: frames handed out at a physical address that is not RAM (the 32-bit
 * PCI hole).  Must stay 0 -- a nonzero value means a device page was handed
 * to a caller, which is how the L65-L73 "zero page"/controller reset
 * happened.  Printed by the 5 s device row. */
int frame_hole_allocs(void);

/* L75: bounds of pool extent `idx` (0-based, pool order).  Returns 0 on
 * success, -1 for an index this arch does not have.  Exists so
 * test_frame_pool_is_ram_only can pin "no extent intersects the PCI hole". */
int frame_extent_bounds(int idx, uint64_t *lo, uint64_t *hi);

/* --- 32 MiB block layer (v1 process regions) ------------------------- */

#define FRAME_BLOCK_SIZE   0x2000000
#define FRAME_BLOCK_FRAMES (FRAME_BLOCK_SIZE / FRAME_SIZE)

int frame_block_count(void);

/* Physical base of block `idx` (32 MiB-aligned, pool order).  Returns 0
 * for an out-of-range index. */
uint64_t frame_block_base_phys(int idx);

/* Are all FRAME_BLOCK_FRAMES frames of the block free? */
int frame_block_is_free(int idx);

/* Claim the lowest fully-free block and mark its frames used; returns
 * the block index, or -1 when none is free.  The whole scan and the
 * mark happen under one frame_lock hold, so two allocators cannot race
 * onto the same block. */
int frame_block_alloc_first_free(void);

/* Release a block's frames (v1 block free). */
void frame_block_free(int idx);

/* Number of fully-free blocks / of blocks currently claimed. */
int frame_phys_block_free_count(void);
int frame_blocks_used_count(void);

/* --- blk-cache reclaim hooks (docs/blk-cache.md section 6) -------------
 * Evict cached units until `want` whole 32 MiB blocks / `want` frames are
 * fully free, or the cache is empty.  Return 1 when the target was
 * reached, 0 otherwise (cache empty / not ready).  Never call with
 * frame_lock held (eviction takes its own locks). */
int frame_reclaim_to_blocks(int want);
int frame_reclaim_to_free_frames(int want);

#ifdef KERNEL_MODE_UNIT_TEST
/* Test-only helpers for the exhaustion-path check: snapshot the whole
 * bitmap, pretend the pool is full, then restore. */
void frame_test_save_all(void);
void frame_test_pretend_full(void);
void frame_test_restore_all(void);
#endif

#endif /* FRAME_H */
