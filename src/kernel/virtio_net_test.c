#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "virtio_net.h"
#include <stdint.h>

extern void uart_puts(const char *s);

void virtio_net_test(void) {
    uint8_t mac[6];
    virtio_net_get_mac(mac);
    
    // Simple check: MAC address shouldn't be all zeros if it was read correctly
    int all_zeros = 1;
    for(int i=0; i<6; i++) {
        if(mac[i] != 0) all_zeros = 0;
    }
    
    if (all_zeros) {
        ASSERT(0);
        return;
    }
    
    ASSERT(1);
}

void virtio_net_test_suite(void) {
    uart_puts("Running virtio_net tests...\n");
    if (!virtio_net_is_active()) {
        uart_puts("  VirtIO network device is not active/present - bypassing test.\n");
        return;
    }
    virtio_net_test();
}

#endif
