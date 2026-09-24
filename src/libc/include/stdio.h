#ifndef HOBBYOS_STDIO_H
#define HOBBYOS_STDIO_H

#include <sys/types.h>
#include <stdarg.h>
#include <stddef.h>

/* HobbyOS Phase-2 sysroot: stdio.h — printf family + FILE layer.
 * No float support (userland builds with -mgeneral-regs-only; the plan
 * defers strtod/FPU), so %f/%e/%g/%a and long double are intentionally
 * absent.  The ' (thousands) flag and %m are also not implemented yet. */

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* --- FILE --- */
  typedef struct __hb_FILE FILE;

  /* The FILE struct lives in file.c; only the opaque pointer is public. */

  extern FILE *stdin;
  extern FILE *stdout;
  extern FILE *stderr;

  /* --- printf family --- */
  int printf(const char *format, ...);
  int fprintf(FILE *stream, const char *format, ...);
  int sprintf(char *str, const char *format, ...);
  int snprintf(char *str, size_t size, const char *format, ...);
  int vprintf(const char *format, va_list ap);
  int vfprintf(FILE *stream, const char *format, va_list ap);
  int vsprintf(char *str, const char *format, va_list ap);
  int vsnprintf(char *str, size_t size, const char *format, va_list ap);
  /* scanf family: declared for source compatibility (host tests link glibc's;
     no on-device implementation yet, and no on-device program uses them). */
  int scanf(const char *format, ...);
  int fscanf(FILE *stream, const char *format, ...);
  int sscanf(const char *str, const char *format, ...);
  int vscanf(const char *format, va_list ap);
  int vfscanf(FILE *stream, const char *format, va_list ap);
  int vsscanf(const char *str, const char *format, va_list ap);

  /* --- FILE layer --- */
  FILE *fopen(const char *path, const char *mode);
  FILE *fdopen(int fd, const char *mode);
  int fclose(FILE *stream);
  size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
  size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
  int fgetc(FILE *stream);
  int fputc(int c, FILE *stream);
  int getc(FILE *stream);  /* == fgetc */
  int putc(int c, FILE *stream); /* == fputc */
  char *fgets(char *s, int size, FILE *stream);
  int fputs(const char *s, FILE *stream);
  int putchar(int c);
  int getchar(void);
  int puts(const char *s);
  int fflush(FILE *stream);
  int feof(FILE *stream);
  int ferror(FILE *stream);
  void clearerr(FILE *stream);
  int fileno(FILE *stream);
  int fseek(FILE *stream, off_t offset, int whence);
  long ftell(FILE *stream);
  void rewind(FILE *stream);
  int ungetc(int c, FILE *stream);
  int setvbuf(FILE *stream, char *buf, int mode, size_t size);
  void setbuf(FILE *stream, char *buf);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

  /* --- constants --- */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#ifndef EOF
#define EOF (-1)
#endif

  /* --- helpers --- */
  void perror(const char *s); /* prints "s: <strerror(errno)>" to stderr */

  /* --- line reading --- */
  ssize_t getline(char **lineptr, size_t *n, FILE *stream);
  ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);

  int rename(const char *oldpath, const char *newpath);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_STDIO_H */
