/* hb_assert.c — host-build shim for the HobbyOS sysroot assert.h.
 *
 * The sysroot's assert.h (<src/libc/include/assert.h>) expands
 * assert(expr) to a call to _assert_fail(file, line, func, expr), which
 * the in-OS libc (src/libc/src/stdlib.c) implements.  glibc provides
 * only __assert_fail, with a different signature, so host builds of the
 * GNU ports (cut_host and friends) link this shim to bridge the two:
 * the failure message formatting (including the program name prefix and
 * abort) stays glibc's.
 */

void __assert_fail (const char *expr, const char *file, unsigned int line, const char *func);

void _assert_fail (const char *file, int line, const char *func, const char *expr) {
  __assert_fail (expr, file, (unsigned int) line, func);
}
