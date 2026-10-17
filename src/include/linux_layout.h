/*
 * src/include/linux_layout.h — Linux x86_64 ABI struct layouts for the compat
 * layer (GB-015).
 *
 * These are the layouts the ELF-compat layer believes a musl guest believes the
 * kernel has: musl passes these structs by pointer straight to the syscall, and
 * the compat shims must fill (or read) them at exactly these sizes/offsets.  A
 * wrong layout does not fail loudly — it corrupts a struct the guest then
 * trusts.  So every size and offset here is LOCKED by src/host/linux_layout_test.c
 * against tests/fixtures/abi/linux_layouts.golden, which is measured on a real
 * Linux x86_64 box by tools/linux-abi/gen_abi_golden.py (the "Rule 1 — numbers
 * are measured, never remembered" rule of docs/linux-abi.md).  Never edit a
 * number here from memory; change it, regenerate the golden, and the test either
 * agrees or fails.
 *
 * Naming: linux_*_t suffixed types are the compat view; they deliberately do
 * not collide with the native HobbyOS sysroot types (src/libc/include/...),
 * which have their own, different layouts.  Two-arch rule: x64-only, guarded by
 * __x86_64__ where it is included into a build that also compiles for arm.
 */
#ifndef HOBBYOS_LINUX_LAYOUT_H
#define HOBBYOS_LINUX_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* file timestamps use this on x86_64 (two 64-bit words) */
typedef struct {
  int64_t  tv_sec;
  int64_t  tv_nsec;
} linux_timespec_t;

typedef struct {
  int64_t  tv_sec;
  int64_t  tv_usec;
} linux_timeval_t;

/* struct stat, x86_64 asm-generic-derived layout (144 B) — NOT the
 * asm-generic ordering, per docs/linux-abi.md §6. */
typedef struct {
  uint64_t st_dev;
  uint64_t st_ino;
  uint64_t st_nlink;
  uint32_t st_mode;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t __pad0;             /* padding to align st_rdev at 40 */
  uint64_t st_rdev;
  int64_t  st_size;
  int64_t  st_blksize;
  int64_t  st_blocks;
  linux_timespec_t st_atim;
  linux_timespec_t st_mtim;
  linux_timespec_t st_ctim;
  int64_t  __glibc_reserved[3];
} linux_stat_t;

/* struct statx (256 B) — mask/size/mode pinned; the rest is demand-driven. */
typedef struct {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0;
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  linux_timespec_t stx_atime;
  linux_timespec_t stx_btime;
  linux_timespec_t stx_ctime;
  linux_timespec_t stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t stx_mnt_id;
  uint32_t stx_dio_mem_align;
  uint32_t stx_dio_offset_align;
  uint64_t __spare3[12];
} linux_statx_t;

/* sigset_t the compat layer sees: 128 bytes on x86_64 (glibc __sigset_t is 16
 * unsigned longs; musl mirrors it so the fields after it line up — sa_flags at
 * 136, sa_restorer at 144, sizeof 152).  The 8-byte sigset used by the
 * rt_sigprocmask SYSCALL is a separate, kernel-side quantity (the shim
 * marshals between them). */
typedef struct {
  uint64_t __bits[16];
} linux_sigset_t;

/* struct sigaction, x86_64 (152 B): handler union @0, sa_mask @8,
 * sa_flags @136, sa_restorer @144 (docs/linux-abi.md §6/§8). */
typedef union {
  void (*sa_handler)(int);
  void (*sa_sigaction)(int, void *, void *);
} linux_sigaction_handler_t;

typedef struct {
  linux_sigaction_handler_t sa_handler_u;   /* 0 */
  linux_sigset_t sa_mask;                   /* 8 */
  int      sa_flags;                        /* 136 */
  void     (*sa_restorer)(void);            /* 144 */
} linux_sigaction_t;                        /* 152 */

/* ucontext_t (968 B); mcontext_t gregs is a 23-entry greg_t[23] @0, fpregs ptr
 * @184.  Only the fields musl's rt_sigreturn needs are fully pinned. */
typedef struct {
  uint64_t gregs[23];
  void    *fpregs;
  uint64_t __reserved1[8];
} linux_mcontext_t;

