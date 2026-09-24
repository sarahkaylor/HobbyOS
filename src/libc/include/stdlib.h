#ifndef HOBBYOS_STDLIB_H
#define HOBBYOS_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* HobbyOS Phase-1 libc: stdlib.h subset (posix.md Phase 1). Implementations
   * in src/libc/src/stdlib.c, host-compiled as hb_* for glibc comparison. */

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647 /* 2^31-1, same as glibc */

  /* The only locale is "C", so a multibyte character is always exactly one
   * byte (C99 7.20.7.1). GNU sources transcribed into the sysroot read this
   * to pick their single-byte code paths. */
#define MB_CUR_MAX 1

  /* Heap (implementations in src/user/malloc.c, linked as user_malloc.o) */
  void *malloc(size_t size);
  void *calloc(size_t nmemb, size_t size);
  void *realloc(void *ptr, size_t size);
  void free(void *ptr);

  /* Numeric conversions (strtol family: full base handling, endptr, ERANGE
   * clamping; underscores between digits accepted as a glibc extension). */
  int atoi(const char *nptr);
  long atol(const char *nptr);
  long long atoll(const char *nptr);

  long strtol(const char *nptr, char **endptr, int base);
  long long strtoll(const char *nptr, char **endptr, int base);
  unsigned long strtoul(const char *nptr, char **endptr, int base);
  unsigned long long strtoull(const char *nptr, char **endptr, int base);

  int abs(int j);
  long labs(long j);
  long long llabs(long long j);

  void qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));
  void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
                int (*compar)(const void *, const void *));

  int rand(void);
  void srand(unsigned int seed);

  /* In-memory environment table (POSIX environ; the plan defers kernel-side
   * env to Phase 8). Copy strings so setenv/putenv own them. */
  extern char **environ;
  char *getenv(const char *name);
  int setenv(const char *name, const char *value, int overwrite);
  int putenv(char *string); /* gnu-style "NAME=VALUE", takes ownership */
  int unsetenv(const char *name);

  void abort(void) __attribute__((noreturn));
  void exit(int status) __attribute__((noreturn));

  /* atexit: handlers run LIFO inside exit() (libc.c calls back into the
   * stdlib.c registry via __hb_atexit_run).  Returns 0, or -1 if the
   * table is full. */
  int atexit(void (*function)(void));

  /* POSIX/GNU temp-file creation: replace the "XXXXXX" trailer of
   * template with unique letters and open O_CREAT|O_EXCL|O_RDWR. */
  int mkstemp(char *template);
  int mkostemp(char *template, int flags);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_STDLIB_H */
