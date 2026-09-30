#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IPV4 0x0800

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define MAX_SOCKETS 16

struct eth_hdr {
  uint8_t dst_mac[6];
  uint8_t src_mac[6];
  uint16_t type;
} __attribute__((packed));

struct arp_hdr {
  uint16_t hw_type;
  uint16_t proto_type;
  uint8_t hw_len;
  uint8_t proto_len;
  uint16_t opcode;
  uint8_t sender_mac[6];
  uint32_t sender_ip;
  uint8_t target_mac[6];
  uint32_t target_ip;
} __attribute__((packed));

struct ipv4_hdr {
  uint8_t ihl : 4;
  uint8_t version : 4;
  uint8_t tos;
  uint16_t total_len;
  uint16_t id;
  uint16_t frag_off;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t checksum;
  uint32_t src_ip;
  uint32_t dst_ip;
} __attribute__((packed));

struct icmp_hdr {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t id;
  uint16_t seq;
} __attribute__((packed));

struct udp_hdr {
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t length;
  uint16_t checksum;
} __attribute__((packed));

struct tcp_hdr {
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t seq;
  uint32_t ack;
  uint8_t ns : 1;
  uint8_t reserved : 3;
  uint8_t data_offset : 4;
  uint8_t fin : 1;
  uint8_t syn : 1;
  uint8_t rst : 1;
  uint8_t psh : 1;
  uint8_t ack_flag : 1;
  uint8_t urg : 1;
  uint8_t ece : 1;
  uint8_t cwr : 1;
  uint16_t window_size;
  uint16_t checksum;
  uint16_t urgent_ptr;
} __attribute__((packed));

enum socket_state {
  SOCKET_CLOSED,
  SOCKET_SYN_SENT,
  SOCKET_ESTABLISHED,
  SOCKET_FIN_WAIT
};

#define SOCKET_RX_BUF_SIZE (4 * 1048576)  // 4MB — absorbs burst fire-and-forget DMA sync

struct socket_pcb {
  int in_use;
  int protocol; // IP_PROTO_TCP or IP_PROTO_UDP
  uint32_t local_ip;
  uint16_t local_port;
  uint32_t remote_ip;
  uint16_t remote_port;
  enum socket_state state;

  // TCP State
  uint32_t seq;
  uint32_t ack;

  // Cached destination MAC — set after first successful ARP resolution.
  // Eliminates per-packet ARP lookup for the RDMA hot path.
  uint8_t cached_mac[6];
  int mac_cached;  // 1 if cached_mac is valid

  // Receive buffer
  volatile uint8_t rx_buf[SOCKET_RX_BUF_SIZE];
  volatile uint32_t rx_head;
  volatile uint32_t rx_tail;

  // --- Phase F1 (browser.md A.1a) state --------------------------------
  // O_NONBLOCK as set through fcntl(F_SETFL); a non-blocking connect_fd()
  // starts the handshake and returns -EINPROGRESS instead of waiting.
  int nonblock;
  // SO_ERROR: 0 while healthy, else the errno of the failure that ended the
  // handshake/connection (ETIMEDOUT, ECONNREFUSED, ECONNRESET).  A pending
  // handshake keeps this at 0 (POSIX: EINPROGRESS is implicit).
  int connect_err;
  // 1 once a connect() was initiated on this socket (SO_ERROR/select become
  // meaningful); 0 for a socket that was never connected.
  int connect_started;
  // TCP handshake retransmission bookkeeping (F1.3): syn_retries counts SYNs
  // sent, syn_last_ms is the ms timestamp of the last one.
  int syn_retries;
  uint64_t syn_last_ms;
};

/**
 * Initializes the networking subsystem.
 * Sets up the internal PCB table and registers the socket subsystem.
 */
void net_init(void);
void net_refresh_mac(void);

/**
 * Sets the local IP address, subnet mask, and default gateway.
 * @param ip Local IP address in network byte order.
 * @param netmask Subnet mask in network byte order.
 * @param gateway Default gateway IP in network byte order.
 */
void net_set_ip(uint32_t ip, uint32_t netmask, uint32_t gateway);

/**
 * Retrieves the currently configured local IP address.
 * @return Local IP address in network byte order.
 */
uint32_t net_get_ip(void);

/**
 * Processes an incoming ethernet packet.
 * @param packet Pointer to the raw ethernet frame.
 * @param len Length of the frame in bytes.
 */
void net_rx_packet(uint8_t* packet, uint32_t len);

// Helpers

/**
 * Computes the 16-bit internet checksum over a buffer.
 * @param buf Pointer to the data.
 * @param len Length of the data in bytes.
 * @return Computed checksum.
 */
uint16_t net_checksum(const void *buf, uint32_t len);

/**
 * Converts a 16-bit value from host to network byte order.
 * @param v 16-bit host value.
 * @return 16-bit network value.
 */
uint16_t htons(uint16_t v);

/**
 * Converts a 16-bit value from network to host byte order.
 * @param v 16-bit network value.
 * @return 16-bit host value.
 */
uint16_t ntohs(uint16_t v);

