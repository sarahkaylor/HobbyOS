#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "net.h"
#include "fs.h"
#include "process.h"
#include "timer.h"
#include "errno.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern int process_kill(int pid);

/* Bring up a mock current process, exactly like fs_test.c does: file_* and
 * the F1 socket helpers take a `struct process *` and the trap layer would
 * hand them current_process(). */
static struct process *net_test_begin(int *pid_out) {
  int pid = process_create();
  if (pid < 0) return 0;
  process_get_pcb(pid)->is_kernel_process = 1;
  set_current_process_pid(0, pid);
  *pid_out = pid;
  return current_process();
}

static void net_test_end(int pid) {
  set_current_process_pid(0, -1);
  process_kill(pid);
}

void net_test(void) {
  // Test ntohl / htonl
  ASSERT(ntohl(0x12345678) == 0x78563412);
  ASSERT(htonl(0x12345678) == 0x78563412);
  ASSERT(ntohs(0x1234) == 0x3412);

  // Test checksum (simple IP header example)
  uint8_t ip_header[] = {
    0x45, 0x00, 0x00, 0x73,
    0x00, 0x00, 0x40, 0x00,
    0x40, 0x11, 0x00, 0x00,
    0xc0, 0xa8, 0x00, 0x01,
    0xc0, 0xa8, 0x00, 0xc7
  };

  uint16_t sum = net_checksum(ip_header, sizeof(ip_header));
  EXPECT_EQ(sum, 0x61b8);
}

/* F1.1: SYS_SOCKET's kernel side -- create a socket into the fd table and
 * check the fd behaves as a socket (SO_TYPE, fcntl flags, error paths). */
static void test_net_socket_fd_table(void) {
  uart_puts("  Running test_net_socket_fd_table...\n");
  tests_run++;

  int pid = -1;
  struct process *cur = net_test_begin(&pid);
  ASSERT(cur != 0);

  int fd = file_socket(cur, K_AF_INET, K_SOCK_STREAM, 0);
  EXPECT_EQ((fd >= 0), 1);
  EXPECT_EQ(cur->open_fds[fd] >= 0, 1);

  int type = 0;
  int len = (int)sizeof(type);
  EXPECT_EQ(file_socket_getopt(cur, fd, K_SOL_SOCKET, K_SO_TYPE, &type, &len), 0);
  EXPECT_EQ(type, K_SOCK_STREAM);
  EXPECT_EQ(len, (int)sizeof(int));

  int err = -1;
  len = (int)sizeof(err);
  EXPECT_EQ(file_socket_getopt(cur, fd, K_SOL_SOCKET, K_SO_ERROR, &err, &len), 0);
  EXPECT_EQ(err, 0);

  /* fcntl round-trip on the O_NONBLOCK flag (F1.1/F1.3). */
  EXPECT_EQ(file_fcntl(cur, fd, K_F_GETFL, 0), 0);
  EXPECT_EQ(file_fcntl(cur, fd, K_F_SETFL, K_O_NONBLOCK), 0);
  EXPECT_EQ(file_fcntl(cur, fd, K_F_GETFL, 0), K_O_NONBLOCK);
  EXPECT_EQ(file_fcntl(cur, fd, K_F_SETFL, 0), 0);
  EXPECT_EQ(file_fcntl(cur, fd, K_F_GETFL, 0), 0);
  EXPECT_EQ(file_fcntl(cur, fd, 9 /* unknown cmd */, 0), -EINVAL);

  /* A datagram socket reports SOCK_DGRAM; duplicate create also works. */
  int fd2 = file_socket(cur, K_AF_INET, K_SOCK_DGRAM, K_IPPROTO_UDP);
  EXPECT_EQ((fd2 >= 0), 1);
  EXPECT_EQ((fd2 != fd), 1);
  len = (int)sizeof(type);
  EXPECT_EQ(file_socket_getopt(cur, fd2, K_SOL_SOCKET, K_SO_TYPE, &type, &len), 0);
  EXPECT_EQ(type, K_SOCK_DGRAM);

  /* Error paths: bad domain, mismatched protocol/type, unknown option. */
  /* P4 §6.6 (consented 2026-09-30): AF_UNIX is real now — the old "AF_UNIX
   * unsupported in v1 -> EAFNOSUPPORT" expectation flips to success; an
   * unsupported type (SOCK_RAW) is the remaining rejection. */
  int afu = file_socket(cur, K_AF_UNIX, K_SOCK_STREAM, 0);
  EXPECT_EQ((afu >= 0), 1);
  if (afu >= 0) EXPECT_EQ(file_close(cur, afu), 0);
  EXPECT_EQ(file_socket(cur, K_AF_UNIX, K_SOCK_RAW, 0), -EPROTONOSUPPORT);
  EXPECT_EQ(file_socket(cur, K_AF_INET, K_SOCK_STREAM, K_IPPROTO_UDP),
            -EPROTONOSUPPORT);
  len = (int)sizeof(type);
  EXPECT_EQ(file_socket_getopt(cur, fd, K_SOL_SOCKET, 1234, &type, &len),
            -ENOPROTOOPT);
  EXPECT_EQ(file_socket_getopt(cur, fd, 7 /* bad level */, K_SO_TYPE, &type, &len),
            -ENOPROTOOPT);
  EXPECT_EQ(file_socket_connect(cur, 99, 0x01010101, 0x5000), -EBADF);

  /* connect_fd on a non-socket fd: ENOTSOCK. */
  int ffd = file_open(cur, "TEST.TXT", 0);
  if (ffd >= 0) {
    EXPECT_EQ(file_socket_connect(cur, ffd, 0x01010101, 0x5000), -ENOTSOCK);
    /* fcntl is socket-scoped in v1: a non-socket fd reports ENOTSOCK. */
    EXPECT_EQ(file_fcntl(cur, ffd, K_F_GETFL, 0), -ENOTSOCK);
    file_close(cur, ffd);
  }

  /* SO_REUSEADDR is an accepted no-op, in both directions. */
  int one = 1;
  EXPECT_EQ(file_socket_setopt(cur, fd, K_SOL_SOCKET, K_SO_REUSEADDR, &one, 4), 0);
  EXPECT_EQ(file_socket_getopt(cur, fd, K_SOL_SOCKET, K_SO_REUSEADDR, &one, &len), 0);
  EXPECT_EQ(one, 0);

  EXPECT_EQ(file_close(cur, fd), 0);
  EXPECT_EQ(file_close(cur, fd2), 0);
  net_test_end(pid);
}

