#ifndef HOBBYOS_STDIO_H
#define HOBBYOS_STDIO_H

/* HobbyOS Phase-2 sysroot: stdio.h — printf family + FILE layer.
 * No float support (userland builds with -mgeneral-regs-only; the plan
 * defers strtod/FPU), so %f/%e/%g/%a and long double are intentionally
 * absent.  The ' (thousands) flag and %m are also not implemented yet. */

#include <stddef.h>
#include <stdarg.h>

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

/* --- FILE layer --- */
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputs(const char *s, FILE *stream);
int putchar(int c);
int getchar(void);
int puts(const char *s);
int fflush(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fileno(FILE *stream);

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

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_STDIO_H */
