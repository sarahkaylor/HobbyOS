/*
 * src/user/linux_probe_pipestorm.c - C1-k4: blocking IPC under SMP load, the
 * shape of Wine's client/server traffic (docs/cp-k4.md §1).
 *
 * ORDINARY musl Linux program (no HobbyOS header): the same static-PIE ELF runs
 * on the reference kernel (strix, Linux) and in the HobbyOS guest
 * (tools/gate/t3_linux_probe.sh pipestorm, 8 vCPUs).  Every row is ONE
 * write(2).  Each row runs in its own forked child with a deadline: a lost
 * wakeup shows as "HANG" (the child is SIGKILLed) instead of wedging the probe.
 * The last row is "[LP] done rc=<fails>".
 *
 * Rows (all graded):
 *  R1 thr_pingpong   6 thread pairs, 1-byte tokens over two pipes
 *  R2 proc_pingpong  4 forked children ping-pong with the row process
 *  R3 poll_fanin     24 client threads, one poll() server thread
 *  R4 select_fanin   16 client threads, one select() server thread
 *  R5 epoll_fanin    48 client threads (> 32 fds), one epoll server thread
 *  R6 wine_proto     wineserver's protocol: 6 processes x 4 threads send
 *                    writev(64-byte header + 0..3000 bytes) on a request pipe,
 *                    a single-threaded epoll server reads them NON-BLOCKING
 *                    (header, then data, resuming on EAGAIN) and answers with
 *                    writev(header + 0..3000 bytes); every 8th request is a
 *                    "select" answered later through a third (wait) pipe
 *  R7 bigwrite       one blocking write() of 100000 bytes and one writev() of
 *                    70000 bytes each return the full count (Linux pipe_write)
 *  R8 nonblock       O_NONBLOCK: an empty read is EAGAIN at once; capacity
 *                    >= 4096; a write that does not fit PIPE_BUF is EAGAIN
 *  R9 unix_pingpong  6 thread pairs over AF_UNIX socketpairs (read/write)
 *  R10 efd_pingpong  6 thread pairs over two eventfds each
 *  R11 close_wake    a reader parked in read() sees EOF when the last writer
 *                    closes; a writer parked on a full pipe sees EPIPE
 *  R12 sig_read      a reader parked in read() / recv() keeps getting
 *                    SA_RESTART signals while data trickles in: every byte
 *                    arrives, in order
 *  R13 kill_busy     30 times: a 13-thread child (4 spinning, 4 in 1 ms
 *                    nanosleep loops, 5 parked in read()) is SIGKILLed; the
 *                    parent reaps WTERMSIG 9 and the machine stays up
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROW_DEADLINE_MS 30000

static void out(const char *fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Read exactly n bytes (EINTR retried).  Returns n, or what it got. */
static long rfull(int fd, void *b, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, (char *)b + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return (long)got;
    got += (size_t)r;
  }
  return (long)got;
}

static volatile int row_bad; /* per row process */
static char row_why[160];
static pid_t row_pid;
static int kid_fd = -1; /* write end: helpers a row forks (killed on HANG) */
static void note_kid(pid_t k) {
  if (kid_fd >= 0) (void)!write(kid_fd, &k, sizeof k);
}
static void bad(const char *fmt, ...) {
  if (__sync_fetch_and_add(&row_bad, 1) == 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(row_why, sizeof row_why, fmt, ap);
    va_end(ap);
    /* printed at once: a row that then hangs is killed before it reports,
       and a forked helper's report cannot reach the row's pipe at all */
    out("[LP] info %s%d: %s\n", getpid() != row_pid ? "child " : "row ", (int)getpid(), row_why);
  }
}

/* ---- R1 / R9 / R10: ping-pong pairs ------------------------------------ */

#define PAIRS 6
#define PP_N 4000
enum { PP_PIPE, PP_UNIX, PP_EFD };
struct pp {
  int kind;
  int a_r, a_w, b_r, b_w; /* ping writes a_w, pong reads a_r; pong writes b_w */
  long done;
};