/* F1.1/F1.3: the PCB-level handshake state machine -- a due SYN is
 * retransmitted, an exhausted budget fails with ETIMEDOUT, and a UDP
 * "connect" completes at once. */
static void test_net_connect_state(void) {
  uart_puts("  Running test_net_connect_state...\n");
  tests_run++;

  int pid = -1;
  struct process *cur = net_test_begin(&pid);
  ASSERT(cur != 0);

  int fd = file_socket(cur, K_AF_INET, K_SOCK_STREAM, 0);
  ASSERT(fd >= 0);
  struct socket_pcb *pcb = file_socket_pcb(cur, fd);
  ASSERT(pcb != 0);

  /* A fresh, never-connected TCP socket must not claim readiness: nothing
   * queued, no error, and no EOF lie (F1 readiness semantics). */
  int fresh = file_socket(cur, K_AF_INET, K_SOCK_STREAM, 0);
  ASSERT(fresh >= 0);
  struct socket_pcb *fpcb = file_socket_pcb(cur, fresh);
  ASSERT(fpcb != 0);
  EXPECT_EQ(net_socket_ready(fpcb, 0), 0);
  EXPECT_EQ(net_socket_ready(fpcb, 1), 0);
  EXPECT_EQ(net_socket_error(fpcb), 0);
  file_close(cur, fresh);

  /* Non-blocking TCP connect to TEST-NET-1 (192.0.2.1, never routed):
   * the handshake must start and stay pending, not fail. */
  net_socket_set_nonblock(pcb, 1);
  EXPECT_EQ(file_socket_connect(cur, fd, 0x010200C0u /* 192.0.2.1 net order */,
                                0x5000 /* port 80 in net order */),
            -EINPROGRESS);
  EXPECT_EQ(pcb->state, SOCKET_SYN_SENT);
  EXPECT_EQ(pcb->syn_retries, 1);
  EXPECT_EQ(net_socket_error(pcb), 0);
  /* Not writable yet (handshake pending), and not readable. */
  EXPECT_EQ(net_socket_ready(pcb, 1), 0);
  EXPECT_EQ(net_socket_ready(pcb, 0), 0);

  /* A due retransmit: back off 600ms with one SYN sent -> the tick sends
   * the second SYN and the counter advances (the "lost SYN" survives). */
  pcb->syn_last_ms = timer_get_ms() - 600;
  net_socket_tick(pcb);
  EXPECT_EQ(pcb->syn_retries, 2);
  EXPECT_EQ(pcb->state, SOCKET_SYN_SENT);

  /* Budget exhausted: after the final backoff the handshake fails
   * ETIMEDOUT instead of hanging forever. */
  pcb->syn_retries = 4;
  pcb->syn_last_ms = timer_get_ms() - 10000;
  net_socket_tick(pcb);
  EXPECT_EQ(pcb->state, SOCKET_CLOSED);
  EXPECT_EQ(pcb->connect_err, ETIMEDOUT);
  EXPECT_EQ(pcb->connect_started, 0);
  /* SO_ERROR reports it, writability becomes true so select() waiters wake
   * (that is how a failed non-blocking connect is observed). */
  EXPECT_EQ(net_socket_error(pcb), ETIMEDOUT);
  EXPECT_EQ(net_socket_ready(pcb, 1), 1);
  file_close(cur, fd);

  /* UDP connect completes immediately, with or without O_NONBLOCK. */
  int ufd = file_socket(cur, K_AF_INET, K_SOCK_DGRAM, 0);
  ASSERT(ufd >= 0);
  EXPECT_EQ(file_socket_connect(cur, ufd, 0x08080808u, 0x3500 /* port 53 */), 0);
  struct socket_pcb *upcb = file_socket_pcb(cur, ufd);
  ASSERT(upcb != 0);
  EXPECT_EQ(upcb->state, SOCKET_ESTABLISHED);
  EXPECT_EQ(net_socket_ready(upcb, 1), 1); /* a UDP peer is always writable */
  EXPECT_EQ(net_socket_ready(upcb, 0), 0); /* nothing queued */
  file_close(cur, ufd);

  net_test_end(pid);
}

