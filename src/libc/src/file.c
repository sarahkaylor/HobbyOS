/*
 * HobbyOS Phase-2 sysroot: file.c — the FILE layer over fds.
 *
 * Design: reads are buffered (512 B) so fgetc/fgets are cheap; writes are
 * unbuffered (the console/file write path is already row-buffered and
 * immediate errors are easier to surface).  stdin/stdout/stderr are static
 * FILEs whose fd fields are fixed at definition time (0/1/2) — a zeroed
 * bss FILE would make stdout write to fd 0 and lose all output.
 *
 * Everything here calls the fd wrappers (read/write/open/close) from
 * user/libc.c, which resolves at link time from libc.a.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "malloc.h"

struct __hb_FILE {
  int fd;
  int mode;                  /* 0 unused, 'r', 'w', 'a' */
  unsigned char *rbuf;       /* read buffer or NULL */
  size_t rsize, rpos, rlen;  /* capacity, cursor, valid bytes */
  off_t fdpos;               /* absolute offset of the fd read cursor */
  int pushback;              /* -1 = none, else one pushed-back char */
  int eof;
  int err;
};

#define HB_FBUF_SIZE 512

static FILE stdio_files[3] = {
  { .fd = 0, .mode = 'r', .pushback = -1 }, /* stdin */
  { .fd = 1, .mode = 'w', .pushback = -1 }, /* stdout */
  { .fd = 2, .mode = 'w', .pushback = -1 }, /* stderr */
};                      /* NOTE: must bear the real fd from birth — a
                           zeroed bss FILE would make stdout write to fd 0.
                           pushback must start at -1 too: 0 would be served
                           as a phantom NUL byte by the first fgetc/fread. */

static void hb_file_init(FILE *f, int fd) {
  f->fd = fd;
  f->mode = 'r';
  f->rbuf = NULL;
  f->rsize = 0;
  f->rpos = 0;
  f->rlen = 0;
  f->fdpos = 0;
  f->pushback = -1;
  f->eof = 0;
  f->err = 0;
}

FILE *stdin = &stdio_files[0];
FILE *stdout = &stdio_files[1];
FILE *stderr = &stdio_files[2];

/* Lazy: establish the rbuf for reads on first use. */
static int hb_file_ensure_buf(FILE *f) {
  if (f->rbuf)
    return 0;
  f->rbuf = malloc(HB_FBUF_SIZE);
  if (!f->rbuf) {
    f->err = 1;
    return -1;
  }
  f->rsize = HB_FBUF_SIZE;
  f->rpos = 0;
  f->rlen = 0;
  return 0;
}

static ssize_t hb_file_fill(FILE *f) {
  ssize_t n;
  if (f->rpos < f->rlen) {
    /* refill eagerly: move leftover to front and read more */
    size_t rem = f->rlen - f->rpos;
    if (rem) memmove(f->rbuf, f->rbuf + f->rpos, rem);
    f->rpos = 0;
    f->rlen = rem;
    n = read(f->fd, f->rbuf + rem, f->rsize - rem);
    if (n > 0) {
      f->rlen += (size_t)n;
      f->fdpos += n;
    }
    if (n <= 0) f->eof = (n == 0);
    return n;
  }
  f->rpos = 0;
  f->rlen = 0;
  /* The statically initialized stdin/stdout/stderr FILEs go through this
     path without ever visiting fdopen/fopen, so their rbuf is still NULL
     here.  Reading into NULL would fail the kernel's user-window check
     (-EFAULT, "Bad address") — establish the buffer lazily just like
     fdopen does. */
  if (hb_file_ensure_buf(f) != 0)
    return -1;
  n = read(f->fd, f->rbuf, f->rsize);
  if (n > 0) {
    f->rlen = (size_t)n;
    f->fdpos += n;   /* fd cursor moved past the freshly-buffered data */
  } else if (n == 0) f->eof = 1;
  else if (n < 0) f->err = 1;
  return n;
}

FILE *fdopen(int fd, const char *mode) {
  FILE *f;
  if (fd < 0)
    return NULL;
  f = malloc(sizeof(FILE));
  if (!f)
    return NULL;
  hb_file_init(f, fd);
  if (mode && *mode)
    f->mode = *mode;
  if (f->mode == 'w' || f->mode == 'a')
    ; /* writes unbuffered; nothing to set up */
  else if (hb_file_ensure_buf(f) != 0) {
    free(f);
    return NULL;
  }
  return f;
}

