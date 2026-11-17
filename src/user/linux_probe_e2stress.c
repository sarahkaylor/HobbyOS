/* src/user/linux_probe_e2stress.c - Linux-ABI probe: many processes at once in
 * one ext2 directory, the way Wine clients and racing wineservers use a
 * prefix (lane C3-ext2w; docs/cp-p8.md stop 6: "[YLOCK-WEDGE] site=ext2_lock"
 * with two Wine processes starting together on /EXT/pfx).
 *
 * argv[1] the volume (default /EXT; every row SKIPs without it).  NPROC
 * worker processes (each with 2 extra threads) run for SECS seconds in
 * <vol>/st, all at the same moment (a pipe start barrier), mixing:
 *   - create / write / fsync / rename-over / unlink of shared names
 *   - fcntl F_SETLK / F_GETLK / F_SETLKW on one shared lock file (wineserver)
 *   - getdents64 of the shared directory into freshly mmapped buffers
 *   - read() of an ext2 file INTO an ext2 MAP_PRIVATE mapping not yet touched
 *     (the copy faults the page in through the ext2 page cache) and write()
 *     FROM one (the trap layer's prefault reads it through the same cache)
 *   - MAP_SHARED stores + msync(MS_SYNC), ftruncate, stat, fchdir round trips
 *   - a thread doing all of that while its process calls exit_group, and
 *     racing "servers" that try the lock, lose and exit at once
 * The parent waits (bounded); every child must exit 0 (or by its own
 * exit_group), and afterwards the directory must list, every surviving file
 * must read back as written, and the volume must take a final fsync.
 * Rows "[LP] PASS|FAIL|info ...", last row "[LP] done rc=<fails>".
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NPROC 6
#define SECS 20
#define NNAMES 24

static int fails;
static char R[200], D[260];

static void out(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}
static void ck(int ok, const char *what, long v) {
  out("[LP] %s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
  if (!ok) fails++;
}
static const char *PD(const char *fmt, ...) {
  static __thread char b[4][400];
  static __thread int k;
  char leaf[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(leaf, sizeof leaf, fmt, ap);
  va_end(ap);
  k = (k + 1) & 3;
  snprintf(b[k], sizeof b[k], "%s/%s", D, leaf);
  return b[k];
}
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
/* file content: every byte of a file names its writer and generation */
static void mk(uint8_t *b, size_t n, uint32_t tag) {
  for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(tag + i * 7u);
}
static int ok_bytes(const uint8_t *b, size_t n) {
  if (n == 0) return 1;
  uint32_t tag = b[0];
  for (size_t i = 0; i < n; i++)
    if (b[i] != (uint8_t)(tag + i * 7u)) return 0;
  return 1;
}

static uint64_t t_end;
static int me;

static void lock_dance(int fd, int blocking) {
  struct flock fl = {0};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = me % 4;
  fl.l_len = 1;
  if (fcntl(fd, blocking ? F_SETLKW : F_SETLK, &fl) == 0) {
    struct flock q = fl;
    q.l_start = (me + 1) % 4;
    (void)fcntl(fd, F_GETLK, &q);
    fl.l_type = F_UNLCK;
    (void)fcntl(fd, F_SETLK, &fl);
  }
}

