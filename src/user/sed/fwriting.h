/* HobbyOS port shim: <fwriting.h> semantics on the HobbyOS FILE.
 *
 * GNU sed's ck_fclose() asks whether a stream is open for writing
 * before flushing/closing it.  gnulib's fwriting() answers by poking at
 * glibc FILE internals; the HobbyOS FILE (src/libc/src/file.c) tracks
 * the same fact explicitly and exposes it as hb_fp_is_writing().
 */
#ifndef HOBBYOS_FWRITING_H
#define HOBBYOS_FWRITING_H 1

#include <stdio.h>

int hb_fp_is_writing(FILE *f);

#define fwriting(fp) hb_fp_is_writing(fp)

#endif /* HOBBYOS_FWRITING_H */
