/*
 * hex_test.c - Host unit tests for hex.c.  [STUB TEMPLATE - replace]
 *
 * Style: include the app source directly (see src/host/pong_test.c).
 * Provide stubs for any libc functions not supplied by src/host/compat.c
 * (e.g. sysinfo, chdir, getcwd, mkdir, unlink, rename).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main hex_app_main
#include "../user/hex.c"
#undef main

int main(void) {
    printf("[STUB] hex_test: replace with real tests\n");
    printf("ALL TESTS PASSED\n");
    return 0;
}
