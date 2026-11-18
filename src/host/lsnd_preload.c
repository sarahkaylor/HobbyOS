/* src/host/lsnd_preload.c -- LD_PRELOAD shim: the /dev/snd nodes served by the
 * kernel's ALSA code (src/kernel/linux_snd.c built with LSND_HOST_TEST) on
 * the host model of the device (src/host/lsnd_host_dev.c).  Used by
 * tools/cp/audio/host-alsa-test.sh to run an UNMODIFIED alsa-lib (and its
 * clients) against the kernel ABI without booting (docs/cp-audio.md §5).
 *
 * Every open under /dev/snd/ and /dev/aload is intercepted: the two nodes
 * the kernel has are served, every other one is ENOENT, so the host's real
 * sound devices are never opened.  A served fd is backed by /dev/null (so
 * fcntl/dup/close behave); its minor is kept in a table.  mmap of a served fd
 * fails (as the kernel's does); ioctl goes to linux_snd_ioctl(), a park is a
 * 1 ms sleep and a retry; poll on served fds is linux_snd_poll_probe(). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "../include/linux_snd.h"
#include "../include/linux_snd_params.h"
#include <stdlib.h>

/* LSND_TRACE=1: every HW_REFINE / HW_PARAMS with its input, output and result. */
static void dump_hw(const char *tag, const void *a) {
  const struct lsnd_hw_params *p = a;
  static const char *nm[12] = {"sbits", "fbits", "ch", "rate", "ptime", "psize",
                               "pbytes", "periods", "btime", "bsize", "bbytes", "tick"};
  fprintf(stderr, "[TRACE] %s acc=%#x fmt=%#x sub=%#x rmask=%#x cmask=%#x", tag, p->masks[0].bits[0],
          p->masks[1].bits[0], p->masks[2].bits[0], p->rmask, p->cmask);
  for (int k = 0; k < 12; k++) {
    const struct lsnd_interval *v = &p->intervals[k];
    if (v->min == 0 && v->max == 0xFFFFFFFFu && !v->flags) continue;
    fprintf(stderr, " %s%c%u,%u%c%s%s", nm[k], (v->flags & 1) ? '(' : '[', v->min, v->max,
            (v->flags & 2) ? ')' : ']', (v->flags & 4) ? "i" : "", (v->flags & 8) ? "E" : "");
  }
  fprintf(stderr, "\n");
}

void linux_snd_init(void);
static int minor_of[4096];
static int inited;

static void init_once(void) {
  if (inited) return;
  inited = 1;
  for (int i = 0; i < 4096; i++) minor_of[i] = -1;
  linux_snd_init();
}

static int served(int fd) { return fd >= 0 && fd < 4096 && inited && minor_of[fd] >= 0; }

static int snd_open(const char *path, int flags, int *handled) {
  *handled = 0;
  if (!path) return -1;
  if (strncmp(path, "/dev/snd/", 9) && strncmp(path, "/dev/aload", 10)) return -1;
  *handled = 1;
  init_once();
  int m = linux_snd_path_minor(path);
  if (m < 0) { errno = ENOENT; return -1; }
  int r = linux_snd_open(m, (uint32_t)flags);
  if (r < 0) { errno = -r; return -1; }
  int (*ro)(const char *, int, ...) = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
  int fd = ro("/dev/null", O_RDWR | (flags & (O_NONBLOCK | O_CLOEXEC)));
  if (fd < 0 || fd >= 4096) { linux_snd_release(m); errno = EMFILE; return -1; }
  minor_of[fd] = m;
  fprintf(stderr, "[SHIM] open %s -> fd %d (minor %d)\n", path, fd, m);
  return fd;
}

#define OPEN_WRAP(name)                                         \
  int name(const char *path, int flags, ...) {                  \
    int h, mode = 0;                                            \
    if (flags & O_CREAT) {                                      \
      va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); \
    }                                                           \
    int fd = snd_open(path, flags, &h);                         \
    if (h) return fd;                                           \
    int (*r)(const char *, int, ...) = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, #name); \
    return r(path, flags, mode);                                \
  }
OPEN_WRAP(open)
OPEN_WRAP(open64)
int __open_2(const char *path, int flags) { return open(path, flags); }
int __open64_2(const char *path, int flags) { return open(path, flags); }
int openat(int dfd, const char *path, int flags, ...) {
  int h, mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
  int fd = snd_open(path, flags, &h);
  if (h) return fd;
  int (*r)(int, const char *, int, ...) = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat");
  return r(dfd, path, flags, mode);
}

int close(int fd) {
  int (*r)(int) = (int (*)(int))dlsym(RTLD_NEXT, "close");
  if (served(fd)) {
    int m = minor_of[fd];
    minor_of[fd] = -1;
    linux_snd_release(m);
  }
  return r(fd);
}

int ioctl(int fd, unsigned long req, ...) {
  va_list ap;
  va_start(ap, req);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (!served(fd)) {
    int (*r)(int, unsigned long, ...) = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    return r(fd, req, arg);
  }
  int fl = fcntl(fd, F_GETFL);
  int tr = getenv("LSND_TRACE") && ((uint32_t)req == 0xC2604110u || (uint32_t)req == 0xC2604111u);
  if (tr) dump_hw((uint32_t)req == 0xC2604110u ? "REFINE in " : "PARAMS in ", arg);
  for (;;) {
    int r = linux_snd_ioctl(0, minor_of[fd], (uint32_t)fl, (uint32_t)req, (uint64_t)(uintptr_t)arg);
    if (tr && r != LSND_PARKED) {
      fprintf(stderr, "[TRACE]   -> r=%d\n", r);
      if (r == 0) dump_hw("       out", arg);
    }
    if (r == LSND_PARKED) {
      struct timespec t = {0, 1000000};
      nanosleep(&t, 0);
      continue;
    }
    if (r < 0) { errno = -r; return -1; }
    return r;
  }
}

static void *snd_mmap(void *a, size_t l, int p, int f, int fd, off_t o, const char *name) {
  if (served(fd)) { errno = ENXIO; return MAP_FAILED; }
  void *(*r)(void *, size_t, int, int, int, off_t) =
      (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, name);
  return r(a, l, p, f, fd, o);
}
void *mmap(void *a, size_t l, int p, int f, int fd, off_t o) { return snd_mmap(a, l, p, f, fd, o, "mmap"); }
void *mmap64(void *a, size_t l, int p, int f, int fd, off_t o) { return snd_mmap(a, l, p, f, fd, o, "mmap64"); }

int poll(struct pollfd *fds, nfds_t n, int timeout) {
  int (*r)(struct pollfd *, nfds_t, int) = (int (*)(struct pollfd *, nfds_t, int))dlsym(RTLD_NEXT, "poll");
  int any = 0;
  for (nfds_t i = 0; i < n; i++) any |= served(fds[i].fd);
  if (!any) return r(fds, n, timeout);
  for (int waited = 0;; waited++) {
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
      fds[i].revents = 0;
      if (!served(fds[i].fd)) continue;
      int rd, wr, er;
      linux_snd_poll_probe(minor_of[fds[i].fd], &rd, &wr, &er);
      if (rd && (fds[i].events & POLLIN)) fds[i].revents |= POLLIN;
      if (wr && (fds[i].events & POLLOUT)) fds[i].revents |= POLLOUT;
      if (er) fds[i].revents |= POLLERR;
      if (fds[i].revents) ready++;
    }
    if (ready || (timeout >= 0 && waited >= timeout)) return ready;
    struct timespec t = {0, 1000000};
    nanosleep(&t, 0);
  }
}