/* F1.2: the select() engine -- pipe readiness, ready counts, timeout
 * booking, the -2 park request, and error paths. */
static void test_net_select_engine(void) {
  uart_puts("  Running test_net_select_engine...\n");
  tests_run++;

  int pid = -1;
  struct process *cur = net_test_begin(&pid);
  ASSERT(cur != 0);

  struct fd_set_k rd, wr;
  for (int i = 0; i < K_FD_SET_WORDS; i++) {
    rd.bits[i] = 0;
    wr.bits[i] = 0;
  }

  /* nfds out of range is EINVAL (frozen FD_SETSIZE 256). */
  EXPECT_EQ(file_select(cur, K_FD_SETSIZE + 1, &rd, 0, 0, 0), -EINVAL);
  EXPECT_EQ(file_select(cur, -1, &rd, 0, 0, 0), -EINVAL);

  /* An fd that is not open in the mask is EBADF. */
  rd.bits[0] = 1u << 7;
  EXPECT_EQ(file_select(cur, 8, &rd, 0, 0, 0), -EBADF);
  rd.bits[0] = 0;

  int pfd[2];
  EXPECT_EQ(file_pipe(cur, pfd), 0);
  int rfd = pfd[0], wfd = pfd[1];

  /* Empty pipe + 0 timeout: not ready, masks cleared, count 0. */
  rd.bits[rfd / 32] = 1u << (rfd % 32);
  EXPECT_EQ(file_select(cur, rfd + 1, &rd, 0, 0, 0), 0);
  EXPECT_EQ(rd.bits[rfd / 32], 0u);

  /* Write one byte: the read end becomes ready and the mask survives. */
  uint8_t byte = 0x5A;
  EXPECT_EQ(file_write(cur, wfd, &byte, 1, 0), 1);
  rd.bits[rfd / 32] = 1u << (rfd % 32);
  EXPECT_EQ(file_select(cur, rfd + 1, &rd, 0, 0, 0), 1);
  EXPECT_EQ((rd.bits[rfd / 32] & (1u << (rfd % 32))) != 0, 1);
  uint8_t got = 0;
  EXPECT_EQ(file_read(cur, rfd, &got, 1, 0), 1);
  EXPECT_EQ(got, 0x5A);

  /* Write mask on a pipe with space is ready; two pipes ready count 2. */
  int pfd2[2];
  EXPECT_EQ(file_pipe(cur, pfd2), 0);
  EXPECT_EQ(file_write(cur, wfd, &byte, 1, 0), 1);
  EXPECT_EQ(file_write(cur, pfd2[1], &byte, 1, 0), 1);
  for (int i = 0; i < K_FD_SET_WORDS; i++) rd.bits[i] = 0;
  rd.bits[rfd / 32] |= 1u << (rfd % 32);
  rd.bits[pfd2[0] / 32] |= 1u << (pfd2[0] % 32);
  int nfds = (rfd > pfd2[0] ? rfd : pfd2[0]) + 1;
  EXPECT_EQ(file_select(cur, nfds, &rd, 0, 0, 0), 2);

  /* Drain both pipes again so the next check really has nothing ready. */
  uint8_t got2 = 0;
  EXPECT_EQ(file_read(cur, rfd, &got2, 1, 0), 1);
  EXPECT_EQ(file_read(cur, pfd2[0], &got2, 1, 0), 1);

  /* Nothing ready with a positive timeout asks for the syscall restart
   * (-2) after parking the process; the deadline is still pending. */
  for (int i = 0; i < K_FD_SET_WORDS; i++) rd.bits[i] = 0;
  rd.bits[rfd / 32] = 1u << (rfd % 32);
  int r = file_select(cur, rfd + 1, &rd, 0, 0, 100);
  EXPECT_EQ(r, -2);
  EXPECT_EQ(cur->state, PROC_STATE_BLOCKED);
  EXPECT_EQ((cur->wake_ms > 0), 1);
  cur->state = PROC_STATE_READY;
  cur->wake_ms = 0;

  file_close(cur, rfd);
  file_close(cur, wfd);
  file_close(cur, pfd2[0]);
  file_close(cur, pfd2[1]);
  net_test_end(pid);
}

