/* src/user/linux_probe_e2churn.c - Linux-ABI probe: ext2 write-back churn
 * beside poll/select/pipe traffic, for minutes (lane C3-ext2w; docs/cp-p8.md
 * stops 8 and 9: guest wedges on integ5 around the ext2 flusher task's exit
 * and an NVMe wait).
 *
 * argv[1] the volume (default /EXT; SKIP without it), argv[2] seconds
 * (default 150).  At once:
 *   W  4 writer processes, all in the same 600 ms windows: write a small
 *      file, fsync, close, rename over a shared name, then wait for the next
 *      window -- the cache goes dirty and clean again every window, which is
 *      what made the old flusher task exit and respawn (once per window,
 *      ~250 times a run); one writer also streams a 16 MiB "shader cache"
 *      file with 64 KiB writes + fdatasync
 *   P  3 pipe pairs ping-ponging 1-byte messages through poll() / select()
 *      / blocking read, the X relay pattern
 * Every child reports progress counters through a pipe; the parent checks
 * that every child kept making progress until the end (a stall is a FAIL
 * row naming the child), then that the files read back.
 * Rows "[LP] PASS|FAIL|info ...", last row "[LP] done rc=<fails>".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NW 4
#define NP 3

static int fails;
static char D[260];

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
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void mk(uint8_t *b, size_t n, uint32_t tag) {
  for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(tag + i * 13u);
}
static int ok_bytes(const uint8_t *b, size_t n) {
  if (!n) return 1;
  uint8_t t = b[0];
  for (size_t i = 0; i < n; i++)
    if (b[i] != (uint8_t)(t + i * 13u)) return 0;
  return 1;
}

/* progress report: one byte per unit of work, to the parent's pipe */
static int repfd;
static void tick(uint8_t who) { (void)!write(repfd, &who, 1); }

static uint64_t g_t0;

static void writer(int idx, uint64_t t_end) {
  static uint8_t b[65536];
  char p[300], q[300];
  uint32_t r = 977u * (uint32_t)(idx + 1);
  uint64_t big_off = 0;
  int bigfd = -1;
  if (idx == 0) {
    snprintf(p, sizeof p, "%s/vc.cache", D);
    bigfd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  }
  for (uint32_t i = 0; now_ms() < t_end; i++) {
    r = r * 1103515245u + 12345u;
    snprintf(p, sizeof p, "%s/w%d.tmp", D, idx);
    snprintf(q, sizeof q, "%s/f%u", D, (r >> 8) % 16);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      size_t n = 100 + (r >> 4) % 20000;
      mk(b, n, (uint8_t)(r >> 16));
      (void)!write(fd, b, n);
      (void)fsync(fd); /* clean again right away */
      close(fd);
      (void)rename(p, q);
    }
    if (bigfd >= 0 && big_off < (16u << 20)) {
      mk(b, sizeof b, (uint8_t)(big_off >> 16));
      (void)!write(bigfd, b, sizeof b);
      big_off += sizeof b;
      (void)fdatasync(bigfd);
    }
    /* sync(2), not only fsync: fsync leaves the superblock's counts for the
       next full commit, so only a sync makes the cache wholly clean */
    if (idx == NW - 1) { usleep(30000); sync(); }
    tick((uint8_t)idx);
    /* every writer waits for the next 600 ms window boundary: the cache is
       clean for ~0.5 s of every window, then dirty again -- the old flusher
       exited and respawned once per window */
    uint64_t ph = (now_ms() - g_t0) % 600u;
    usleep((useconds_t)((600u - ph) * 1000u + (uint32_t)idx * 7000u));
  }
  if (bigfd >= 0) close(bigfd);
  _exit(0);
}

/* A pipe pair: the child echoes; the parent side (also a child here) sends
   and waits with poll, select or a plain read, in turn. */
static void pinger(int idx, uint64_t t_end) {
  int a[2], b[2];
  if (pipe(a) || pipe(b)) _exit(3);
  pid_t e = fork();
  if (e == 0) {
    char c;
    close(a[1]);
    close(b[0]);
    while (read(a[0], &c, 1) == 1)
      if (write(b[1], &c, 1) != 1) break;
    _exit(0);
  }
  close(a[0]);
  close(b[1]);
  char c = 'x';
  for (uint32_t i = 0; now_ms() < t_end; i++) {
    if (write(a[1], &c, 1) != 1) break;
    if ((i % 3) == 0) {
      struct pollfd pf = {b[0], POLLIN, 0};
      if (poll(&pf, 1, 5000) != 1) break;
    } else if ((i % 3) == 1) {
      fd_set s;
      FD_ZERO(&s);
      FD_SET(b[0], &s);
      struct timeval tv = {5, 0};
      if (select(b[0] + 1, &s, 0, 0, &tv) != 1) break;
    }
    if (read(b[0], &c, 1) != 1) break;
    if ((i & 63) == 0) tick((uint8_t)(NW + idx));
    if ((i & 255) == 0) usleep(2000);
  }
  close(a[1]);
  waitpid(e, 0, 0);
  _exit(0);
}

