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
 * Closes a socket connection and frees the PCB.
 * @param pcb Pointer to the socket PCB to close.
 */
void net_socket_close(struct socket_pcb* pcb);

uint32_t net_get_netmask(void);
uint32_t net_get_gateway(void);
void net_get_mac(uint8_t mac[6]);

#endif // NET_H
