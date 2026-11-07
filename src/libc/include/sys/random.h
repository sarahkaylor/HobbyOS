/* HobbyOS sysroot: <sys/random.h> — getentropy() (P3.2).
 *
 * glibc declares getentropy() here (since 2.25); libc++'s std::random_device
 * getentropy backend calls it.  The implementation (src/user/libc.c) wraps
 * SYS_GETRANDOM, the kernel entropy syscall RANDTST.BIN already exercises.
 */
#ifndef HOBBYOS_SYS_RANDOM_H
#define HOBBYOS_SYS_RANDOM_H 1

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* Fill buf[0..length) with random bytes.  length <= 256 (larger requests
   * fail with EIO, glibc's documented behavior).  Returns 0 on success,
   * -1 with errno set otherwise. */
  int getentropy(void *buf, size_t length);
  /* M4/L5: getrandom(2) itself.  src/user/libc.c implements it (over
   * SYS_GETRANDOM) but no header declared it; Mesa's src/util/rand_xor.c
   * seeds its xorshift state with it.  flags: GRND_NONBLOCK = 1. */
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#endif
  ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_RANDOM_H */
