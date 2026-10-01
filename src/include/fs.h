#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fat16.h"
#include "lock.h"
#include "process.h"
#include "nfs.h"
#include "ipc_proto.h"

struct socket_pcb;
struct usock;

/**
 * Types of files supported by the VFS layer.
 */
typedef enum {
  FILE_TYPE_EMPTY,    /**< Unallocated file slot */
  FILE_TYPE_FAT16,    /**< Regular file on FAT16 filesystem */
  FILE_TYPE_PIPE,     /**< Anonymous pipe for IPC */
  FILE_TYPE_SOCKET,   /**< Network socket */
  FILE_TYPE_NFS,      /**< Regular file on a mounted NFS export (read-only) */
  FILE_TYPE_UNIXSOCK, /**< P4: AF_UNIX socketpair end (struct usock) */
  FILE_TYPE_MEMFD     /**< P2.4 (S4): memory file backed by a vm_object */
} file_type_t;

struct vm_object;

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
      int dirty;                   /**< Entry changed since open; close must sync */
    } fat16;
    struct {
      struct pipe *ptr;            /**< Pointer to the pipe structure */
      int end;                     /**< 0 for read end, 1 for write end */
    } pipe;
    struct {
      struct socket_pcb *pcb;      /**< Pointer to the protocol control block */
    } socket;
    struct {
      struct usock *ptr;           /**< P4: AF_UNIX pair object */
      int end;                     /**< 0|1: which half this fd is */
    } usock;
    struct {
      struct nfs_fh fh;            /**< NFSv3 file handle */
      uint64_t size;               /**< Size from the open attributes */
      uint32_t cursor;             /**< Current read position */
      int is_dir;                  /**< Opened entry is a directory */
      int mount_idx;               /**< Index of the mount serving it */
    } nfs;
    struct {
      struct vm_object *obj;       /**< P2.4 (S4): memfd backing object;
                                        holds one object ref per fd instance */
      int mfd_flags;               /**< MFD_* passed at create (recorded) */
    } memfd;
  };
};

#define MAX_GLOBAL_FILES 128

void fs_init(void);
struct trap_frame;
int file_open(struct process *p, const char *filename, int flags);
int file_close(struct process *p, int fd);
int file_dup(struct process *p, int fd);
int file_dup2(struct process *p, int oldfd, int newfd);
int file_gfd_is_pipe(int gfd);
int file_read(struct process *p, int fd, void *buf, int size, struct trap_frame *tf);
int file_write(struct process *p, int fd, const void *buf, int size, struct trap_frame *tf);
int file_pipe(struct process *p, int fds[2]);
int file_available(struct process *p, int fd);
/* P2.4 (S4, design section 4.3): memfd objects.  file_memfd_create
 * installs a FILE_TYPE_MEMFD fd (fd or -errno); file_memfd_obj resolves
 * an fd to its backing object (NULL when not a memfd); file_ftruncate
 * (below) sets a memfd's object size (SHRINK/GROW seals enforced). */
int file_memfd_create(struct process *p, const char *name, int flags);
struct vm_object *file_memfd_obj(struct process *p, int fd);
int file_memfd_seals(struct process *p, int fd, int cmd, int arg);
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
 * directly into user memory (validated at the trap layer).  st_ino is
 * synthesized per backend: FAT16 directory-entry location, NFS file-handle
 * fold, 0 for pipes/sockets. */
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

/* --- Phase F1 (browser.md A.1a — frozen): socket/select syscall support -- */

/* select() mask width is frozen at FD_SETSIZE 256 (the A.1a amendment):
 * eight 32-bit words per set, same layout as the userland fd_set. */
#define K_FD_SETSIZE 256
#define K_FD_SET_WORDS (K_FD_SETSIZE / 32)

struct fd_set_k {
  uint32_t bits[K_FD_SET_WORDS];
};

/* Values mirroring the userland ABI (libc.h): AF_INET, SOCK_STREAM/DGRAM,
 * IPPROTO_TCP/UDP, SOL_SOCKET, SO_ERROR/TYPE/REUSEADDR, fcntl cmds/flags. */