static int pp_send(struct pp *p, int fd, unsigned v) {
  if (p->kind == PP_EFD) {
    uint64_t x = v + 1; /* eventfd: counter value carries the token */
    return write(fd, &x, 8) == 8 ? 0 : -1;
  }
  unsigned char c = (unsigned char)v;
  return write(fd, &c, 1) == 1 ? 0 : -1;
}
static int pp_recv(struct pp *p, int fd, unsigned *v) {
  if (p->kind == PP_EFD) {
    uint64_t x = 0;
    ssize_t r;
    do r = read(fd, &x, 8); while (r < 0 && errno == EINTR);
    if (r != 8) return -1;
    *v = (unsigned)(x - 1);
    return 0;
  }
  unsigned char c;
  if (rfull(fd, &c, 1) != 1) return -1;
  *v = c;
  return 0;
}

static void *pp_ping(void *vp) {
  struct pp *p = vp;
  for (long i = 0; i < PP_N; i++) {
    unsigned want = p->kind == PP_EFD ? (unsigned)(i & 0xFFFF) : (unsigned)(i & 0xFF);
    if (pp_send(p, p->a_w, want)) { bad("ping send i=%ld errno=%d", i, errno); return 0; }
    unsigned got;
    if (pp_recv(p, p->b_r, &got)) { bad("ping recv i=%ld errno=%d", i, errno); return 0; }
    unsigned exp = p->kind == PP_EFD ? ((want * 7u) & 0xFFFF) : ((want * 7u) & 0xFF);
    if (got != exp) { bad("ping token i=%ld got=%u want=%u", i, got, exp); return 0; }
    p->done = i + 1;
  }
  return 0;
}
static void *pp_pong(void *vp) {
  struct pp *p = vp;
  for (long i = 0; i < PP_N; i++) {
    unsigned got;
    if (pp_recv(p, p->a_r, &got)) { bad("pong recv i=%ld errno=%d", i, errno); return 0; }
    unsigned m = p->kind == PP_EFD ? 0xFFFF : 0xFF;
    if (got != (unsigned)(i & m)) { bad("pong token i=%ld got=%u", i, got); return 0; }
    if (pp_send(p, p->b_w, (got * 7u) & m)) { bad("pong send i=%ld", i); return 0; }
  }
  return 0;
}

static int row_pairs(int kind) {
  struct pp pp[PAIRS];
  pthread_t th[PAIRS * 2];
  for (int i = 0; i < PAIRS; i++) {
    pp[i].kind = kind;
    pp[i].done = 0;
    if (kind == PP_PIPE) {
      int a[2], b[2];
      if (pipe(a) || pipe(b)) { bad("pipe errno=%d", errno); return 1; }
      pp[i].a_r = a[0]; pp[i].a_w = a[1]; pp[i].b_r = b[0]; pp[i].b_w = b[1];
    } else if (kind == PP_UNIX) {
      int s[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, s)) { bad("socketpair errno=%d", errno); return 1; }
      pp[i].a_w = s[0]; pp[i].a_r = s[1]; pp[i].b_w = s[1]; pp[i].b_r = s[0];
    } else {
      int e1 = eventfd(0, 0), e2 = eventfd(0, 0);
      if (e1 < 0 || e2 < 0) { bad("eventfd errno=%d", errno); return 1; }
      pp[i].a_w = pp[i].a_r = e1; pp[i].b_w = pp[i].b_r = e2;
    }
  }
  for (int i = 0; i < PAIRS; i++) {
    pthread_create(&th[2 * i], 0, pp_pong, &pp[i]);
    pthread_create(&th[2 * i + 1], 0, pp_ping, &pp[i]);
  }
  for (int i = 0; i < PAIRS * 2; i++) pthread_join(th[i], 0);
  return 0;
}

static int r1(void) { return row_pairs(PP_PIPE); }
static int r9(void) { return row_pairs(PP_UNIX); }
static int r10(void) { return row_pairs(PP_EFD); }

/* ---- R2: forked ping-pong ---------------------------------------------- */