/* One round of everything; `salt` varies names and sizes. */
static void round_ops(uint32_t salt, int lockfd, int dirfd, int rootfd) {
  static __thread uint8_t buf[40000];
  uint32_t r = salt * 2654435761u + (uint32_t)me * 40503u;
  int a = (int)(r % NNAMES), b = (int)((r >> 8) % NNAMES);
  size_t n = 1 + (r >> 4) % sizeof buf;
  /* create + write + fsync + rename over a shared name */
  int fd = open(PD("t%d.%u", me, salt & 7), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    mk(buf, n, (uint8_t)(r >> 16));
    (void)!write(fd, buf, n);
    if ((r & 3) == 0) (void)fsync(fd);
    close(fd);
    (void)rename(PD("t%d.%u", me, salt & 7), PD("n%d", a));
  }
  if ((r & 7) == 3) (void)unlink(PD("n%d", b));
  lock_dance(lockfd, (r & 15) == 5);
  /* getdents into a fresh anonymous mapping (first touch inside the call) */
  {
    char *g = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g != MAP_FAILED) {
      int d = open(D, O_RDONLY | O_DIRECTORY);
      if (d >= 0) {
        while (syscall(SYS_getdents64, d, g, 65536) > 0) {}
        close(d);
      }
      munmap(g, 65536);
    }
  }
  /* read() into an untouched ext2 MAP_PRIVATE mapping, write() from one */
  {
    int sfd = open(PD("n%d", b), O_RDONLY);
    int mfd = open(PD("map%d", me % 3), O_RDWR);
    if (sfd >= 0 && mfd >= 0) {
      uint8_t *m = mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_PRIVATE, mfd, 0);
      if (m != MAP_FAILED) {
        (void)!read(sfd, m + 4096 * (r % 4), 8192);
        int o = open(PD("w%d", me), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (o >= 0) {
          (void)!write(o, m + 16384, 8192);
          close(o);
        }
        munmap(m, 32768);
      }
      uint8_t *s = mmap(0, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
      if (s != MAP_FAILED) {
        s[(r >> 3) % 16384] = (uint8_t)r;
        (void)msync(s, 16384, (r & 1) ? MS_SYNC : MS_ASYNC);
        munmap(s, 16384);
      }
    }
    if (sfd >= 0) close(sfd);
    if (mfd >= 0) close(mfd);
  }
  /* stat, fchdir round trip, ftruncate */
  struct stat st;
  (void)stat(PD("n%d", a), &st);
  if (fchdir(dirfd) == 0) {
    (void)stat("lock", &st);
    (void)fchdir(rootfd);
  }
  fd = open(PD("n%d", a), O_RDWR);
  if (fd >= 0) {
    if ((r & 31) == 9) (void)ftruncate(fd, 0);
    close(fd);
  }
}

static void *thread_main(void *arg) {
  int lockfd = open(PD("lock"), O_RDWR);
  int dirfd = open(D, O_RDONLY | O_DIRECTORY);
  int rootfd = open("/", O_RDONLY | O_DIRECTORY);
  for (uint32_t i = (uint32_t)(uintptr_t)arg * 1000u; now_ms() < t_end; i++)
    round_ops(i, lockfd, dirfd, rootfd);
  return 0;
}

/* A worker process: two threads plus itself, until the deadline. */
static void worker(int idx, int startfd) {
  me = idx;
  char c;
  (void)!read(startfd, &c, 1); /* the start barrier */
  pthread_t t1, t2;
  pthread_create(&t1, 0, thread_main, (void *)(uintptr_t)1);
  pthread_create(&t2, 0, thread_main, (void *)(uintptr_t)2);
  int lockfd = open(PD("lock"), O_RDWR);
  int dirfd = open(D, O_RDONLY | O_DIRECTORY);
  int rootfd = open("/", O_RDONLY | O_DIRECTORY);
  uint32_t i = 0;
  while (now_ms() < t_end) round_ops(i++, lockfd, dirfd, rootfd);
  if (idx & 1) _exit(0); /* exit_group with both threads mid-syscall */
  pthread_join(t1, 0);
  pthread_join(t2, 0);
  _exit(0);
}

/* A racing "wineserver": take the lock or exit at once (server/request.c
   acquire_lock: EAGAIN -> exit(2)), several at the same moment. */
static void server_race(int idx, int startfd) {
  me = idx;
  char c;
  (void)!read(startfd, &c, 1);
  int fd = open(PD("lock"), O_RDWR);
  struct flock fl = {0};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_len = 1;
  if (fd < 0 || fcntl(fd, F_SETLK, &fl) != 0) _exit(2);
  /* the winner "loads the registry": a big read into a fresh buffer */
  int h = open(PD("hive"), O_RDONLY);
  if (h >= 0) {
    char *b = malloc(2u << 20);
    if (b) (void)!read(h, b, 2u << 20);
    close(h);
  }
  usleep(200000);
  _exit(0);
}

