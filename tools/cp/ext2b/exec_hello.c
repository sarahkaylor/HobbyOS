/* tools/cp/ext2b/exec_hello.c — the glibc 2.31 programs the cp/ext2b gate
 * stores ON the ext2 volume and runs from there (docs/cp-ext2b.md section 5).
 * Built twice by tools/cp/ext2_gate.sh against the pinned glibc 2.31 sysroot:
 *   hs  -static-pie                       (the loader reads the image from ext2)
 *   hd  -pie, PT_INTERP and RUNPATH /EXT/lpx/glibc (ld.so and libc.so.6 are
 *       read and mmap'd from ext2 as well)
 * argv[1] = tag ("static" | "dyn").  Writes <root>/lpx/out/<tag>.txt:
 *   "ok <tag> pid=<n> libc=<version> libcpath=<where libc.so.6 was mapped from>"
 * and prints one "[LP] exec ..." row.  The main probe (linux_probe_ext2b.c)
 * waits for the marker. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char libcpath[512] = "static";

static int cb(struct dl_phdr_info *i, size_t sz, void *d) {
  (void)sz;
  (void)d;
  if (i->dlpi_name && strstr(i->dlpi_name, "libc.so"))
    snprintf(libcpath, sizeof libcpath, "%s", i->dlpi_name);
  return 0;
}

int main(int argc, char **argv) {
  const char *tag = argc > 1 ? argv[1] : "unknown";
  dl_iterate_phdr(cb, 0);
  char line[800], path[300], tmp[320];
  snprintf(line, sizeof line, "ok %s pid=%d libc=%s libcpath=%s argv0=%s\n", tag,
           (int)getpid(), gnu_get_libc_version(), libcpath, argv[0]);
  snprintf(path, sizeof path, "/EXT/lpx/out/%s.txt", tag);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  int ok = fd >= 0 && write(fd, line, strlen(line)) == (ssize_t)strlen(line);
  if (fd >= 0) close(fd);
  ok = ok && rename(tmp, path) == 0;
  printf("[LP] exec %s %s", ok ? "marker" : "MARKER-FAILED", line);
  fflush(stdout);
  return ok ? 0 : 1;
}