#define R2_KIDS 4
#define R2_N 2000
static int r2(void) {
  int a[R2_KIDS][2], b[R2_KIDS][2];
  pid_t kid[R2_KIDS];
  for (int k = 0; k < R2_KIDS; k++) {
    if (pipe(a[k]) || pipe(b[k])) { bad("pipe errno=%d", errno); return 1; }
    kid[k] = fork();
    if (kid[k] < 0) { bad("fork errno=%d", errno); return 1; }
    note_kid(kid[k]);
    if (kid[k] == 0) {
      for (long i = 0; i < R2_N; i++) {
        unsigned char c;
        if (rfull(a[k][0], &c, 1) != 1 || c != (unsigned char)i) _exit(3);
        c = (unsigned char)(c ^ 0x5A);
        if (write(b[k][1], &c, 1) != 1) _exit(4);
      }
      _exit(0);
    }
  }
  for (long i = 0; i < R2_N; i++) {
    for (int k = 0; k < R2_KIDS; k++) {
      unsigned char c = (unsigned char)i;
      if (write(a[k][1], &c, 1) != 1) { bad("write kid=%d i=%ld", k, i); return 1; }
    }
    for (int k = 0; k < R2_KIDS; k++) {
      unsigned char c;
      if (rfull(b[k][0], &c, 1) != 1 || c != (unsigned char)((unsigned char)i ^ 0x5A)) {
        bad("reply kid=%d i=%ld", k, i);
        return 1;
      }
    }
  }
  for (int k = 0; k < R2_KIDS; k++) {
    int st = 0;
    waitpid(kid[k], &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad("kid %d status 0x%x", k, st);
  }
  return 0;
}

/* ---- R3 / R4 / R5: fan-in servers --------------------------------------- */

#define FAN_MAX 48
#define FAN_PER 400
enum { FAN_POLL, FAN_SELECT, FAN_EPOLL };
struct fan {
  int mode, n;
  int req[FAN_MAX][2], rep[FAN_MAX][2];
  int idx;
};
static struct fan fan;

static void *fan_client(void *v) {
  int i = (int)(intptr_t)v;
  for (int k = 0; k < FAN_PER; k++) {
    uint32_t m[16];
    for (int j = 0; j < 16; j++) m[j] = (uint32_t)(i * 100000 + k * 16 + j);
    if (write(fan.req[i][1], m, 64) != 64) { bad("client %d write k=%d errno=%d", i, k, errno); return 0; }
    uint32_t r[16];
    if (rfull(fan.rep[i][0], r, 64) != 64) { bad("client %d reply k=%d", i, k); return 0; }
    for (int j = 0; j < 16; j++)
      if (r[j] != (m[j] ^ 0xA5A5A5A5u)) { bad("client %d reply word k=%d", i, k); return 0; }
  }
  return 0;
}

static int fan_serve_one(int i) {
  uint32_t m[16];
  if (rfull(fan.req[i][0], m, 64) != 64) { bad("server read ch=%d", i); return -1; }
  for (int j = 0; j < 16; j++) m[j] ^= 0xA5A5A5A5u;
  if (write(fan.rep[i][1], m, 64) != 64) { bad("server write ch=%d", i); return -1; }
  return 0;
}

static void *fan_server(void *v) {
  (void)v;
  long need = (long)fan.n * FAN_PER, served = 0;
  int ep = -1;
  if (fan.mode == FAN_EPOLL) {
    ep = epoll_create1(0);
    if (ep < 0) { bad("epoll_create1 errno=%d", errno); return 0; }
    for (int i = 0; i < fan.n; i++) {
      struct epoll_event e;
      memset(&e, 0, sizeof e);
      e.events = EPOLLIN;
      e.data.u32 = (uint32_t)i;
      if (epoll_ctl(ep, EPOLL_CTL_ADD, fan.req[i][0], &e)) {
        bad("epoll_ctl ADD #%d fd=%d errno=%d", i, fan.req[i][0], errno);
        return 0;
      }
    }
  }
  while (served < need) {
    if (fan.mode == FAN_POLL) {
      struct pollfd pf[FAN_MAX];
      for (int i = 0; i < fan.n; i++) { pf[i].fd = fan.req[i][0]; pf[i].events = POLLIN; pf[i].revents = 0; }
      int r = poll(pf, (nfds_t)fan.n, 2000);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) { bad("poll errno=%d", errno); return 0; }
      for (int i = 0; i < fan.n; i++)
        if (pf[i].revents & POLLIN) { if (fan_serve_one(i)) return 0; served++; }
    } else if (fan.mode == FAN_SELECT) {
      fd_set rs;
      FD_ZERO(&rs);
      int mx = 0;
      for (int i = 0; i < fan.n; i++) { FD_SET(fan.req[i][0], &rs); if (fan.req[i][0] > mx) mx = fan.req[i][0]; }
      struct timeval tv = {2, 0};
      int r = select(mx + 1, &rs, 0, 0, &tv);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) { bad("select errno=%d", errno); return 0; }
      for (int i = 0; i < fan.n; i++)
        if (FD_ISSET(fan.req[i][0], &rs)) { if (fan_serve_one(i)) return 0; served++; }
    } else {
      struct epoll_event ev[64];
      int r = epoll_wait(ep, ev, 64, 2000);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) { bad("epoll_wait errno=%d", errno); return 0; }
      for (int j = 0; j < r; j++) {
        if (fan_serve_one((int)ev[j].data.u32)) return 0;
        served++;
      }
    }
  }
  return 0;
}

