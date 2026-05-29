#include "net.h"
#include "virtio_net.h"
#include "lock.h"
#include "timer.h"
#include "process.h"

extern void uart_puts(const char* s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

static uint32_t local_ip = 0;
static uint32_t local_netmask = 0;
static uint32_t local_gateway = 0;
static uint8_t local_mac[6];

static struct socket_pcb sockets[MAX_SOCKETS];
static spinlock_t net_lock;

// Simple ARP cache
struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    int valid;
};
#define ARP_CACHE_SIZE 16
static struct arp_entry arp_cache[ARP_CACHE_SIZE];

uint16_t htons(uint16_t v) {
    return (v >> 8) | (v << 8);
}
uint16_t ntohs(uint16_t v) {
    return htons(v);
}
uint32_t htonl(uint32_t v) {
    return ((v & 0xff) << 24) |
           ((v & 0xff00) << 8) |
           ((v & 0xff0000) >> 8) |
           ((v & 0xff000000) >> 24);
}
uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

uint16_t net_checksum(const void *buf, uint32_t len) {
    const uint16_t *ptr = buf;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t *)ptr;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}

void net_init(void) {
    // Initialize global network spinlock to serialize accesses to the socket table
    spinlock_init(&net_lock);
    
    // Clear all socket Control Blocks
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sockets[i].in_use = 0;
    }
    
    // Invalidate the ARP cache entries
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
    }
    
    // Cache the device's hardware MAC address
    virtio_net_get_mac(local_mac);
}

void net_refresh_mac(void) {
    virtio_net_get_mac(local_mac);
}

void net_set_ip(uint32_t ip, uint32_t netmask, uint32_t gateway) {
    local_ip = ip;
    local_netmask = netmask;
    local_gateway = gateway;
    uart_puts("Network configured: IP ");
    
    uint32_t nip = ntohl(ip);
    print_int((nip >> 24) & 0xFF); uart_puts(".");
    print_int((nip >> 16) & 0xFF); uart_puts(".");
    print_int((nip >> 8) & 0xFF); uart_puts(".");
    print_int(nip & 0xFF);
    
    uart_puts("\n");
}

uint32_t net_get_ip(void) {
    return local_ip;
}

static void arp_cache_update(uint32_t ip, const uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j=0; j<6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            for (int j=0; j<6; j++) arp_cache[i].mac[j] = mac[j];
            arp_cache[i].valid = 1;
            return;
        }
    }
}

void net_arp_request(uint32_t target_ip);

static int arp_resolve(uint32_t ip, uint8_t* mac_out) {
    if (ip == 0xFFFFFFFF) {
        for (int i=0; i<6; i++) mac_out[i] = 0xFF;
        return 1;
    }
    
    // Route off-subnet traffic to the gateway
    if (local_ip != 0 && local_netmask != 0 && local_gateway != 0) {
        if ((ip & local_netmask) != (local_ip & local_netmask)) {
            ip = local_gateway;
        }
    }
    
    // Fast path: check if already in cache
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j=0; j<6; j++) mac_out[j] = arp_cache[i].mac[j];
            return 1;
        }
    }
    
    // Slow path: send ARP request and wait for reply
    if (local_ip != 0) {
        for (int retry = 0; retry < 50; retry++) {
            net_arp_request(ip);
            
            // Wait up to 5ms for RX interrupt to process the reply
            uint64_t start_wait = timer_get_ms();
            while (timer_get_ms() - start_wait < 5) {
                safe_wfi();
            }
            
            // Check cache again
            for (int i = 0; i < ARP_CACHE_SIZE; i++) {
                if (arp_cache[i].valid && arp_cache[i].ip == ip) {
                    for (int j=0; j<6; j++) mac_out[j] = arp_cache[i].mac[j];
                    return 1;
                }
            }
        }
    }
    
    return 0;
}

