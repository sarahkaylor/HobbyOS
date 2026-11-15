/*
 * tools/jdk/lockhold.c -- the second PROCESS for LockProbe's cross-process
 * fcntl test.
 *
 * Why a native helper and not a second JVM: the guest's JVM takes ~a minute to
 * start under TCG, and the point of this process is only to hold an fcntl
 * record lock on one byte range and answer questions about it.  It exercises
 * the same kernel entry point the JVM does (syscall 72, F_SETLK/F_GETLK over
 * struct flock), so the lock table sees one owner either way.  What the JVM
 * supplies is the *other* side, which is what is under test.
 *
 * Protocol (all on stdout through the inherited fd 1, so the console log is
 * the transcript):
 *
 *   lockhold <path> <holdms> <stepfile|-> <answered|->
 *     1. open(path, O_RDWR|O_CREAT, 0666)
 *     2. F_SETLK F_WRLCK 0..EOF        -> "LOCKHOLD: locked rc=<n>"
 *     3. wait for <stepfile> to exist  (the Java side's cue)
 *     4. F_GETLK F_WRLCK 0..EOF        -> "LOCKHOLD: getlk type=<n> pid=<n>"
 *        F_UNLCK our own range
 *        F_SETLK F_WRLCK 0..EOF again -> "LOCKHOLD: relock rc=<n> errno=<n>"
 *        (rc==0 while the JVM believes it holds an exclusive lock proves the
 *         JVM's lock never reached the kernel table)
 *     5. F_UNLCK, close, exit 0
 *
 * An optional <holdms> deadline bounds step 3 so a wedged peer still ends the
 * run instead of hanging the boot.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int setlk(int fd, short type, off_t start, off_t len) {
  struct flock fl;
  memset(&fl, 0, sizeof fl);
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  fl.l_start = start;
  fl.l_len = len;
  return fcntl(fd, F_SETLK, &fl);
}

static int getlk(int fd, short type, off_t start, off_t len, struct flock *out) {
  memset(out, 0, sizeof *out);
  out->l_type = type;
  out->l_whence = SEEK_SET;
  out->l_start = start;
  out->l_len = len;
  if (fcntl(fd, F_GETLK, out) != 0) return -1;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("LOCKHOLD: usage <path> <holdms> [stepfile] [answered]\n");
    return 2;
  }
  const char *path = argv[1];
  long holdms = atol(argv[2]);
  const char *step = (argc > 3) ? argv[3] : "-";

  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    printf("LOCKHOLD: open failed errno=%d\n", errno);
    return 3;
  }
  printf("LOCKHOLD: pid=%d opened %s\n", (int)getpid(), path);
  fflush(stdout);

  int rc = setlk(fd, F_WRLCK, 0, 0);
  int e1 = errno;
  printf("LOCKHOLD: locked rc=%d errno=%d\n", rc, rc == 0 ? 0 : e1);
  fflush(stdout);

  /* Hand the JVM a moment to reach its own lock attempt before the cue
   * file is even looked for: the cue is created after the JVM's tryLock and
   * its blocking lock() have both returned. */
  if (strcmp(step, "-") != 0) {
    long waited = 0;
    while (access(step, F_OK) != 0) {
      if (holdms > 0 && waited >= holdms) {
        printf("LOCKHOLD: step timeout after %ldms\n", waited);
        fflush(stdout);
        break;
      }
      usleep(100 * 1000);
      waited += 100;
    }
    printf("LOCKHOLD: step seen after %ldms\n", waited);
    fflush(stdout);
  }

  struct flock g;
  if (getlk(fd, F_WRLCK, 0, 0, &g) == 0) {
    printf("LOCKHOLD: getlk type=%d pid=%d start=%ld len=%ld\n",
           (int)g.l_type, (int)g.l_pid, (long)g.l_start, (long)g.l_len);
  } else {
    printf("LOCKHOLD: getlk failed errno=%d\n", errno);
  }
  fflush(stdout);

  /* Drop OUR lock, then immediately try to take it again.  Nothing of ours
   * remains in the table, so a conflict here can only be the JVM's. */
  rc = setlk(fd, F_UNLCK, 0, 0);
  printf("LOCKHOLD: unlock rc=%d errno=%d\n", rc, rc == 0 ? 0 : errno);
  rc = setlk(fd, F_WRLCK, 0, 0);
  printf("LOCKHOLD: relock rc=%d errno=%d\n", rc, rc == 0 ? 0 : errno);
  fflush(stdout);

  setlk(fd, F_UNLCK, 0, 0);
  close(fd);
  printf("LOCKHOLD: done\n");
  fflush(stdout);
  return 0;
}
