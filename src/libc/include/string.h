#ifndef HOBBYOS_STRING_H
#define HOBBYOS_STRING_H

#include <stddef.h>
#include <locale.h> /* locale_t for the *_l variants */

#ifdef __cplusplus
extern "C" {
#endif

  /* HobbyOS Phase-1 libc: POSIX.1-2008 string.h subset.
   * Implementations live in src/libc/src/string.c. Under HOST_TEST they
   * compile renamed to hb_* so host tests can link them ALONGSIDE glibc and
   * property-test byte-exact behavior against it.
   */

  size_t strlen(const char *s);
  size_t strnlen(const char *s, size_t maxlen);

  int strcmp(const char *s1, const char *s2);
  int strcoll(const char *s1, const char *s2); /* C locale: == strcmp */
  int strncmp(const char *s1, const char *s2, size_t n);
  /* P3.2: strxfrm completes the <cstring> surface libc++'s headers
   * re-export (C locale: identity transform, like glibc). */
  size_t strxfrm(char *dst, const char *src, size_t n);

  /* xlocale/P3.2 variants (C locale: the locale argument is ignored). */
  int strcoll_l(const char *s1, const char *s2, locale_t loc);
  size_t strxfrm_l(char *dst, const char *src, size_t n, locale_t loc);
  /* Also declared by glibc's <strings.h>, which the HOST_TEST build pulls
   * in via include_next.  glibc marks these __THROW (= noexcept(true) in
   * C++), so the C++ declarations here must carry the same exception
   * specification or a host C++ TU sees "exception specification in
   * declaration does not match previous declaration".  On the device the
   * spec is equally true: the C++ runtime is built with -fno-exceptions. */
#ifdef __cplusplus
  int strcasecmp(const char *s1, const char *s2) noexcept;
  int strncasecmp(const char *s1, const char *s2, size_t n) noexcept;
#else
  int strcasecmp(const char *s1, const char *s2);
  int strncasecmp(const char *s1, const char *s2, size_t n);
#endif

  char *strcpy(char *dst, const char *src);
  char *strncpy(char *dst, const char *src, size_t n);
  char *strcat(char *dst, const char *src);
  char *strncat(char *dst, const char *src, size_t n);

  char *strchr(const char *s, int c);
  char *strrchr(const char *s, int c);
  char *strstr(const char *haystack, const char *needle);
  char *strcasestr(const char *haystack, const char *needle); /* GNU */
  char *strpbrk(const char *s, const char *accept);
  size_t strspn(const char *s, const char *accept);
  size_t strcspn(const char *s, const char *reject);

  char *strtok(char *str, const char *delim);
  char *strtok_r(char *str, const char *delim, char **saveptr);

  char *strdup(const char *s);

  char *strerror(int errnum);

  /* XSI strerror_r (P3.2: libc++ system_error.cpp uses it).  Returns 0 on
   * success, or the error number when the buffer is too small (ERANGE) or
   * errnum is unknown (EINVAL), glibc-XSI semantics. */
  int strerror_r(int errnum, char *buf, size_t buflen);

  int memcmp(const void *s1, const void *s2, size_t n);
  void *memmove(void *dest, const void *src, size_t n);
  void *memcpy(void *dest, const void *src, size_t n);
  void *memset(void *s, int c, size_t n);
  void *memchr(const void *s, int c, size_t n);
  void *memrchr(const void *s, int c, size_t n); /* GNU: last occurrence */

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_STRING_H */
