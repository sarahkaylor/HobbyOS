#ifndef FILE_OBJ_H
#define FILE_OBJ_H

/*
 * so-vm lane A (docs/shared-libs.md section 4): the VM_OBJ_FILE page cache.
 *
 * One VM_OBJ_FILE object per FAT16 file, keyed by (start_cluster,
 * file_size) — the identity the loader already uses for the lazy v2 IMAGE
 * (as->img_start_cluster / img_file_size).  Every process that mmaps the
 * same file gets the SAME object and therefore the same physical frames:
 * that is the acceptance property of the shared-library page cache
 * (docs/shared-libs.md section 1 consequence 2).  Pages are filled from
 * the file on first touch by the demand fault, never eagerly at map time.
 *
 * Lock order (see vm.c's header comment; the full documented order is in
 * lock.h): file_obj_lock -> vm_lock -> frame_lock, and file_obj_lock ->
 * fat_lock never happens.  file_obj_lock is a plain unregistered leaf-ish
 * lock (the lock-order tracker ignores unregistered locks); vm.c never
 * takes it, and the fill path runs with NO lock held because it performs
 * disk I/O (fat_lock sits ABOVE vm_lock in the documented order).
 */

#include <stdint.h>

struct vm_object;
struct process;

/* --- implemented in fs.c (CROSS-LANE additive; fs.h is frozen for lane A) ---
 * Resolve an fd to a regular FAT16 file's on-disk identity so SYS_MMAP can
 * key the page cache.  Returns 0 and fills the out-params (either may be
 * NULL), -EBADF for an absent fd, -ENOTSUP for any other fd type
 * (memfd/pipe/socket/NFS/dir — those keep their existing mmap answers). */
int file_fat16_ident(struct process *p, int fd, uint16_t *cluster,
                     uint32_t *size);

/* GB-308: write MAP_SHARED pages back through an fd the process still holds on
 * that file (the object cannot carry a path -- this kernel has no kmalloc).  0,
 * or -EBADF when no such fd exists, in which case the caller must not claim
 * durability. */
struct trap_frame;
int file_writeback_fat16(struct process *p, uint16_t cluster, uint32_t size,
                         uint32_t off, const void *kbuf, uint32_t len,
                         struct trap_frame *tf);

/* L54: resolve an fd to a REGULAR NFS file's identity (mount index + NFSv3
 * file handle + size).  This is what lets v2 mmap a file on /NFS: RimWorld's
 * XML loader uses MemoryMappedFile on Data/*.xml, and answering -ENOTSUP
 * turned every def load into "Could not map file".  Returns 0, or -EBADF /
 * -ENOTSUP for a non-NFS (or directory) fd. */
struct nfs_fh;
int file_nfs_ident(struct process *p, int fd, int *mount_idx,
                   struct nfs_fh *fh, uint32_t *size);

/* cp/ext2b: resolve an fd to a REGULAR ext2 (/EXT) file: inode, current size
 * and the O_* flags it was opened with (mmap's EACCES rules).  0, or -EBADF /
 * -ENOTSUP for any other fd type.  The page cache is ext2_obj.h. */
int file_ext2_ident(struct process *p, int fd, uint32_t *ino, uint64_t *size,
                    uint32_t *oflags);

/* --- /dev/fb0 (GB-025 / M3, implemented in fs.c) -----------------------
 * 1 when `fd` is the framebuffer node opened from /dev/fb0.  vm_mmap maps
 * it to the scanout smem (USER_FB_BASE_V2) instead of a vm_object, and the
 * compat ioctl route answers FBIOGET_VSCREENINFO/FSCREENINFO + FBIOBLANK
 * from it. 0 for every other fd (including an absent one), so callers need
 * no separate error path. */
int file_is_devfb(struct process *p, int fd);

/* --- /dev/gpu (M1, implemented in fs.c) --------------------------------
 * 1 when `fd` is the node opened from /dev/gpu.  The compat ioctl route
 * dispatches GPU_IOC_* through gpu_abi_ioctl() when this is true, and the
 * close path calls gpu_abi_release_fd() so a client's context/resources die
 * with its fd.  0 for every other fd (including an absent one). */
int file_is_devgpu(struct process *p, int fd);

/* --- /dev/nvctl (nv driver, implemented in fs.c) ------------------------
 * 1 when `fd` is the node opened from /dev/nvctl.  The compat ioctl route
 * dispatches NV_IOC_* through nv_ioctl() when this is true.  The node exists
 * whether or not a card does: a request on a machine with no NVIDIA GPU
 * answers -ENODEV, which is a fact the caller can print, rather than ENOENT,
 * which is indistinguishable from a missing driver. */
int file_is_devnv(struct process *p, int fd);

