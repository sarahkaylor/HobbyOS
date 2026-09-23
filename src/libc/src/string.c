/* HobbyOS Phase-1 libc: string.c — POSIX.1-2008 string.h subset.
 *
 * Design (see posix.md §2.3/Phase 1):
 *  - Self-contained: no stdio, no heap, no other libc modules.
 *  - HOST_TEST compiles these renamed to hb_* so host tests can link this
 *    translation unit ALONGSIDE glibc and property-test byte-exact behavior
 *    against glibc's implementation on the same inputs.
 *  - Non-HOST builds define the real names and are archived into libc.a.
 */
#include <stddef.h>

#ifndef HOST_TEST
#include "string.h"
#include "malloc.h"
#endif

/* Recursion guard for our own use below (non-host only). */
#ifdef HOST_TEST
#define strlen hb_strlen
#define strnlen hb_strnlen
#define strcmp hb_strcmp
#define strncmp hb_strncmp
#define strcasecmp hb_strcasecmp
#define strncasecmp hb_strncasecmp
#define strcpy hb_strcpy
#define strncpy hb_strncpy
#define strcat hb_strcat
#define strncat hb_strncat
#define strchr hb_strchr
#define strrchr hb_strrchr
#define strstr hb_strstr
#define strpbrk hb_strpbrk
#define strspn hb_strspn
#define strcspn hb_strcspn
#define strtok hb_strtok
#define strtok_r hb_strtok_r
#define strdup hb_strdup
#define strerror hb_strerror
#define memcmp hb_memcmp
#define memmove hb_memmove
#define memchr hb_memchr
#else
#include "string.h"
#endif

/* Case-folding for the *casecmp family. Byte-exact with glibc in the C
 * locale for the 7-bit range; bytes >= 0x80 compare as-is (glibc folds
 * them only under a locale). */
static unsigned char hb_lower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c + ('a' - 'A'));
    return c;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n > 0 && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strcasecmp(const char *s1, const char *s2)
{
    unsigned char a, b;
    do {
        a = hb_lower((unsigned char)*s1++);
        b = hb_lower((unsigned char)*s2++);
    } while (a && a == b);
    return (int)a - (int)b;
}

int strncasecmp(const char *s1, const char *s2, size_t n)
{
    unsigned char a, b;
    while (n > 0) {
        a = hb_lower((unsigned char)*s1++);
        b = hb_lower((unsigned char)*s2++);
        if (a != b) return (int)a - (int)b;
        if (a == 0) return 0;
        n--;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != '\0') ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d) d++;
    size_t i = 0;
    while (i < n && src[i] != '\0') {
        d[i] = src[i];
        i++;
    }
    d[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c)
{
    unsigned char uc = (unsigned char)c;
    for (;;) {
        unsigned char cur = (unsigned char)*s;
        if (cur == uc) return (char *)s;
        if (cur == 0) return 0;
        s++;
    }
}

char *strrchr(const char *s, int c)
{
    unsigned char uc = (unsigned char)c;
    const char *found = 0;
    for (;;) {
        unsigned char cur = (unsigned char)*s;
        if (cur == uc) found = s;
        if (cur == 0) break;
        s++;
    }
    return (char *)found;
}

char *strstr(const char *haystack, const char *needle)
{
    if (*needle == '\0') return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0') return (char *)haystack;
        if (*h == '\0') return 0;
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        const char *a = accept;
        while (*a) {
            if (*a == *s) return (char *)s;
            a++;
        }
    }
    return 0;
}

size_t strspn(const char *s, const char *accept)
{
    const char *p = s;
    for (; *p; p++) {
        const char *a = accept;
        int in = 0;
        while (*a) {
            if (*a == *p) {
                in = 1;
                break;
            }
            a++;
        }
        if (!in) break;
    }
    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *reject)
{
    const char *p = s;
    for (; *p; p++) {
        const char *r = reject;
        while (*r) {
            if (*r == *p) return (size_t)(p - s);
            r++;
        }
    }
    return (size_t)(p - s);
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *p;
    if (str == 0) str = *saveptr;
    if (str == 0) return 0;

    /* Skip leading delimiters. */
    p = str + strspn(str, delim);
    if (*p == '\0') {
        *saveptr = p; /* glibc saves the end-of-string position (may be 0) */
        return 0;
    }
    str = p;
    p = str + strcspn(str, delim);
    if (*p != '\0') {
        *p = '\0';
        *saveptr = p + 1;
    } else {
        *saveptr = p; /* past the NUL */
    }
    return str;
}

