#ifndef ENDIAN_COMPAT_H
#define ENDIAN_COMPAT_H

#if defined(HAVE_ENDIAN_H)
#include <endian.h>
#elif defined(HAVE_SYS_ENDIAN_H)
#include <sys/endian.h>
#elif defined(HAVE_LIBKERN_OSBYTEORDER_H)
	#include <libkern/OSByteOrder.h>

	#define htobe16(x) OSSwapHostToBigInt16(x)
	#define htole16(x) OSSwapHostToLittleInt16(x)
	#define be16toh(x) OSSwapBigToHostInt16(x)
	#define le16toh(x) OSSwapLittleToHostInt16(x)

	#define htobe32(x) OSSwapHostToBigInt32(x)
	#define htole32(x) OSSwapHostToLittleInt32(x)
	#define be32toh(x) OSSwapBigToHostInt32(x)
	#define le32toh(x) OSSwapLittleToHostInt32(x)

	#define htobe64(x) OSSwapHostToBigInt64(x)
	#define htole64(x) OSSwapHostToLittleInt64(x)
	#define be64toh(x) OSSwapBigToHostInt64(x)
	#define le64toh(x) OSSwapLittleToHostInt64(x)
#elif defined(HAVE_HOBBYOS_ENDIAN)
	/* HobbyOS port (W2-MKFS): both target arches (aarch64 and x86_64) are
	 * little-endian and the sysroot has no <endian.h>; pin explicit swaps. */
	#include <stdint.h>

	#define htobe16(x) ((uint16_t)((((x) & 0x00ffU) << 8) | (((x) & 0xff00U) >> 8)))
	#define htole16(x) ((uint16_t)(x))
	#define be16toh(x) htobe16(x)
	#define le16toh(x) htole16(x)

	#define htobe32(x) ((uint32_t)((((x) & 0x000000ffU) << 24) | (((x) & 0x0000ff00U) << 8) | (((x) & 0x00ff0000U) >> 8) | (((x) & 0xff000000U) >> 24)))
	#define htole32(x) ((uint32_t)(x))
	#define be32toh(x) htobe32(x)
	#define le32toh(x) htole32(x)

	#define htobe64(x) ((uint64_t)((((uint64_t)(x) & 0x00000000000000ffULL) << 56) | (((uint64_t)(x) & 0x000000000000ff00ULL) << 40) | (((uint64_t)(x) & 0x0000000000ff0000ULL) << 24) | (((uint64_t)(x) & 0x00000000ff000000ULL) << 8) | (((uint64_t)(x) & 0x000000ff00000000ULL) >> 8) | (((uint64_t)(x) & 0x0000ff0000000000ULL) >> 24) | (((uint64_t)(x) & 0x00ff000000000000ULL) >> 40) | (((uint64_t)(x) & 0xff00000000000000ULL) >> 56)))
	#define htole64(x) ((uint64_t)(x))
	#define be64toh(x) htobe64(x)
	#define le64toh(x) htole64(x)
#else
#error No endian.h available and no fallback code
#endif

#endif
