#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>

#define MAC_ADDR_LEN 6

struct virtio_net_config {
    uint8_t mac[MAC_ADDR_LEN];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} __attribute__((packed));

/**
 * Initializes the VirtIO network device.
 * Probes the MMIO address space for the virtio-net device, negotiates
 * features (including MAC address), and sets up the RX and TX virtqueues.
 * @return 0 on successful initialization, -1 if no device is found or configuration fails.
 */
int virtio_net_init(void);

/**
 * Gets the locally assigned MAC address of the network interface.
 * @param mac Pointer to a 6-byte array where the MAC address will be stored.
 */
void virtio_net_get_mac(uint8_t *mac);

/**
 * Sends an Ethernet frame via the virtio network interface.
 * @param buf Pointer to the contiguous buffer containing the ethernet frame.
 * @param len Length of the frame in bytes.
 * @return 0 on successful transmission, -1 if the device is uninitialized.
 */
int virtio_net_send(const void *buf, uint32_t len);

/**
 * Handles VirtIO network interrupts from the Generic Interrupt Controller.
 * Acknowledges the interrupt and processes any received packets in the RX queue.
 */
void virtio_net_handle_irq(void);

#endif // VIRTIO_NET_H
