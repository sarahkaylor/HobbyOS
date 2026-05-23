#include "dhcp.h"
#include "net.h"
#include "virtio_net.h"
#include "timer.h"
#include "lock.h"

extern void uart_puts(const char* s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);
extern void net_arp_request(uint32_t target_ip);

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

struct dhcp_packet {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic_cookie;
    uint8_t options[308];
} __attribute__((packed));

static uint32_t dhcp_xid = 0x12345678;
static int dhcp_state = 0; // 0 = INIT, 1 = REQUESTING, 2 = BOUND

static uint32_t offered_ip = 0;
static uint32_t server_ip = 0;

static void send_dhcp_discover(void) {
    uint8_t buf[sizeof(struct dhcp_packet)];
    for(int i=0; i<sizeof(buf); i++) buf[i] = 0;
    
    // Construct the DHCP Discover packet body
    // Sets the hardware type, lengths, and a randomly assigned transaction ID
    struct dhcp_packet* dhcp = (struct dhcp_packet*)buf;
    dhcp->op = 1; // BootRequest
    dhcp->htype = 1; // Ethernet
    dhcp->hlen = 6;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->flags = htons(0x8000); // Broadcast flag
    
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    for(int i=0; i<6; i++) dhcp->chaddr[i] = mac[i];
    
    dhcp->magic_cookie = htonl(0x63825363); // Standard DHCP Magic Cookie
    
    // Append standard DHCP options required for a Discover message
    int opt = 0;
    dhcp->options[opt++] = 53; // Option 53: DHCP Message Type
    dhcp->options[opt++] = 1;  // Length of option: 1 byte
    dhcp->options[opt++] = 1;  // Message Type: 1 (DHCP Discover)
    
    dhcp->options[opt++] = 255; // Option 255: End marker
    
    // Dispatch via a raw UDP broadcast socket
    struct socket_pcb* pcb = net_socket_create(IP_PROTO_UDP);
    pcb->local_port = DHCP_CLIENT_PORT;
    pcb->remote_port = DHCP_SERVER_PORT;
    pcb->local_ip = 0;
    pcb->remote_ip = 0xFFFFFFFF; // Broadcast
    
    net_socket_send(pcb, buf, sizeof(struct dhcp_packet));
    net_socket_close(pcb);
    
    uart_puts("DHCP Discover sent\n");
}

static void send_dhcp_request(void) {
    uint8_t buf[sizeof(struct dhcp_packet)];
    for(int i=0; i<sizeof(buf); i++) buf[i] = 0;
    
    struct dhcp_packet* dhcp = (struct dhcp_packet*)buf;
    dhcp->op = 1; // BootRequest (Client to Server)
    dhcp->htype = 1; // Hardware Type: Ethernet
    dhcp->hlen = 6; // Hardware Address Length: 6 bytes for MAC
    dhcp->xid = htonl(dhcp_xid); // Transaction ID
    dhcp->flags = htons(0x8000); // Broadcast flag: tell server to reply via broadcast
    
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    for(int i=0; i<6; i++) dhcp->chaddr[i] = mac[i];
    
    dhcp->magic_cookie = htonl(0x63825363); // Standard DHCP Magic Cookie
    
    int opt = 0;
    dhcp->options[opt++] = 53; // Option 53: DHCP Message Type
    dhcp->options[opt++] = 1;  // Length of option: 1 byte
    dhcp->options[opt++] = 3;  // Message Type: 3 (DHCP Request)
    
    dhcp->options[opt++] = 50; // Option 50: Requested IP Address
    dhcp->options[opt++] = 4;  // Length: 4 bytes (IPv4)
    uint32_t n_ip = offered_ip;
    dhcp->options[opt++] = n_ip & 0xFF;
    dhcp->options[opt++] = (n_ip >> 8) & 0xFF;
    dhcp->options[opt++] = (n_ip >> 16) & 0xFF;
    dhcp->options[opt++] = (n_ip >> 24) & 0xFF;
    
    dhcp->options[opt++] = 54; // Option 54: Server Identifier
    dhcp->options[opt++] = 4;  // Length: 4 bytes (IPv4)
    uint32_t s_ip = server_ip;
    dhcp->options[opt++] = s_ip & 0xFF;
    dhcp->options[opt++] = (s_ip >> 8) & 0xFF;
    dhcp->options[opt++] = (s_ip >> 16) & 0xFF;
    dhcp->options[opt++] = (s_ip >> 24) & 0xFF;
    
    dhcp->options[opt++] = 255; // Option 255: End marker
    
    struct socket_pcb* pcb = net_socket_create(IP_PROTO_UDP);
    pcb->local_port = DHCP_CLIENT_PORT;
    pcb->remote_port = DHCP_SERVER_PORT;
    pcb->local_ip = 0;
    pcb->remote_ip = 0xFFFFFFFF; // Broadcast
    
    net_socket_send(pcb, buf, sizeof(struct dhcp_packet));
    net_socket_close(pcb);
    
    uart_puts("DHCP Request sent\n");
}

