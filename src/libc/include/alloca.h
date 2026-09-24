/* HobbyOS sysroot: <alloca.h> — stack allocation.
 *
 * alloca is a compiler builtin; no library support or runtime state is
 * involved. Transcribed GNU sources include this under HAVE_ALLOCA
 * (see gnu_compat.h); the builtin works on both supported architectures.
 */
#ifndef HOBBYOS_ALLOCA_H
#define HOBBYOS_ALLOCA_H 1

#define alloca(size) __builtin_alloca(size)

#endif /* HOBBYOS_ALLOCA_H */
