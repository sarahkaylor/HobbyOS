/*
 * src/kernel/linux_compat_netlink.c - C1-k4: a minimal AF_NETLINK /
 * NETLINK_ROUTE socket for the Linux-ABI class (docs/cp-k4.md §7).
 *
 * Why: glibc 2.31's if_nameindex() and getifaddrs() are netlink clients
 * (sysdeps/unix/sysv/linux/ifaddrs.c, if_index.c).  With socket(AF_NETLINK)
 * answering EAFNOSUPPORT, if_nameindex() returns NULL and Wine 11's
 * nsiproxy.so update_if_table() dereferences it at once
 * (nsiproxy.so+0x7736 `mov (%rax),%r15d` right after the call): every
 * winedevice.exe that hosts nsiproxy.sys died "VA=0 PC=...F736" in C4-x2 P4
 * run 8 and C5-vk2 run 5.  nsiproxy's poll_events() also opens one and
 * subscribes to address changes.
 *
 * What it is: socket() returns one end of a kernel-made AF_UNIX DGRAM pair;
 * the kernel keeps the other end (detached from the fd table) and answers
 * every request datagram the program sends with the reply datagram(s) Linux
 * would send for this machine -- lo (127.0.0.1/8) and eth0 (the DHCP address
 * /24, gateway .2, MAC 52:54:00:12:34:56).  Supported: RTM_GETLINK,
 * RTM_GETADDR (AF_INET and AF_UNSPEC; AF_INET6 dumps are empty) and
 * RTM_GETROUTE dumps; anything else gets NLMSG_ERROR -EOPNOTSUPP.  bind()
 * stores the groups and never fires an event (the addresses never change).
 * getsockname() reports {AF_NETLINK, pid}; recvmsg()/recvfrom() report the
 * kernel as the sender ({AF_NETLINK, 0, 0}), which glibc checks.
 */
#include "linux_compat.h"

#include <stdint.h>

#include "errno.h"
#include "fs.h"
#include "lock.h"
#include "process.h"
#include "unix.h"

extern uint32_t net_get_ip(void);
extern int fs_fd_gfd(struct process *p, int fd);
extern struct usock *fs_gfd_usock(int gfd, int *end);
extern int fs_detach_fd(struct process *p, int fd);
extern void fs_close_global(int g_fd);
extern int file_socketpair(struct process *p, int domain, int type, int proto,
                           int fds[2]);
extern int file_fcntl(struct process *p, int fd, int cmd, uint64_t arg);

#define L_AF_NETLINK 16
#define L_NETLINK_ROUTE 0
#define L_NL_MAX 32

static struct {
  int used;
  int ugfd;           /* the program's end (global file index) */
  struct usock *uptr; /* identity check: the slot was not recycled */
  int pgfd;           /* the kernel's end, detached from every fd table */
  struct usock *pptr;
  int pend;
  uint32_t pid, groups;
} l_nl[L_NL_MAX];
static spinlock_t l_nl_lock;

/* A netlink entry whose program end is gone: close the kernel's end. */
static void l_nl_sweep(void) {
  int drop[L_NL_MAX];
  int nd = 0;
  uint64_t fl = spinlock_acquire_irqsave(&l_nl_lock);
  for (int i = 0; i < L_NL_MAX; i++) {
    if (!l_nl[i].used) continue;
    int e;
    if (fs_gfd_usock(l_nl[i].ugfd, &e) != l_nl[i].uptr) {
      drop[nd++] = l_nl[i].pgfd;
      l_nl[i].used = 0;
    }
  }
  spinlock_release_irqrestore(&l_nl_lock, fl);
  for (int i = 0; i < nd; i++) fs_close_global(drop[i]);
}

