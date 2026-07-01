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
    return ((ip & 0xFF) << 24) |
           ((ip & 0xFF00) << 8) |
           ((ip & 0xFF0000) >> 8) |
           ((ip & 0xFF000000) >> 24);
}

static int atoi(const char *s) {
    int res = 0;
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res;
}

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));
    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);
    if (argc < 2) {
        print("Usage: nc destination_ip port\n");
        return 1;
    }
    uint32_t ip = parse_ip(argv[0]);
    int port = atoi(argv[1]);
    
    int fd = connect(ip, port, 17);
    if (fd < 0) {
        print("nc: failed to connect\n");
        return 1;
    }
    
    char stdin_buf[1024];
    int stdin_col = 0;
    while (1) {
        if (available(0)) {
            char c;
            if (read(0, &c, 1) > 0) {
                write(1, &c, 1);
                if (c == '\n' || c == '\r') {
                    stdin_buf[stdin_col++] = '\n';
                    stdin_buf[stdin_col] = '\0';
                    write(fd, stdin_buf, stdin_col);
                    stdin_col = 0;
                } else {
                    if (stdin_col < (int)sizeof(stdin_buf) - 2) {
                        stdin_buf[stdin_col++] = c;
                    }
                }
            }
        }
        
        if (available(fd)) {
            char net_buf[1024];
            int n = read(fd, net_buf, sizeof(net_buf));
            if (n > 0) {
                write(1, net_buf, n);
            }
        }
        
        sleep(10);
    }
    
    close(fd);
    return 0;
}
