/* src/user/linux_probe_fatren.c - Linux-ABI probe: rename on the FAT volume
 * while other processes open, stat and read there (lane C3-fat, P9-8).
 *
 * P9-8: the X session wedged on fat_lock.  IceWM's fontconfig was inside
 * rename(2) ("...cache-12.NEW" -> "...cache-12" in /TMP/FCCACHE) holding
 * fat_lock, its CPU halted, and two other CPUs spun in ylock_acquire under
 * find_entry_in_root (an open) and fat16_read_direct_locked (a read).  This
 * probe recreates that traffic and fails, by name, when any worker stops
 * making progress, instead of hanging the boot silently.
 *
 * A plain musl x86_64 program: the reference on a Linux host, the measurement
 * in HobbyOS.  argv[1] = seconds (default 30), argv[2] = the FAT directory
 * (default "/" - the FAT16 boot volume for a compat process in HobbyOS),
 * argv[3] = an ext2 volume for the optional device-token pressure workers
 * (default "/EXT"; skipped when absent).  Rows "[LP] ...", last row
 * "[LP] done rc=<fails>".
 *
 *   R1 fontconfig's cache update, in a FAT subdirectory: create a long-named
 *      TMP file, write it, rename TMP -> .NEW, rename .NEW -> final (replacing
 *      the previous final) - 2 processes, each on its own names
 *   R2 an 8.3 rename in the FAT ROOT replacing an existing name (the root's
 *      own sector walk) - 1 process
 *   O1 open/read/close of root and subdirectory files, plus lookups of names
 *      that do not exist (a full root scan each) - 3 processes
 *   E1 (with an ext2 volume) write + fsync + unlink 64 KiB files: the ext2
 *      flusher and these writers share the one device token with the FAT
 *   W1 watchdog: every worker advances at least once every WEDGE_S seconds
 *   V1 afterwards: each final file holds its last generation's bytes, and no
 *      .TMP/.NEW is left behind
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NR 2      /* R1 workers (subdirectory, long names) */
#define NO 3      /* O1 workers */
#define NE 2      /* E1 workers (only with an ext2 volume) */
#define NW (NR + 1 + NO + NE)
#define WEDGE_S 20
#define BODY 2048 /* bytes per cache file: one cluster on the boot volume */

struct shared {
  volatile uint64_t ops[NW];   /* progress counter per worker */
  volatile int done[NW];       /* 1 finished, 2 failed */
  volatile uint32_t gen[NR];   /* last generation each R1 worker committed */
  volatile int stop;
  volatile uint32_t soft[NW];  /* soft (non-fatal) failures per worker */
  char why[NW][96];
};
static struct shared *S;
static int fails;

static void out(const char *fmt, ...) {
  char b[384];
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
static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}
static void wfail(int w, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(S->why[w], sizeof S->why[w], fmt, ap);
  va_end(ap);
  S->done[w] = 2;
  _exit(1);
}
/* A correctness failure that must not stop the traffic: the first one is kept
 * for the report, the worker keeps going (a wedge is the other half of P9-8). */
static void wsoft(int w, const char *fmt, ...) {
  if (S->soft[w]++ == 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S->why[w], sizeof S->why[w], fmt, ap);
    va_end(ap);
  }
}
static void join(char *dst, size_t n, const char *dir, const char *name) {
  size_t l = strlen(dir);
  snprintf(dst, n, "%s%s%s", dir, (l && dir[l - 1] == '/') ? "" : "/", name);
}
static void fill(char *b, uint32_t tag, int w) {
  for (int i = 0; i < BODY; i++) b[i] = (char)('a' + ((tag + (uint32_t)i + (uint32_t)w) % 26));
  memcpy(b, &tag, sizeof tag);
}
static int write_file(const char *p, const char *b, int n) {
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  int ok = write(fd, b, (size_t)n) == n;
  ok = (close(fd) == 0) && ok;
  return ok ? 0 : -1;
}

/* R1: the fontconfig sequence that was inside rename(2) when P9-8 wedged. */
static void worker_r1(int w, const char *sub, double t_end) {
  char tmp[256], nw[256], fin[200], name[120], body[BODY];
  snprintf(name, sizeof name, "d129258b8fb6c48f6c0317f05e00500%d-x86_64.cache-12", w);
  join(fin, sizeof fin, sub, name);
  uint32_t g = 0;
  while (now_s() < t_end && !S->stop) {
    g++;
    snprintf(tmp, sizeof tmp, "%s.TMP-%c%caAz8C", fin, 'a' + (int)(g % 26), 'a' + w);
    snprintf(nw, sizeof nw, "%s.NEW", fin);
    fill(body, g, w);
    S->ops[w]++;
    if (write_file(tmp, body, BODY) != 0) { wsoft(w, "R1 create errno=%d g=%u", errno, g); unlink(tmp); continue; }
    if (rename(tmp, nw) != 0) { wsoft(w, "R1 rename TMP->NEW errno=%d g=%u", errno, g); unlink(tmp); continue; }
    if (rename(nw, fin) != 0) { wsoft(w, "R1 rename NEW->final errno=%d g=%u", errno, g); unlink(nw); continue; }
    S->gen[w] = g;
  }
  S->done[w] = 1;
  _exit(0);
}

