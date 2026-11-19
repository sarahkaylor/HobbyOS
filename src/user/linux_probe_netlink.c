/*
 * src/user/linux_probe_netlink.c - C1-k4: AF_NETLINK / NETLINK_ROUTE, as
 * libc's if_nameindex()/getifaddrs() and Wine's nsiproxy use it
 * (docs/cp-k4.md §7).
 *
 * ORDINARY musl program; the same ELF runs on strix (Linux) and in the
 * guest.  musl's if_nameindex()/getifaddrs() are netlink clients exactly
 * like glibc 2.31's (RTM_GETLINK / RTM_GETADDR dumps).  Every row is ONE
 * write(2); last row "[LP] done rc=N".
 *
 *  N1  socket(AF_NETLINK, SOCK_RAW|SOCK_CLOEXEC, NETLINK_ROUTE), bind with
 *      RTMGRP_IPV4_IFADDR, getsockname answers AF_NETLINK
 *  N2  if_nameindex() is not NULL and names "lo" plus one more interface
 *      (Wine's nsiproxy update_if_table() dereferences it unchecked)
 *  N3  getifaddrs() lists 127.0.0.1 and one non-loopback IPv4 address
 *  N4  a raw RTM_GETROUTE dump has a default route and ends in NLMSG_DONE,
 *      sender {AF_NETLINK, pid 0}
 *  N5  a subscribed socket with no address change: poll() times out and a
 *      MSG_DONTWAIT recv() is EAGAIN (nsiproxy's poll_events loop)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The kernel UAPI bits this needs (musl-gcc ships no linux/ headers). */
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_ROUTE 0
#define RTMGRP_IPV4_IFADDR 0x10
#define NLM_F_REQUEST 1
#define NLM_F_DUMP 0x300
#define NLMSG_DONE 3
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26
#define RT_TABLE_MAIN 254
struct sockaddr_nl {
  unsigned short nl_family, nl_pad;
  unsigned int nl_pid, nl_groups;
};
struct nlmsghdr {
  unsigned int nlmsg_len;
  unsigned short nlmsg_type, nlmsg_flags;
  unsigned int nlmsg_seq, nlmsg_pid;
};
struct rtmsg {
  unsigned char rtm_family, rtm_dst_len, rtm_src_len, rtm_tos, rtm_table, rtm_protocol, rtm_scope, rtm_type;
  unsigned int rtm_flags;
};
#define NLMSG_ALIGN(l) (((l) + 3) & ~3)
#define NLMSG_DATA(h) ((void *)((char *)(h) + NLMSG_ALIGN(sizeof(struct nlmsghdr))))
#define NLMSG_OK(h, len) ((len) >= (int)sizeof(struct nlmsghdr) && (h)->nlmsg_len >= sizeof(struct nlmsghdr) && (h)->nlmsg_len <= (unsigned)(len))
#define NLMSG_NEXT(h, len) ((len) -= NLMSG_ALIGN((h)->nlmsg_len), (struct nlmsghdr *)((char *)(h) + NLMSG_ALIGN((h)->nlmsg_len)))

static int fails;
static void out(const char *fmt, ...) {
  char b[300];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}