int main(int argc, char **argv) {
  const char *vol = argc > 1 ? argv[1] : "/EXT";
  int secs = argc > 2 ? atoi(argv[2]) : 150;
  struct stat st;
  if (stat(vol, &st) != 0 || !S_ISDIR(st.st_mode)) {
    out("[LP] SKIP %s absent: no data disk in this boot\n", vol);
    out("[LP] done rc=0\n");
    return 0;
  }
  snprintf(D, sizeof D, "%s/churn", vol);
  mkdir(D, 0755);
  out("[LP] info e2churn: %d writers (dirty/clean cycles + a 16 MiB cache file) and %d poll/select pipe pairs, %d s\n",
      NW, NP, secs);
  int rp[2];
  if (pipe(rp)) { ck(0, "pipe", errno); out("[LP] done rc=%d\n", fails); return 1; }
  uint64_t t0 = now_ms(), t_end = t0 + (uint64_t)secs * 1000u;
  g_t0 = t0;
  pid_t kids[NW + NP];
  for (int i = 0; i < NW + NP; i++) {
    pid_t p = fork();
    if (p == 0) {
      close(rp[0]);
      repfd = rp[1];
      if (i < NW) writer(i, t_end);
      else pinger(i - NW, t_end);
    }
    kids[i] = p;
  }
  close(rp[1]);
  fcntl(rp[0], F_SETFL, O_NONBLOCK);
  uint64_t last[NW + NP], cnt[NW + NP];
  for (int i = 0; i < NW + NP; i++) { last[i] = t0; cnt[i] = 0; }
  uint64_t worst_gap = 0;
  int worst_who = -1;
  int left = NW + NP;
  uint64_t next_row = t0 + 30000;
  while (left > 0 && now_ms() < t_end + 60000) {
    uint8_t buf[256];
    ssize_t n = read(rp[0], buf, sizeof buf);
    uint64_t t = now_ms();
    for (ssize_t k = 0; k < n; k++)
      if (buf[k] < NW + NP) {
        uint64_t g = t - last[buf[k]];
        if (g > worst_gap && t < t_end) { worst_gap = g; worst_who = buf[k]; }
        last[buf[k]] = t;
        cnt[buf[k]]++;
      }
    int stt;
    while (waitpid(-1, &stt, WNOHANG) > 0) left--;
    if (t >= next_row) {
      out("[LP] info e2churn t=%llus writers %llu %llu %llu %llu pipes %llu %llu %llu\n",
          (unsigned long long)((t - t0) / 1000), (unsigned long long)cnt[0], (unsigned long long)cnt[1],
          (unsigned long long)cnt[2], (unsigned long long)cnt[3], (unsigned long long)cnt[4],
          (unsigned long long)cnt[5], (unsigned long long)cnt[6]);
      next_row += 30000;
    }
    if (n <= 0) usleep(20000);
  }
  for (int i = 0; i < NW + NP; i++)
    if (left > 0) kill(kids[i], 9);
  ck(left == 0, "every writer and pipe pair finished (no wedge)", left);
  int idle = 0;
  for (int i = 0; i < NW + NP; i++)
    if (cnt[i] == 0) idle++;
  ck(idle == 0, "every child made progress", idle);
  ck(worst_gap < 20000, "no child stalled 20 s or more (worst gap ms, who)", (long)worst_gap * 100 + worst_who);
  /* the files read back as written */
  int badf = 0, nf = 0;
  static uint8_t rb[20200];
  for (int k = 0; k < 16; k++) {
    char p[300];
    snprintf(p, sizeof p, "%s/f%d", D, k);
    int fd = open(p, O_RDONLY);
    if (fd < 0) continue;
    nf++;
    ssize_t n = read(fd, rb, sizeof rb);
    close(fd);
    if (n < 0 || !ok_bytes(rb, (size_t)n)) badf++;
  }
  ck(nf > 0 && badf == 0, "every f* file holds one writer's bytes (files)", nf);
  char p[300];
  snprintf(p, sizeof p, "%s/vc.cache", D);
  if (stat(p, &st) == 0) out("[LP] info e2churn: cache file %lld bytes\n", (long long)st.st_size);
  sync();
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
