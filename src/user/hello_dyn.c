/* src/user/hello_dyn.c — the dynamic-linking pilot (docs/shared-libs.md §5.5).
 *
 * The first native HobbyOS program that is NOT statically linked: it is an
 * ET_DYN executable whose PT_INTERP is /LDHOBBY.SO, and every libc call below
 * (print_console, malloc, strcpy, free, exit) is resolved out of LIBC.SO at
 * run time.  Markers go to the serial console via print_console so a headless
 * QEMU boot can assert them without a desktop.
 *
 * What each call proves:
 *   print_console  JUMP_SLOT into a DSO + a syscall from relocated code
 *   malloc/free    libc.so's own data/bss are private and writable (COW)
 *   strcpy         a second PLT-resolved libc entry point
 *   errno          the interpreter's static TLS block is the one libc sees
 */
#include "libc.h"

int main(int argc, char **argv) {
  (void)argv;

  print_console("HDYN: entered (dynamic)\n");

  char *p = malloc(64);
  if (!p) {
    print_console("HDYN: FAIL malloc returned 0\n");
    return 1;
  }
  strcpy(p, "HDYN: libc.so symbols resolved\n");
  print_console(p);
  free(p);

  if (argc < 1) {
    print_console("HDYN: FAIL argc not set\n");
    return 1;
  }

  print_console("HDYN: done\n");
  return 0;
}
