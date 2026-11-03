/* libsync.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * Only src/gallium/auxiliary/util/u_tests.c includes it: Android's
 * sync-fence helpers, used by gallium's self-test harness for explicit
 * fencing.  Nothing in libGL calls u_tests, HobbyOS has no sync_file, and
 * the entry points fail closed in gl_stubs.c.  Present so the TU compiles
 * instead of being excluded by hand. */
#ifndef HOBBYOS_LIBSYNC_H
#define HOBBYOS_LIBSYNC_H 1

#ifdef __cplusplus
extern "C" {
#endif

int sync_wait(int fd, int timeout);
int sync_merge(const char *name, int fd1, int fd2);
int sync_inc(int fd);
int sync_dec(int fd);

#ifdef __cplusplus
}
#endif

#endif
