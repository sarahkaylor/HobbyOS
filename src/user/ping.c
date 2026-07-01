#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

static uint32_t parse_ip(const char *s) {
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t part = 0;
        while (*s >= '0' && *s <= '9') {
            part = part * 10 + (*s - '0');
            s++;
        }
        ip = (ip << 8) | (part & 0xFF);
        if (*s == '.') s++;
    }
    // Convert to network byte order
    return ((ip & 0xFF) << 24) |
           ((ip & 0xFF00) << 8) |
           ((ip & 0xFF0000) >> 8) |
           ((ip & 0xFF000000) >> 24);
}

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));
    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);
    if (argc < 1) {
        print("Usage: ping destination_ip\n");
        return 1;
    }
    uint32_t ip = parse_ip(argv[0]);
    print("PING ");
    print(argv[0]);
    print(" with 4 bytes of data:\n");
    
    int fd = connect(ip, 7, 17);
    if (fd < 0) {
        print("ping: failed to connect/create socket\n");
        return 1;
    }
    
    char send_buf[] = "PING";
    write(fd, send_buf, 4);
    
    int start_ms = sysinfo(1, 0, 0);
    int received = 0;
    while (sysinfo(1, 0, 0) - start_ms < 1000) {
        if (available(fd)) {
            char recv_buf[32];
            int n = read(fd, recv_buf, sizeof(recv_buf));
            if (n > 0) {
                int elapsed = sysinfo(1, 0, 0) - start_ms;
                print("Reply from ");
                print(argv[0]);
                print(": bytes=");
                print_dec(n);
                print(" time=");
                print_dec(elapsed);
                print("ms\n");
                received = 1;
                break;
            }
        }
        sleep(10);
    }
    
    if (!received) {
        print("Request timed out.\n");
    }
    close(fd);
    return 0;
}