/* F1.4: entropy sanity -- consecutive draws differ and a page is never all
 * zero. */
static void test_net_getrandom(void) {
  uart_puts("  Running test_net_getrandom...\n");
  tests_run++;

  uint64_t draws[16];
  static uint8_t page[4096];
  int all_zero = 1;
  int any_zero_run = 1;

  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(net_get_random_bytes(&draws[i], sizeof(uint64_t)), 8);
    if (i > 0) {
      EXPECT_EQ((draws[i] != draws[i - 1]), 1);
    }
  }
  EXPECT_EQ(net_get_random_bytes(page, sizeof(page)), (int)sizeof(page));
  for (unsigned i = 0; i < sizeof(page); i++) {
    if (page[i] != 0) all_zero = 0;
  }
  EXPECT_EQ(all_zero, 0);
  /* A full zero 8-byte window has probability ~2^-64; catching it here
   * means the mixer collapsed. */
  for (unsigned i = 0; i + 8 <= sizeof(page); i++) {
    uint64_t w = 0;
    for (int b = 0; b < 8; b++) w |= (uint64_t)page[i + b] << (8 * b);
    if (w != 0) any_zero_run = 0;
  }
  EXPECT_EQ(any_zero_run, 0);
}

/* l8-netfix (WK-4): build an Ethernet+IPv4+TCP frame for the RX-layer
 * regression tests below.  `frame_len` is the FULL padded frame length the
 * driver hands net_rx_packet(); `datagram_len` is the IPv4 total-length
 * field as it appears on the wire (Ethernet minimum-frame padding must not
 * be counted as IP payload).  No checksums are validated by the L4
 * handlers, so none are filled in here. */
static void netfix_build_tcp_frame(uint8_t* f, uint32_t frame_len,
                                   uint32_t datagram_len, uint16_t src_port,
                                   uint16_t dst_port, uint32_t seq,
                                   uint32_t ack, const uint8_t* payload,
                                   uint32_t plen) {
  struct eth_hdr* eth = (struct eth_hdr*)f;
  struct ipv4_hdr* ip = (struct ipv4_hdr*)(f + sizeof(struct eth_hdr));
  struct tcp_hdr* tcp = (struct tcp_hdr*)(f + sizeof(struct eth_hdr) +
                                          sizeof(struct ipv4_hdr));
  uint8_t* p = f + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) +
               sizeof(struct tcp_hdr);
  int i;
  for (i = 0; i < 6; i++) eth->dst_mac[i] = 0x52;
  for (i = 0; i < 6; i++) eth->src_mac[i] = 0x53;
  eth->type = htons(ETH_TYPE_IPV4);

  ip->version = 4;
  ip->ihl = 5;
  ip->tos = 0;
  ip->total_len = htons((uint16_t)datagram_len);
  ip->id = 0;
  ip->frag_off = 0;
  ip->ttl = 64;
  ip->protocol = IP_PROTO_TCP;
  ip->checksum = 0;
  ip->src_ip = 0x0202000Au; /* 10.0.2.2 (wire order) */
  ip->dst_ip = 0x0F02000Au; /* 10.0.2.15 (wire order) */

  tcp->src_port = htons(src_port);
  tcp->dst_port = htons(dst_port);
  tcp->seq = htonl(seq);
  tcp->ack = htonl(ack);
  tcp->data_offset = 5;
  tcp->reserved = 0;
  tcp->ns = 0;
  tcp->fin = 0;
  tcp->syn = 0;
  tcp->rst = 0;
  tcp->psh = 0;
  tcp->ack_flag = 1;
  tcp->urg = 0;
  tcp->ece = 0;
  tcp->cwr = 0;
  tcp->window_size = 0;
  tcp->checksum = 0;
  tcp->urgent_ptr = 0;

  for (i = 0; i < (int)plen; i++) p[i] = payload[i];
  for (i = (int)(sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) +
                 sizeof(struct tcp_hdr) + plen);
       i < (int)frame_len; i++)
    f[i] = 0; /* Ethernet minimum-frame padding */
}