/* Slot of the netlink socket behind `fd`, or -1. */
static int l_nl_of(struct process *p, int fd) {
  int g = fs_fd_gfd(p, fd);
  if (g < 0) return -1;
  int e;
  struct usock *u = fs_gfd_usock(g, &e);
  if (!u) return -1;
  for (int i = 0; i < L_NL_MAX; i++)
    if (l_nl[i].used && l_nl[i].ugfd == g && l_nl[i].uptr == u) return i;
  return -1;
}

int linux_netlink_is(struct process *p, int fd) { return l_nl_of(p, fd) >= 0; }

/* socket(AF_NETLINK, SOCK_RAW|SOCK_DGRAM [|flags], NETLINK_ROUTE). */
int linux_netlink_socket(struct process *p, int type, int protocol) {
  int base = type & 0xF;
  if (base != K_SOCK_RAW && base != K_SOCK_DGRAM) return -ESOCKTNOSUPPORT;
  if (protocol != L_NETLINK_ROUTE) return -EPROTONOSUPPORT;
  l_nl_sweep();
  int fds[2] = {-1, -1};
  int r = file_socketpair(p, K_AF_UNIX, K_SOCK_DGRAM | (type & ~0xF), 0, fds);
  if (r != 0) return r;
  int ug = fs_fd_gfd(p, fds[0]);
  int pg = fs_detach_fd(p, fds[1]);
  int ue, pe;
  struct usock *uu = fs_gfd_usock(ug, &ue);
  struct usock *pu = fs_gfd_usock(pg, &pe);
  uint64_t fl = spinlock_acquire_irqsave(&l_nl_lock);
  int slot = -1;
  for (int i = 0; i < L_NL_MAX; i++)
    if (!l_nl[i].used) { slot = i; break; }
  if (slot >= 0) {
    l_nl[slot].used = 1;
    l_nl[slot].ugfd = ug;
    l_nl[slot].uptr = uu;
    l_nl[slot].pgfd = pg;
    l_nl[slot].pptr = pu;
    l_nl[slot].pend = pe;
    l_nl[slot].pid = (uint32_t)process_group(p)->pid;
    l_nl[slot].groups = 0;
  }
  spinlock_release_irqrestore(&l_nl_lock, fl);
  if (slot < 0 || !uu || !pu) {
    if (pg >= 0) fs_close_global(pg);
    (void)file_close(p, fds[0]);
    return -ENOBUFS;
  }
  return fds[0];
}

/* bind(fd, struct sockaddr_nl *, len): remember the groups. */
int linux_netlink_bind(struct process *p, int fd, const uint8_t *sa, int len) {
  int s = l_nl_of(p, fd);
  if (s < 0) return -ENOTSOCK;
  if (len < 12 || (sa[0] | (sa[1] << 8)) != L_AF_NETLINK) return -EINVAL;
  l_nl[s].groups = (uint32_t)sa[8] | ((uint32_t)sa[9] << 8) |
                   ((uint32_t)sa[10] << 16) | ((uint32_t)sa[11] << 24);
  return 0;
}

