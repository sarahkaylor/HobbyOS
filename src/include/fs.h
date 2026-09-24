#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fat16.h"
#include "lock.h"
#include "process.h"
#include "nfs.h"

struct socket_pcb;

/**
 * Types of files supported by the VFS layer.
 */
typedef enum {
  FILE_TYPE_EMPTY,    /**< Unallocated file slot */
  FILE_TYPE_FAT16,    /**< Regular file on FAT16 filesystem */
  FILE_TYPE_PIPE,     /**< Anonymous pipe for IPC */
  FILE_TYPE_SOCKET,   /**< Network socket */
  FILE_TYPE_NFS       /**< Regular file on a mounted NFS export (read-only) */
} file_type_t;

/**
 * Represents an open file instance in the global file table.
 * Contains type-specific data and synchronization primitives.
 */
struct file {
  file_type_t type;   /**< Type of the file */
  int ref_count;      /**< Number of processes currently using this file */
  spinlock_t lock;    /**< Lock for atomic access to file state */
  union {
    struct {
      struct fat16_dir_entry entry; /**< FAT16 directory entry copy */
      uint32_t dir_sector;         /**< Sector on disk containing the entry */
      uint32_t dir_offset;         /**< Offset within the sector */
      uint32_t cursor;             /**< Current read/write position */
    } fat16;
    struct {
      struct pipe *ptr;            /**< Pointer to the pipe structure */
      int end;                     /**< 0 for read end, 1 for write end */
    } pipe;
    struct {
      struct socket_pcb *pcb;      /**< Pointer to the protocol control block */
    } socket;
    struct {
      struct nfs_fh fh;            /**< NFSv3 file handle */
      uint64_t size;               /**< Size from the open attributes */
      uint32_t cursor;             /**< Current read position */
      int is_dir;                  /**< Opened entry is a directory */
      int mount_idx;               /**< Index of the mount serving it */
    } nfs;
  };
};

#define MAX_GLOBAL_FILES 128

void fs_init(void);
struct trap_frame;
int file_open(struct process *p, const char *filename);
int file_close(struct process *p, int fd);
int file_read(struct process *p, int fd, void *buf, int size, struct trap_frame *tf);
int file_write(struct process *p, int fd, const void *buf, int size, struct trap_frame *tf);
int file_pipe(struct process *p, int fds[2]);
int file_available(struct process *p, int fd);
int file_connect(struct process *p, uint32_t ip, uint16_t port, int protocol);
int file_mkdir(struct process *p, const char *path);

// Helpers for process management
/**
 * Increments the reference count of a global file.
 */
void fs_reopen(int global_fd);
/**
 * Duplicates a file descriptor (not currently used in the main logic).
 */
int fs_duplicate_fd(int global_fd);

/**
 * Reads a directory entry by index from the FAT16 root directory.
 *
 * Parameters:
 *   index    - The 0-based index of the file in the directory.
 *   out_name - Buffer to store the resulting filename (at least 12 bytes).
 *
 * Returns:
 *   0 on success, -1 if no more entries exist.
 */
void fs_close_global(int g_fd);

/* --- Phase 3: lseek/stat support ------------------------------------- */

/* ABI mirror of the sysroot's struct stat (LP64: sys/types.h typedefs a
 * dev_t/ino_t/off_t as long and mode_t/uid_t/gid_t as unsigned int).
 * Member-for-member identical layout; the kernel fills one of these
 * directly into user memory (validated at the trap layer). */
struct k_stat {
  unsigned long st_dev;
  unsigned long st_ino;
  unsigned int  st_mode;
  long          st_nlink;
  unsigned int  st_uid;
  unsigned int  st_gid;
  unsigned long st_rdev;
  long          st_size;
  long          st_blksize;
  long          st_blocks;
  long          st_atime, st_mtime, st_ctime;
};

#define K_S_IFMT  0170000
#define K_S_IFDIR 0040000
#define K_S_IFREG 0100000
#define K_S_IFIFO 0010000
#define K_S_IFSOCK 0140000

/* Reposition the read cursor of an open file (FAT16/NFS).  Pipes report
 * ESPIPE via *errp.  Returns the new absolute position, or -1 on error. */
int64_t file_seek(struct process *p, int fd, int64_t offset, int whence,
                  int *errp);
/* Fill *st for an open fd or a path.  Return 0 on success, -1 with *errp
 * set (EBADF/ENOENT/EINVAL). */
int file_stat_fd(struct process *p, int fd, struct k_stat *st, int *errp);
int file_stat_path(struct process *p, const char *path, struct k_stat *st,
                   int *errp);

#endif // FS_H
