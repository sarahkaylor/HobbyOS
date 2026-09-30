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
 *   x64  [0x20000000, 0x70000000) + [0x80000000, 0x180000000)
 *                                                         1,376,256 frames
 * Both extents are identity-mapped kernel-side on both arches (ARM
 * L1[1..8], x64 PD0/PD1..PD7), so a frame's kernel VA equals its
 * physical address and table frames are usable with ordinary stores.
 */
#define FRAME_SIZE  0x1000
#define FRAME_SHIFT 12

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

int frame_total_count(void);
int frame_free_count(void);
int frame_used_count(void);
int frame_high_water(void);

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

#ifdef KERNEL_MODE_UNIT_TEST
/* Test-only helpers for the exhaustion-path check: snapshot the whole
 * bitmap, pretend the pool is full, then restore. */
void frame_test_save_all(void);
void frame_test_pretend_full(void);
void frame_test_restore_all(void);
#endif

#endif /* FRAME_H */