static void handle_arp(uint8_t* packet, uint32_t len) {
    if (len < sizeof(struct eth_hdr) + sizeof(struct arp_hdr)) return;
    struct arp_hdr* arp = (struct arp_hdr*)(packet + sizeof(struct eth_hdr));
    
    uart_puts("[NET ARP] handle_arp: opcode=");
    print_int(ntohs(arp->opcode));
    uart_puts(" sender_ip=");
    uart_print_hex(arp->sender_ip);
    uart_puts(" target_ip=");
    uart_print_hex(arp->target_ip);
    uart_puts(" local_ip=");
    uart_print_hex(local_ip);
    uart_puts("\n");

    if (ntohs(arp->hw_type) == 1 && ntohs(arp->proto_type) == ETH_TYPE_IPV4) {
        arp_cache_update(arp->sender_ip, arp->sender_mac);
        
        if (ntohs(arp->opcode) == 1 && arp->target_ip == local_ip) { // Request for us
            uint8_t reply[sizeof(struct eth_hdr) + sizeof(struct arp_hdr)];
            struct eth_hdr* eth_out = (struct eth_hdr*)reply;
            struct arp_hdr* arp_out = (struct arp_hdr*)(reply + sizeof(struct eth_hdr));
            
            for (int i=0; i<6; i++) {
                eth_out->dst_mac[i] = arp->sender_mac[i];
                eth_out->src_mac[i] = local_mac[i];
                arp_out->target_mac[i] = arp->sender_mac[i];
                arp_out->sender_mac[i] = local_mac[i];
            }
            eth_out->type = htons(ETH_TYPE_ARP);
            
            arp_out->hw_type = htons(1); // Hardware Type: 1 for Ethernet
            arp_out->proto_type = htons(ETH_TYPE_IPV4); // Protocol Type: IPv4
            arp_out->hw_len = 6; // Hardware Address Length: 6 bytes (MAC)
            arp_out->proto_len = 4; // Protocol Address Length: 4 bytes (IPv4)
            arp_out->opcode = htons(2); // Opcode: 2 for ARP Reply
            arp_out->sender_ip = local_ip;
            arp_out->target_ip = arp->sender_ip;
            
            virtio_net_send(reply, sizeof(reply));
        }
    }
}

// Forward decl for DHCP
extern void dhcp_rx(uint8_t* packet, uint32_t len);

static void handle_udp(struct ipv4_hdr* ip, uint8_t* packet, uint32_t len) {
    if (len < sizeof(struct udp_hdr)) return;
    struct udp_hdr* udp = (struct udp_hdr*)packet;

    uart_puts("[NET UDP] handle_udp: dst_port=");
    print_int(ntohs(udp->dst_port));
    uart_puts(" len=");
    print_int(len);
    uart_puts("\n");
    
    if (ntohs(udp->dst_port) == 68) { // DHCP Client
        dhcp_rx((uint8_t*)ip, len + sizeof(struct ipv4_hdr));
        return;
    }
    
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].protocol == IP_PROTO_UDP && sockets[i].local_port == ntohs(udp->dst_port)) {
            // Append to socket rx buffer
            uint32_t data_len = ntohs(udp->length) - sizeof(struct udp_hdr);
            uint8_t* data = packet + sizeof(struct udp_hdr);
            for (uint32_t j = 0; j < data_len; j++) {
                sockets[i].rx_buf[sockets[i].rx_tail % 2048] = data[j];
                sockets[i].rx_tail++;
            }
            break;
        }
    }
    spinlock_release_irqrestore(&net_lock, flags);
}

static void handle_tcp(struct ipv4_hdr* ip, uint8_t* packet, uint32_t len) {
    if (len < sizeof(struct tcp_hdr)) return;
    struct tcp_hdr* tcp = (struct tcp_hdr*)packet;
    
    // Lock the socket table to securely process the incoming TCP segment
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].protocol == IP_PROTO_TCP &&
            sockets[i].local_port == ntohs(tcp->dst_port) &&
            sockets[i].remote_port == ntohs(tcp->src_port) &&
            sockets[i].remote_ip == ip->src_ip) {
            
            // Advance TCP state machine
            if (sockets[i].state == SOCKET_SYN_SENT && tcp->syn && tcp->ack_flag) {
                // Connection established, acknowledge the server's SYN
                sockets[i].state = SOCKET_ESTABLISHED;
                sockets[i].ack = ntohl(tcp->seq) + 1;
                sockets[i].seq = ntohl(tcp->ack);
            } else if (sockets[i].state == SOCKET_ESTABLISHED) {
                // Process incoming payload and push to socket's ring buffer
                uint32_t data_offset = tcp->data_offset * 4;
                if (data_offset < len) {
                    uint32_t data_len = len - data_offset;
                    uint8_t* data = packet + data_offset;
                    for (uint32_t j = 0; j < data_len; j++) {
                        sockets[i].rx_buf[sockets[i].rx_tail % 2048] = data[j];
                        sockets[i].rx_tail++;
                    }
                    sockets[i].ack += data_len;
                }
                
                // Handle remote FIN segment indicating connection closure
                if (tcp->fin) {
                    sockets[i].ack++;
                    sockets[i].state = SOCKET_CLOSED;
                }
            }
            break;
        }
    }
    spinlock_release_irqrestore(&net_lock, flags);
}