static void put32(uint8_t *b, uint32_t v) {
  b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static uint32_t get32(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* sockaddr_nl {family, pad, pid, groups} into a 12-byte buffer. */
void linux_netlink_name(struct process *p, int fd, int self, uint8_t *sa) {
  int s = l_nl_of(p, fd);
  for (int i = 0; i < 12; i++) sa[i] = 0;
  put16(sa, L_AF_NETLINK);
  if (s >= 0 && self) {
    put32(sa + 4, l_nl[s].pid);
    put32(sa + 8, l_nl[s].groups);
  }
}

/* ---- reply builder ------------------------------------------------------ */

struct nlb {
  uint8_t b[2048];
  int n;
};
static uint8_t *nl_msg(struct nlb *o, uint16_t type, uint16_t flags, uint32_t seq,
                       uint32_t pid, int body) {
  if (o->n + 16 + body > (int)sizeof o->b) return 0;
  uint8_t *h = o->b + o->n;
  for (int i = 0; i < 16 + body; i++) h[i] = 0;
  put32(h, (uint32_t)(16 + body));
  put16(h + 4, type);
  put16(h + 6, flags);
  put32(h + 8, seq);
  put32(h + 12, pid);
  o->n += 16 + body;
  return h;
}
static void nl_attr(struct nlb *o, uint8_t *h, uint16_t type, const void *v, int len) {
  int al = (4 + len + 3) & ~3;
  if (o->n + al > (int)sizeof o->b) return;
  uint8_t *a = o->b + o->n;
  for (int i = 0; i < al; i++) a[i] = 0;
  put16(a, (uint16_t)(4 + len));
  put16(a + 2, type);
  for (int i = 0; i < len; i++) a[4 + i] = ((const uint8_t *)v)[i];
  o->n += al;
  put32(h, get32(h) + (uint32_t)al);
}

static void nl_ifaces(uint8_t ip[4]) {
  uint32_t v = net_get_ip();
  ip[0] = (uint8_t)v; ip[1] = (uint8_t)(v >> 8); ip[2] = (uint8_t)(v >> 16); ip[3] = (uint8_t)(v >> 24);
  if (!v) { ip[0] = 10; ip[1] = 0; ip[2] = 2; ip[3] = 15; } /* QEMU user net */
}

static void nl_links(struct nlb *o, uint32_t seq, uint32_t pid) {
  static const uint8_t mac0[6] = {0, 0, 0, 0, 0, 0};
  static const uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  static const uint8_t bc1[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  for (int k = 0; k < 2; k++) {
    uint8_t *h = nl_msg(o, 16 /* RTM_NEWLINK */, 2 /* NLM_F_MULTI */, seq, pid, 16);
    if (!h) return;
    uint8_t *ifi = h + 16;
    put16(ifi + 2, k ? 1 : 772);                  /* ARPHRD_ETHER / LOOPBACK */
    put32(ifi + 4, (uint32_t)(k + 1));             /* ifindex */
    put32(ifi + 8, k ? 0x1043u : 0x49u);           /* UP|BCAST|RUNNING|MCAST ; UP|LOOPBACK|RUNNING */
    nl_attr(o, h, 3 /* IFLA_IFNAME */, k ? "eth0" : "lo", k ? 5 : 3);
    uint32_t mtu = k ? 1500u : 65536u;
    uint8_t mb[4];
    put32(mb, mtu);
    nl_attr(o, h, 4 /* IFLA_MTU */, mb, 4);
    nl_attr(o, h, 1 /* IFLA_ADDRESS */, k ? mac1 : mac0, 6);
    nl_attr(o, h, 2 /* IFLA_BROADCAST */, k ? bc1 : mac0, 6);
  }
}

static void nl_addrs(struct nlb *o, uint32_t seq, uint32_t pid) {
  uint8_t ip[4];
  nl_ifaces(ip);
  for (int k = 0; k < 2; k++) {
    uint8_t *h = nl_msg(o, 20 /* RTM_NEWADDR */, 2, seq, pid, 8);
    if (!h) return;
    uint8_t *ifa = h + 16;
    ifa[0] = 2;                       /* AF_INET */
    ifa[1] = k ? 24 : 8;              /* prefixlen */
    ifa[2] = 0x80;                    /* IFA_F_PERMANENT */
    ifa[3] = k ? 0 : 254;             /* RT_SCOPE_UNIVERSE / HOST */
    put32(ifa + 4, (uint32_t)(k + 1));
    uint8_t lo[4] = {127, 0, 0, 1};
    const uint8_t *a = k ? ip : lo;
    nl_attr(o, h, 1 /* IFA_ADDRESS */, a, 4);
    nl_attr(o, h, 2 /* IFA_LOCAL */, a, 4);
    if (k) {
      uint8_t bc[4] = {ip[0], ip[1], ip[2], 255};
      nl_attr(o, h, 4 /* IFA_BROADCAST */, bc, 4);
    }
    nl_attr(o, h, 3 /* IFA_LABEL */, k ? "eth0" : "lo", k ? 5 : 3);
  }
}

static void nl_routes(struct nlb *o, uint32_t seq, uint32_t pid) {
  uint8_t ip[4];
  nl_ifaces(ip);
  uint8_t gw[4] = {ip[0], ip[1], ip[2], 2};
  uint8_t net[4] = {ip[0], ip[1], ip[2], 0};
  uint8_t oif[4];
  put32(oif, 2);
  for (int k = 0; k < 2; k++) {
    uint8_t *h = nl_msg(o, 24 /* RTM_NEWROUTE */, 2, seq, pid, 12);
    if (!h) return;
    uint8_t *rt = h + 16;
    rt[0] = 2;                 /* AF_INET */
    rt[1] = k ? 24 : 0;        /* dst_len */
    rt[4] = 254;               /* RT_TABLE_MAIN */
    rt[5] = k ? 2 : 3;         /* RTPROT_KERNEL / RTPROT_BOOT */
    rt[6] = k ? 253 : 0;       /* RT_SCOPE_LINK / UNIVERSE */
    rt[7] = 1;                 /* RTN_UNICAST */
    if (k) nl_attr(o, h, 1 /* RTA_DST */, net, 4);
    else nl_attr(o, h, 5 /* RTA_GATEWAY */, gw, 4);
    nl_attr(o, h, 4 /* RTA_OIF */, oif, 4);
  }
}

/* sendto/sendmsg of one request datagram: answer it into the program's
 * receive queue.  Returns the request length, or -errno. */
int linux_netlink_send(struct process *p, int fd, const uint8_t *req, int len) {
  int s = l_nl_of(p, fd);
  if (s < 0) return -ENOTSOCK;
  if (len < 16) return -EINVAL;
  static struct nlb o; /* serialised by l_nl_lock */
  uint64_t fl = spinlock_acquire_irqsave(&l_nl_lock);
  o.n = 0;
  int off = 0;
  while (off + 16 <= len) {
    uint32_t mlen = get32(req + off);
    if (mlen < 16 || off + (int)mlen > len) break;
    uint16_t type = (uint16_t)(req[off + 4] | (req[off + 5] << 8));
    uint16_t flags = (uint16_t)(req[off + 6] | (req[off + 7] << 8));
    uint32_t seq = get32(req + off + 8);
    uint32_t pid = l_nl[s].pid;
    uint8_t fam = mlen > 16 ? req[off + 16] : 0;
    int dump = (flags & 0x300) != 0; /* NLM_F_ROOT|NLM_F_MATCH */
    if (type == 18 /* RTM_GETLINK */ && dump) {
      nl_links(&o, seq, pid);
    } else if (type == 22 /* RTM_GETADDR */ && dump) {
      if (fam == 0 || fam == 2) nl_addrs(&o, seq, pid);
    } else if (type == 26 /* RTM_GETROUTE */ && dump) {
      if (fam == 0 || fam == 2) nl_routes(&o, seq, pid);
    } else {
      uint8_t *h = nl_msg(&o, 2 /* NLMSG_ERROR */, 0, seq, pid, 4 + 16);
      if (h) {
        put32(h + 16, (uint32_t)-EOPNOTSUPP);
        for (int i = 0; i < 16; i++) h[20 + i] = req[off + i];
      }
      off += (int)((mlen + 3) & ~3u);
      continue;
    }
    uint8_t *d = nl_msg(&o, 3 /* NLMSG_DONE */, 2, seq, pid, 4);
    (void)d;
    off += (int)((mlen + 3) & ~3u);
  }
  int w = o.n > 0 ? usock_send(l_nl[s].pptr, l_nl[s].pend, o.b, o.n, 1) : 0;
  spinlock_release_irqrestore(&l_nl_lock, fl);
  if (w < 0) return -ENOBUFS; /* the program never drained its replies */
  return len;
}
