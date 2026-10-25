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
