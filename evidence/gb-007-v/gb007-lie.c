#include <unistd.h>
/* GB-007-V: a deliberately wrong shim for one syscall, so leg A lies.
   Linked into leg A only, via the musl-gcc wrapper below. */
int lie_close(int fd) { (void)fd; return 0; }
