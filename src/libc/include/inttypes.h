#ifndef HOBBYOS_INTTYPES_H
#define HOBBYOS_INTTYPES_H

/* HobbyOS Phase-1 sysroot: inttypes.h — printf/scanf format macros over
 * the compiler's <stdint.h>, plus the intmax_t/imaxabs/imaxdiv interface.
 * clang's freestanding headers do NOT ship inttypes.h, so ported code
 * would fail to find it without this (Phase 1 makes it a header-only
 * sysroot deliverable). imax functions come in a later phase. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* --- format macros (C99 7.8.1), host and bare-metal --- */
  /* int64_t is `long` on LP64 (all four project targets) and `long long` on
   * 32-bit; the glibc-style conditionals keep the macros correct for both. */
#ifdef __LP64__
#define PRId64 "ld"
#define PRIi64 "li"
#define PRIo64 "lo"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIoMAX "lo"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"
#define SCNd64 "ld"
#define SCNi64 "li"
#define SCNo64 "lo"
#define SCNu64 "lu"
#define SCNx64 "lx"
#define SCNdMAX "ld"
#define SCNiMAX "li"
#define SCNoMAX "lo"
#define SCNuMAX "lu"
#define SCNxMAX "lx"
#else
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIo64 "llo"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define PRIdMAX "lld"
#define PRIiMAX "lli"
#define PRIoMAX "llo"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
#define PRIXMAX "llX"
#define SCNd64 "lld"
#define SCNi64 "lli"
#define SCNo64 "llo"
#define SCNu64 "llu"
#define SCNx64 "llx"
#define SCNdMAX "lld"
#define SCNiMAX "lli"
#define SCNoMAX "llo"
#define SCNuMAX "llu"
#define SCNxMAX "llx"
#endif

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"

#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"

#define SCNd8  "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNi8  "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNo8  "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNu8  "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNx8  "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"

#define SCNdPTR "ld"
#define SCNiPTR "li"
#define SCNoPTR "lo"
#define SCNuPTR "lu"
#define SCNxPTR "lx"

  /* --- imax functions (implementations land with a later libc phase;
   * declarations are provided so ported code that merely calls them gets an
   * honest link error for now). --- */
  typedef struct {
    intmax_t quot;
    intmax_t rem;
  } imaxdiv_t;

  intmax_t strtoimax(const char *nptr, char **endptr, int base);
  uintmax_t strtoumax(const char *nptr, char **endptr, int base);
  intmax_t imaxabs(intmax_t j);
  imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_INTTYPES_H */
