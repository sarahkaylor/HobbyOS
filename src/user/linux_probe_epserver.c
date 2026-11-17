/* src/user/linux_probe_epserver.c - Linux-ABI probe: a wineserver-shaped
 * epoll server with clients that come and go (lane C3-ext2w3; the C0-console
 * batch's reg_gate lost wineserver pid 7 to "[RESUME-PREFLIGHT] pid=7
 * cs=0x0000800000000000 rip=0xFFFF800200C02000 ... -> QUARANTINE" right after
 * its "[EPOLLHW] pid=7 n=16" row, during wineboot -i).
 *
 * argv[1] seconds (default 90).  One server process keeps ONE epoll set with
 * a request pipe per live client (the wineserver layout); it grows past 16
 * and 32 entries as clients arrive, EPOLL_CTL_DELs a client on hangup, and
 * answers every request on the client's reply pipe.  The parent keeps 40
 * clients alive: each forks, sends 50..400 requests, checks every answer and
 * exits; a dead client is replaced at once, so the set churns all the time.
 * Some clients are threaded (2 threads sharing the pipes, as Wine clients
 * are).  Rows: the server answered until the end (a quarantined or wedged
 * server shows as clients that never get an answer), every answer was right,
 * and enough clients completed.  Last row "[LP] done rc=<fails>".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NCLI 40

static int fails;
static void out(const char *fmt, ...) {
  char b[400];
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

/* a new client: the server's fd numbers of its request and reply pipes */
struct hello { int req_rd, rep_wr; };

static int ctl[2];        /* clients -> server: hello records */
static int stats[2];      /* clients -> parent: one byte per finished client */

static void server(uint64_t t_end) {
  int ep = epoll_create1(0);
  struct epoll_event ev = {0};
  ev.events = EPOLLIN;
  ev.data.u64 = (uint64_t)-1;
  epoll_ctl(ep, EPOLL_CTL_ADD, ctl[0], &ev);
  int rep_of[4096];
  for (int i = 0; i < 4096; i++) rep_of[i] = -1;
  int live = 0, hw = 0;
  while (now_ms() < t_end + 5000) {
    struct epoll_event got[32];
    int n = epoll_wait(ep, got, 32, 200);
    for (int k = 0; k < n; k++) {
      if (got[k].data.u64 == (uint64_t)-1) { /* a new client: add its pipe */
        struct hello h;
        if (read(ctl[0], &h, sizeof h) == (ssize_t)sizeof h && h.req_rd >= 0 && h.req_rd < 4096) {
          rep_of[h.req_rd] = h.rep_wr;
          struct epoll_event e2 = {0};
          e2.events = EPOLLIN | EPOLLHUP;
          e2.data.u64 = (uint64_t)h.req_rd;
          if (epoll_ctl(ep, EPOLL_CTL_ADD, h.req_rd, &e2) == 0) {
            live++;
            if (live > hw) hw = live;
          }
        }
        continue;
      }
      int fd = (int)got[k].data.u64;
      uint32_t req;
      ssize_t r = read(fd, &req, sizeof req);
      if (r == (ssize_t)sizeof req) {
        uint32_t ans = req * 2654435761u + 7u;
        (void)!write(rep_of[fd], &ans, sizeof ans);
      } else if (r == 0 || (got[k].events & EPOLLHUP)) {
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, 0);
        close(fd);
        if (rep_of[fd] >= 0) close(rep_of[fd]);
        rep_of[fd] = -1;
        live--;
      }
    }
  }
  out("[LP] info epserver: server epoll high-water %d entries\n", hw);
  _exit(0);
}

struct cli { int req_wr, rep_rd, n, bad; pthread_mutex_t *mu; };
static void *cli_thread(void *a) {
  struct cli *c = a;
  for (int i = 0; i < c->n; i++) {
    uint32_t req = (uint32_t)rand(), ans = 0;
    pthread_mutex_lock(c->mu); /* one request in flight per pipe pair */
    int ok = write(c->req_wr, &req, sizeof req) == (ssize_t)sizeof req &&
             read(c->rep_rd, &ans, sizeof ans) == (ssize_t)sizeof ans;
    pthread_mutex_unlock(c->mu);
    if (!ok) { c->bad++; break; }
    if (ans != req * 2654435761u + 7u) c->bad++;
    usleep(3000); /* clients live long enough to overlap (the set grows) */
  }
  return 0;
}

