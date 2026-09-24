/* src/user/hello.c — Phase 0 demo: a POSIX-style main(argc, argv) program
 * built with crt0.o + libc.a instead of a hand-rolled _start.  It prints
 * the argument vector and returns its status through exit().  The shell
 * resolves `hello` to HELLO.BIN; in KERNEL_MODE_TEST the scheduler spawns
 * it with no args so argv == { "HELLO" }.  print() is captured by the
 * desktop window when launched from the desktop and falls back to the
 * serial console when stdout isn't wired. */
#include "libc.h"

int main(int argc, char **argv) {
  print("[HELLO] entering main\n");
  print("[HELLO] argc=");
  print_dec(argc);
  print("\n");
  for (int i = 0; i < argc; i++) {
    print("[HELLO] argv[");
    print_dec(i);
    print("]=\"");
    print(argv[i]);
    print("\"\n");
  }
  print("[HELLO] main returned 0\n");
  return 0;
}