typedef struct {
  void     *ss_sp;
  int       ss_flags;
  /* size_t */ uint64_t ss_size;
} linux_stack_t;

typedef struct {
  uint64_t       uc_flags;
  void          *uc_link;
  linux_stack_t  uc_stack;
  linux_mcontext_t uc_mcontext;
  linux_sigset_t uc_sigmask;
  /* glibc carries a full FP-state suffix (struct _fpstate + padding) so the
     struct is 968 B; musl mirrors the size.  Only the fields above are pinned
     by docs/linux-abi.md §6; this pad reproduces the measured total. */
  uint64_t __fpregs_mem[68];   /* 544 B -> total 968 */
} linux_ucontext_t;

/* struct termios, x86_64 (60 B): c_line @16 (1 byte), c_cc @17; glibc's c_cc is
 * 32 bytes (NCCS) which carries the offsets: c_ispeed @52, c_ospeed @56.
 * docs/linux-abi.md §6 pins c_cc @17, c_ispeed @52, c_ospeed @56. */
typedef struct {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t  c_line;
  uint8_t  c_cc[32];
  uint32_t c_ispeed;
  uint32_t c_ospeed;
} linux_termios_t;

/* struct winsize (8 B) */
typedef struct {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
} linux_winsize_t;

/* struct epoll_event — PACKED, 12 B: events @0, data @4. */
typedef struct {
  uint32_t events;
  /* epoll_data_t = union{void*ptr;int fd;uint32_t u32;uint64_t u64;} -> 8 B */
  uint64_t data;
} __attribute__((packed)) linux_epoll_event_t;

/* struct pollfd (8 B) */
typedef struct {
  int fd;
  short events;
  short revents;
} linux_pollfd_t;

/* struct iovec (16 B) */
typedef struct {
  void    *iov_base;
  uint64_t iov_len;
} linux_iovec_t;

/* struct dirent64 (280 B): d_ino @0, d_off @8, d_reclen @16, d_type @18, d_name @19 */
typedef struct {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
} linux_dirent64_t;

/* --- resource / process-identity structs the W0 shims fill (docs §6) ------ */

/* struct rlimit (16 B): rlim_cur @0, rlim_max @8 (both u64). */
typedef struct {
  uint64_t rlim_cur;
  uint64_t rlim_max;
} linux_rlimit_t;

/* struct rusage (144 B) — only the total size is pinned; glibc fills the
 * per-process resource counters. */
typedef struct {
  uint8_t _p[144];
} linux_rusage_t;

/* struct itimerval (32 B): it_interval (timeval) @0, it_value (timeval) @16. */
typedef struct {
  linux_timeval_t it_interval;
  linux_timeval_t it_value;
} linux_itimerval_t;

/* struct sysinfo (112 B) — only the total size is pinned here. */
typedef struct {
  uint8_t _p[112];
} linux_sysinfo_t;

/* struct utsname (390 B): six 65-byte NUL-padded char arrays. */
typedef struct {
  char sysname[65];     /* 0   */
  char nodename[65];    /* 65  */
  char release[65];     /* 130 */
  char version[65];     /* 195 */
  char machine[65];     /* 260 */
  char domainname[65];  /* 325 */
} linux_utsname_t;      /* 390 */

/* struct flock (32 B): l_type @0, l_whence @2, l_start @8, l_len @16, l_pid @24. */
typedef struct {
  short   l_type;    /* 0  */
  short   l_whence;  /* 2  */
  int64_t l_start;   /* 8  */
  int64_t l_len;     /* 16 */
  int     l_pid;     /* 24 */
} linux_flock_t;     /* 32 */

/* struct timezone (8 B): two ints (tz_minuteswest, tz_dsttime), pinned by
 * gettimeofday's tz argument. */
typedef struct {
  int tz_minuteswest;  /* 0 */
  int tz_dsttime;      /* 4 */
} linux_timezone_t;    /* 8 */

/* struct dirent, x86_64 -- same on-disk shape as dirent64 (280 B: d_ino @0,
 * d_off @8, d_reclen @16, d_type @18, d_name @19); kept as a separate compat
 * struct because musl/glibc may reference either name for getdents/getdents64. */
typedef struct {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
} linux_dirent_t;    /* 280 */

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_LINUX_LAYOUT_H */
