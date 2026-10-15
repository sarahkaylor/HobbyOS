/* HobbyOS sysroot compat header: <machine/endian.h>.
 * LLVM's llvm/ADT/bit.h:51 includes <machine/endian.h> for non-Linux
 * targets when BYTE_ORDER is undefined (freestanding aarch64-none-elf has
 * no OS endian.h).  A real bare-metal sysroot (newlib et al) supplies
 * machine/endian.h; the HobbyOS sysroot does not, so the LLVM toolchain
 * staging copies this in (lane L1 spike; AD-3 build-config only, no LLVM
 * source patch).  Values: little-endian (aarch64 HobbyOS userland).
 */
#ifndef _MACHINE_ENDIAN_H_
#define _MACHINE_ENDIAN_H_

#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN    4321
#define _BYTE_ORDER    _LITTLE_ENDIAN

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN    4321
#define BYTE_ORDER    LITTLE_ENDIAN

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN

#endif /* _MACHINE_ENDIAN_H_ */