void dhcp_rx(uint8_t* packet, uint32_t len) {
    if (len < sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + 240) return; // Minimal DHCP
    struct ipv4_hdr* ip = (struct ipv4_hdr*)packet;
    struct udp_hdr* udp = (struct udp_hdr*)(packet + sizeof(struct ipv4_hdr));
    struct dhcp_packet* dhcp = (struct dhcp_packet*)(packet + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr));
    
    // Process only DHCP replies matching our active transaction ID
    if (dhcp->op == 2 /* BootReply */ && ntohl(dhcp->xid) == dhcp_xid) {
        uint8_t msg_type = 0;
        uint32_t netmask = 0;
        uint32_t router = 0;
        
        // Parse DHCP options payload securely
        int opt = 0;
        while (opt < 308 && dhcp->options[opt] != 255 /* Option 255: End */) {
            uint8_t code = dhcp->options[opt++];
            if (code == 0 /* Option 0: Pad */) continue;
            uint8_t length = dhcp->options[opt++];
            
            if (code == 53) { // Option 53: DHCP Message Type
                msg_type = dhcp->options[opt];
            } else if (code == 1) { // Option 1: Subnet Mask
                netmask = dhcp->options[opt] | (dhcp->options[opt+1] << 8) | (dhcp->options[opt+2] << 16) | (dhcp->options[opt+3] << 24);
            } else if (code == 3) { // Option 3: Router
                router = dhcp->options[opt] | (dhcp->options[opt+1] << 8) | (dhcp->options[opt+2] << 16) | (dhcp->options[opt+3] << 24);
            } else if (code == 54) { // Option 54: Server Identifier
                server_ip = dhcp->options[opt] | (dhcp->options[opt+1] << 8) | (dhcp->options[opt+2] << 16) | (dhcp->options[opt+3] << 24);
            }
            opt += length;
        }
        
        // Handle state machine transitions (Offer -> Request -> Ack)
        if (msg_type == 2) { // Offer
            offered_ip = dhcp->yiaddr;
            uart_puts("DHCP Offer received\n");
            dhcp_state = 1;
            send_dhcp_request();
        } else if (msg_type == 5) { // Ack
            uart_puts("DHCP Ack received\n");
            if (netmask == 0) netmask = 0x00FFFFFF;
            if (router == 0) router = ip->src_ip;
            
            // Finalize network configuration properties and prime the ARP cache
            net_set_ip(dhcp->yiaddr, netmask, router);
            net_arp_request(router); // ARPing the router proactively
            dhcp_state = 2;
        }
    }
}

void dhcp_init(void) {
#ifdef __x86_64__
    uart_puts("Skipping DHCP on x86_64 (using static IP configuration).\n");
    return;
#endif
    uart_puts("Initializing DHCP...\n");
    send_dhcp_discover();
    
    // Wait for DHCP to bind, with a 10-second timeout
    uint64_t start = timer_get_ms();
    while (dhcp_state != 2) {
        if (timer_get_ms() - start > 10000) {
            uart_puts("[DHCP] Error: Initialization timeout. No DHCP server found.\n");
            return;
        }
        safe_wfi();
    }
    uart_puts("DHCP initialization complete.\n");
}