/* l8-netfix bug #2 (WK-4): a TCP connection must not deliver Ethernet
 * minimum-frame padding as payload.  QEMU slirp pads every 54-byte
 * header-only segment (ACK/FIN/SYN-ACK) to 60 bytes with zeros; the kernel
 * must derive the L4 length from the IPv4 total-length field, never from
 * the padded frame length.  Regression: an ACK padded to 60 bytes before
 * the data segment used to prepend six 0x00 bytes to recv() (the WK-4
 * "stray NULs before the HTTP response"). */
static void test_net_ether_padding(void) {
  uart_puts("  Running test_net_ether_padding...\n");
  tests_run++;

  int pid = -1;
  struct process* cur = net_test_begin(&pid);
  ASSERT(cur != 0);

  /* 10.0.2.15/24, gw 10.0.2.2 — slirp's user-network layout. */
  net_set_ip(0x0F02000Au, 0x00FFFFFFu, 0x0202000Au);
  net_refresh_mac();

  int fd = file_socket(cur, K_AF_INET, K_SOCK_STREAM, 0);
  ASSERT(fd >= 0);
  struct socket_pcb* pcb = file_socket_pcb(cur, fd);
  ASSERT(pcb != 0);

  /* Established against the host echo server. */
  pcb->state = SOCKET_ESTABLISHED;
  pcb->remote_ip = 0x0202000Au; /* 10.0.2.2 */
  pcb->remote_port = 8765;
  pcb->seq = 1000;
  pcb->ack = 100;

  /* 1) Header-only ACK, padded on the wire to the 60-byte Ethernet
        minimum: must contribute ZERO payload bytes. */
  uint8_t ack_frame[60];
  netfix_build_tcp_frame(ack_frame, sizeof(ack_frame), 40, /* ip len */
                         pcb->remote_port, pcb->local_port, pcb->ack, pcb->seq,
                         0, 0);
  net_rx_packet(ack_frame, (uint32_t)sizeof(ack_frame));
  EXPECT_EQ(net_socket_available(pcb), 0);

  /* 2) A real data segment (8 payload bytes, frame > 60 so no pad):
        recv must return exactly the payload with no leading NULs. */
  uint8_t data_frame[sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) +
                     sizeof(struct tcp_hdr) + 8];
  static const uint8_t payload[8] = {'A', 'B', 'C', 'D',
                                     'E', 'F', 'G', 'H'};
  netfix_build_tcp_frame(data_frame, (uint32_t)sizeof(data_frame),
                         40 + 8, /* ip len */
                         pcb->remote_port, pcb->local_port, pcb->ack, pcb->seq,
                         payload, 8);
  net_rx_packet(data_frame, (uint32_t)sizeof(data_frame));
  EXPECT_EQ(net_socket_available(pcb), 8);

  uint8_t got[16];
  for (int i = 0; i < 16; i++) got[i] = 0xEE;
  int n = net_socket_recv(pcb, got, 16);
  EXPECT_EQ(n, 8);
  int match = 1;
  for (int i = 0; i < 8; i++) {
    if (got[i] != payload[i]) match = 0;
  }
  EXPECT_EQ(match, 1);
  EXPECT_EQ(net_socket_available(pcb), 0);

  /* 3) Defensive clamp: a corrupt total_len larger than the frame must not
        push the L4 parse past the buffer (no overflow past 60 bytes). */
  uint8_t ack2[60];
  netfix_build_tcp_frame(ack2, sizeof(ack2), 4000, /* absurd ip len */
                         pcb->remote_port, pcb->local_port, pcb->ack, pcb->seq,
                         0, 0);
  net_rx_packet(ack2, (uint32_t)sizeof(ack2));
  EXPECT_EQ(net_socket_available(pcb), 0);

  file_close(cur, fd);
  net_test_end(pid);
}

void net_test_suite(void) {
  uart_puts("Running net stack tests...\n");
  net_test();
  test_net_socket_fd_table();
  test_net_connect_state();
  test_net_select_engine();
  test_net_getrandom();
  test_net_ether_padding();
}

#endif
