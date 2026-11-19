/* tools/cp/p2/pfxls.c - Gate P2 diagnostic (lane C2-wine2): a musl Linux
 * program the native winep2 launcher runs between steps, so the state of the
 * prefix on the compat tmpfs (which a native process cannot see) lands in the
 * serial log.  Prints "[PFX] ..." rows: entry counts of the prefix's system32,
 * a few key files (size, first bytes), .update-timestamp vs wine.inf mtime. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static void one(const char *p) {
  struct stat st;
  int r = stat(p, &st);
  unsigned char b[4] = {0};
  int fd = open(p, O_RDONLY);
  long n = fd >= 0 ? read(fd, b, 4) : -1;
  if (fd >= 0) close(fd);
  printf("[PFX] %s stat=%d size=%ld mode=%o open=%d read=%ld head=%02x%02x%02x%02x\n", p, r,
         r ? -1L : (long)st.st_size, r ? 0u : (unsigned)st.st_mode, fd >= 0, n, b[0], b[1], b[2], b[3]);
}

/* "-c DIR": count DIR's entries with getdents64 the way glibc readdir does
   (and with a tiny buffer, to exercise the continuation cookie). */
static int count_dir(const char *dir) {
  DIR *d = opendir(dir);
  long n = 0;
  struct dirent *e;
  char last[300] = "";
  while (d && (e = readdir(d))) { n++; snprintf(last, sizeof last, "%s", e->d_name); }
  if (d) closedir(d);
  printf("[PFX] count %s open=%d entries=%ld last=%s\n", dir, d != 0, n, last);
  fflush(stdout);
  return 0;
}

/* "-h FILE...": FNV-1a of the file read three ways -- sequential 64 KiB
   read(), pread() of 4 KiB pages in reverse order, and a MAP_PRIVATE mmap --
   so a corrupted NFS READ shows as a hash that differs from the host's (and
   from the other two ways). */
static uint64_t fnv(const unsigned char *b, size_t n, uint64_t h) {
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
  return h;
}
static void hash_file(const char *f) {
  struct stat st;
  int fd = open(f, O_RDONLY);
  if (fd < 0 || fstat(fd, &st) != 0) { printf("[PFX] hash %s open-fail\n", f); return; }
  size_t n = (size_t)st.st_size;
  unsigned char *a = malloc(n + 1), *b = malloc(n + 1);
  size_t got = 0;
  long r;
  while (got < n && (r = read(fd, a + got, n - got < 65536 ? n - got : 65536)) > 0) got += (size_t)r;
  uint64_t h1 = fnv(a, got, 0xcbf29ce484222325ULL);
  for (long off = (long)((n - 1) & ~4095UL); off >= 0; off -= 4096) {
    size_t len = n - (size_t)off < 4096 ? n - (size_t)off : 4096;
    if (pread(fd, b + off, len, off) != (long)len) memset(b + off, 0xEE, len);
  }
  uint64_t h2 = fnv(b, n, 0xcbf29ce484222325ULL);
  void *m = mmap(0, n, PROT_READ, MAP_PRIVATE, fd, 0);
  uint64_t h3 = m == MAP_FAILED ? 0 : fnv(m, n, 0xcbf29ce484222325ULL);
  long firstdiff = -1;
  for (size_t i = 0; i < got && i < n; i++) if (a[i] != b[i]) { firstdiff = (long)i; break; }
  printf("[PFX] hash %s size=%zu read=%016llx pread=%016llx mmap=%016llx seq_vs_pread_firstdiff=%ld\n", f, n,
         (unsigned long long)h1, (unsigned long long)h2, (unsigned long long)h3, firstdiff);
  fflush(stdout);
  if (m != MAP_FAILED) munmap(m, n);
  free(a); free(b); close(fd);
}

int main(int argc, char **argv) {
  if (argc > 3 && !strcmp(argv[1], "-H")) { /* -H N files: N concurrent hashers */
    int n = atoi(argv[2]);
    for (int k = 0; k < n; k++)
      if (fork() == 0) {
        for (int i = 3; i < argc; i++) hash_file(argv[i]);
        _exit(0);
      }
    for (int k = 0; k < n; k++) wait(0);
    return 0;
  }
  if (argc > 2 && !strcmp(argv[1], "-h")) {
    for (int i = 2; i < argc; i++) hash_file(argv[i]);
    return 0;
  }
  if (argc > 2 && !strcmp(argv[1], "-c")) {
    for (int i = 2; i < argc; i++) count_dir(argv[i]);
    return 0;
  }
  const char *pfx = argc > 1 ? argv[1] : "/tmp/pfx";
  char p[512];
  const char *dirs[] = {"drive_c/windows/system32", "drive_c/windows", "dosdevices", "", 0};
  for (int i = 0; dirs[i]; i++) {
    snprintf(p, sizeof p, "%s/%s", pfx, dirs[i]);
    DIR *d = opendir(p);
    long n = 0, nz = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
      n++;
      if (i == 3 || i == 2) printf("[PFX] %s entry %s\n", p, e->d_name);
    }
    if (d) closedir(d);
    printf("[PFX] dir %s open=%d entries=%ld\n", p, d != 0, n);
    (void)nz;
  }
  const char *files[] = {"drive_c/windows/system32/kernel32.dll", "drive_c/windows/system32/kernelbase.dll",
                         "drive_c/windows/system32/ntdll.dll", "drive_c/windows/system32/start.exe",
                         "drive_c/windows/system32/cmd.exe", "dosdevices/c:/windows/system32/kernel32.dll",
                         ".update-timestamp", "system.reg", 0};
  for (int i = 0; files[i]; i++) {
    snprintf(p, sizeof p, "%s/%s", pfx, files[i]);
    one(p);
  }
  snprintf(p, sizeof p, "%s/.update-timestamp", pfx);
  char t[64] = {0};
  int fd = open(p, O_RDONLY);
  if (fd >= 0) { (void)!read(fd, t, sizeof t - 1); close(fd); }
  struct stat st;
  int r = stat("/CP/WINE/share/wine/wine.inf", &st);
  printf("[PFX] update-timestamp=\"%s\" wine.inf stat=%d mtime=%ld\n", strtok(t, "\n") ? t : "", r, r ? -1L : (long)st.st_mtime);
  fflush(stdout);
  return 0;
}
