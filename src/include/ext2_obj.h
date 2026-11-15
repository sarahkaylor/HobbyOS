#ifndef EXT2_OBJ_H
#define EXT2_OBJ_H

/*
 * ext2_obj.h - VM_OBJ_EXT2: the page cache behind mmap of /EXT files
 * (cp/ext2b, docs/cp-ext2b.md section 4).
 *
 * One vm_object per (inode, capacity), shared by every mapping of the file,
 * so all MAP_SHARED mappings and every not-yet-copied MAP_PRIVATE page of one
 * file see the same frames.  Pages are read from the file on first touch
 * (ext2_obj_page, no lock held by the caller).
 *
 * Coherence with read()/write() (Linux's unified page cache, approximated):
 *   - write()/truncate() update the cached frames of the bytes they change
 *     (ext2_obj_note_write_locked / _truncate_locked, called by ext2_vfs.c
 *     under the ext2 lock), so a mapping sees later writes -- and a private
 *     mapping sees them until its page is copied, as on Linux;
 *   - MAP_SHARED writable mappings (fd opened O_RDWR) mark the object for
 *     WRITE-BACK: stores land in the cached frames and reach the file at
 *     msync(), munmap(), fsync()/fdatasync()/sync, close() of an fd of the
 *     file, read() of the file (so read() sees mapped stores), and when the
 *     cache entry is reaped after the last mapping is gone.  Only pages whose
 *     bytes differ from the file are written (holes stay holes).
 *
 * Lifetime: the cache entry holds an ext2 open reference on the inode
 * (ext2_iget), so an unlinked file stays allocated while it is mapped; an
 * entry whose object only the cache still references is reaped (written back
 * if needed, inode released) at the next ext2 open/close/unlink/rename/sync.
 *
 * Capacity: max(file size, offset + length) of the mmap that created the
 * object, capped at VM_OBJ_MAX_BYTES (64 MiB).  A later mmap that needs more
 * gets a new object; the old one keeps serving its mappings and still
 * receives write() updates.  Pages past EOF read as zeros (Linux would
 * SIGBUS) and a truncate zeroes cached bytes past the new size.
 *
 * Lock order: ext2 lock (ylock) -> ext2_obj lock (spin) -> vm_lock (spin).
 */

#include <stdint.h>

struct vm_object;

/* Get-or-create the object for `ino` with at least `cap` bytes of page
 * capacity; one reference for the caller.  `wb` != 0 marks it for
 * write-back (a MAP_SHARED mapping through a writable fd).  NULL when the
 * cache or the object pool is full, or cap > VM_OBJ_MAX_BYTES. */
struct vm_object *ext2_obj_get(uint32_t ino, uint64_t cap, int wb);

/* The frame backing byte `off` (4096-aligned), read from the file on first
 * touch; bytes past EOF are zero.  NO lock may be held (disk I/O).  0 on
 * failure (I/O error, out of frames, past the capacity). */
uint64_t ext2_obj_page(struct vm_object *o, uint64_t off);

/* Read the file's page at `off` into frame `dst` (zero past EOF), bypassing
 * the object -- the independent read the executable-page gate compares
 * against.  4096 when the page starts inside the file, 0 when it starts at
 * or past EOF, -EIO.  NO lock may be held. */
int ext2_obj_read(struct vm_object *o, uint64_t off, uint64_t dst);

/* ---- called by ext2_vfs.c with the ext2 lock HELD ---------------------- */
/* `old_size`: the file size BEFORE the change.  Cached bytes at or past the
 * smaller of the old and new size are re-zeroed: a store a MAP_SHARED
 * mapping made past EOF must never surface as file content when the file
 * later grows (Linux zeroes the EOF page's tail for the same reason). */
void ext2_obj_note_write_locked(uint32_t ino, uint64_t off, uint64_t len,
                                uint64_t old_size);
void ext2_obj_note_truncate_locked(uint32_t ino, uint64_t old_size,
                                   uint64_t size);
/* Write back dirty pages of write-back objects (ino 0 = every inode).
 * Returns the number of pages written (> 0 means the caller must flush). */
int ext2_obj_writeback_locked(uint32_t ino);
/* Drop entries no mapping references any more.  Returns 1 when anything was
 * written or released (the caller flushes). */
int ext2_obj_reap_locked(void);

/* msync()/munmap() hook (compat rows): write back every write-back object.
 * Cheap when there is none.  Takes the ext2 lock itself. */
void ext2_obj_sync_all(void);

/* Diagnostics. */
int ext2_obj_count(void);

#endif /* EXT2_OBJ_H */
