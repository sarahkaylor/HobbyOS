#include "libc.h"
#include <stdint.h>

#define IP_PROTO_TCP 6

__attribute__((section(".text._start"))) void _start(void) {
    print("User net_test starting...\n");
    
    // Connect to 1.1.1.1 port 80 (HTTP)
    // 1.1.1.1 in network byte order: 0x01010101
    // Actually, simple stack: ntohl/htonl needs to be implemented or just use byte values
    // 1.1.1.1 is 0x01010101 (same endianness)
    uint32_t target_ip = 0x01010101; 
    uint16_t target_port = 80;
    
    print("Connecting to 1.1.1.1:80...\n");
    int fd = connect(target_ip, target_port, IP_PROTO_TCP);
    if (fd < 0) {
        print("net_test failed: connect returned error\n");
        sleep(3000);
        exit(-1);
    }
    
    print("Connected! Sending HTTP GET...\n");
    const char* request = "GET / HTTP/1.1\r\nHost: 1.1.1.1\r\nConnection: close\r\n\r\n";
    int req_len = 0;
    while (request[req_len]) req_len++;
    
    if (write(fd, request, req_len) < 0) {
        print("net_test failed: write error\n");
        sleep(3000);
        exit(-1);
    }
    
    print("Request sent. Reading response...\n");
    char buf[128];
    int read_bytes = read(fd, buf, sizeof(buf) - 1);
    
    if (read_bytes < 0) {
        print("net_test failed: read error\n");
        sleep(3000);
        exit(-1);
    }
    
    if (read_bytes == 0) {
        print("net_test failed: no data received\n");
        sleep(3000);
        exit(-1);
    }
    
    buf[read_bytes] = '\0';
    print("Received data:\n");
    print(buf);
    print("\n");
    
    close(fd);
    print("net_test success!\n");
    
    // Wait to be shut down or exit properly
    // Let's shut down qemu by returning success
    sleep(3000);
    exit(0);
}
