/* HobbyOS sysroot compat header: <sysexits.h> (constants only).
 * LLVM's llvm/Support/ExitCodes.h:30 #errors when EX_IOERR is unavailable
 * (the include of <sysexits.h> at ExitCodes.h:22), and
 * llvm/lib/Support/CrashRecoveryContext.cpp:387 uses EX_IOERR.
 * HobbyOS libc has no <sysexits.h>; this is a pure constants header (no
 * behavior), staged by the LLVM toolchain (lane L1 spike, AD-3 build-config
 * only).  Values from the BSD/glibc sysexits.h.
 */
#ifndef _SYSEXITS_H_
#define _SYSEXITS_H_

#define EX_OK          0  /* successful termination */
#define EX__BASE       64 /* base value for error messages */
#define EX_USAGE       64
#define EX_DATAERR     65
#define EX_NOINPUT     66
#define EX_NOUSER      67
#define EX_NOHOST      68
#define EX_UNAVAILABLE 69
#define EX_SOFTWARE    70
#define EX_OSERR       71
#define EX_OSFILE      72
#define EX_CANTCREAT   73
#define EX_IOERR       74
#define EX_TEMPFAIL    75
#define EX_PROTOCOL    76
#define EX_NOPERM      77
#define EX_CONFIG      78
#define EX__MAX        78

#endif /* _SYSEXITS_H_ */
