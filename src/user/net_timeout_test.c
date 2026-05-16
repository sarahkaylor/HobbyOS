#include "libc.h"
#include <stdint.h>

#define IP_PROTO_TCP 6

int main(void) {
    print("User net_timeout_test starting...\n");
    
    // Connect to 10.0.2.16 port 80
    // 10.0.2.16 is an unused IP in the QEMU SLIRP subnet.
    // It should cause a connection timeout.
    // 10 = 0x0A, 0 = 0x00, 2 = 0x02, 16 = 0x10
    // Little endian representation: 0x1002000A
    uint32_t target_ip = 0x1002000A; 
    uint16_t target_port = 80;
    
    print("Connecting to 10.0.2.16:80 (expecting timeout)...\n");
    int fd = connect(target_ip, target_port, IP_PROTO_TCP);
    if (fd < 0) {
        print("net_timeout_test success: connect timed out and returned error as expected.\n");
        exit(0);
    }
    
    print("net_timeout_test failed: connect unexpectedly succeeded!\n");
    close(fd);
    exit(-1);
}