static int row_fan(int mode, int n) {
  fan.mode = mode;
  fan.n = n;
  for (int i = 0; i < n; i++)
    if (pipe(fan.req[i]) || pipe(fan.rep[i])) { bad("pipe #%d errno=%d", i, errno); return 1; }
  pthread_t srv, cl[FAN_MAX];
  pthread_create(&srv, 0, fan_server, 0);
  for (int i = 0; i < n; i++) pthread_create(&cl[i], 0, fan_client, (void *)(intptr_t)i);
  for (int i = 0; i < n; i++) pthread_join(cl[i], 0);
  pthread_join(srv, 0);
  return 0;
}
static int r3(void) { return row_fan(FAN_POLL, 24); }
static int r4(void) { return row_fan(FAN_SELECT, 16); }
static int r5(void) { return row_fan(FAN_EPOLL, 48); }

/* ---- R6: wineserver's request/reply/wait protocol ---------------------- */

#define W_PROCS 6
#define W_THR 4
#define W_CH (W_PROCS * W_THR)
#define W_PER 150
#define W_MAXDATA 3000
struct whdr {
  uint32_t magic, ch, k, size, kind, status, pad[10];
};
_Static_assert(sizeof(struct whdr) == 64, "64-byte header, like Wine's");
static int wreq[W_CH][2], wrep[W_CH][2], wwait[W_CH][2];

static unsigned w_reqsize(int ch, int k) { return (unsigned)((k * 37 + ch * 11) % W_MAXDATA); }
static unsigned w_repsize(int ch, int k) { return (unsigned)((k * 13 + ch * 29) % W_MAXDATA); }

static void *w_client(void *v) {
  int ch = (int)(intptr_t)v;
  static __thread unsigned char buf[W_MAXDATA];
  for (int k = 0; k < W_PER; k++) {
    struct whdr h;
    memset(&h, 0, sizeof h);
    h.magic = 0x57494E45; h.ch = (uint32_t)ch; h.k = (uint32_t)k;
    h.size = w_reqsize(ch, k);
    h.kind = (k % 8 == 7) ? 1 : 0;
    for (unsigned j = 0; j < h.size; j++) buf[j] = (unsigned char)(j + ch + k);
    struct iovec iov[2] = {{&h, 64}, {buf, h.size}};
    ssize_t w = writev(wreq[ch][1], iov, h.size ? 2 : 1);
    if (w != (ssize_t)(64 + h.size)) { bad("client ch=%d k=%d writev=%ld want=%u errno=%d", ch, k, (long)w, 64 + h.size, errno); _exit(5); }
    struct whdr r;
    if (rfull(wrep[ch][0], &r, 64) != 64) { bad("client ch=%d k=%d reply hdr", ch, k); _exit(6); }
    if (r.magic != 0x52455031 || r.ch != (uint32_t)ch || r.k != (uint32_t)k) { bad("client ch=%d k=%d reply mismatch k=%u", ch, k, r.k); _exit(7); }
    if (r.size && rfull(wrep[ch][0], buf, r.size) != (long)r.size) { bad("client ch=%d k=%d reply data", ch, k); _exit(8); }
    for (unsigned j = 0; j < r.size; j++)
      if (buf[j] != (unsigned char)(j * 3 + ch)) { bad("client ch=%d k=%d reply byte %u", ch, k, j); _exit(9); }
    if (r.status == 0x103 /* STATUS_PENDING */) {
      uint32_t wk[4];
      if (rfull(wwait[ch][0], wk, 16) != 16 || wk[0] != (uint32_t)k) { bad("client ch=%d k=%d wake", ch, k); _exit(10); }
    }
  }
  return 0;
}

struct wstate { int have_hdr; struct whdr h; unsigned got; unsigned char data[W_MAXDATA]; };
static struct wstate wst[W_CH];

