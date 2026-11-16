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
#include <sys/stat.h>
#include <unistd.h>

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

int main(int argc, char **argv) {
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