/* R2: 8.3 rename in the FAT root, replacing the previous name. */
static void worker_r2(int w, const char *root, double t_end) {
  char a[200], b[200], body[BODY];
  join(a, sizeof a, root, "FRN_A.TMP");
  join(b, sizeof b, root, "FRN_B.DAT");
  uint32_t g = 0;
  while (now_s() < t_end && !S->stop) {
    g++;
    fill(body, g, w);
    if (write_file(a, body, 512) != 0) wfail(w, "R2 create errno=%d", errno);
    if (rename(a, b) != 0) wfail(w, "R2 rename errno=%d g=%u", errno, g);
    S->ops[w]++;
  }
  S->done[w] = 1;
  _exit(0);
}

/* O1: the opens and reads that queued behind the rename. */
static void worker_o1(int w, const char *root, const char *sub, double t_end) {
  char p[200], buf[4096];
  const char *miss[] = {"NOSUCH1.TXT", "no-such-long-name-for-a-root-scan.bin", "FRN_X.DAT"};
  struct stat st;
  uint64_t k = 0;
  while (now_s() < t_end && !S->stop) {
    k++;
    /* a real file read in pieces (fat16_read / the direct reader) */
    join(p, sizeof p, root, "FRN_RO.BIN");
    int fd = open(p, O_RDONLY);
    if (fd < 0) wfail(w, "O1 open %s errno=%d", p, errno);
    ssize_t n, tot = 0;
    while ((n = read(fd, buf, (size_t)(1000 + (k % 3000)))) > 0) tot += n;
    close(fd);
    if (tot != 3 * 4096) wfail(w, "O1 short read %ld", (long)tot);
    /* lookups that walk the whole root */
    join(p, sizeof p, root, miss[k % 3]);
    if (stat(p, &st) == 0 && (k % 3) != 2) wfail(w, "O1 phantom %s", p);
    /* the subdirectory the renamers work in */
    if ((k & 3) == 0) {
      DIR *d = opendir(sub);
      if (d) {
        while (readdir(d)) {
        }
        closedir(d);
      }
    }
    S->ops[w]++;
  }
  S->done[w] = 1;
  _exit(0);
}

