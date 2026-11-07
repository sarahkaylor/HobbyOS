/* sys/file.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * flock() itself is in HobbyOS's libc.a (src/user/libc.c, over the record
 * locks) but was never declared in a header; this adds the header and the
 * LOCK_* bits (Linux numbering).  src/util/{fossilize_db,mesa_cache_db}.c
 * use it to serialise cache files - both are compiled but unreachable with
 * -Dshader-cache=disabled. */
#ifndef HOBBYOS_SYS_FILE_H
#define HOBBYOS_SYS_FILE_H 1

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#ifdef __cplusplus
extern "C" {
#endif
int flock(int fd, int operation);
#ifdef __cplusplus
}
#endif

#endif
