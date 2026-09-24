/* HobbyOS sysroot: gnu_compat.h — feature set for transcribed GNU code.
 *
 * This header replaces gnulib's <libc-config.h>, which exists to feed
 * gnulib's autoconf-generated <config.h> into code shared with glibc.
 * The HobbyOS sysroot has no autoconf machinery, so the feature set is
 * stated explicitly here; every HAVE_* value reflects what src/libc
 * actually provides. The GNU sources transcribed into the sysroot include
 * this header first, before any system header (regex today; dfa, obstack
 * and friends next).
 *
 * Derived from gnulib libc-config.h:
 *   Copyright 2017-2020 Free Software Foundation, Inc.  (GPL-3+)
 */
#ifndef HOBBYOS_GNU_COMPAT_H
#define HOBBYOS_GNU_COMPAT_H 1

/* The GNU sources expose their GNU-only declarations (RE_SYNTAX_* and
 * friends) only when _GNU_SOURCE is set before the first system header.
 * Gnulib's config.h does the same for gnulib builds. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <stddef.h>

/* --- What the HobbyOS sysroot provides --------------------------------- */

/* Byte mode only: no wide-character or multibyte support yet, so GNU
 * regex keeps RE_ENABLE_I18N off (regex_internal.h's gate reads these). */
#define HAVE_WCTYPE_H 0
#define HAVE_ISWCTYPE 0

/* No NLS yet: gettext() is the identity (regex_internal.h falls back to
 * a no-op gettext when HAVE_LIBINTL_H/HAVE_LIBINTL stay undefined). */

#define HAVE_ALLOCA 1
#define HAVE_ISBLANK 1
#define HAVE_DECL_ISBLANK 1

/* --- glibc <features.h> equivalents ------------------------------------ */
#ifndef __GNUC_PREREQ
#if defined __GNUC__ && defined __GNUC_MINOR__
#define __GNUC_PREREQ(maj, min) ((maj) < __GNUC__ + ((min) <= __GNUC_MINOR__))
#else
#define __GNUC_PREREQ(maj, min) 0
#endif
#endif

#ifndef __glibc_clang_prereq
#if defined __clang_major__ && defined __clang_minor__
#define __glibc_clang_prereq(maj, min) \
  ((maj) < __clang_major__ + ((min) <= __clang_minor__))
#else
#define __glibc_clang_prereq(maj, min) 0
#endif
#endif

#ifndef __glibc_likely
#define __glibc_likely(cond) __builtin_expect(!!(cond), 1)
#endif
#ifndef __glibc_unlikely
#define __glibc_unlikely(cond) __builtin_expect(!!(cond), 0)
#endif

/* From glibc <errno.h>. */
#ifndef __set_errno
#define __set_errno(val) (errno = (val))
#endif

/* --- glibc <sys/cdefs.h> equivalents (only what the sources use) ------- */
#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__ \
  __attribute__((__warn_unused_result__))
#endif
#ifndef __wur
#define __wur __attribute_warn_unused_result__
#endif
#ifndef __THROW
#define __THROW
#endif
#ifndef __nonnull
#define __nonnull(params)
#endif
#ifndef __restrict_arr
#define __restrict_arr __restrict
#endif

/* --- glibc <libc-symbols.h> / <shlib-compat.h> substitutes ------------- */
#define attribute_hidden
#define libc_hidden_proto(name, ...)
#define libc_hidden_def(name)
#define libc_hidden_weak(name)
#define libc_hidden_ver(local, name)
#define strong_alias(name, aliasname)
#define weak_alias(name, aliasname)
#define SHLIB_COMPAT(lib, introduced, obsoleted) 0
#define versioned_symbol(lib, local, symbol, version)

#endif /* HOBBYOS_GNU_COMPAT_H */
