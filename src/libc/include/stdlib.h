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

  void abort(void);
  void exit(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_STDLIB_H */