static void handle_icmp(struct ipv4_hdr* ip, uint8_t* packet, uint32_t len) {
    if (len < sizeof(struct icmp_hdr)) return;
    struct icmp_hdr* icmp = (struct icmp_hdr*)packet;
    
    if (icmp->type == 8 && icmp->code == 0) { // Echo request
        // Send Echo reply
        uint8_t reply[1500];
        uint32_t reply_len = sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + len;
        
        struct eth_hdr* eth_out = (struct eth_hdr*)reply;
        struct ipv4_hdr* ip_out = (struct ipv4_hdr*)(reply + sizeof(struct eth_hdr));
        struct icmp_hdr* icmp_out = (struct icmp_hdr*)(reply + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr));
        
        uint8_t dest_mac[6];
        if (!arp_resolve(ip->src_ip, dest_mac)) return; // Don't reply if we don't know MAC
        
        for(int i=0; i<6; i++) {
            eth_out->dst_mac[i] = dest_mac[i];
            eth_out->src_mac[i] = local_mac[i];
        }
        eth_out->type = htons(ETH_TYPE_IPV4);
        
        ip_out->version = 4; // IPv4
        ip_out->ihl = 5; // Internet Header Length: 5 32-bit words (20 bytes, no options)
        ip_out->tos = 0; // Type of Service: routine traffic
        ip_out->total_len = htons(sizeof(struct ipv4_hdr) + len);
        ip_out->id = 0; // Identification: 0 since we don't fragment
        ip_out->frag_off = 0; // Fragment Offset: 0 (no fragmentation)
        ip_out->ttl = 64; // Time to Live: standard default value
        ip_out->protocol = IP_PROTO_ICMP; // Next level protocol: ICMP
        ip_out->checksum = 0; // Initial checksum is 0 for calculation
        ip_out->src_ip = local_ip;
        ip_out->dst_ip = ip->src_ip;
        ip_out->checksum = net_checksum(ip_out, sizeof(struct ipv4_hdr));
        
        for (uint32_t i = 0; i < len; i++) {
            ((uint8_t*)icmp_out)[i] = packet[i];
        }
        icmp_out->type = 0; // Echo reply
        icmp_out->checksum = 0;
        icmp_out->checksum = net_checksum(icmp_out, len);
        
        virtio_net_send(reply, reply_len);
    }
}

void net_rx_packet(uint8_t* packet, uint32_t len) {
    if (len < sizeof(struct eth_hdr)) return;
    struct eth_hdr* eth = (struct eth_hdr*)packet;

    uart_puts("[NET RX] Packet received, len=");
    print_int(len);
    uart_puts(" type=");
    uart_print_hex(ntohs(eth->type));
    uart_puts("\n");
    
    // Demultiplex incoming Ethernet frames based on EtherType
    if (ntohs(eth->type) == ETH_TYPE_ARP) {
        handle_arp(packet, len);
    } else if (ntohs(eth->type) == ETH_TYPE_IPV4) {
        if (len < sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr)) return;
        struct ipv4_hdr* ip = (struct ipv4_hdr*)(packet + sizeof(struct eth_hdr));
        
        // Discard packets not destined for us or broadcast
        if (ip->dst_ip != local_ip && ip->dst_ip != 0xFFFFFFFF) return;
        
        // Demultiplex incoming IPv4 datagrams by protocol
        if (ip->protocol == IP_PROTO_ICMP) {
            handle_icmp(ip, packet + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr), len - sizeof(struct eth_hdr) - sizeof(struct ipv4_hdr));
        } else if (ip->protocol == IP_PROTO_UDP) {
            handle_udp(ip, packet + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr), len - sizeof(struct eth_hdr) - sizeof(struct ipv4_hdr));
        } else if (ip->protocol == IP_PROTO_TCP) {
            handle_tcp(ip, packet + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr), len - sizeof(struct eth_hdr) - sizeof(struct ipv4_hdr));
        }
    }
}

