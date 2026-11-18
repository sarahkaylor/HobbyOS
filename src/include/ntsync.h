#ifndef HOBBYOS_NTSYNC_H
#define HOBBYOS_NTSYNC_H
/*
 * C2-ntsync: /dev/ntsync, the Linux NT-synchronisation driver
 * (drivers/misc/ntsync.c, include/uapi/linux/ntsync.h, Linux 6.14+), for the
 * Linux-ABI compat class.  docs/cp-ntsync.md has the design.
 *
 * The UAPI below is Linux's, byte for byte: Wine 11.0 is compiled against
 * the real header (strix's /usr/include/linux/ntsync.h, vendored into the
 * Wine build) and the probe checks every number against it on a Linux box.
 * The kernel cannot include <linux/...>, so the layouts are restated here and
 * pinned with static asserts.
 */
#include <stdint.h>

struct ntsync_sem_args_k {
  uint32_t count;
  uint32_t max;
};
struct ntsync_mutex_args_k {
  uint32_t owner;
  uint32_t count;
};
struct ntsync_event_args_k {
  uint32_t manual;
  uint32_t signaled;
};
#define NTSYNC_WAIT_REALTIME_K 0x1u
struct ntsync_wait_args_k {
  uint64_t timeout; /* absolute ns, CLOCK_MONOTONIC or (flag) REALTIME; ~0 = none */
  uint64_t objs;    /* user pointer to `count` int fds */
  uint32_t count;
  uint32_t index;   /* out: the satisfied index (count = the alert) */
  uint32_t flags;
  uint32_t owner;   /* the waiter's mutex owner id (Wine: its thread id) */
  uint32_t alert;   /* fd of an event that ends the wait with index=count; 0 = none */
  uint32_t pad;
};
_Static_assert(sizeof(struct ntsync_sem_args_k) == 8, "ntsync_sem_args");
_Static_assert(sizeof(struct ntsync_mutex_args_k) == 8, "ntsync_mutex_args");
_Static_assert(sizeof(struct ntsync_event_args_k) == 8, "ntsync_event_args");
_Static_assert(sizeof(struct ntsync_wait_args_k) == 40, "ntsync_wait_args");

#define NTSYNC_MAX_WAIT_COUNT_K 64

/* _IOC(dir, 'N', nr, size): dir W=1 R=2 WR=3 (asm-generic/ioctl.h). */
#define NTSYNC_IOC_K(dir, nr, size)                                          \
  ((uint32_t)(((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) |          \
              ((uint32_t)'N' << 8) | (uint32_t)(nr)))
#define NTSYNC_IOC_CREATE_SEM_K NTSYNC_IOC_K(1, 0x80, 8)
#define NTSYNC_IOC_SEM_RELEASE_K NTSYNC_IOC_K(3, 0x81, 4)
#define NTSYNC_IOC_WAIT_ANY_K NTSYNC_IOC_K(3, 0x82, 40)
#define NTSYNC_IOC_WAIT_ALL_K NTSYNC_IOC_K(3, 0x83, 40)
#define NTSYNC_IOC_CREATE_MUTEX_K NTSYNC_IOC_K(1, 0x84, 8)
#define NTSYNC_IOC_MUTEX_UNLOCK_K NTSYNC_IOC_K(3, 0x85, 8)
#define NTSYNC_IOC_MUTEX_KILL_K NTSYNC_IOC_K(1, 0x86, 4)
#define NTSYNC_IOC_CREATE_EVENT_K NTSYNC_IOC_K(1, 0x87, 8)
#define NTSYNC_IOC_EVENT_SET_K NTSYNC_IOC_K(2, 0x88, 4)
#define NTSYNC_IOC_EVENT_RESET_K NTSYNC_IOC_K(2, 0x89, 4)
#define NTSYNC_IOC_EVENT_PULSE_K NTSYNC_IOC_K(2, 0x8a, 4)
#define NTSYNC_IOC_SEM_READ_K NTSYNC_IOC_K(2, 0x8b, 8)
#define NTSYNC_IOC_MUTEX_READ_K NTSYNC_IOC_K(2, 0x8c, 8)
#define NTSYNC_IOC_EVENT_READ_K NTSYNC_IOC_K(2, 0x8d, 8)
_Static_assert(NTSYNC_IOC_CREATE_SEM_K == 0x40084e80u, "CREATE_SEM");
_Static_assert(NTSYNC_IOC_WAIT_ANY_K == 0xc0284e82u, "WAIT_ANY");
_Static_assert(NTSYNC_IOC_EVENT_READ_K == 0x80084e8du, "EVENT_READ");

/* ---- hooks (fs.c, linux_compat_fd.c, linux_compat_thr.c) --------------- */
struct process;
struct trap_frame;
struct file;

/* open("/dev/ntsync"): a new device instance for the file (0 or -errno). */
int ntsync_dev_open(struct file *f);
/* last close of a FILE_TYPE_NTSYNC slot (device or object). */
void ntsync_file_release(struct file *f);
/* ioctl(2) on a FILE_TYPE_NTSYNC fd: writes tf->regs[0] or parks (the
 * syscall re-executes, docs/cp-ntsync.md §2.3); returns 1. */
int ntsync_ioctl(struct process *p, struct trap_frame *tf, int fd,
                 uint32_t req, uint64_t arg);
/* Is thread `t` parked inside an ntsync wait?  linux_compat_thr.c: such a
 * park is ended by a handled signal with -EINTR even under SA_RESTART (the
 * wait's queue entries must be torn down before a handler can run; Wine
 * retries on EINTR, exactly what it does for Linux's -ERESTARTSYS). */
int ntsync_thread_waiting(const struct process *t);

/* fs.c helpers for the driver. */
int file_is_ntsync(struct process *p, int fd);
/* the object a FILE_TYPE_NTSYNC fd names, or 0.  Lock-free table read: the
 * driver calls it under its own lock, which every release also takes. */
void *file_ntsync_obj(struct process *p, int fd);
/* install a new FILE_TYPE_NTSYNC fd (O_RDWR | O_CLOEXEC) for `obj`:
 * the fd, or -EMFILE / -ENFILE (the caller still owns `obj`). */
int file_ntsync_install(struct process *p, void *obj);

#endif
