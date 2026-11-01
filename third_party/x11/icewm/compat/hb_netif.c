/* if_nameindex() family for the HobbyOS IceWM build (see ../include/net/if.h). */
#include <net/if.h>
#include <stdlib.h>

struct if_nameindex *if_nameindex(void) {
  /* Terminator only: { 0, NULL }. */
  return (struct if_nameindex *)calloc(1, sizeof(struct if_nameindex));
}
void if_freenameindex(struct if_nameindex *p) { free(p); }
unsigned int if_nametoindex(const char *n) { (void)n; return 0; }
