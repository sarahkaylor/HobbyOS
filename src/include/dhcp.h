#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

/**
 * Initializes the DHCP client.
 * Constructs and broadcasts a DHCP Discover packet to obtain an IP address,
 * subnet mask, and gateway router from the local network DHCP server.
 */
void dhcp_init(void);

#endif // DHCP_H
