/* hb_stubs.h - declarations for the HobbyOS glue in hb_stubs.c.
 *
 * This file is first-party HobbyOS port code (docs/gnu-ports.md house
 * style): make's own sources stay byte-identical, the libc symbols the OS
 * lacks are provided here.  See the notes in src/user/make/config.h.
 *
 * The functions are the real libc symbols (getlogin, getloadavg, dup)
 * rather than
 * make-only renames, so any other ported program in the tree can link them
 * the same way; they live in MAKE.BIN only.
 */
#ifndef HOBBYOS_MAKE_HB_STUBS_H
#define HOBBYOS_MAKE_HB_STUBS_H

#ifdef __cplusplus
extern "C" {
#endif

  /* Sysroot gap (audit S3.2): no getlogin(3).  Make calls it during the ~
   * expansion in read.c:3114.  Returns LOGNAME or the synthetic account
   * name "user" (which getpwnam() resolves to home "/"). */
  char *getlogin(void);

  /* Sysroot gap (audit S3.2): no getloadavg(3).  Referenced by load_too_high
   * under -l (job.c:2138); -j1 v1 never runs it.  Returns -1 with errno 0,
   * make's "not supported" convention. */
  int getloadavg(double loadavg[], int nelem);

  /* Sysroot gap: <unistd.h> declares dup() but no libc object implements it.
   * posixos.c os_anontmp() calls it on its tmpfile fallback path; this is
   * the POSIX dup() (lowest free descriptor) over the sysroot's dup2(). */
  int dup(int oldfd);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_MAKE_HB_STUBS_H */
