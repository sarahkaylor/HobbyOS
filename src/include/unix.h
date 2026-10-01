#ifndef UNIX_H
#define UNIX_H

#include <stdint.h>
#include "lock.h"
#include "ipc_proto.h"

/* P4 (docs/browser/p4-ipc-design.md section 2.1): AF_UNIX endpoint pairs.
 * One `struct usock` is a socketpair; each open fd is an end (0|1) with its
 * own ref count, mirroring the pipe model.  A unconnected endpoint (from
 * socket(AF_UNIX,...)) is a pair whose other end never existed
 * (refs[peer] == 0, connected == 0). */

#define K_UNIX_STREAM     1  /* byte ring per direction                */
#define K_UNIX_SEQPACKET  2  /* message queue per direction            */
#define K_UNIX_DGRAM      3  /* message queue per direction            */
#define K_UNIX_RING       65536 /* STREAM bytes per direction          */
#define K_UNIX_MSG_MAX    8192  /* max bytes per message                */
#define K_UNIX_MSGS       8     /* queued messages per direction        */
#define K_UNIX_MAX_FDS    K_IPC_MAX_FDS   /* fds per message            */
#define K_UNIX_MAX_IOV    K_IPC_MAX_IOV   /* iovecs per sendmsg/recvmsg */
#define MAX_UNIX_PAIRS    16    /* static pool: 16 pairs              */

/* One direction of a pair: sender -> receiver.  STREAM uses ring/head/tail/
 * used as a 65536-byte byte ring; SEQPACKET/DGRAM use them as a byte arena
 * (head = write offset, tail = read offset, used = queued bytes) with the
 * message descriptors in msg_len/msg_fds/msg_nfds indexed by mhead..mtail.
 * rwait/wwait are pid masks of parked readers/writers, exactly like
 * struct pipe's (wakes are drained only after the lock is dropped). */
struct usock_chan {
  uint8_t ring[K_UNIX_RING];
  uint16_t msg_len[K_UNIX_MSGS];
  int16_t msg_fds[K_UNIX_MSGS][K_UNIX_MAX_FDS]; /* global fd indices, -1 */
  uint8_t msg_nfds[K_UNIX_MSGS];
  uint32_t head, tail, used; /* byte ring / message arena cursors */
  uint32_t mhead, mtail;     /* SEQPACKET/DGRAM: message ring cursors */
  uint32_t mcount;           /* queued messages; full = K_UNIX_MSGS  */
  uint64_t rwait, wwait;     /* pid masks: parked readers / writers */
};

struct usock {
  spinlock_t lock;
  int in_use;
  int type;         /* K_UNIX_STREAM/SEQPACKET/DGRAM */
  int connected;    /* 0 for a socket() endpoint no peer ever attached */
  uint32_t refs[2]; /* open-fd refs per end (0 = side gone) */
  int nonblock[2];  /* per-end O_NONBLOCK (fcntl F_SETFL) */
  struct usock_chan chan[2]; /* chan[d]: d=0 -> A->B, d=1 -> B->A */
};

/* Static pool init (main.c, next to pipes_init). */
void unix_init(void);

/* Constructors: socketpair (both ends, refs 1/1, connected) and
 * socket(AF_UNIX) (one end, refs 1/0, unconnected).  NULL when the pool is
 * exhausted. */
struct usock *usock_alloc_pair(int type);
struct usock *usock_alloc_single(int type);

/* Per-fd ref counting (dup/fork/SCM_RIGHTS install / file close). */
void usock_reopen(struct usock *u, int end);
void usock_close(struct usock *u, int end);

/* Probe one end's readiness.  r/w/e carry the select channels; hup is the
 * split-off hang-up channel (select's exception bit = e|hup; poll maps
 * e -> POLLERR, hup -> POLLHUP). */
void usock_probe(struct usock *u, int end, int *r, int *w, int *e, int *hup);

/* Queued bytes: STREAM bytes / total queued message bytes; -1 when `end` is
 * a write end or `u` is not usable (matches the pipe convention). */
int usock_available(struct usock *u, int end);

void usock_set_nonblock(struct usock *u, int end, int on);
int usock_get_nonblock(struct usock *u, int end);
int usock_type(struct usock *u);

/* --- I/O engines (the pipe park model, per direction) ------------------- */

/* STREAM byte ring.  Return bytes read/written (>= 0), -errno, or the -2
 * "parked, restart the syscall" marker.  `nonblock` forces the one-shot
 * nonblocking path (MSG_DONTWAIT). */
int usock_send(struct usock *u, int end, const void *buf, int n, int nonblock);
int usock_recv(struct usock *u, int end, void *buf, int n, int nonblock);

/* SEQPACKET/DGRAM: one call = one message, atomic, whole-or-nothing.
 * `io` is an array of kernel-validated iovecs (user base addresses); data is
 * copied byte-wise straight to/from user memory.  msg_fds are GLOBAL fd
 * indices owned by the message once queued.  recvmsg installs the received
 * fds into the receiver's group table via fs_msg_install_gfd(); fds that do
 * not fit the caller's control buffer (maxfds) are closed per §4.2 step 3.
 * `flags`-derived bits: MSG_CMSG_CLOEXEC marks installed fds. */
int usock_send_msg(struct usock *u, int end, const struct k_iovec *io,
                   int niov, int total, const int *gfds, int nfds,
                   int nonblock);
int usock_recv_msg(struct usock *u, int end, const struct k_iovec *io,
                   int niov, int bufcap, int *got, int *trunc,
                   int *gfds_out, int maxfds, int *nfds_out, int *ctrl_trunc,
                   int nonblock, int cmsg_cloexec);

#endif /* UNIX_H */