static void ck(int ok, const char *fmt, ...) {
  char b[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  out("[LP] %s %s\n", ok ? "PASS" : "FAIL", b);
  if (!ok) fails++;
}

int main(void) {
  /* N1 */
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  int e1 = errno;
  struct sockaddr_nl a;
  memset(&a, 0, sizeof a);
  a.nl_family = AF_NETLINK;
  a.nl_groups = RTMGRP_IPV4_IFADDR;
  int b = fd >= 0 ? bind(fd, (struct sockaddr *)&a, sizeof a) : -1;
  struct sockaddr_nl got;
  socklen_t gl = sizeof got;
  memset(&got, 0, sizeof got);
  int g = fd >= 0 ? getsockname(fd, (struct sockaddr *)&got, &gl) : -1;
  ck(fd >= 0 && b == 0 && g == 0 && got.nl_family == AF_NETLINK,
     "N1 netlink socket fd=%d errno=%d bind=%d getsockname=%d family=%d", fd, fd < 0 ? e1 : 0, b, g, got.nl_family);

  /* N2 */
  struct if_nameindex *ni = if_nameindex();
  int nn = 0, lo = 0;
  if (ni)
    for (struct if_nameindex *x = ni; x->if_index; x++) {
      nn++;
      if (!strcmp(x->if_name, "lo")) lo = 1;
    }
  ck(ni && lo && nn >= 2, "N2 if_nameindex=%s count=%d lo=%d", ni ? "ok" : "NULL", nn, lo);
  if (ni) if_freenameindex(ni);

  /* N3 */
  struct ifaddrs *ia = 0;
  int gr = getifaddrs(&ia);
  int have_lo = 0, have_ext = 0;
  char ext[32] = "";
  for (struct ifaddrs *x = ia; gr == 0 && x; x = x->ifa_next) {
    if (!x->ifa_addr || x->ifa_addr->sa_family != AF_INET) continue;
    struct in_addr in = ((struct sockaddr_in *)x->ifa_addr)->sin_addr;
    if (ntohl(in.s_addr) == 0x7f000001) have_lo = 1;
    else if (!have_ext) { have_ext = 1; inet_ntop(AF_INET, &in, ext, sizeof ext); }
  }
  ck(gr == 0 && have_lo && have_ext, "N3 getifaddrs=%d lo=%d ext=%s", gr, have_lo, ext);
  if (ia) freeifaddrs(ia);

  /* N4 */
  int ok4 = 0, dflt = 0, done = 0, sender_ok = 1;
  int rf = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (rf >= 0) {
    struct {
      struct nlmsghdr h;
      struct rtmsg r;
    } req;
    memset(&req, 0, sizeof req);
    req.h.nlmsg_len = sizeof req;
    req.h.nlmsg_type = RTM_GETROUTE;
    req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.h.nlmsg_seq = 4242;
    req.r.rtm_family = AF_INET;
    struct sockaddr_nl k;
    memset(&k, 0, sizeof k);
    k.nl_family = AF_NETLINK;
    if (sendto(rf, &req, sizeof req, 0, (struct sockaddr *)&k, sizeof k) == (ssize_t)sizeof req) {
      for (int tries = 0; tries < 16 && !done; tries++) {
        char buf[8192];
        struct sockaddr_nl from;
        struct iovec iov = {buf, sizeof buf};
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_name = &from;
        m.msg_namelen = sizeof from;
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        int n = (int)recvmsg(rf, &m, 0);
        if (n <= 0) break;
        if (m.msg_namelen < sizeof from || from.nl_family != AF_NETLINK || from.nl_pid != 0) sender_ok = 0;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
          if (h->nlmsg_seq != 4242) continue;
          if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
          if (h->nlmsg_type == RTM_NEWROUTE) {
            struct rtmsg *r = NLMSG_DATA(h);
            if (r->rtm_family == AF_INET && r->rtm_dst_len == 0 && r->rtm_table == RT_TABLE_MAIN) dflt = 1;
          }
        }
      }
    }
    ok4 = dflt && done && sender_ok;
    close(rf);
  }
  ck(ok4, "N4 RTM_GETROUTE dump default=%d done=%d sender_ok=%d", dflt, done, sender_ok);

  /* N5 */
  int pr = -2, rr = -2, re = 0;
  if (fd >= 0) {
    struct pollfd pf = {fd, POLLIN, 0};
    pr = poll(&pf, 1, 100);
    char c[64];
    rr = (int)recv(fd, c, sizeof c, MSG_DONTWAIT);
    re = errno;
  }
  ck(pr == 0 && rr == -1 && (re == EAGAIN || re == EWOULDBLOCK), "N5 idle subscription poll=%d recv=%d errno=%d", pr,
     rr, re);
  if (fd >= 0) close(fd);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