static int r6(void) {
  for (int c = 0; c < W_CH; c++)
    if (pipe(wreq[c]) || pipe(wrep[c]) || pipe(wwait[c])) { bad("pipe ch=%d errno=%d", c, errno); return 1; }
  pid_t kid[W_PROCS];
  for (int p = 0; p < W_PROCS; p++) {
    kid[p] = fork();
    if (kid[p] < 0) { bad("fork errno=%d", errno); return 1; }
    note_kid(kid[p]);
    if (kid[p] == 0) {
      pthread_t t[W_THR];
      for (int i = 0; i < W_THR; i++) pthread_create(&t[i], 0, w_client, (void *)(intptr_t)(p * W_THR + i));
      for (int i = 0; i < W_THR; i++) pthread_join(t[i], 0);
      _exit(row_bad ? 2 : 0);
    }
  }
  int ep = epoll_create1(0);
  if (ep < 0) { bad("epoll_create1 errno=%d", errno); return 1; }
  for (int c = 0; c < W_CH; c++) {
    fcntl(wreq[c][0], F_SETFL, O_NONBLOCK); /* wineserver: create_thread() */
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = EPOLLIN;
    e.data.u32 = (uint32_t)c;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, wreq[c][0], &e)) { bad("epoll_ctl ch=%d errno=%d", c, errno); return 1; }
  }
  long need = (long)W_CH * W_PER, served = 0;
  int pend_ch[W_CH], pend_k[W_CH], npend = 0;
  static unsigned char rbuf[W_MAXDATA];
  long last = now_ms();
  while (served < need || npend) {
    struct epoll_event ev[32];
    int r = epoll_wait(ep, ev, 32, npend ? 1 : 1000);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) { bad("epoll_wait errno=%d", errno); return 1; }
    if (r == 0 && npend) { /* timer: complete the oldest pending select */
      uint32_t wk[4] = {(uint32_t)pend_k[0], 0, 0, 0};
      if (write(wwait[pend_ch[0]][1], wk, 16) != 16) { bad("wake write"); return 1; }
      npend--;
      memmove(pend_ch, pend_ch + 1, sizeof(int) * (size_t)npend);
      memmove(pend_k, pend_k + 1, sizeof(int) * (size_t)npend);
      continue;
    }
    if (r == 0 && now_ms() - last > 10000) { bad("server idle 10 s served=%ld/%ld", served, need); return 1; }
    for (int j = 0; j < r; j++) {
      int c = (int)ev[j].data.u32;
      struct wstate *s = &wst[c];
      /* wineserver read_request(): header, then the data, resuming on EAGAIN */
      if (!s->have_hdr) {
        ssize_t n = read(wreq[c][0], &s->h, 64);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n != 64) { bad("server ch=%d header read=%ld errno=%d", c, (long)n, errno); return 1; }
        if (s->h.magic != 0x57494E45 || s->h.ch != (uint32_t)c || s->h.size > W_MAXDATA) { bad("server ch=%d bad header k=%u size=%u", c, s->h.k, s->h.size); return 1; }
        s->have_hdr = 1;
        s->got = 0;
      }
      int broke = 0;
      while (s->got < s->h.size) {
        ssize_t n = read(wreq[c][0], s->data + s->got, s->h.size - s->got);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) { broke = 1; break; }
        if (n <= 0) { bad("server ch=%d data read=%ld errno=%d", c, (long)n, errno); return 1; }
        s->got += (unsigned)n;
      }
      if (broke) continue;
      for (unsigned b = 0; b < s->h.size; b++)
        if (s->data[b] != (unsigned char)(b + c + s->h.k)) { bad("server ch=%d k=%u data byte %u", c, s->h.k, b); return 1; }
      struct whdr rp;
      memset(&rp, 0, sizeof rp);
      rp.magic = 0x52455031; rp.ch = (uint32_t)c; rp.k = s->h.k;
      rp.size = w_repsize(c, (int)s->h.k);
      rp.status = s->h.kind ? 0x103 : 0;
      for (unsigned b = 0; b < rp.size; b++) rbuf[b] = (unsigned char)(b * 3 + c);
      struct iovec iov[2] = {{&rp, 64}, {rbuf, rp.size}};
      ssize_t w = writev(wrep[c][1], iov, rp.size ? 2 : 1);
      if (w != (ssize_t)(64 + rp.size)) { bad("server ch=%d k=%u reply writev=%ld want=%u errno=%d", c, s->h.k, (long)w, 64 + rp.size, errno); return 1; }
      if (s->h.kind) { pend_ch[npend] = c; pend_k[npend] = (int)s->h.k; npend++; }
      s->have_hdr = 0;
      served++;
      last = now_ms();
    }
  }
  for (int p = 0; p < W_PROCS; p++) {
    int st = 0;
    waitpid(kid[p], &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad("client proc %d status 0x%x", p, st);
  }
  return 0;
}