#define K_AF_INET      2
#define K_SOCK_STREAM  1
#define K_SOCK_DGRAM   2
#define K_IPPROTO_TCP  6
#define K_IPPROTO_UDP  17
#define K_SOL_SOCKET   1
#define K_SO_REUSEADDR 2
#define K_SO_TYPE      3
#define K_SO_ERROR     4
#define K_F_GETFD      1
#define K_F_SETFD      2
#define K_F_GETFL      3
#define K_F_SETFL      4
#define K_FD_CLOEXEC   1
#define K_O_NONBLOCK   0x800
/* P2.4 (S4, design section 4.3): memfd sealing commands + seal bits
 * (Linux numbers; SHRINK/GROW enforced at ftruncate, WRITE advisory). */
#define K_F_ADD_SEALS  1033
#define K_F_GET_SEALS  1034
#define K_F_SEAL_SEAL  0x0001
#define K_F_SEAL_SHRINK 0x0002
#define K_F_SEAL_GROW  0x0004
#define K_F_SEAL_WRITE 0x0008
/* memfd_create(2) flags (recorded; MFD_CLOEXEC also arms fd_cloexec). */
#define K_MFD_CLOEXEC      0x0001
#define K_MFD_ALLOW_SEALING 0x0002

/* P6.1 (browser.md section 6): advisory record locks (SQLite).  The
 * commands ride the EXISTING SYS_FCNTL number (68) -- no new syscall
 * rows -- and the numbers match the sysroot's fcntl.h (Linux numbering).
 * The argument is a user pointer to the LP64 struct flock (32 bytes,
 * 8-byte natural alignment), which the kernel parses byte-wise; the
 * commands/lock types mirror <fcntl.h>. */
#define K_F_GETLK      5
#define K_F_SETLK      6
#define K_F_SETLKW     7
#define K_F_RDLCK      0
#define K_F_WRLCK      1
#define K_F_UNLCK      2

/* Size of the user-ABI LP64 struct flock the lock commands marshal in the
 * fcntl argument (2 + 2 + 8 + 8 + 4 bytes + 4 pad). */
#define K_FLOCK_SIZE   32

/* P4 (docs/browser/p4-ipc-design.md sections 2-6): AF_UNIX + fd flags. */
#define K_AF_UNIX       1
#define K_SOCK_RAW      3
#define K_SOCK_SEQPACKET 5
#define K_SOCK_CLOEXEC  0x80000
#define K_SO_DOMAIN     39

/* SYS_SOCKET: create a socket and install it in the process fd table.
 * Returns the user fd (>= 0) or -errno (EAFNOSUPPORT, EPROTONOSUPPORT,
 * EMFILE, ENFILE). */
int file_socket(struct process *p, int domain, int type, int protocol);

/* SYS_CONNECT_FD on an existing socket fd (ip/port in network byte order).
 * Returns 0 (connected — or handshake started for UDP), -EINPROGRESS when a
 * non-blocking TCP handshake is under way, or -errno (EBADF, ENOTSOCK,
 * ECONNREFUSED, ETIMEDOUT, ...). */
int file_socket_connect(struct process *p, int fd, uint32_t ip_be,
                        uint16_t port_be);

/* SYS_FCNTL: F_GETFL / F_SETFL over a socket fd (O_NONBLOCK stored on the
 * PCB); any other command, or a non-socket fd, is -EINVAL.  P6.1 adds the
 * record-lock commands (K_F_GETLK/K_F_SETLK/K_F_SETLKW) over regular FAT16
 * files; `arg` carries either an int flag word or a user pointer to the
 * LP64 struct flock, so it is passed with its full 64-bit width. */
int file_fcntl(struct process *p, int fd, int cmd, uint64_t arg);

/* P6.1: release every record lock owned by process `pid` on the file
 * identified by `file_id` (dir-entry identity).  Callers hold proc_lock
 * or are single-threaded (close/exit paths). */
void file_locks_release(int pid, uint64_t file_id);

/* P6.1: drop every record lock owned by `pid` (process teardown sweep). */
void file_locks_release_pid(int pid);

/* P6.1: SYS_FTRUNCATE (row 33) -- resize an open regular file.  Extends
 * with zero fill, shrinks by freeing whole trailing clusters.  Returns 0
 * or -errno (EBADF/ESPIPE/EINVAL/ENOSPC). */
int file_ftruncate(struct process *p, int fd, int64_t length);

