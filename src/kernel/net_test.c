#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "net.h"

extern void uart_puts(const char *s);

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

void net_test_suite(void) {
    uart_puts("Running net stack tests...\n");
    net_test();
}

#endif
