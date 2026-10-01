/*
 * uchar.h — standard C11 <uchar.h> for the HobbyOS sysroot.
 *
 * Target (freestanding): neither the compiler resource dir nor the sysroot
 * shipped a <uchar.h>, so this header provides the two C11 character types
 * (char16_t / char32_t = uint_least16_t / uint_least32_t, C11 7.28) itself.
 *
 * Why it exists (l3-icu-wire, docs/browser/icu-usage-note.md): ICU's
 * common/unicode/ptypes.h does `#include <uchar.h>` in C mode — platform.h
 * hardcodes U_HAVE_CHAR16_T to 1 for conformant C (non-Darwin) — so a
 * target-compiled C TU including <unicode/utypes.h> is the browser's ICU
 * consumption path.  Without this header the angled lookup fails and clang
 * then walks ICU's OWN common/unicode/uchar.h through its same-directory
 * fallback (the errored include is still processed), re-entering the
 * Unicode property header before umachine.h has defined U_CDECL_BEGIN /
 * U_CAPI — a 20-error cascade.  The BEFORE/AFTER reproduction is recorded
 * in the note; the in-wave consumer ICUSMK.BIN pins the fixed path.
 *
 * Host (HOST_TEST): include_next pulls the real host header (char16_t,
 * mbstate_t, the mbrtoc16/c16rtomb family) so parity builds behave like
 * glibc.
 *
 * GUARD: HOBBYOS_UCHAR_H — deliberately NOT UCHAR_H.  ICU's own
 * common/unicode/uchar.h uses UCHAR_H as its include guard; a sysroot
 * header sharing it would make the two silently suppress each other in
 * include order (a TU taking <unicode/utypes.h> before <unicode/uchar.h>
 * would lose every u_charType/u_isalpha/... declaration — verified before
 * this file landed).  Same failure class as the old signal.h/_SIGNAL_H
 * wrapper bug; keep the guard name.
 */
#ifndef HOBBYOS_UCHAR_H
#define HOBBYOS_UCHAR_H

#if defined(HOST_TEST)
#include_next <uchar.h>
#elif defined(__has_include_next) && __has_include_next(<uchar.h>)
/* A freestanding environment that does provide a real one wins. */
#include_next <uchar.h>
#elif !defined(__cplusplus)
/* Target: C++ gets both types as keywords and needs nothing here.  The
 * compiler builtin macros keep the typedefs correct on both supported
 * architectures (aarch64 / x86_64 LP64).  mbstate_t and the
 * mbrtoc16/c16rtomb conversion functions are not provided (no consumer
 * needs them yet; add with a real implementation if one appears). */
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
#endif

#endif /* HOBBYOS_UCHAR_H */