/**
 * Converts a 32-bit value from host to network byte order.
 * @param v 32-bit host value.
 * @return 32-bit network value.
 */
uint32_t htonl(uint32_t v);

/**
 * Converts a 32-bit value from network to host byte order.
 * @param v 32-bit network value.
 * @return 32-bit host value.
 */
uint32_t ntohl(uint32_t v);

// Socket API

/**
 * Creates a new socket Protocol Control Block (PCB).
 * @param protocol Protocol type (IP_PROTO_TCP or IP_PROTO_UDP).
 * @return Pointer to the allocated PCB, or NULL if no slots are available.
 */
struct socket_pcb* net_socket_create(int protocol);

/**
 * Initiates a connection on a socket to a remote host.
 * @param pcb Pointer to the socket PCB.
 * @param ip Remote IP address in network byte order.
 * @param port Remote port number in host byte order.
 * @return 0 on success, -1 on failure.
 */
int net_socket_connect(struct socket_pcb* pcb, uint32_t ip, uint16_t port);

/**
 * Starts a connection without waiting for the handshake (F1.1/F1.3).
 * Sends the initial SYN and returns immediately; for UDP it just records the
 * peer (a UDP "connect" completes at once).  Completion is observed through
 * net_socket_ready(pcb, 1) / net_socket_error(pcb).
 * @return 0 on success (handshake in progress for TCP), -1 on error.
 */
int net_socket_connect_start(struct socket_pcb* pcb, uint32_t ip,
                             uint16_t port);

/**
 * Drives a pending TCP handshake: retransmits the SYN with bounded
 * exponential backoff (500/1000/2000/4000 ms) and, once the retry budget is
 * exhausted, fails the connect with ETIMEDOUT.  Idempotent; safe to call
 * from any poll path.  A lost SYN therefore does not hang the connect — a
 * completed handshake (handle_tcp) or a RST ends it sooner.
 */
void net_socket_tick(struct socket_pcb* pcb);

/**
 * Readiness probe for select()/poll-shaped callers (F1.2).
 * @param pcb        Socket PCB.
 * @param for_write  1 to test writability (POLLOUT), 0 for readability.
 * @return 1 ready, 0 not ready.
 *
 * Writability semantics: once a TCP handshake is ESTABLISHED the socket is
 * writable; a FAILED handshake also reports writable so a select() waiter
 * observes the error (the v1 stack has no send queue, so "writable" never
 * means "some N bytes will be accepted without blocking").
 */
int net_socket_ready(struct socket_pcb* pcb, int for_write);

/**
 * SO_ERROR value for a socket: 0 while healthy, else the errno of the
 * failure.  Drives the handshake bookkeeping first, so a timeout that is
 * already due is reported without waiting for another poll.
 */
int net_socket_error(struct socket_pcb* pcb);

/** O_NONBLOCK accessors (fcntl F_GETFL/F_SETFL). */
void net_socket_set_nonblock(struct socket_pcb* pcb, int on);
int net_socket_get_nonblock(struct socket_pcb* pcb);

/**
 * Bytes waiting in a socket's receive ring (0 when empty).  Unlike
 * file_read on a socket this never blocks.
 */
int net_socket_available(struct socket_pcb* pcb);

/**
 * Sends data over an established socket connection.
 * @param pcb Pointer to the socket PCB.
 * @param buf Pointer to the data to send.
 * @param len Length of the data to send in bytes.
 * @return Number of bytes sent, or -1 on error.
 */
int net_socket_send(struct socket_pcb* pcb, const void* buf, uint32_t len);

/**
 * Receives data from an established socket connection.
 * Blocks until data is available.
 * @param pcb Pointer to the socket PCB.
 * @param buf Pointer to the buffer to store received data.
 * @param len Maximum number of bytes to receive.
 * @return Number of bytes received, or -1 on error.
 */
int net_socket_recv(struct socket_pcb* pcb, void* buf, uint32_t len);

/**
 * @brief Receive data with a caller-provided timeout in milliseconds.
 *
 * Returns bytes received, 0 when the socket closed with no data pending,
 * or -1 on timeout.  Used by the NFS RPC client so a missing server cannot
 * stall a syscall for the fixed 5s of net_socket_recv().
 */
int net_socket_recv_timeout(struct socket_pcb* pcb, void* buf, uint32_t len, int timeout_ms);

/**
 * Closes a socket connection and frees the PCB.
 * @param pcb Pointer to the socket PCB to close.
 */
void net_socket_close(struct socket_pcb* pcb);

uint32_t net_get_netmask(void);
uint32_t net_get_gateway(void);
void net_get_mac(uint8_t mac[6]);

/* F1.7 (DHCP → DNS hand-off): the primary DNS server learned from DHCP,
 * network byte order; 0 when unknown. */
void net_set_dns(uint32_t dns);
uint32_t net_get_dns(void);

/* F1.4 entropy backing SYS_GETRANDOM: fill `buf` with `len` bytes from the
 * mixed kernel entropy source (cycle counter/TSC + timing jitter + RTC +
 * a startup seed).  Returns `len`. */
int net_get_random_bytes(void* buf, uint32_t len);

#endif // NET_H