int main(int argc, char **argv) {
  int secs = argc > 1 ? atoi(argv[1]) : 90;
  if (pipe(ctl) || pipe(stats)) { ck(0, "pipes", errno); out("[LP] done rc=%d\n", fails); return 1; }
  uint64_t t0 = now_ms(), t_end = t0 + (uint64_t)secs * 1000u;
  out("[LP] info epserver: %d clients at a time for %d s, one epoll set in the server\n", NCLI, secs);
  /* The parent pre-creates a pool of pipe pairs, forks the server (which
     inherits them all and keeps the server ends), then forks each client on
     one pair and tells the server over ctl which pair just came alive: the
     server's epoll set grows and shrinks as clients come and go. */
  enum { POOL = 220 }; /* 4 fds a pair: under the 1024 soft RLIMIT_NOFILE */
  static int rq[POOL][2], rp[POOL][2];
  int npool = 0;
  for (; npool < POOL; npool++)
    if (pipe(rq[npool]) || pipe(rp[npool])) break;
  pid_t srv = fork();
  if (srv == 0) {
    close(ctl[1]);
    for (int i = 0; i < npool; i++) { close(rq[i][1]); close(rp[i][0]); }
    server(t_end);
  }
  close(ctl[0]);
  /* the parent drops the server's ends; every client gets one pool pair */
  for (int i = 0; i < npool; i++) { close(rq[i][0]); close(rp[i][1]); }
  ck(npool >= NCLI * 4, "setup: pipe pool", npool);
  int next = 0, live = 0, done = 0, badc = 0;
  pid_t kids[POOL];
  int kidpair[POOL];
  int nk = 0;
  uint64_t last_done = now_ms(), worst = 0;
  while (now_ms() < t_end && next < npool) {
    while (live < NCLI && next < npool) {
      struct hello h = {rq[next][0], rp[next][1]}; /* the server's fd numbers */
      (void)!write(ctl[1], &h, sizeof h);
      int pr = next++;
      pid_t c = fork();
      if (c == 0) {
        srand((unsigned)(pr * 7919 + 1));
        pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        struct cli a = {rq[pr][1], rp[pr][0], 100 + (pr * 37) % 300, 0, &mu};
        struct cli b = a;
        b.n = a.n / 2;
        pthread_t t;
        int threaded = pr & 1;
        if (threaded) pthread_create(&t, 0, cli_thread, &b);
        cli_thread(&a);
        if (threaded) pthread_join(t, 0);
        _exit(a.bad + b.bad ? 3 : 0);
      }
      close(rq[pr][1]);
      close(rp[pr][0]);
      kids[nk] = c;
      kidpair[nk++] = pr;
      live++;
    }
    int st;
    pid_t w = waitpid(-1, &st, WNOHANG);
    if (w > 0) {
      live--;
      done++;
      if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) badc++;
      uint64_t t = now_ms();
      if (t - last_done > worst) worst = t - last_done;
      last_done = t;
    } else {
      usleep(10000);
      uint64_t t = now_ms();
      if (t - last_done > worst) worst = t - last_done;
      if (t - last_done > 30000) break; /* the server stopped answering */
    }
  }
  /* drain */
  uint64_t dl = now_ms() + 30000;
  while (live > 0 && now_ms() < dl) {
    int st;
    pid_t w = waitpid(-1, &st, WNOHANG);
    if (w > 0 && w != srv) {
      live--;
      done++;
      if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) badc++;
    } else {
      usleep(20000);
    }
  }
  for (int i = 0; i < nk; i++) kill(kids[i], 9);
  (void)kidpair;
  close(ctl[1]);
  int sst = 0;
  waitpid(srv, &sst, 0);
  ck(worst < 30000, "the server kept answering (worst gap between finished clients, ms)", (long)worst);
  ck(live == 0, "every client finished", live);
  ck(badc == 0, "every answer was right (bad clients)", badc);
  ck(done >= 48, "clients completed", done);
  /* (the high-water row above says whether the set crossed 16 and 32) */
  ck(WIFEXITED(sst) && WEXITSTATUS(sst) == 0, "the server exited normally", sst);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
