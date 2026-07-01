#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
    struct sys_netinfo net;
    int res = sysinfo(4, &net, sizeof(net));
    if (res < 0) {
        print("ifconfig: failed to retrieve network info\n");
        return 1;
    }
    
    print("eth0: flags=UP\n");
    print("        inet ");
    
    uint32_t ip = net.ip;
    print_dec(ip & 0xFF); print(".");
    print_dec((ip >> 8) & 0xFF); print(".");
    print_dec((ip >> 16) & 0xFF); print(".");
    print_dec((ip >> 24) & 0xFF);
    print("\n");
    
    print("        netmask ");
    uint32_t mask = net.subnet_mask;
    print_dec(mask & 0xFF); print(".");
    print_dec((mask >> 8) & 0xFF); print(".");
    print_dec((mask >> 16) & 0xFF); print(".");
    print_dec((mask >> 24) & 0xFF);
    print("\n");
    
    print("        gateway ");
    uint32_t gw = net.gateway;
    print_dec(gw & 0xFF); print(".");
    print_dec((gw >> 8) & 0xFF); print(".");
    print_dec((gw >> 16) & 0xFF); print(".");
    print_dec((gw >> 24) & 0xFF);
    print("\n");
    
    print("        ether ");
    for (int i = 0; i < 6; i++) {
        print_hex(net.mac[i]);
        if (i < 5) print(":");
    }
    print("\n");
    return 0;
}