/* --- /dev/nvshim (PLAN.md §3 WP-F, implemented in fs.c) -----------------
 * 1 when `fd` is the node opened from /dev/nvshim.  The compat ioctl route
 * dispatches NVSHIM_IOC_* through nv_shim_ioctl(), and the close path calls
 * nv_shim_release_fd() so every object the client created dies with its fd.
 * Served by the mock RM, so it works with no card and on both arches. */
int file_is_devnvshim(struct process *p, int fd);

/* C4-audio: the ALSA node's minor when `fd` is one (linux_snd.h), else -1. */
int file_devsnd_minor(struct process *p, int fd);

/* --- the cache (file_obj.c) ------------------------------------------ */

/* Get-or-create the page-cache object for this file and return it with ONE
 * reference owned by the caller (release with vm_object_unref).  The cache
 * holds its own reference, so the object outlives every mapping.  Returns
 * NULL when the file is empty, larger than VM_OBJ_MAX_BYTES (the per-object
 * page-cache cap), the frame pool is exhausted, or every VM_OBJ_MAX cache
 * slot is taken by a live file — the cache never evicts a mapping. */
struct vm_object *file_obj_get(uint16_t start_cluster, uint32_t file_size);

/* The physical frame backing byte offset `off` (4096-aligned) of the
 * object, reading that 4 KiB page from the FAT16 file on first touch.  A
 * short tail (the last page of a file whose size is not a multiple of
 * FRAME_SIZE, or a read hitting EOF) is zero-filled, so bytes past EOF
 * read as zeros.  MUST be called with NO lock held (it performs disk I/O).
 * Returns 0 on failure (invalidated object, I/O error, out of frames).
 * cp-pagein (H9-12): `*err` (may be NULL) says WHICH failure a 0 is:
 * -ENOMEM when no frame could be allocated for the page (nothing was read:
 * a shortage, not a fact about the file), -EIO for everything else (a read
 * that failed or came back short, a dead object, past the capacity).  It is
 * 0 when a frame is returned. */
uint64_t file_obj_page(struct vm_object *o, uint64_t off, int *err);

/* --- L54: the NFS half of the page cache ------------------------------- */
/* Get-or-create the VM_OBJ_NFS object for (mount_idx, fh) with one reference
 * for the caller; every mapping of the same file shares its frames.  Pages
 * are read with nfs_read_file() on first touch.  Returns NULL on an empty or
 * over-cap file, a full slot table, or frame exhaustion. */
struct vm_object *nfs_obj_get(int mount_idx, const struct nfs_fh *fh,
                              uint32_t size);

/* The frame backing byte offset `off` of an NFS object, filled on first
 * touch.  MUST be called with NO lock held (it performs an RPC).  Returns 0
 * on failure, with `*err` as for file_obj_page (-ENOMEM: no frame; -EIO: the
 * read failed or was short).  Read-only: there is no invalidation path
 * because NFS mounts are read-only in this VFS. */
uint64_t nfs_obj_page(struct vm_object *o, uint64_t off, int *err);

/* cp-pagein (H9-12): page-cache reclaim for a page-in that found no frame.
 * Drops every FAT16 and NFS cache entry whose object only the cache still
 * references (no mapping: refs == 1), so its frames go back to the pool --
 * the clean page cache Linux reclaims before it declares OOM.  The next mmap
 * of such a file re-reads it.  MUST be called with NO lock held.  Returns
 * the number of cache entries dropped. */
int file_obj_reclaim_unmapped(void);

/* Invalidation (docs/shared-libs.md section 4 item 4): detach the cached
 * object so the next mmap re-reads the file.  Existing mappings keep the
 * pages they already faulted in (the object stays alive on their
 * references) but cannot fault in new ones — the residual hazard (a live
 * mapping keeps the frames it already touched) is bounded and called out
 * in the lane A report.  file_obj_invalidate_all
 * is what fs.c's write hook calls (one hook, all files: over-invalidation
 * only costs a re-read).  Both are safe with no lock held. */
void file_obj_invalidate(uint16_t start_cluster, uint32_t file_size);
void file_obj_invalidate_all(void);

/* cp/pfx: write() coherence.  After a FAT16 write of [off, off+len) reached
 * the disk (file_size = the size now), copy the written bytes into every
 * frame already materialized by a live object of that file, so MAP_SHARED
 * views and MAP_PRIVATE pages the mapper has not written see them (Linux).
 * NO lock may be held (disk I/O).  fs.c's FAT write path calls it. */
void file_obj_note_write(uint16_t cluster, uint32_t off, uint32_t len, uint32_t file_size);

/* Number of live cache entries (diagnostics / unit tests). */
int file_obj_count(void);

#endif /* FILE_OBJ_H */
