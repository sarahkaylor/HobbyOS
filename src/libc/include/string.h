#ifndef HOBBYOS_STRING_H
#define HOBBYOS_STRING_H

#include <stddef.h>

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
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);

char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);

char *strdup(const char *s);

char *strerror(int errnum);

int memcmp(const void *s1, const void *s2, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memchr(const void *s, int c, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_STRING_H */
