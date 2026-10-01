#ifndef HOBBYOS_STDLIB_H
#define HOBBYOS_STDLIB_H

#include <stddef.h>
#include <locale.h> /* locale_t for the *_l variants */

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

  /* ---- C99 integer division (P3.2: names <cstdlib> needs) ---- */
  typedef struct {
    int quot;
    int rem;
  } div_t;
  typedef struct {
    long quot;
    long rem;
  } ldiv_t;
  typedef struct {
    long long quot;
    long long rem;
  } lldiv_t;
  div_t div(int numer, int denom);
  ldiv_t ldiv(long numer, long denom);
  lldiv_t lldiv(long long numer, long long denom);

  /* ---- C99 float conversion (P3.2; src/libc/src/strtod.c) ---- */
  double atof(const char *nptr);
  double strtod(const char *nptr, char **endptr);
  float strtof(const char *nptr, char **endptr);
  long double strtold(const char *nptr, char **endptr);

  /* xlocale/P3.2 variants (C locale: the locale argument is ignored). */
  long strtol_l(const char *nptr, char **endptr, int base, locale_t loc);
  long long strtoll_l(const char *nptr, char **endptr, int base, locale_t loc);
  unsigned long strtoul_l(const char *nptr, char **endptr, int base,
                          locale_t loc);
  unsigned long long strtoull_l(const char *nptr, char **endptr, int base,
                                locale_t loc);
  double strtod_l(const char *nptr, char **endptr, locale_t loc);
  float strtof_l(const char *nptr, char **endptr, locale_t loc);
  long double strtold_l(const char *nptr, char **endptr, locale_t loc);

  /* ---- C11 aligned allocation (P3.2: libc++ operator new over-aligned
   * path / std::aligned_alloc).  Alignment must be a power of two and a
   * multiple of sizeof(void*); size need not be a multiple of alignment
   * (unlike C11's strict reading — the HobbyOS allocator handles any size,
   * matching glibc's extension). */
  void *aligned_alloc(size_t alignment, size_t size);

  /* ---- C11 quick exit (P3.2) ---- */
  int at_quick_exit(void (*function)(void));
  void quick_exit(int status) __attribute__((noreturn));

  /* ---- byte-string conversions (C99; declared here per C99 7.20) ---- */
  int mblen(const char *s, size_t n);
  int mbtowc(wchar_t *pwc, const char *s, size_t n);
  size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
  int wctomb(char *s, wchar_t wc);
  size_t wcstombs(char *dst, const wchar_t *src, size_t n);

  /* system(): declared for source compatibility (libc++ <cstdlib> needs
   * the name); not implemented on HobbyOS — no shell contract. */
  int system(const char *command);

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

  /* P6.3: build `environ` from the kernel environment blob (SYS_GETENV);
   * idempotent -- crt0 calls it before main(), getenv() re-tries lazily.
   * Implemented in src/user/libc.c (the one object every link flavour
   * carries). */
  void environ_init(void);

  void abort(void) __attribute__((noreturn));
  void exit(int status) __attribute__((noreturn));

  /* atexit: handlers run LIFO inside exit() (libc.c calls back into the
   * stdlib.c registry via __hb_atexit_run).  Returns 0, or -1 if the
   * table is full. */
  int atexit(void (*function)(void));

  /* POSIX/GNU temp-file creation: replace the "XXXXXX" trailer of
   * the name buffer with unique letters and open O_CREAT|O_EXCL|O_RDWR.
   * (The parameter is named tmpl, not template: 'template' is a C++
   * keyword and these declarations are inside an extern "C" block that
   * C++ translation units compile.  Parameter names are not part of the
   * ABI, so the rename is source-compatible for C callers.) */
  int mkstemp(char *tmpl);
  int mkostemp(char *tmpl, int flags);
  int mkstemps(char *tmpl, int suffixlen); /* GNU: XXXXXX before a suffix */

  /* POSIX pathname canonicalization: resolve to an absolute path with no
   * "." / ".." components.  This VFS has no symlinks, so the resolution is
   * purely textual; resolved==NULL allocates (free() it). */
  char *realpath(const char *path, char *resolved);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_STDLIB_H */
