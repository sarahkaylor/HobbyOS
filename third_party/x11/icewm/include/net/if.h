/* Lane-local <net/if.h> for the HobbyOS IceWM build: enough for IceWM's
 * network-status applet (apppstatus.cc) to compile.  HobbyOS exposes no
 * interface list through this API: if_nameindex() returns an empty list
 * (../compat/hb_netif.c), so the applet shows no devices. */
#ifndef HB_ICEWM_NET_IF_H
#define HB_ICEWM_NET_IF_H
#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif
#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING     0x40
struct ifreq {
  char ifr_name[IFNAMSIZ];
  union {
    short ifru_flags;
    int ifru_ivalue;
    char ifru_pad[24];
  } ifr_ifru;
};
#define ifr_flags ifr_ifru.ifru_flags
#define SIOCGIFFLAGS 0x8913
struct if_nameindex {
  unsigned int if_index;
  char *if_name;
};
struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *ptr);
unsigned int if_nametoindex(const char *ifname);
#ifdef __cplusplus
}
#endif
#endif
