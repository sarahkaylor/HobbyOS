#include "ipc_proto.h"
#include "errno.h"

/* P4 pure codec (docs/browser/p4-ipc-design.md section 4.1): parse/validate/
 * emit for the msghdr/iovec/cmsg byte blocks and the poll revents mapping.
 * All multi-byte fields are assembled byte-wise (little-endian, the LP64
 * ABI both arches use) so this TU never emits an unaligned wide load and
 * stays drivable from the host suite. */

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr64(uint8_t *p, uint64_t v) {
  wr32(p, (uint32_t)v);
  wr32(p + 4, (uint32_t)(v >> 32));
}

uint32_t ipc_cmsg_align(uint32_t len) { return (len + 7u) & ~7u; }

uint32_t ipc_cmsg_len(uint32_t nfds) { return 16u + 4u * nfds; }

uint32_t ipc_cmsg_space(uint32_t nfds) { return ipc_cmsg_align(ipc_cmsg_len(nfds)); }

uint32_t ipc_cmsg_emit(uint8_t *buf, const int *fds, int nfds) {
  uint32_t clen = ipc_cmsg_len((uint32_t)nfds);
  wr64(buf + 0, (uint64_t)clen); /* size_t cmsg_len: 8 bytes on LP64 */
  wr32(buf + 8, (uint32_t)K_SOL_SOCKET);
  wr32(buf + 12, (uint32_t)K_SCM_RIGHTS);
  for (int i = 0; i < nfds; i++)
    wr32(buf + 16 + i * 4, (uint32_t)fds[i]);
  return clen;
}

int ipc_cmsg_parse(const uint8_t *ctrl, uint32_t ctrl_len,
                   struct ipc_cmsg_fds *out) {
  out->nfds = 0;
  if (!ctrl || ctrl_len == 0) return -EINVAL;

  uint32_t off = 0;
  for (;;) {
    if (ctrl_len - off < 16) break; /* no full cmsghdr left */
    uint64_t clen = rd64(ctrl + off + 0);
    int32_t level = (int32_t)rd32(ctrl + off + 8);
    int32_t type = (int32_t)rd32(ctrl + off + 12);
    /* Header layout (LP64, what the sysroot's <sys/socket.h> hands over):
     * cmsg_len @0 (size_t, 8 bytes), cmsg_level @8, cmsg_type @12; the
     * payload starts at 16 (sizeof(struct cmsghdr) == 16). */
    if (clen < 16 || clen > (uint64_t)(ctrl_len - off)) return -EINVAL;
    if ((clen & 3u) != 0 || ((clen - 16u) & 3u) != 0) return -EINVAL;
    if (level != K_SOL_SOCKET) return -EINVAL;
    if (type != K_SCM_RIGHTS) return -EINVAL;

    uint32_t n = (uint32_t)((clen - 16u) / 4u);
    if (n == 0) return -EINVAL; /* SCM_RIGHTS carries at least one fd */
    if (out->nfds + (int)n > K_IPC_MAX_FDS) return -EMSGSIZE;
    for (uint32_t i = 0; i < n; i++)
      out->fds[out->nfds++] = (int)rd32(ctrl + off + 16 + i * 4);

    off += (uint32_t)clen;
    uint32_t nxt = ipc_cmsg_align(off);
    if (nxt <= ctrl_len) off = nxt; /* consume the alignment padding */
    else break;
  }
  if (out->nfds == 0) return -EINVAL;
  return 0;
}

uint32_t ipc_cmsg_fit(uint32_t controllen, uint32_t nfds) {
  uint32_t n = 0;
  while (n < nfds && n < K_IPC_MAX_FDS &&
         ipc_cmsg_space(n + 1) <= controllen)
    n++;
  return n;
}

int ipc_msg_flags_ok(int flags, int recv) {
  if (flags & K_MSG_PEEK) return -EOPNOTSUPP;
  int allowed = recv ? (K_MSG_DONTWAIT | K_MSG_CMSG_CLOEXEC)
                     : (K_MSG_DONTWAIT | K_MSG_NOSIGNAL);
  if (flags & ~allowed) return -EINVAL;
  return 0;
}

short ipc_poll_map(int r, int w, int e, int hup, short events) {
  short rev = 0;
  if (r && (events & K_POLLIN)) rev |= K_POLLIN;
  if (w && (events & K_POLLOUT)) rev |= K_POLLOUT;
  if (e) rev |= K_POLLERR;
  if (hup) rev |= K_POLLHUP;
  return rev;
}