FILE *fopen(const char *path, const char *mode) {
  int flags = O_RDONLY;
  FILE *f;
  if (!path || !mode)
    return NULL;

  switch (*mode) {
  case 'r':
    flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
    break;
  case 'w':
    flags = (mode[1] == '+') ? (O_RDWR | O_CREAT | O_TRUNC)
                             : (O_WRONLY | O_CREAT | O_TRUNC);
    break;
  case 'a':
    flags = (mode[1] == '+') ? (O_RDWR | O_CREAT | O_APPEND)
                             : (O_WRONLY | O_CREAT | O_APPEND);
    break;
  default:
    errno = EINVAL;
    return NULL;
  }

  {
    int fd = open(path, flags);
    if (fd < 0)
      return NULL;
    f = fdopen(fd, mode);
    if (!f)
      close(fd);
    return f;
  }
}

int fclose(FILE *f) {
  int r = 0;
  if (!f)
    return EOF;
  if (f >= stdio_files && f <= &stdio_files[2])
    return 0;              /* never close the standard streams */
  r = close(f->fd);
  if (f->rbuf)
    free(f->rbuf);
  free(f);
  return r;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
  size_t total = size * nmemb;
  size_t got = 0;
  unsigned char *p = ptr;

  if (total == 0)
    return 0;
  if (!(f->mode == 'r' || f->mode == 'a')) {
    f->err = 1;
    return 0;
  }

  if (f->pushback != -1) {
    p[0] = (unsigned char)f->pushback;  /* one byte, not one item */
    f->pushback = -1;
    got++;
    p++;
    total--;
  }
  if (f->rpos < f->rlen) {
    size_t avail = f->rlen - f->rpos;
    size_t take = avail < total ? avail : total;
    memcpy(p, f->rbuf + f->rpos, take);
    f->rpos += take;
    got += take;
    p += take;
  }
  while (got < total && !f->eof) {
    ssize_t n = read(f->fd, p, total - got);
    if (n > 0) {
      got += (size_t)n;
      p += n;
    } else if (n == 0) {
      f->eof = 1;
      break;
    } else {
      f->err = 1;
      break;
    }
  }
  return size ? got / size : 0;
}

int fgetc(FILE *f) {
  unsigned char c;
  if (f->pushback != -1) {
    c = (unsigned char)f->pushback;
    f->pushback = -1;
    return c;
  }
  if (f->rpos >= f->rlen) {
    ssize_t n;
    if (f->eof)
      return EOF;
    n = hb_file_fill(f);
    if (n <= 0)
      return EOF;
  }
  c = f->rbuf[f->rpos++];
  return c;
}

char *fgets(char *s, int size, FILE *f) {
  int i = 0;
  if (size <= 0)
    return NULL;
  while (i < size - 1) {
    int c = fgetc(f);
    if (c == EOF) {
      if (i == 0)
        return NULL;
      break;
    }
    s[i++] = (char)c;
    if (c == '\n')
      break;
  }
  s[i] = '\0';
  return s;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
  size_t total = size * nmemb;
  size_t done = 0;

  if (total == 0)
    return 0;
  /* Kernel writes (notably pipes) may accept less than the full request;
     keep going so a partial write never silently drops the tail. */
  while (done < total) {
    ssize_t n = write(f->fd, (const char *)ptr + done, total - done);
    if (n <= 0) {
      f->err = 1;
      break;
    }
    done += (size_t)n;
  }
  return size ? done / size : 0;
}

int fputc(int c, FILE *f) {
  unsigned char uc = (unsigned char)c;
  if (write(f->fd, &uc, 1) == 1)
    return uc;
  f->err = 1;
  return EOF;
}

int fputs(const char *s, FILE *f) {
  size_t len = strlen(s);
  if (len && write(f->fd, s, len) != (ssize_t)len) {
    f->err = 1;
    return EOF;
  }
  return 0;
}

int putchar(int c) { return fputc(c, stdout); }
int getchar(void)  { return fgetc(stdin); }
int getc(FILE *f)  { return fgetc(f); }
int putc(int c, FILE *f) { return fputc(c, f); }

int puts(const char *s) {
  if (fputs(s, stdout) == EOF)
    return EOF;
  if (fputc('\n', stdout) == EOF)
    return EOF;
  return 0;
}

int fflush(FILE *f) {
  (void)f;                   /* unbuffered writes: nothing pending */
  return 0;
}

/* --- seek / ungetc / buffering-mode --- */

