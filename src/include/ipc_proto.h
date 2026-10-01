#ifndef IPC_PROTO_H
#define IPC_PROTO_H

#include <stdint.h>

/* P4 pure codec (docs/browser/p4-ipc-design.md section 4.1): kernel mirrors
 * of the sysroot's msghdr/iovec/cmsghdr, driven over byte arrays so the EL1
 * unit suites and the host suite exercise exactly the same code
 * (nfs_proto.c precedent).  No process, lock or user-memory knowledge
 * lives in this TU. */

/* LP64 Linux-compatible mirrors; layout member-for-member identical to the
 * sysroot's <sys/socket.h>/<sys/uio.h> definitions. */
struct k_iovec {
  uint64_t base;
  uint64_t len;
};

struct k_msghdr {
  uint64_t name;      /* msg_name (unused: connected sockets only) */
  uint32_t namelen;   /* msg_namelen */
  uint32_t pad0;
  uint64_t iov;       /* struct iovec * */
  uint64_t iovlen;    /* size_t */
  uint64_t control;   /* void * */
  uint64_t controllen;
  int32_t flags;      /* msg_flags (out for recvmsg) */
  uint32_t pad1;
};

struct k_pollfd {
  int fd;
  short events;
  short revents;
};

/* Caps the syscall layer enforces (P4 design section 2.1; frozen constants). */
#define K_IPC_MAX_FDS 16
#define K_IPC_MAX_IOV 8
#define K_IPC_CTRL_MAX 512 /* max control block copied to kernel scratch */

/* msg flags (Linux values; only these are accepted). */
#define K_MSG_PEEK        0x2
#define K_MSG_CTRUNC      0x8
#define K_MSG_TRUNC       0x20
#define K_MSG_DONTWAIT    0x40
#define K_MSG_NOSIGNAL    0x4000
#define K_MSG_CMSG_CLOEXEC 0x40000000

/* SOL_SOCKET / SCM_RIGHTS (Linux values). */
#define K_SOL_SOCKET 1
#define K_SCM_RIGHTS 1

/* poll bits (Linux values). */
#define K_POLLIN   0x001
#define K_POLLPRI  0x002
#define K_POLLOUT  0x004
#define K_POLLERR  0x008
#define K_POLLHUP  0x010
#define K_POLLNVAL 0x020

/* Parsed SCM_RIGHTS cmsg: the fd numbers are the caller's native ints, read
 * byte-wise out of the control block. */
struct ipc_cmsg_fds {
  int nfds;
  int fds[K_IPC_MAX_FDS];
};

/* --- pure helpers (host- and EL1-testable over byte arrays) ------------- */

/* cmsg geometry, mirroring the sysroot's CMSG_* macros. */
uint32_t ipc_cmsg_align(uint32_t len);
uint32_t ipc_cmsg_len(uint32_t nfds);   /* CMSG_LEN(4*nfds)  */
uint32_t ipc_cmsg_space(uint32_t nfds); /* CMSG_SPACE(4*nfds) */

/* Write one SCM_RIGHTS cmsg for `nfds` fds into `buf` (>= ipc_cmsg_space);
 * returns the number of bytes the cmsg occupies (ipc_cmsg_len).  `buf` is a
 * byte array so this stays endianness-explicit. */
uint32_t ipc_cmsg_emit(uint8_t *buf, const int *fds, int nfds);

/* Parse a control block.  Accepts one or more SCM_RIGHTS cmsgs (level
 * SOL_SOCKET, type SCM_RIGHTS) totalling 1..K_IPC_MAX_FDS fds; anything
 * malformed (short block, unknown level/type, non-multiple-of-4 payload,
 * misaligned lengths) is -EINVAL; more than the cap is -EMSGSIZE.
 * Returns 0 on success. */
int ipc_cmsg_parse(const uint8_t *ctrl, uint32_t ctrl_len,
                   struct ipc_cmsg_fds *out);

/* How many of `nfds` whole fds fit a control buffer of `controllen` bytes
 * (CMSG_SPACE accounting, capped at K_IPC_MAX_FDS). */
uint32_t ipc_cmsg_fit(uint32_t controllen, uint32_t nfds);

/* sendmsg/recvmsg flags validation, Linux-shaped: returns 0 or -errno.
 * `recv` selects the accepted set: send accepts MSG_NOSIGNAL|MSG_DONTWAIT;
 * recv accepts MSG_DONTWAIT|MSG_CMSG_CLOEXEC.  MSG_PEEK -> -EOPNOTSUPP;
 * any other bit -> -EINVAL. */
int ipc_msg_flags_ok(int flags, int recv);

/* Probe results -> poll revents for one entry: POLLIN/POLLOUT only when
 * requested (POLLIN|POLLOUT in `events`); POLLERR/POLLHUP are always
 * reported.  Returns the revents bits. */
short ipc_poll_map(int r, int w, int e, int hup, short events);

#endif /* IPC_PROTO_H */