/* SYS_SELECT engine (F1.2).  Returns the number of ready descriptors with
 * the masks rewritten to hold only the ready fds; 0 on timeout; -2 when the
 * caller must restart the syscall after the process was parked (the caller
 * rewinds its ELR and schedules, like pipe_read's -2); -errno on error
 * (EBADF for an fd outside the table, EINVAL for nfds out of range).
 *
 * Wake-up strategy: no busy-spin.  A select with nothing ready parks the
 * process through the scheduler exactly like sys_sleep (PROC_STATE_BLOCKED
 * + wake_ms) for a short 10ms slice and the syscall restarts on wake; the
 * deadline is remembered per pid so the caller's timeout is exact.  Each
 * restart also polls the sockets, which is what drives non-blocking connect
 * SYN retransmission while a program waits in select(). */
int file_select(struct process *p, int nfds, struct fd_set_k *rd,
                struct fd_set_k *wr, struct fd_set_k *ex, int timeout_ms);

/* P1: drop a pid's remembered select deadline (group teardown / thread
 * death: a parked select must not resurrect a dead thread's slice). */
void file_select_forget(int pid);

/* Socket PCB behind an fd, or NULL when the fd is not an open socket.
 * Kernel-internal (slice barriers/tests): the returned pointer must not
 * outlive the fd. */
struct socket_pcb *file_socket_pcb(struct process *p, int fd);

/* SYS_GETSOCKOPT / SYS_SETSOCKOPT (SOL_SOCKET only).  SO_ERROR and SO_TYPE
 * are real; SO_REUSEADDR is accepted as a no-op.  Unknown option or level:
 * -ENOPROTOOPT. */
int file_socket_getopt(struct process *p, int fd, int level, int optname,
                       void *val, int *len);
int file_socket_setopt(struct process *p, int fd, int level, int optname,
                       const void *val, int len);

/* Reposition the read cursor of an open file (FAT16/NFS).  Pipes report
 * ESPIPE via *errp.  Returns the new absolute position, or -1 on error. */
int64_t file_seek(struct process *p, int fd, int64_t offset, int whence,
                  int *errp);
/* Fill *st for an open fd or a path.  Return 0 on success, -1 with *errp
 * set (EBADF/ENOENT/EINVAL). */
int file_stat_fd(struct process *p, int fd, struct k_stat *st, int *errp);
int file_stat_path(struct process *p, const char *path, struct k_stat *st,
                   int *errp);

/* --- P4 (docs/browser/p4-ipc-design.md; freeze consented 2026-09-30) ----- */

/* SYS_SOCKETPAIR (77): AF_UNIX pair.  Returns 0 with ufds[0]/ufds[1] filled
 * (user pointers already range-checked by the trap layer) or -errno
 * (EAFNOSUPPORT/EPROTONOSUPPORT/EINVAL/EMFILE/EFAULT). */
int file_socketpair(struct process *p, int domain, int type, int proto,
                    int *ufds);

/* SYS_SENDMSG (78) / SYS_RECVMSG (79): `umsg` points at a user msghdr whose
 * storage the trap layer has range-checked for sizeof(struct k_msghdr);
 * the iovec array and control block are validated and copied to kernel
 * scratch here.  Returns bytes / -errno (-EOPNOTSUPP for INET sockets,
 * -ENOTSOCK for pipes/files, -ENOTCONN for an unconnected unix endpoint). */
int file_sendmsg(struct process *p, int fd, uint64_t umsg, int flags);
int file_recvmsg(struct process *p, int fd, uint64_t umsg, int flags);

/* SYS_POLL (76) engine: probes every entry, rewrites revents, parks
 * (returns -2) through the shared select deadline table.  `fds` is user
 * memory (range-checked, aligned) accessed directly.  Returns the number of
 * entries with nonzero revents, 0 on timeout, -errno (EINVAL/EFAULT). */
int file_poll(struct process *p, struct k_pollfd *fds, int nfds,
              int timeout_ms);

/* 1 when the global slot holds an IPC endpoint end (pipe or unix socket
 * end): the spawn2 default-stderr exclusion class. */
int file_gfd_is_ipc_endpoint(int gfd);

/* SCM_RIGHTS fd machine (§4.2): take a message-owned reference to an open
 * global fd (validate + ref++ + per-fd type reopen under that file's lock);
 * install a message-owned reference into the receiver group's table as the
 * lowest free user fd (no ref bump: the reference transfers). */
int fs_msg_ref_gfd(int gfd);
int fs_msg_install_gfd(struct process *p, int gfd);

#endif // FS_H