/* ---- R7: big blocking writes ------------------------------------------ */

static int r7_fd;
static long r7_sum, r7_n;
static void *r7_reader(void *v) {
  long want = (long)(intptr_t)v;
  unsigned char b[1000];
  long got = 0, sum = 0;
  while (got < want) {
    ssize_t r = read(r7_fd, b, sizeof b);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    for (ssize_t i = 0; i < r; i++) sum += b[i] ^ (unsigned char)((got + i) * 7);
    got += r;
  }
  r7_n = got;
  r7_sum = sum;
  return 0;
}
static int r7(void) {
  static unsigned char big[100000];
  for (int i = 0; i < 100000; i++) big[i] = (unsigned char)(i * 7);
  int p[2];
  if (pipe(p)) return 1;
  r7_fd = p[0];
  pthread_t t;
  pthread_create(&t, 0, r7_reader, (void *)(intptr_t)(100000 + 70000));
  ssize_t w = write(p[1], big, 100000);
  if (w != 100000) bad("write(100000) returned %ld", (long)w);
  /* writev of 3 segments continuing the same byte pattern (offset 100000) */
  static unsigned char s1[100], s2[69800], s3[100];
  for (int i = 0; i < 100; i++) s1[i] = (unsigned char)((100000 + i) * 7);
  for (int i = 0; i < 69800; i++) s2[i] = (unsigned char)((100100 + i) * 7);
  for (int i = 0; i < 100; i++) s3[i] = (unsigned char)((169900 + i) * 7);
  struct iovec iov[3] = {{s1, 100}, {s2, 69800}, {s3, 100}};
  ssize_t wv = (w == 100000) ? writev(p[1], iov, 3) : -1;
  if (w == 100000 && wv != 70000) bad("writev(70000) returned %ld", (long)wv);
  close(p[1]);
  pthread_join(t, 0);
  if (!row_bad && (r7_n != 170000 || r7_sum != 0)) bad("reader got %ld bytes, mismatch sum %ld", r7_n, r7_sum);
  return 0;
}

/* ---- R8: O_NONBLOCK ------------------------------------------------------ */

static int r8(void) {
  int p[2];
  if (pipe2(p, O_NONBLOCK)) { bad("pipe2 errno=%d", errno); return 1; }
  char c;
  long t0 = now_ms();
  ssize_t r = read(p[0], &c, 1);
  if (!(r == -1 && errno == EAGAIN)) bad("empty read=%ld errno=%d (want -1 EAGAIN)", (long)r, errno);
  if (now_ms() - t0 > 500) bad("empty read took %ld ms", now_ms() - t0);
  static char blk[4096];
  long cap = 0;
  for (;;) {
    ssize_t w = write(p[1], blk, 1);
    if (w == 1) { cap++; if (cap > (1 << 20)) break; continue; }
    if (!(w == -1 && errno == EAGAIN)) bad("fill write=%ld errno=%d", (long)w, errno);
    break;
  }
  if (cap < 4096) bad("capacity %ld < PIPE_BUF 4096", cap);
  /* drain 100 bytes: a 4096-byte write (== PIPE_BUF) must not go in partially */
  for (int i = 0; i < 100; i++) (void)!read(p[0], &c, 1);
  ssize_t w = write(p[1], blk, 4096);
  if (!(w == -1 && errno == EAGAIN)) bad("PIPE_BUF write into 100 free bytes=%ld errno=%d", (long)w, errno);
  out("[LP] info R8 capacity=%ld\n", cap);
  /* blocking fd with F_SETFL O_NONBLOCK, then back */
  int q[2];
  if (pipe(q)) return 1;
  fcntl(q[0], F_SETFL, O_NONBLOCK);
  r = read(q[0], &c, 1);
  if (!(r == -1 && errno == EAGAIN)) bad("F_SETFL O_NONBLOCK read=%ld errno=%d", (long)r, errno);
  return 0;
}

