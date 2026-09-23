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
    static char *argv[CRT0_MAX_ARGS];

    int argc = 0;
    if (get_progname(namebuf, sizeof namebuf) == 0 && namebuf[0] != '\0') {
        argv[argc++] = namebuf;
    }
    if (get_args(argbuf, sizeof argbuf) == 0) {
        argc += parse_args(argbuf, &argv[argc], CRT0_MAX_ARGS - 1 - argc);
    }
    if (argc == 0) {
        argv[argc++] = "HobbyOS"; /* no name and no args: still need argv[0] */
    }
    argv[argc] = 0;

    int rc = main(argc, argv);
    exit(rc);
}
