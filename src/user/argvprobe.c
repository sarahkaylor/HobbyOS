/* w6-run probe: print argc/argv as received by crt0, so the in-OS exec/arg
 * path can be verified against a known command line.  Built host-side like
 * any user bin (USER_CFLAGS + crt0.o + libc.a + linker.ld), staged as
 * /ARGVPROBE.BIN, driven through the SELFHOST.BIN driver (serial relay). */
#include "libc.h"

int main(int argc, char **argv) {
  char buf[16];
  print("[ARGVPROBE] argc=");
  print_dec(argc);
  print("\n");
  for (int i = 0; i < argc; i++) {
    print("[ARGVPROBE] argv[");
    print_dec(i);
    print("]=");
    print(argv[i]);
    print("\n");
  }
  print("[ARGVPROBE] done\n");
  return 0;
}