/* ---- R11: close wakes ---------------------------------------------------- */

static int r11_fd;
static volatile int r11_res;
static void *r11_reader(void *v) {
  (void)v;
  char c;
  ssize_t r;
  do r = read(r11_fd, &c, 1); while (r < 0 && errno == EINTR);
  r11_res = (int)r;
  return 0;
}
static void *r11_writer(void *v) {
  (void)v;
  static char blk[8192];
  for (;;) {
    ssize_t w = write(r11_fd, blk, sizeof blk);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) { r11_res = w < 0 ? -errno : 0; return 0; }
  }
}
static int r11(void) {
  signal(SIGPIPE, SIG_IGN);
  for (int i = 0; i < 300; i++) {
    int p[2];
    if (pipe(p)) { bad("pipe errno=%d", errno); return 1; }
    r11_fd = p[0];
    r11_res = 99;
    pthread_t t;
    pthread_create(&t, 0, r11_reader, 0);
    for (int s = 0; s < (i % 7) * 50; s++) sched_yield();
    close(p[1]);
    pthread_join(t, 0);
    close(p[0]);
    if (r11_res != 0) { bad("EOF iter %d read=%d", i, r11_res); return 1; }
  }
  for (int i = 0; i < 100; i++) {
    int p[2];
    if (pipe(p)) { bad("pipe errno=%d", errno); return 1; }
    r11_fd = p[1];
    r11_res = 99;
    pthread_t t;
    pthread_create(&t, 0, r11_writer, 0);
    for (int s = 0; s < (i % 5) * 100; s++) sched_yield();
    close(p[0]);
    pthread_join(t, 0);
    close(p[1]);
    if (r11_res != -EPIPE) { bad("EPIPE iter %d write=%d", i, r11_res); return 1; }
  }
  return 0;
}

/* ---- R12: signals during a parked read ------------------------------------ */

#define R12_N 1500
static int r12_rd, r12_wr, r12_sock;
static volatile int r12_stop;
static volatile long r12_hits;
static pid_t r12_tid;
static void r12_h(int s) { (void)s; r12_hits++; }
static void *r12_reader(void *v) {
  (void)v;
  r12_tid = (pid_t)syscall(SYS_gettid);
  for (int i = 0; i < R12_N; i++) {
    unsigned char c;
    ssize_t r;
    do {
      r = r12_sock ? recv(r12_rd, &c, 1, 0) : read(r12_rd, &c, 1);
    } while (r < 0 && errno == EINTR);
    if (r != 1 || c != (unsigned char)i) { bad("%s byte %d r=%ld c=%u errno=%d", r12_sock ? "recv" : "read", i, (long)r, c, errno); break; }
  }
  r12_stop = 1;
  return 0;
}
static void *r12_signaller(void *v) {
  (void)v;
  while (!r12_tid) sched_yield();
  pid_t pid = getpid();
  while (!r12_stop) {
    syscall(SYS_tgkill, pid, r12_tid, SIGUSR1);
    for (int s = 0; s < 20; s++) sched_yield();
  }
  return 0;
}
static int r12_one(int sock) {
  int fds[2];
  if (sock ? socketpair(AF_UNIX, SOCK_STREAM, 0, fds) : pipe(fds)) { bad("pipe errno=%d", errno); return 1; }
  r12_sock = sock;
  r12_rd = fds[0];
  r12_wr = fds[1];
  r12_stop = 0;
  r12_tid = 0;
  pthread_t a, b;
  pthread_create(&a, 0, r12_reader, 0);
  pthread_create(&b, 0, r12_signaller, 0);
  for (int i = 0; i < R12_N && !r12_stop; i++) {
    unsigned char c = (unsigned char)i;
    if (write(r12_wr, &c, 1) != 1) { bad("writer %d errno=%d", i, errno); break; }
    for (int s = 0; s < (i % 13) * 3; s++) sched_yield();
  }
  pthread_join(a, 0);
  r12_stop = 1;
  pthread_join(b, 0);
  return 0;
}
static int r12(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = r12_h;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGUSR1, &sa, 0);
  r12_one(0);
  if (!row_bad) r12_one(1);
  if (!row_bad) out("[LP] info R12 signals=%ld\n", r12_hits);
  return 0;
}

/* ---- row runner ------------------------------------------------------------ */

/* ---- R13: SIGKILL of a busy multi-threaded group ------------------------- */

