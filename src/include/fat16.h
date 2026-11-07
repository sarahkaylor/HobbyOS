#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>

/**
 * Standard 32-byte FAT16 directory entry structure.
 */
struct fat16_dir_entry {
  char name[11];              /**< 8.3 filename format */
  uint8_t attr;               /**< File attributes */
  uint8_t reserved[10];       /**< Reserved for future use */
  uint16_t time;              /**< Creation/Modification time */
  uint16_t date;              /**< Creation/Modification date */
  uint16_t start_cluster;     /**< First cluster of the file's data */
  uint32_t file_size;         /**< File size in bytes */
} __attribute__((packed));

/* selfhost w10-fstime: decode a dirent's FAT date/time fields into Unix
 * epoch seconds (UTC).  Mirror of the encode helper inside fat16.c; the
 * stat path reports real mtime/ctime/atime from it and the unit tests
 * verify the wire format. */
uint64_t fat16_entry_epoch(const struct fat16_dir_entry *e);

struct file; // Forward declaration

// Initializes the FAT16 filesystem by reading the boot sector.
// Returns 0 on success, or -1 on failure.
int fat16_init(void);

// Opens a file from the filesystem into a generic file structure.
int fat16_open(const char* filename, struct file* f);

/* GB-020: open a DIRECTORY (for getdents64).  Returns 0 and fills f (typed
 * FILE_TYPE_FAT16DIR by the caller) on success, -1 if not a directory. */
int fat16_open_dir(const char* path, struct file* f);

/* SEC-1: excl != 0 gives atomic O_CREAT|O_EXCL semantics (create-if-absent
   decided inside one fat_lock critical section). Returns 0 on success, -2 if
   the name exists and excl was set (caller maps to EEXIST), -1 otherwise. */
int fat16_open_ex(const char* filename, struct file* f, int excl);

// Reads bytes from a FAT16 file.
int fat16_read(struct file* f, void* buf, int size);
int fat16_read_direct(struct file* f, uint64_t dest, int size);
/* L9 (pagein): positional coalesced read for demand-paged v2 images. */
int fat16_read_direct_pos(uint16_t start_cluster, uint32_t file_size,
                          uint64_t dest, int size, uint32_t pos);

/* M6/L69: why did an executable page read back as zeros?
 *
 * A page of zeros reached an exec mapping while the NVMe controller was
 * enabled and ready and the ring was consistent, so "the device answered
 * zeros" and "the block cache published a zero frame" look identical from the
 * exec gate — its three reads all go through the cache.  This probe separates
 * them with one row per refused page:
 *
 *   off=     the file offset the gate validated
 *   lba=     the first sector the cluster chain maps that page to
 *   c_nz=    non-zero bytes in the CACHED frame (0 with found=1 == poisoned)
 *   r_nz=    non-zero bytes from a cache-BYPASSING device read of the page
 *   f_nz=    non-zero bytes after dropping the cache and re-reading normally
 *
 * `c_nz=0 r_nz>0` proves the cache held a page the file does not contain
 * (poisoned frame); `c_nz=0 r_nz=0` says the device itself answered zeros
 * (a chain/LBA problem or a lying device — check the image on the host).
 * Read-only apart from the invalidation the last field performs.
 */
int fat16_page_probe(uint16_t start_cluster, uint32_t file_size, uint32_t pos);

/* M6/L69: drop the block cache's copies of one page's sectors, so the next
 * read re-fetches from the device instead of serving a frame that was filled
 * by a transfer we do not trust.  Returns the number of sectors dropped. */
int fat16_page_drop_cache(uint16_t start_cluster, uint32_t file_size,
                          uint32_t pos, uint32_t len);

/* M6/L69: the authoritative read — same positional semantics as
 * fat16_read_direct_pos but it bypasses the block cache and asks the device
 * directly.  A page the cache serves as zeros may still be in the file; this
 * is the read that decides.  Returns the byte count or -1. */
int fat16_read_direct_pos_raw(uint16_t start_cluster, uint32_t file_size,
                              uint64_t dest, int size, uint32_t pos);
#ifdef KERNEL_MODE_UNIT_TEST
/* M6/L63 fault injection: mode 1 = short count, mode 2 = `size` bytes of
 * zeros (a full-length read the device never really delivered); applies to
 * reads starting at `off`, `times` times.  Unit tier only. */
void fat16_test_inject(int mode, uint32_t off, int times);
/* M6/L69: the first LBA a file offset maps to (unit test: lets a test poison
 * the exact cache unit a page-in will read).  Returns 0 or -1. */
int fat16_test_lba(uint16_t start_cluster, uint32_t pos, uint32_t *lba);
#endif

// Writes bytes to a FAT16 file.
int fat16_write(struct file* f, const void* buf, int size);

// Closes a FAT16 file (syncs to disk).
int fat16_close(struct file* f);
int fat16_truncate(struct file* f);

// P6.1: resize to an arbitrary byte length (zero-fill extend, free trailing
// clusters on shrink).
int fat16_truncate_to(struct file* f, uint32_t new_size);

// Seeks a FAT16 file.
int fat16_seek(struct file* f, int offset);

// Deletes a file.
int fat16_unlink(const char* filename);

// Renames a file.
int fat16_rename(const char* oldname, const char* newname);

// Creates a directory.
int fat16_mkdir(const char* path);

/* W2-14 (selfhost): removes an EMPTY directory with POSIX-errno-faithful
 * semantics: 0 on success, else a precise negative errno (-ENOENT,
 * -ENOTDIR, -ENOTEMPTY, -EBUSY for root/./.., -EIO).  unlink stays
 * dir-refusing; rmdir is the only directory-removal primitive. */
int fat16_rmdir(const char *path);

// Changes directory: validates `path` (absolute or cwd-relative) and writes
// the canonical absolute path of the resulting directory into out_new_cwd.
// Returns 0 on success, -1 when the path does not name a directory.
int fat16_chdir(const char *path, char *out_new_cwd);

// Reads a directory entry at the specified index.
int fat16_read_dir(const char* path, int index, char* out_name, uint8_t* out_attr, uint32_t* out_size);

/* GB-020: getdents64 backend for an open DIRECTORY fd.  Enumerates the
 * directory (from f->fat16.entry.start_cluster) into linux_dirent64 records,
 * starting at resume index start_index (= f->fat16.cursor on the first call),
 * and stores the next resume index back into f->fat16.cursor.  Returns the
 * number of bytes written to ubuf (0 at end of dir), -1 on read error. */
int fat16_getdents64(struct file *f, int start_index, void *ubuf, int bufsize);

// Computes volume statistics for the data area.
// total_bytes = total data cluster bytes; free_bytes = free cluster bytes.
// Returns 0 on success, -1 on error.
int fat16_stats(uint64_t* out_total, uint64_t* out_free);

// Resolves a path to its directory entry, sector, and offset.
int fat16_resolve_path(const char* path, struct fat16_dir_entry* out_entry, uint32_t* out_sector, uint32_t* out_offset);

// Resolves the parent directory and last component name.
int fat16_resolve_parent(const char* path, struct fat16_dir_entry* out_parent_entry, char* out_last_component);

#endif // FAT16_H
