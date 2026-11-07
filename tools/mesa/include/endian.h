/* endian.h - HobbyOS shim for the Mesa cross build (L5).
 *
 * HobbyOS/x86_64 is little-endian; there is no <endian.h> in the tree's libc.
 * Mesa's src/util/u_endian.h includes this header when HAVE_ENDIAN_H is set
 * and uses __BYTE_ORDER / __LITTLE_ENDIAN plus the host<->little-endian
 * conversion macros.  Only those names are provided - deliberately not
 * glibc's whole surface. */
#ifndef _HOBBYOS_ENDIAN_H
#define _HOBBYOS_ENDIAN_H 1

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412
#define __BYTE_ORDER    __LITTLE_ENDIAN
#define __FLOAT_WORD_ORDER __LITTLE_ENDIAN

#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

#if defined(__clang__)
#define __bswap16(x) __builtin_bswap16(x)
#define __bswap32(x) __builtin_bswap32(x)
#define __bswap64(x) __builtin_bswap64(x)
#else
#define __bswap16(x) ((unsigned short)(((x) >> 8) | ((x) << 8)))
#define __bswap32(x) ((unsigned int)__builtin_bswap32(x))
#define __bswap64(x) ((unsigned long long)__builtin_bswap64(x))
#endif

/* little-endian host: the to/from forms are the identity */
static inline unsigned short htole16(unsigned short x) { return x; }
static inline unsigned int   htole32(unsigned int x)   { return x; }
static inline unsigned long long htole64(unsigned long long x) { return x; }
static inline unsigned short le16toh(unsigned short x) { return x; }
static inline unsigned int   le32toh(unsigned int x)   { return x; }
static inline unsigned long long le64toh(unsigned long long x) { return x; }
static inline unsigned short htobe16(unsigned short x) { return __bswap16(x); }
static inline unsigned int   htobe32(unsigned int x)   { return __bswap32(x); }
static inline unsigned long long htobe64(unsigned long long x) { return __bswap64(x); }
static inline unsigned short be16toh(unsigned short x) { return __bswap16(x); }
static inline unsigned int   be32toh(unsigned int x)   { return __bswap32(x); }
static inline unsigned long long be64toh(unsigned long long x) { return __bswap64(x); }

#endif