int main(int argc, char **argv) {
  snprintf(R, sizeof R, "%s", argc > 1 ? argv[1] : "/EXT");
  struct stat st;
  if (stat(R, &st) != 0 || !S_ISDIR(st.st_mode)) {
    out("[LP] SKIP %s absent: no data disk in this boot\n", R);
    out("[LP] done rc=0\n");
    return 0;
  }
  snprintf(D, sizeof D, "%s/st", R);
  mkdir(D, 0755);
  /* fixtures: the lock file, three mappable files, a 1.8 MB "hive" */
  static uint8_t big[1 << 16];
  int fd = open(PD("lock"), O_RDWR | O_CREAT, 0644);
  if (fd >= 0) close(fd);
  for (int k = 0; k < 3; k++) {
    fd = open(PD("map%d", k), O_RDWR | O_CREAT | O_TRUNC, 0644);
    mk(big, 32768, (uint8_t)k);
    if (fd >= 0) { (void)!write(fd, big, 32768); close(fd); }
  }
  fd = open(PD("hive"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  for (int k = 0; fd >= 0 && k < 28; k++) { mk(big, sizeof big, (uint8_t)k); (void)!write(fd, big, sizeof big); }
  if (fd >= 0) close(fd);
  sync();
  out("[LP] info e2stress: %d workers x 3 threads + 2 x 4 racing servers, %d s, dir %s\n",
      NPROC, SECS, D);

  int sp[2];
  if (pipe(sp) != 0) { ck(0, "pipe", errno); out("[LP] done rc=%d\n", fails); return 1; }
  t_end = now_ms() + SECS * 1000u;
  pid_t kids[NPROC + 8];
  int nk = 0;
  for (int i = 0; i < NPROC; i++) {
    pid_t p = fork();
    if (p == 0) { close(sp[1]); worker(i, sp[0]); }
    if (p > 0) kids[nk++] = p;
  }
  for (int i = 0; i < 4; i++) {
    pid_t p = fork();
    if (p == 0) { close(sp[1]); server_race(100 + i, sp[0]); }
    if (p > 0) kids[nk++] = p;
  }
  close(sp[0]);
  char go[NPROC + 4];
  memset(go, 'g', sizeof go);
  (void)!write(sp[1], go, sizeof go); /* everyone starts at once */
  /* a second server race mid-run */
  usleep(SECS * 500000);
  int sp2[2];
  if (pipe(sp2) == 0) {
    for (int i = 0; i < 4; i++) {
      pid_t p = fork();
      if (p == 0) { close(sp2[1]); server_race(200 + i, sp2[0]); }
      if (p > 0) kids[nk++] = p;
    }
    close(sp2[0]);
    (void)!write(sp2[1], go, 4);
    close(sp2[1]);
  }
  close(sp[1]);
  int bad = 0, left = nk;
  uint64_t deadline = now_ms() + (SECS + 60) * 1000u;
  while (left > 0 && now_ms() < deadline) {
    int stt = 0;
    pid_t w = waitpid(-1, &stt, WNOHANG);
    if (w > 0) {
      left--;
      int code = WIFEXITED(stt) ? WEXITSTATUS(stt) : 1000 + WTERMSIG(stt);
      if (code != 0 && code != 2) { bad++; out("[LP] info child %d status %d\n", (int)w, code); }
    } else {
      usleep(100000);
    }
  }
  ck(left == 0, "every worker and racing server finished (no wedge)", left);
  ck(bad == 0, "no child died of a signal or failed", bad);
  /* the directory and its files are sane */
  DIR *d = opendir(D);
  int names = 0, badf = 0;
  struct dirent *de;
  while (d && (de = readdir(d))) {
    if (de->d_name[0] != 'n') continue;
    names++;
    int f = open(PD("%s", de->d_name), O_RDONLY);
    if (f < 0) { badf++; continue; }
    static uint8_t rb[40000];
    ssize_t n = read(f, rb, sizeof rb);
    close(f);
    if (n < 0 || !ok_bytes(rb, (size_t)n)) badf++;
  }
  if (d) closedir(d);
  ck(d != 0 && names > 0, "the shared directory lists", names);
  ck(badf == 0, "every surviving n* file reads back as one writer wrote it", badf);
  int s = open(PD("lock"), O_RDWR);
  ck(s >= 0 && fsync(s) == 0, "the volume takes a final fsync", errno);
  if (s >= 0) close(s);
  sync();
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