int fseek(FILE *f, off_t offset, int whence) {
  off_t r;
  /* The fd cursor already sits at fdpos (ahead of unread buffered data).
     Reposition it so the user-visible position lands at the target. */
  if (whence == SEEK_CUR) {
    /* user position = fdpos - slack; move by -(slack) first */
    off_t slack = (off_t)(f->rlen - f->rpos)
    + (f->pushback != -1 ? 1 : 0);
    r = lseek(f->fd, offset - slack, SEEK_CUR);
  } else {
    r = lseek(f->fd, offset, whence);
  }
  if (r < 0)
    return -1;
  f->fdpos = r;
  f->rpos = f->rlen = 0;
  f->pushback = -1;
  f->eof = 0;
  f->err = 0;
  return 0;
}

long ftell(FILE *f) {
  /* user position = fd cursor - buffered-but-unconsumed bytes */
  long pos = (long)(f->fdpos - (off_t)(f->rlen - f->rpos));
  if (f->pushback != -1)
    pos--;
  return pos;
}

void rewind(FILE *f) {
  (void)fseek(f, 0, SEEK_SET);
}

/* True when the stream was opened for writing ('w'/'a'): mirrors
 * glibc's __fwriting, which gnulib's fwriting() maps onto (see the
 * fwriting.h shim in src/user/sed). */
int hb_fp_is_writing(FILE *f) {
  return f != 0 && (f->mode == 'w' || f->mode == 'a');
}

/* Bytes buffered and not yet written out.  Writes here are unbuffered
 * (fwrite goes straight to the fd), so nothing is ever pending. */
size_t __fpending(FILE *f) {
  (void)f;
  return 0;
}

/* Clear end-of-file and error flags (sed calls this when rewinding the
 * hold space stream across lines). */
void clearerr(FILE *f) {
  if (f == 0) return;
  f->eof = 0;
  f->err = 0;
}

int ungetc(int c, FILE *f) {
  if (c == EOF || f->pushback != -1)
    return EOF;
  f->pushback = (unsigned char)c;
  return c;
}

/* Buffering is deliberately immediate (unbuffered writes); accept the
   mode markers and return success without changing behavior. */
int setvbuf(FILE *f, char *buf, int mode, size_t size) {
  (void)f; (void)buf; (void)mode; (void)size;
  return 0;
}

void setbuf(FILE *f, char *buf) {
  (void)f; (void)buf;
}

int feof(FILE *f)   { return f->eof; }
int ferror(FILE *f) { return f->err; }
int fileno(FILE *f) { return f->fd; }

/* --- printf family over FILE --- */
static int hb_fbuf_print(FILE *f, const char *fmt, va_list ap) {
  int need, written;
  char stackbuf[512];
  char *buf = stackbuf;

  va_list ap2;
  va_copy(ap2, ap);
  need = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (need < 0)
    return -1;
  if ((size_t)need >= sizeof stackbuf) {
    buf = malloc((size_t)need + 1);
    if (!buf)
      return -1;
  }
  written = vsnprintf(buf, (size_t)need + 1, fmt, ap);
  if (fwrite(buf, 1, (size_t)written, f) != (size_t)written)
    written = -1;
  if (buf != stackbuf)
    free(buf);
  return written;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
  return hb_fbuf_print(f, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) {
  return hb_fbuf_print(stdout, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = hb_fbuf_print(f, fmt, ap);
  va_end(ap);
  return r;
}

int printf(const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = hb_fbuf_print(stdout, fmt, ap);
  va_end(ap);
  return r;
}

void perror(const char *s) {
  if (s && *s)
    fprintf(stderr, "%s: %s\n", s, strerror(errno));
  else
    fprintf(stderr, "%s\n", strerror(errno));
}

/* --- getline/getdelim: every text tool wants these --- */
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f) {
  size_t used = 0;
  char *buf;

  if (!lineptr || !n) {
    errno = EINVAL;
    return -1;
  }
  if (!*lineptr) {
    *n = 128;
    *lineptr = malloc(*n);
    if (!*lineptr) {
      *n = 0;
      return -1;
    }
  }
  buf = *lineptr;

  for (;;) {
    int c = fgetc(f);
    if (c == EOF) {
      if (used == 0 && ferror(f))
        return -1;
      break;
    }
    if (used + 1 >= *n) {
      size_t nn = *n * 2;
      char *nb = realloc(buf, nn);
      if (!nb)
        return -1;
      buf = nb;
      *lineptr = buf;
      *n = nn;
    }
    buf[used++] = (char)c;
    if (c == delim)
      break;
  }
  buf[used] = '\0';
  return (ssize_t)used;
}

ssize_t getline(char **lineptr, size_t *n, FILE *f) {
  return getdelim(lineptr, n, '\n', f);
}
