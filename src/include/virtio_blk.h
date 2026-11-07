#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>

/**
 * Raw device reads/writes used by the block cache (docs/blk-cache.md):
 * the arch-specific drivers were renamed to *_raw and the PUBLIC
 * virtio_blk_read_sector()/virtio_blk_write_sector() now route through
 * blk_cache.c when it is ready.  The cache fills/invalidates through
 * these; raw keep the exact legacy semantics (including the x64
 * sector-0 write guard).
 */
int virtio_blk_read_raw(uint64_t sector, void* buf, uint32_t count);
int virtio_blk_write_raw(uint64_t sector, const void* buf, uint32_t count);

/**
 * Initializes the VirtIO Block Device using the MMIO transport.
 * Performs the VirtIO handshake and configures the request virtqueue.
 *
 * Returns:
 *   0 on success, or -1 if the device is not found or fails to initialize.
 */
int virtio_blk_init(void);

/**
 * Reads data from the block device into a memory buffer.
 *
 * Parameters:
 *   sector - The starting 512-byte sector index on the disk.
 *   buf    - The destination memory buffer.
 *   count  - The number of sectors to read.
 *
 * Returns:
 *   0 on success, or -1 on failure.
 */
int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count);

/**
 * Writes data from a memory buffer to the block device.
 *
 * Parameters:
 *   sector - The starting 512-byte sector index on the disk.
 *   buf    - The source memory buffer.
 *   count  - The number of sectors to write.
 *
 * Returns:
 *   0 on success, or -1 on failure.
 */
int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count);

/**
 * L67: periodic (5 s) controller-state row.
 *
 * Prints one line with the NVMe registers that a reset changes (CC, CSTS,
 * CAP, VS), the MMIO mapping that carries the doorbell (cpu_pd7[480]), and
 * the driver's request/devoff/reset counters.  It exists so a run yields a
 * TIMELINE of device state changes instead of only the post-mortem of a
 * wait that had already gone wrong: the wedge rows fire when a request is
 * stuck, this row says when the controller changed state, which is what
 * lets it be correlated with what the guest was doing.
 *
 * No-op on architectures without the NVMe path, and before the driver is
 * initialised.
 */
void virtio_blk_diag_row(void);

/**
 * L75: log every observable controller state change.
 *
 * Samples CC, CSTS, INTMS/INTMC and the driver's admin ring indices and
 * prints one `[BLKQ] mmio` row per transition.  Called from the 100 Hz timer
 * tick on CPU 0 (four MMIO reads, so the cost is one row per change and
 * nothing per tick otherwise).  A CC transition with no driver reset behind
 * it is the signature of something else writing the register file.
 */
void virtio_blk_watch_mmio(void);

/**
 * L75: the data-path proof across a full controller re-initialisation.
 *
 * At 4 minutes uptime, once per boot: checksum a fixed, never-written 512 KiB
 * region of the namespace through the block cache and raw, force the FULL
 * re-initialisation (CAP/VS, CC, queues, Identify, cache flush), then
 * checksum both ways again.  Prints one `[BLKQ] selfcheck` row whose
 * `match=1` is the proof that a reset leaves the data path clean and no
 * cache frame survived it.  No-op before the driver is up and on non-NVMe
 * architectures.
 */
void virtio_blk_selfcheck(void);

/**
 * L75: the block-stress probe (random small reads + write-verify cycles over
 * the free tail of the namespace), started as its own kernel thread right
 * after the driver is up.  Compiled in only with `RW_BLK_STRESS=1`
 * (make RW_BLK_STRESS=1 / harness RW_CHURN_SOAK=1); a no-op otherwise.
 *
 * It is a thread, not a call from the 5 s census: the census runs from the
 * scheduler with its locks held and blocking on the device token there wedged
 * the machine (measured).
 */
void virtio_blk_stress_start(void);

/**
 * L67: put the controller back if a host-side reset left it disabled between
 * requests.  Returns 1 when repaired, 0 when already healthy, -1 on failure.
 */
int virtio_blk_health(void);

/**
 * M6/L69: the block device's "generation".  Bumped on every event after which
 * an outstanding transfer's completion cannot be trusted:
 *
 *   - a controller reset this kernel performs (`nvme_reinit`),
 *   - a reset it DETECTS (CC.EN or CSTS.RDY cleared while a command was
 *     outstanding, i.e. someone else reset the controller under us).
 *
 * QEMU aborts the requests that were in flight when it resets the controller,
 * and an aborted AIO can complete with SUCCESS and no DMA at all: the guest
 * then reads whatever the bounce buffer held (the previous command's bytes,
 * often a page of zeros, because a frame from the allocator is virgin memory).
 * A transfer that overlapped an epoch change is therefore never used: the
 * driver re-issues it, and the block cache refuses to publish a frame it
 * filled across the change.  This is the invariant "an aborted DMA must never
 * publish a page", enforced at both layers.
 */
extern volatile uint64_t blk_dev_epoch;

/* Number of transfers discarded because they overlapped a device event. */
extern volatile uint32_t blk_untrusted;

#ifdef KERNEL_MODE_UNIT_TEST
/**
 * M6/L69 test seam (unit tier only): make the device reproduce QEMU's aborted
 * transfer.  The command still succeeds but performs NO DMA — the bounce
 * buffer keeps the previous command's 512 bytes — and the device generation
 * moves, which is how a reset announces itself.
 *
 *   mode 1: abort the next `times` reads of `sector` once each.  The driver
 *           must notice the generation change, re-issue, and return the
 *           DEVICE's bytes (never the stale bounce buffer).
 *   mode 2: abort every read of `sector`.  The driver must fail the read
 *           (-1) rather than hand out bytes it cannot vouch for.
 *
 * mode 0 clears the injection.
 */
void virtio_blk_test_abort(int mode, uint64_t sector, int times);
#endif

#endif // VIRTIO_BLK_H