struct socket_pcb* net_socket_create(int protocol) {
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            sockets[i].in_use = 1;
            sockets[i].protocol = protocol;
            sockets[i].local_ip = local_ip;
            
            // Allocate an ephemeral port
            static uint16_t next_port = 49152;
            sockets[i].local_port = next_port++;
            if (next_port == 0) next_port = 49152;
            
            sockets[i].state = SOCKET_CLOSED;
            sockets[i].rx_head = 0;
            sockets[i].rx_tail = 0;
            sockets[i].seq = 1000; // Random ISN
            sockets[i].ack = 0;
            spinlock_release_irqrestore(&net_lock, flags);
            return &sockets[i];
        }
    }
    spinlock_release_irqrestore(&net_lock, flags);
    return NULL;
}

// Pseudo header for TCP/UDP checksum
struct pseudo_hdr {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t zero;
    uint8_t protocol;
    uint16_t length;
} __attribute__((packed));

static uint16_t tcp_udp_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const uint8_t* payload, uint16_t len) {
    struct pseudo_hdr phdr;
    phdr.src_ip = src_ip;
    phdr.dst_ip = dst_ip;
    phdr.zero = 0;
    phdr.protocol = proto;
    phdr.length = htons(len);
    
    uint32_t sum = 0;
    const uint16_t* ptr = (const uint16_t*)&phdr;
    for (int i = 0; i < sizeof(struct pseudo_hdr)/2; i++) {
        sum += ptr[i];
    }
    ptr = (const uint16_t*)payload;
    int remaining = len;
    while (remaining > 1) {
        sum += *ptr++;
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += *(const uint8_t*)ptr;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}

static void send_tcp_segment(struct socket_pcb* pcb, uint8_t flags, const void* data, uint16_t data_len) {
    uint8_t packet[1500];
    uint32_t total_len = sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + data_len;
    
    struct eth_hdr* eth = (struct eth_hdr*)packet;
    struct ipv4_hdr* ip = (struct ipv4_hdr*)(packet + sizeof(struct eth_hdr));
    struct tcp_hdr* tcp = (struct tcp_hdr*)(packet + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr));
    uint8_t* payload = packet + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);
    
    uint8_t dest_mac[6];
    if (!arp_resolve(pcb->remote_ip, dest_mac)) {
        // Here we could send an ARP request and wait, but for simple stack:
        // Let's assume gateway MAC if outside subnet, or send broadcast ARP and fail this packet.
        // For simplicity, we just broadcast if unknown, or rely on a ping first.
        for(int i=0; i<6; i++) dest_mac[i] = 0xFF; // Broadcast fallback
    }
    
    for (int i=0; i<6; i++) {
        eth->dst_mac[i] = dest_mac[i];
        eth->src_mac[i] = local_mac[i];
    }
    eth->type = htons(ETH_TYPE_IPV4);
    
    ip->version = 4; // IPv4
    ip->ihl = 5; // Internet Header Length: 5 32-bit words (20 bytes, no options)
    ip->tos = 0; // Type of Service: routine traffic
    ip->total_len = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + data_len);
    ip->id = 0; // Identification: 0 since we don't fragment
    ip->frag_off = 0; // Fragment Offset: 0 (no fragmentation)
    ip->ttl = 64; // Time to Live: standard default value
    ip->protocol = IP_PROTO_TCP; // Next level protocol: TCP
    ip->checksum = 0; // Initial checksum is 0 for calculation
    ip->src_ip = pcb->local_ip;
    ip->dst_ip = pcb->remote_ip;
    ip->checksum = net_checksum(ip, sizeof(struct ipv4_hdr));
    
    tcp->src_port = htons(pcb->local_port);
    tcp->dst_port = htons(pcb->remote_port);
    tcp->seq = htonl(pcb->seq);
    tcp->ack = htonl(pcb->ack);
    tcp->data_offset = 5; // TCP header size: 5 32-bit words (20 bytes, no options)
    tcp->reserved = 0;
    tcp->ns = 0; // Nonce Sum flag (ECN)
    tcp->fin = (flags & 0x01) ? 1 : 0;
    tcp->syn = (flags & 0x02) ? 1 : 0;
    tcp->rst = (flags & 0x04) ? 1 : 0;
    tcp->psh = (flags & 0x08) ? 1 : 0;
    tcp->ack_flag = (flags & 0x10) ? 1 : 0;
    tcp->urg = 0; // Urgent pointer not used
    tcp->ece = 0; // ECN-Echo not used
    tcp->cwr = 0; // Congestion Window Reduced not used
    tcp->window_size = htons(2048); // Default receive window size (2048 bytes)
    tcp->checksum = 0; // Initial checksum is 0 for calculation
    tcp->urgent_ptr = 0;
    
    if (data_len > 0) {
        for (int i=0; i<data_len; i++) payload[i] = ((const uint8_t*)data)[i];
    }
    
    tcp->checksum = tcp_udp_checksum(pcb->local_ip, pcb->remote_ip, IP_PROTO_TCP, (uint8_t*)tcp, sizeof(struct tcp_hdr) + data_len);
    
    virtio_net_send(packet, total_len);
    
    if (flags & 0x02) pcb->seq++; // SYN consumes sequence
    if (flags & 0x01) pcb->seq++; // FIN consumes sequence
    pcb->seq += data_len;
}

