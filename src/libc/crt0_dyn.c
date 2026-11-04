/* src/libc/crt0_dyn.c — entry point for DYNAMICALLY LINKED native programs.
 *
 * Same job as crt0.c (build argv, call main, exit with its status) with two
 * deliberate differences, both because an interpreter (LDHOBBY.SO) already
 * ran before this code:
 *
 *   1. NO TLS SETUP.  crt0.c calls ho_tls_setup_initial(), which computes the
 *      thread pointer from THIS IMAGE's __tls_start/__tls_end.  In a dynamic
 *      program the thread block belongs to the whole module set (exe + every
 *      DSO), so the interpreter lays it out, copies each module's .tdata and
 *      installs it with SYS_SET_TLS, then sets libc's ho_tls_ready_flag so
 *      __errno_location() does not re-install a bogus single-module block.
 *      Calling ho_tls_setup_initial() here would clobber that layout.
 *
 *   2. The image is ET_DYN: every address below is link-time relative and the
 *      interpreter has already applied the R_*_RELATIVE relocations, so this
 *      file MUST be compiled -fPIC (see the dyn link rule in the Makefile).
 *
 * Everything else (argv blob, environ, exit status) is the crt0 contract.
 */
#include "libc.h"

extern int main(int argc, char **argv);

#define CRT0D_MAX_ARGS 32
#define CRT0D_ARG_BUF 1024

__attribute__((section(".text._start")))
void _start(void) {
  static char namebuf[32];
  static char argbuf[CRT0D_ARG_BUF];
  static char avbuf[CRT0D_ARG_BUF];
  static char *argv[CRT0D_MAX_ARGS];

  /* P6.3: environ, from the kernel blob.  Safe here: it touches libc.so's
     data, not TLS. */
  environ_init();

  int argc = 0;
  int cnt = get_argv(-1, NULL, 0);
  if (cnt > 0 && cnt < CRT0D_MAX_ARGS) {
    int pos = 0;
    for (int i = 0; i < cnt && pos < (int)sizeof avbuf - 1; i++) {
      int n = get_argv(i, avbuf + pos, (int)sizeof avbuf - pos);
      if (n < 0)
        break;
      argv[argc++] = avbuf + pos;
      pos += n + 1;
    }
  }
  if (argc == 0) {
    if (get_progname(namebuf, sizeof namebuf) == 0 && namebuf[0] != '\0')
      argv[argc++] = namebuf;
    if (get_args(argbuf, sizeof argbuf) == 0)
      argc += parse_args(argbuf, &argv[argc], CRT0D_MAX_ARGS - 1 - argc);
  }
  if (argc == 0)
    argv[argc++] = namebuf;

  exit(main(argc, argv));
}