/* E1: keep the device token busy from the ext2 side (writes + the flusher). */
static void worker_e1(int w, const char *ext, double t_end) {
  char p[200], buf[65536];
  memset(buf, 'e' + w, sizeof buf);
  uint64_t k = 0;
  while (now_s() < t_end && !S->stop) {
    k++;
    snprintf(p, sizeof p, "%s/fatren-e%d-%llu.bin", ext, w, (unsigned long long)(k % 4));
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) wfail(w, "E1 open %s errno=%d", p, errno);
    if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) wfail(w, "E1 write errno=%d", errno);
    if ((k & 1) == 0) (void)fsync(fd);
    close(fd);
    if ((k & 3) == 3) unlink(p);
    S->ops[w]++;
  }
  for (int i = 0; i < 4; i++) {
    snprintf(p, sizeof p, "%s/fatren-e%d-%d.bin", ext, w, i);
    unlink(p);
  }
  S->done[w] = 1;
  _exit(0);
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 30;
  if (secs <= 0) secs = 30;
  const char *root = argc > 2 ? argv[2] : "/";
  const char *ext = argc > 3 ? argv[3] : "/EXT";
  char sub[200], p[200];
  join(sub, sizeof sub, root, "FATREN");

  S = mmap(0, sizeof *S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (S == MAP_FAILED) {
    out("[LP] FAIL mmap shared errno=%d\n[LP] done rc=1\n", errno);
    return 1;
  }
  memset((void *)S, 0, sizeof *S);

  struct stat st;
  int have_ext = (stat(ext, &st) == 0 && S_ISDIR(st.st_mode));
  if (have_ext) {
    /* writable?  (a read-only or absent data disk just skips E1) */
    snprintf(p, sizeof p, "%s/fatren-probe.chk", ext);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) have_ext = 0;
    else { close(fd); unlink(p); }
  }
  out("[LP] fatren probe (C3-fat, P9-8): %d s, FAT dir %s, sub %s, ext2 pressure %s\n",
      secs, root, sub, have_ext ? ext : "(none)");

  /* setup on the FAT volume */
  mkdir(sub, 0755);
  ck(stat(sub, &st) == 0 && S_ISDIR(st.st_mode), "S1 FAT subdirectory exists", 0);
  {
    char b[3 * 4096];
    for (int i = 0; i < (int)sizeof b; i++) b[i] = (char)i;
    join(p, sizeof p, root, "FRN_RO.BIN");
    ck(write_file(p, b, sizeof b) == 0, "S2 read-only target written", 3 * 4096);
  }

  int nw = NR + 1 + NO + (have_ext ? NE : 0);
  pid_t pids[NW];
  double t0 = now_s(), t_end = t0 + secs;
  for (int w = 0; w < nw; w++) {
    pid_t c = fork();
    if (c == 0) {
      if (w < NR) worker_r1(w, sub, t_end);
      else if (w == NR) worker_r2(w, root, t_end);
      else if (w < NR + 1 + NO) worker_o1(w, root, sub, t_end);
      else worker_e1(w, ext, t_end);
      _exit(0);
    }
    pids[w] = c;
    if (c < 0) {
      out("[LP] FAIL fork errno=%d\n", errno);
      fails++;
      nw = w;
      break;
    }
  }

  /* W1: watchdog.  The parent touches no filesystem while it watches. */
  uint64_t last[NW];
  double last_t[NW];
  for (int w = 0; w < nw; w++) { last[w] = 0; last_t[w] = t0; }
  int wedged = -1;
  double wedged_for = 0;
  for (;;) {
    struct timespec ts = {1, 0};
    nanosleep(&ts, 0);
    double t = now_s();
    int alive = 0;
    for (int w = 0; w < nw; w++) {
      if (S->done[w]) continue;
      alive++;
      if (S->ops[w] != last[w]) { last[w] = S->ops[w]; last_t[w] = t; }
      else if (t - last_t[w] >= WEDGE_S && wedged < 0) { wedged = w; wedged_for = t - last_t[w]; }
    }
    if (wedged >= 0 || !alive) break;
    if (t > t_end + 120) { wedged = -2; break; }
  }
  if (wedged != -1) {
    S->stop = 1;
    out("[LP] FAIL W1 worker %d made no progress for %.0f s (ops:", wedged, wedged_for);
    for (int w = 0; w < nw; w++) out(" %d=%llu%s", w, (unsigned long long)S->ops[w], S->done[w] ? "d" : "");
    out(") -- a filesystem wedge (P9-8 shape)\n");
    fails++;
    for (int w = 0; w < nw; w++) if (!S->done[w]) kill(pids[w], SIGKILL);
    out("[LP] done rc=%d\n", fails);
    _exit(fails);
  }
  for (int w = 0; w < nw; w++) {
    int stt = 0;
    waitpid(pids[w], &stt, 0);
    if (S->done[w] == 2) { out("[LP] FAIL worker %d: %s\n", w, S->why[w]); fails++; }
    else if (S->soft[w]) {
      out("[LP] FAIL worker %d: %u failed rounds, first: %s\n", w, S->soft[w], S->why[w]);
      fails++;
    }
  }
  {
    char line[300];
    int k = snprintf(line, sizeof line, "W1 every worker kept moving (ops:");
    for (int w = 0; w < nw && k < (int)sizeof line - 24; w++)
      k += snprintf(line + k, sizeof line - (size_t)k, " %d=%llu", w, (unsigned long long)S->ops[w]);
    snprintf(line + k, sizeof line - (size_t)k, ")");
    int moved = 1;
    for (int w = 0; w < nw; w++) if (!S->ops[w]) moved = 0;
    ck(moved, line, (long)(now_s() - t0));
  }

  /* V1: the final files hold the last committed generation; no leftovers. */
  int bad = 0;
  for (int w = 0; w < NR; w++) {
    char name[120], b[BODY], e[BODY];
    snprintf(name, sizeof name, "d129258b8fb6c48f6c0317f05e00500%d-x86_64.cache-12", w);
    join(p, sizeof p, sub, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0) { bad++; out("[LP] note V1 %s missing errno=%d\n", p, errno); continue; }
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    fill(e, S->gen[w], w);
    if (n != BODY || memcmp(b, e, BODY) != 0) {
      bad++;
      uint32_t tag = 0;
      memcpy(&tag, b, sizeof tag);
      out("[LP] note V1 %s n=%ld gen=%u want gen=%u\n", p, (long)n, tag, S->gen[w]);
    }
  }
  ck(bad == 0, "V1 every final cache file holds its last generation", bad);
  int left = 0;
  DIR *d = opendir(sub);
  if (d) {
    struct dirent *de;
    while ((de = readdir(d))) {
      if (strstr(de->d_name, ".TMP-") || strstr(de->d_name, ".NEW")) {
        left++;
        out("[LP] note V1 leftover %s\n", de->d_name);
      }
    }
    closedir(d);
  }
  ck(left == 0, "V1 no .TMP/.NEW left in the subdirectory", left);

  /* cleanup: leave the boot volume as it was */
  for (int w = 0; w < NR; w++) {
    char name[120];
    snprintf(name, sizeof name, "d129258b8fb6c48f6c0317f05e00500%d-x86_64.cache-12", w);
    join(p, sizeof p, sub, name);
    unlink(p);
  }
  join(p, sizeof p, root, "FRN_B.DAT"); unlink(p);
  join(p, sizeof p, root, "FRN_A.TMP"); unlink(p);
  join(p, sizeof p, root, "FRN_RO.BIN"); unlink(p);
  rmdir(sub);
  out("[LP] done rc=%d\n", fails);
  return fails;
}