static int r13_pipe[2];
static void *r13_spin(void *v) { (void)v; for (volatile unsigned long i = 0;; i++) {} return 0; }
static void *r13_nap(void *v) {
  (void)v;
  for (;;) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
  return 0;
}
static void *r13_read(void *v) { (void)v; char c; for (;;) (void)!read(r13_pipe[0], &c, 1); return 0; }
static int r13(void) {
  for (int it = 0; it < 30; it++) {
    if (pipe(r13_pipe)) { bad("pipe errno=%d", errno); return 1; }
    pid_t k = fork();
    if (k < 0) { bad("fork errno=%d", errno); return 1; }
    note_kid(k);
    if (k == 0) {
      pthread_t t;
      for (int i = 0; i < 4; i++) pthread_create(&t, 0, r13_spin, 0);
      for (int i = 0; i < 4; i++) pthread_create(&t, 0, r13_nap, 0);
      for (int i = 0; i < 4; i++) pthread_create(&t, 0, r13_read, 0);
      r13_read(0);
    }
    struct timespec ts = {0, (10 + (it % 5) * 10) * 1000000L};
    nanosleep(&ts, 0);
    kill(k, SIGKILL);
    int st = 0;
    if (waitpid(k, &st, 0) != k || !WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
      bad("iter %d: status 0x%x", it, st);
      return 1;
    }
    close(r13_pipe[0]);
    close(r13_pipe[1]);
  }
  return 0;
}

static int fails;
static void run_row(const char *name, int (*fn)(void)) {
  long t0 = now_ms();
  int pr[2], kp[2];
  if (pipe(pr) || pipe2(kp, O_NONBLOCK)) { out("[LP] FAIL %s pipe errno=%d\n", name, errno); fails++; return; }
  pid_t pid = fork();
  if (pid < 0) { out("[LP] FAIL %s fork errno=%d\n", name, errno); fails++; return; }
  if (pid == 0) {
    close(pr[0]);
    close(kp[0]);
    kid_fd = kp[1];
    setpgid(0, 0);
    row_bad = 0;
    row_pid = getpid();
    fn();
    char msg[200];
    int n = snprintf(msg, sizeof msg, "%d %s", row_bad, row_bad ? row_why : "ok");
    (void)!write(pr[1], msg, (size_t)n);
    _exit(row_bad ? 1 : 0);
  }
  close(pr[1]);
  close(kp[1]);
  int st = 0, done = 0;
  while (now_ms() - t0 < ROW_DEADLINE_MS) {
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid) { done = 1; break; }
    struct timespec ts = {0, 20 * 1000000};
    nanosleep(&ts, 0);
  }
  long ms = now_ms() - t0;
  if (!done) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    pid_t kk;
    while (read(kp[0], &kk, sizeof kk) == (ssize_t)sizeof kk) kill(kk, SIGKILL);
    out("[LP] FAIL %s HANG: no finish in %d ms (lost wakeup or a blocked non-blocking call)\n", name, ROW_DEADLINE_MS);
    fails++;
    waitpid(pid, &st, 0);
    close(pr[0]);
    close(kp[0]);
    return;
  }
  char msg[200] = {0};
  fcntl(pr[0], F_SETFL, O_NONBLOCK);
  ssize_t n = read(pr[0], msg, sizeof msg - 1);
  if (n < 0) n = 0;
  msg[n] = 0;
  close(pr[0]);
  close(kp[0]);
  int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (ok) out("[LP] PASS %s (%ld ms)\n", name, ms);
  else {
    out("[LP] FAIL %s status=0x%x %s (%ld ms)\n", name, st, n ? msg : "(no report)", ms);
    fails++;
  }
}

int main(void) {
  int rounds = 1;
  for (int round = 0; round < rounds; round++) {
    run_row("R1 thr_pingpong", r1);
    run_row("R2 proc_pingpong", r2);
    run_row("R3 poll_fanin", r3);
    run_row("R4 select_fanin", r4);
    run_row("R5 epoll_fanin(48)", r5);
    run_row("R6 wine_proto", r6);
    run_row("R7 bigwrite", r7);
    run_row("R8 nonblock", r8);
    run_row("R9 unix_pingpong", r9);
    run_row("R10 efd_pingpong", r10);
    run_row("R11 close_wake", r11);
    run_row("R12 sig_read", r12);
    run_row("R13 kill_busy", r13);
  }
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