int net_socket_connect(struct socket_pcb* pcb, uint32_t ip, uint16_t port) {
    if (!pcb || pcb->protocol != IP_PROTO_TCP) return -1;
    pcb->remote_ip = ip;
    pcb->remote_port = port;
    pcb->state = SOCKET_SYN_SENT;
    
    // Initiate TCP three-way handshake by sending a SYN segment
    send_tcp_segment(pcb, 0x02, NULL, 0);
    
    // Block the calling process until the server acknowledges with SYN-ACK
    uint64_t start = timer_get_ms();
    while (pcb->state == SOCKET_SYN_SENT) {
        if (timer_get_ms() - start > 5000) {
            uart_puts("[NET] Error: Connection timeout to IP ");
            uint32_t nip = ntohl(ip);
            print_int((nip >> 24) & 0xFF); uart_puts(".");
            print_int((nip >> 16) & 0xFF); uart_puts(".");
            print_int((nip >> 8) & 0xFF); uart_puts(".");
            print_int(nip & 0xFF);
            uart_puts(" port "); print_int(port); uart_puts("\n");
            
            pcb->state = SOCKET_CLOSED;
            return -1;
        }
        safe_wfi();
    }
    
    if (pcb->state == SOCKET_ESTABLISHED) {
        // Complete the handshake by sending an ACK segment
        send_tcp_segment(pcb, 0x10, NULL, 0);
        return 0;
    }
    return -1;
}

int net_socket_send(struct socket_pcb* pcb, const void* buf, uint32_t len) {
    if (!pcb) return -1;
    if (pcb->protocol == IP_PROTO_TCP) {
        if (pcb->state != SOCKET_ESTABLISHED) return -1;
        // Transmit TCP payload with PUSH and ACK flags set
        send_tcp_segment(pcb, 0x18, buf, len); // PSH | ACK
        return len;
    } else if (pcb->protocol == IP_PROTO_UDP) {
        // Construct a raw UDP datagram for connectionless transmission
        uint8_t packet[1500];
        uint32_t total_len = sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + len;
        
        struct eth_hdr* eth = (struct eth_hdr*)packet;
        struct ipv4_hdr* ip = (struct ipv4_hdr*)(packet + sizeof(struct eth_hdr));
        struct udp_hdr* udp = (struct udp_hdr*)(packet + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr));
        uint8_t* payload = packet + sizeof(struct eth_hdr) + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);
        
        uint8_t dest_mac[6];
        if (!arp_resolve(pcb->remote_ip, dest_mac)) {
            for(int i=0; i<6; i++) dest_mac[i] = 0xFF;
        }
        
        for (int i=0; i<6; i++) {
            eth->dst_mac[i] = dest_mac[i];
            eth->src_mac[i] = local_mac[i];
        }
        eth->type = htons(ETH_TYPE_IPV4);
        
        ip->version = 4; // IPv4
        ip->ihl = 5; // Internet Header Length: 5 32-bit words (20 bytes, no options)
        ip->tos = 0; // Type of Service: routine traffic
        ip->total_len = htons(sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + len);
        ip->id = 0; // Identification: 0 since we don't fragment
        ip->frag_off = 0; // Fragment Offset: 0 (no fragmentation)
        ip->ttl = 64; // Time to Live: standard default value
        ip->protocol = IP_PROTO_UDP; // Next level protocol: UDP
        ip->checksum = 0; // Initial checksum is 0 for calculation
        ip->src_ip = pcb->local_ip;
        ip->dst_ip = pcb->remote_ip;
        ip->checksum = net_checksum(ip, sizeof(struct ipv4_hdr));
        
        udp->src_port = htons(pcb->local_port);
        udp->dst_port = htons(pcb->remote_port);
        udp->length = htons(sizeof(struct udp_hdr) + len);
        udp->checksum = 0;
        
        for (uint32_t i=0; i<len; i++) payload[i] = ((const uint8_t*)buf)[i];
        
        // Compute the UDP pseudo-header checksum required by IPv4 standards
        udp->checksum = tcp_udp_checksum(pcb->local_ip, pcb->remote_ip, IP_PROTO_UDP, (uint8_t*)udp, sizeof(struct udp_hdr) + len);
        if (udp->checksum == 0) udp->checksum = 0xFFFF;
        
        virtio_net_send(packet, total_len);
        return len;
    }
    return -1;
}

