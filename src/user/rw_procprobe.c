/*
 * rw_procprobe.c — M6 launch plumbing probe: the three /proc entries the real
 * RimWorld binary touches, exercised under the Linux-ABI compat track by a
 * real glibc 2.31 program (built with the pinned runtime, like the M1 gate).
 *
 * What it proves, in order:
 *   1. readlink("/proc/self/exe") answers the LOADED EXECUTABLE (the path the
 *      kernel was asked to run), not the interpreter it entered (ld.so) and
 *      not a truncated display name.  The kernel's own serial line
 *      "elf loader: PT_INTERP mapped: interp_base=..." shows that ld.so really
 *      is the entry point, so the two facts together are the proof.
 *   2. /proc/self and /proc/self/cwd resolve like Linux.
 *   3. /proc/self/fd/N readlinks to the open file's path, and OPENING that
 *      link yields a fresh file description at offset 0 (the host trace shows
 *      639 such opens in 20 s: Unity/libudev re-reads a file after reading it
 *      to EOF, which dup() semantics would break).
 *   4. /proc/self/maps opens as a 0444 regular file of size 0 (measured host
 *      shape), fstat agrees, and read() returns Linux-format lines for the
 *      address space the kernel really mapped.
 *
 * Rows are "[PROC] ..."; the last is "[PROC] done failures=<n>".
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/auxv.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what) {
  printf("[PROC] %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

int main(void) {
  char b[512];
  ssize_t n;

  printf("[PROC] proc probe start pid=%d\n", (int)getpid());

  /* 1. /proc/self/exe ---------------------------------------------------- */
  n = readlink("/proc/self/exe", b, sizeof b - 1);
  if (n < 0) {
    printf("[PROC] readlink(/proc/self/exe) errno=%d\n", errno);
    check(0, "readlink /proc/self/exe");
  } else {
    b[n] = '\0';
    printf("[PROC] readlink(/proc/self/exe) = \"%s\" (len=%ld)\n", b, (long)n);
    check(b[0] == '/', "exe link is absolute");
    check(strstr(b, "ld-") == NULL && strstr(b, "LD-") == NULL,
          "exe link is the executable, not the interpreter");
  }
  {
    const char *execfn = (const char *)getauxval(AT_EXECFN);
    printf("[PROC] AT_EXECFN = \"%s\"\n", execfn ? execfn : "(null)");
  }

  /* 2. /proc/self and cwd ------------------------------------------------- */
  n = readlink("/proc/self", b, sizeof b - 1);
  if (n < 0) {
    printf("[PROC] readlink(/proc/self) errno=%d\n", errno);
    check(0, "readlink /proc/self is the pid");
  } else {
    b[n] = '\0';
    printf("[PROC] readlink(/proc/self) = \"%s\"\n", b);
    char want[32];
    snprintf(want, sizeof want, "%d", (int)getpid());
    check(strcmp(b, want) == 0, "readlink /proc/self == pid");
  }
  n = readlink("/proc/self/cwd", b, sizeof b - 1);
  if (n >= 0) {
    b[n] = '\0';
    printf("[PROC] readlink(/proc/self/cwd) = \"%s\"\n", b);
  } else {
    printf("[PROC] readlink(/proc/self/cwd) errno=%d\n", errno);
  }

  /* 3. /proc/self/fd/N: link target + re-open by path --------------------- */
  {
    int fd = open("/PROBE.TXT", O_RDONLY);
    printf("[PROC] open(/PROBE.TXT) = %d\n", fd);
    if (fd < 0) {
      check(0, "open /PROBE.TXT fixture");
    } else {
      char buf[64];
      ssize_t r1 = read(fd, buf, sizeof buf - 1);
      ssize_t r2 = read(fd, buf, sizeof buf - 1); /* read to EOF */
      printf("[PROC] first read=%ld second read=%ld\n", (long)r1, (long)r2);
      check(r1 > 0 && r2 == 0, "fixture read to EOF");

      char link[64];
      snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
      n = readlink(link, b, sizeof b - 1);
      if (n < 0) {
        printf("[PROC] readlink(%s) errno=%d\n", link, errno);
        check(0, "readlink /proc/self/fd/N");
      } else {
        b[n] = '\0';
        printf("[PROC] readlink(%s) = \"%s\"\n", link, b);
        check(strcmp(b, "/PROBE.TXT") == 0, "fd link is the opened path");
      }

      int rfd = open(link, O_RDONLY);
      printf("[PROC] open(%s) = %d\n", link, rfd);
      if (rfd < 0) {
        printf("[PROC] reopen errno=%d\n", errno);
        check(0, "re-open through /proc/self/fd/N");
      } else {
        ssize_t r3 = read(rfd, buf, sizeof buf - 1);
        buf[r3 > 0 ? r3 : 0] = '\0';
        printf("[PROC] reopened read=%ld \"%s\"\n", (long)r3, buf);
        /* Fresh description: offset 0, so the re-open sees the bytes the
           original fd had already consumed.  dup() would answer 0 here. */
        check(r3 > 0 && strncmp(buf, "PROC-FIXTURE", 12) == 0,
              "re-open starts at offset 0 (not dup semantics)");
        close(rfd);
      }
      close(fd);
    }
  }

  /* 4. /proc/self/maps --------------------------------------------------- */
  {
    struct stat st;
    int m = open("/proc/self/maps", O_RDONLY);
    printf("[PROC] open(/proc/self/maps) = %d\n", m);
    if (m < 0) {
      printf("[PROC] maps errno=%d\n", errno);
      check(0, "open /proc/self/maps");
    } else {
      if (fstat(m, &st) == 0) {
        printf("[PROC] fstat(maps) mode=0%o size=%ld\n",
               (unsigned)st.st_mode, (long)st.st_size);
        check((st.st_mode & S_IFMT) == S_IFREG && st.st_size == 0,
              "maps is a regular file of size 0");
      } else {
        check(0, "fstat /proc/self/maps");
      }
      char mbuf[1024];
      ssize_t m1 = read(m, mbuf, sizeof mbuf - 1);
      if (m1 <= 0) {
        check(0, "read /proc/self/maps");
      } else {
        mbuf[m1] = '\0';
        int lines = 0;
        for (ssize_t i = 0; i < m1; i++)
          if (mbuf[i] == '\n') lines++;
        printf("[PROC] maps read=%ld lines=%d\n", (long)m1, lines);
        /* print the first lines verbatim, one per row */
        int printed = 0;
        ssize_t start = 0;
        for (ssize_t i = 0; i < m1 && printed < 20; i++) {
          if (mbuf[i] == '\n') {
            mbuf[i] = '\0';
            printf("[PROC] maps: %s\n", mbuf + start);
            printed++;
            start = i + 1;
          }
        }
        check(lines > 0 && strchr(mbuf, '-') != NULL,
              "maps has Linux-format rows");
      }
      close(m);
    }
  }

  printf("[PROC] done failures=%d\n", failures);
  fflush(stdout);
  return failures ? 1 : 0;
}
