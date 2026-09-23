#ifndef HOBBYOS_STDARG_H
#define HOBBYOS_STDARG_H

/* HobbyOS Phase-1 sysroot: stdarg.h. The compiler provides the real
 * implementation (freestanding builtin); this wrapper exists so that code
 * ported against a standard libc finds <stdarg.h> on the sysroot include
 * path. clang only ships the va_* macros itself in C11 mode; in C99/GNU
 * mode its stdarg.h defers to a compiler include dir via include_next (not
 * always present on LLVM-only toolchains), so define va_start/va_arg/
 * va_end/va_copy from the builtins if they didn't arrive. */
#include_next <stdarg.h>

#ifndef va_start
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)
#endif

#endif /* HOBBYOS_STDARG_H */