int net_socket_recv(struct socket_pcb* pcb, void* buf, uint32_t len) {
    if (!pcb) return -1;
    
    // Suspend execution until data arrives in the socket's internal ring buffer
    uint64_t start = timer_get_ms();
    while (pcb->rx_head == pcb->rx_tail && pcb->state != SOCKET_CLOSED) {
        if (timer_get_ms() - start > 5000) {
            uart_puts("[NET] Error: Recv timeout. No data received.\n");
            return -1;
        }
        safe_wfi();
    }
    
    // Atomically read the available payload to prevent race conditions during interrupt delivery
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    uint32_t avail = pcb->rx_tail - pcb->rx_head;
    if (avail == 0) {
        spinlock_release_irqrestore(&net_lock, flags);
        return 0; // EOF
    }
    
    uint32_t to_read = len < avail ? len : avail;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < to_read; i++) {
        out[i] = pcb->rx_buf[pcb->rx_head % 2048];
        pcb->rx_head++;
    }
    spinlock_release_irqrestore(&net_lock, flags);
    return to_read;
}

void net_socket_close(struct socket_pcb* pcb) {
    if (!pcb) return;
    if (pcb->protocol == IP_PROTO_TCP && pcb->state == SOCKET_ESTABLISHED) {
        pcb->state = SOCKET_FIN_WAIT;
        send_tcp_segment(pcb, 0x11, NULL, 0); // FIN | ACK
        // Wait for close... simple stack just clears it after a while or immediately
    }
    
    uint64_t flags = spinlock_acquire_irqsave(&net_lock);
    pcb->in_use = 0;
    spinlock_release_irqrestore(&net_lock, flags);
}

// Simple ARP broadcast request helper for DHCP
void net_arp_request(uint32_t target_ip) {
    uint8_t packet[sizeof(struct eth_hdr) + sizeof(struct arp_hdr)];
    struct eth_hdr* eth = (struct eth_hdr*)packet;
    struct arp_hdr* arp = (struct arp_hdr*)(packet + sizeof(struct eth_hdr));
    
    for (int i=0; i<6; i++) {
        eth->dst_mac[i] = 0xFF;
        eth->src_mac[i] = local_mac[i];
    }
    eth->type = htons(ETH_TYPE_ARP);
    
    arp->hw_type = htons(1); // Hardware Type: 1 for Ethernet
    arp->proto_type = htons(ETH_TYPE_IPV4); // Protocol Type: IPv4
    arp->hw_len = 6; // Hardware Address Length: 6 bytes (MAC)
    arp->proto_len = 4; // Protocol Address Length: 4 bytes (IPv4)
    arp->opcode = htons(1); // Opcode: 1 for ARP Request
    for (int i=0; i<6; i++) {
        arp->sender_mac[i] = local_mac[i];
        arp->target_mac[i] = 0;
    }
    arp->sender_ip = local_ip;
    arp->target_ip = target_ip;
    
    virtio_net_send(packet, sizeof(packet));
}
