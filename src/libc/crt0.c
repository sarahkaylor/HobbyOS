/* src/libc/crt0.c — C runtime entry point for main(argc, argv) programs.
 *
 * Legacy HobbyOS programs use `_start` directly and link only libc.o +
 * malloc.o. Ported POSIX programs define `int main(int argc, char **argv)`
 * and instead link crt0.o (this file) in place of their own entry point —
 * crt0 builds argv from the kernel-provided args string, prepends the
 * process's binary name as argv[0] (SYS_GETPROGNAME), calls main(), and
 * exits with its return value.
 *
 * The linker script (src/user/linker.ld) places .text._start first, so this
 * must stay the only definition of `_start` in the process image: do not
 * also define `_start` in a crt0-using program (use main()).
 */
#include "libc.h"

extern int main(int argc, char **argv);

#define CRT0_MAX_ARGS 32

__attribute__((section(".text._start")))
void _start(void) {
  static char namebuf[32];
  static char argbuf[256];
  static char avbuf[256];
  static char *argv[CRT0_MAX_ARGS];

  /* P1 (p1-threads-design.md sections 4/6): install this thread's TLS
   * register BEFORE any C code that could touch __thread storage (errno,
   * pthread_self).  The layout is probe-verified against llvm 21 + ld.lld
   * and lives in src/user/linker.ld:
   *   aarch64 (variant I):  TPIDR_EL0 = &__tls_start - 16
   *   x86_64  (variant II): FS base = align_up(&__tls_end, __tls_align),
   *                         with the self-pointer *(void **)FS = FS.
   * ho_tls_setup_initial() (libc.c) does the work and is also the lazy
   * fallback for programs that do not link crt0. */
  ho_tls_setup_initial();

  /* P6.3: materialize `environ` from the kernel environment blob (the
   * spawn/exec contract carries it; SYS_GETENV reads it back) so main()
   * and ported POSIX code see a real environment table. */
  environ_init();

  int argc = 0;

  /* Preferred: the kernel's positional-parameter blob (SYS_GETARGV).
   * It is populated at exec time from the caller's argv[] array, so
   * arguments containing spaces survive verbatim. Spawned programs get
   * argv[0] = binary name, then the flat args split on whitespace. */
  int cnt = get_argv(-1, NULL, 0);
  if (cnt > 0 && cnt < CRT0_MAX_ARGS) {
    int pos = 0;
    for (int i = 0; i < cnt && pos < (int)sizeof avbuf - 1; i++) {
      int n = get_argv(i, avbuf + pos, (int)sizeof avbuf - pos);
      if (n < 0)
        break;
      argv[argc++] = avbuf + pos;
      pos += n + 1;
    }
  }

  /* Legacy fallback: name + flat args string (space-split). Used when the
     kernel predates SYS_GETARGV (kernel tasks, -ENOSYS) or left the blob
     empty (argv == NULL exec). */
  if (argc == 0) {
    if (get_progname(namebuf, sizeof namebuf) == 0 && namebuf[0] != '\0') {
      argv[argc++] = namebuf;
    }
    if (get_args(argbuf, sizeof argbuf) == 0) {
      argc += parse_args(argbuf, &argv[argc], CRT0_MAX_ARGS - 1 - argc);
    }
  }

  if (argc == 0) {
    argv[argc++] = "HobbyOS"; /* no name and no args: still need argv[0] */
  }
  argv[argc] = 0;

  /* C++ static constructors (F2.4, browser.md §6): the compiler deposits
   * each TU's static-init thunk (_GLOBAL__sub_I_*) and every
   * __attribute__((constructor)) handler in .init_array; linker.ld
   * brackets that section with __init_array_start/__init_array_end and
   * the kernel loader copies the whole image, so walking the array here
   * runs every constructor after argv is ready and before main().
   * See src/libc/include/cxxrt.h for the full mechanism. */
  {
    extern void (*__init_array_start[])(void);
    extern void (*__init_array_end[])(void);
    for (void (**ctor)(void) = __init_array_start; ctor < __init_array_end;
         ctor++)
      (*ctor)();
  }

  int rc = main(argc, argv);
  exit(rc);
}