char *strtok(char *str, const char *delim)
{
    static char *saveptr;
    return strtok_r(str, delim, &saveptr);
}

char *strdup(const char *s)
{
#ifdef HOST_TEST
    extern char *stdup_impl(const char *s);
    return stdup_impl(s);
#else
    char *copy;
    size_t len = strlen(s) + 1;
    copy = (char *)malloc(len);
    if (copy != 0) {
        size_t i;
        for (i = 0; i < len; i++) copy[i] = s[i];
    }
    return copy;
#endif
}

/* Out-of-line helper so the HOST build can redirect malloc cleanly
 * (see the #ifdef in strdup). Never called on the bare-metal target. */
#ifdef HOST_TEST
char *stdup_impl(const char *s)
{
    extern void *malloc(unsigned long size);
    char *copy;
    size_t len = (size_t)0;
    const char *p = s;
    while (*p) { len++; p++; }
    len++;
    copy = (char *)malloc(len);
    if (copy != 0) {
        size_t i;
        for (i = 0; i < len; i++) copy[i] = s[i];
    }
    return copy;
}
#endif

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    while (n-- > 0) {
        if (*a != *b) return (int)*a - (int)*b;
        a++;
        b++;
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n-- > 0) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n-- > 0) *--d = *--s;
    }
    return dst;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char uc = (unsigned char)c;
    while (n-- > 0) {
        if (*p == uc) return (void *)p;
        p++;
    }
    return 0;
}

/* strerror: Linux-style messages (matches glibc text for the covered
 * range; POSIX only requires a non-null message). */
static const char *const hb_err_messages[] = {
    "Success",                      /* 0 */
    "Operation not permitted",       /* 1  EPERM */
    "No such file or directory",     /* 2  ENOENT */
    "No such process",               /* 3  ESRCH */
    "Interrupted system call",       /* 4  EINTR */
    "Input/output error",            /* 5  EIO */
    "No such device or address",     /* 6  ENXIO */
    "Argument list too long",        /* 7  E2BIG */
    "Exec format error",             /* 8  ENOEXEC */
    "Bad file descriptor",           /* 9  EBADF */
    "No child processes",            /* 10 ECHILD */
    "Resource temporarily unavailable", /* 11 EAGAIN */
    "Cannot allocate memory",        /* 12 ENOMEM */
    "Permission denied",             /* 13 EACCES */
    "Bad address",                   /* 14 EFAULT */
    "Block device required",         /* 15 ENOTBLK */
    "Device or resource busy",       /* 16 EBUSY */
    "File exists",                   /* 17 EEXIST */
    "Invalid cross-device link",     /* 18 EXDEV */
    "No such device",                /* 19 ENODEV */
    "Not a directory",               /* 20 ENOTDIR */
    "Is a directory",                /* 21 EISDIR */
    "Invalid argument",              /* 22 EINVAL */
    "Too many open files in system", /* 23 ENFILE */
    "Too many open files",           /* 24 EMFILE */
    "Inappropriate ioctl for device",/* 25 ENOTTY */
    "Text file busy",                /* 26 ETXTBSY */
    "File too large",                /* 27 EFBIG */
    "No space left on device",       /* 28 ENOSPC */
    "Illegal seek",                  /* 29 ESPIPE */
    "Read-only file system",         /* 30 EROFS */
    "Too many links",                /* 31 EMLINK */
    "Broken pipe",                   /* 32 EPIPE */
    "Numerical argument out of domain", /* 33 EDOM */
    "Numerical result out of range", /* 34 ERANGE */
    "Operation not supported",       /* 95 EOPNOTSUPP/ENOTSUP (Linux) */
};

static char hb_strerror_buf[64];

char *strerror(int errnum)
{
    static const char unknown[] = "Unknown error ";
    char *out = hb_strerror_buf;
    const char *msg = 0;
    if (errnum >= 0 && errnum <= 34) {
        msg = hb_err_messages[errnum];
    } else if (errnum == 95) {
        msg = hb_err_messages[35];
    }
    if (msg != 0) return (char *)msg;
    /* "Unknown error N" into static buffer (N up to 5 digits) */
    {
        const char *u = unknown;
        char *d = out;
        int n = errnum;
        char digits[12];
        int nd = 0;
        while (*u) *d++ = *u++;
        if (n < 0) {
            *d++ = '-';
            n = (int)(-(long)n);
        }
        if (n == 0) digits[nd++] = '0';
        while (n > 0) {
            digits[nd++] = (char)('0' + (n % 10));
            n /= 10;
        }
        while (nd > 0) *d++ = digits[--nd];
        *d = '\0';
    }
    return out;
}
