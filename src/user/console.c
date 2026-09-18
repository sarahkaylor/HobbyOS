/*
 * console.c — CONSOLE.BIN: the desktop console window / EL0 smoke test.
 *
 * Two roles, selected at build time from the build mode (the same fact the
 * kernel uses: -DKERNEL_MODE_TEST for `make MODE=test`):
 *
 *  - Desktop builds (any mode except test): an interactive console. Prints
 *    a banner, then hands the window over to the shell (SH.BIN) by spawning
 *    it with this process's stdin/stdout/stderr inherited. The desktop
 *    writes window keystrokes to our stdin pipe and reads the shell's
 *    output back from our stdout pipe; the shell holds both pipes, so the
 *    window lives for as long as the shell does. Closing the window makes
 *    the desktop close its pipe ends, the shell sees EOF on stdin and
 *    exits, and the desktop then closes the window's bookkeeping. This
 *    process itself exits right after the handoff — the window survives
 *    because the shell owns the pipe ends, not this process.
 *
 *  - Test builds (MODE=test): the original boot smoke — prove EL0 print
 *    and exit work. This MUST keep exiting: the test kernel halts once all
 *    boot programs have exited, so an interactive console would hang the
 *    suite.
 */
#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

/* Interactive path: print a banner and hand our pipes to the shell. */
int console_run_shell(void) {
  print("HobbyOS console - close the window to end the session.\n");
  int pid = spawn2("SH.BIN", 0, 1, 1, 0);
  if (pid < 0) {
    print("console: failed to start the shell (SH.BIN)\n");
    return 0;
  }
  return 1;
}

/* Smoke path (test builds): unchanged since the original bring-up. */
int console_run_smoke(void) {
  print("\n==============================\n"
        "Hello from isolated EL0 Space!\n"
        "==============================\n");

  // Compute (delay)
  for (volatile int i = 0; i < 2000000; i++) {}

  print("Computation finished. Exiting.\n");
  return 0;
}

int main(void) {
#ifdef KERNEL_MODE_TEST
  return console_run_smoke();
#else
  return console_run_shell();
#endif
}
