#ifndef HOBBYOS_FCNTL_H
#define HOBBYOS_FCNTL_H

/* HobbyOS Phase-2 sysroot: fcntl.h — O_* open flags.
 * The kernel honors O_CREAT, O_EXCL and O_TRUNC in SYS_OPEN (missing
 * files without O_CREAT fail ENOENT, POSIX-style); O_APPEND is still
 * accepted but treated as plain write. */

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_APPEND 8
#define O_CREAT  0x40   /* 0100 octal POSIX value */
#define O_TRUNC  0x200  /* 01000 octal POSIX value */
#define O_EXCL   0x80   /* 0200 octal POSIX value */

  /* P2.4 (S4): memfd(2) sealing (Linux numbering; SHRINK/GROW enforced at
   * ftruncate, WRITE recorded advisory). */
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008

  /* HobbyOS does not track permissions yet: access modes beyond these are
   * accepted and ignored, so ported code that passes O_BINARY or similar
   * still compiles and runs. */

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_FCNTL_H */
