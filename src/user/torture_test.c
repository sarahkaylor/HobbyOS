/*
 * TORTURE.BIN — "POSIX torture" acceptance (browser.md §6 P8.1).
 *
 * The P8 gate suite: the combined-POSIX-systems acceptance the WebKit port
 * leans on, exercised as one in-OS program (wave position: last) plus a
 * bounded soak loop for the P8.2 combined run (MODE=soak passes the "soak"
 * argument through load_and_run_program_in_scheduler_args).
 *
 * Groups (P8.1):
 *   1. 64 thread create/join cycles x mutex/condvar churn (shared counter
 *      + trylock EBUSY contract + a broadcast turnstile), run in bounded
 *      bursts so the shared PCB table is never grabbed full-width;
 *   2. socketpair + SCM_RIGHTS fd-pass loops (in-process rounds + one
 *      cross-fork round with bounded polls);
 *   3. mmap/fault/free storms (demand-zero fault-in, PROT_NONE zap +
 *      re-demand, MADV_DONTNEED re-zero, munmap/re-map; plus a forked
 *      PROT-kill fault case);
 *   4. exec/wait cycles (fork -> execve("/TORTURE.BIN", "child N") ->
 *      waitpid status check), slot-pressure tolerant;
 *   5. poll on many fds (as many pipes as the 32-fd table allows, exact
 *      revents, timeout path, mixed burst);
 *   6. memory high-water recording over sysinfo(2), numbers printed.
 *
 * Defensive design (mandated by the wave's slot pressure, see browser.md
 * §11 Wave 1f tail): thread/fork/exec launches that are REJECTED under
 * process-table pressure are noted and skipped after a bounded retry — they
 * never turn into a FAIL.  Real errors (non-EAGAIN pthread_create, wrong
 * math, lost condvar wakeups, bad fd-pass payloads, wrong wrap statuses)
 * still FAIL.  Every wait is bounded: the suite self-terminates in every
 * mode.
 *
 * Modes: no args = quick (bounded, sized for the in-wave budget);
 * "soak" = repeat the combined groups until a 25-minute wall cap, printing
 * the soak numbers per round; "soak=<sec>" = same with a custom cap;
 * "child N" = the exec/wait helper image (exits with N immediately).
 *
 * Output convention: "  TORTURE <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "TORTURE FAILED: <n>"; "[TORTURE]"
 * info lines (mem/time/counts) and "[SOAK]" per-round numbers.
 *
 * The -DHOST_TEST build (obj/host_torture_test.o, wired into `make
 * host_tests`) runs the same logic against glibc: real pthreads/sockets/
 * mmap/poll/fork, the exec/wait child via /proc/self/exe, and the compat
 * sysinfo mock (with the mock's memory override exercising the high-water
 * recorder).
 */

#ifdef HOST_TEST
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* src/host/compat.c mocks (declared here; the host TU uses native headers,
 * not libc.h — the ipc_test/thrd_test convention, so the fixed-arity
 * libc.h wrappers cannot collide with glibc's). */
void print_console(const char *str);
void print_dec(long val);
struct sys_meminfo {
  uint64_t total_bytes;
  uint64_t free_bytes;
};
int sysinfo(int cmd, void *buf, int size);
/* compat.c sysinfo memory overrides (used by mem-highwater-monotonic). */
extern int mock_sysinfo_mem_enabled;
extern unsigned long long mock_sysinfo_mem_total, mock_sysinfo_mem_free;
#else
#include "libc.h"
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

/* ---- modes ------------------------------------------------------------- */

static int soak_mode;
static unsigned long soak_cap_ms = 28UL * 60UL * 1000UL; /* default 28 min */

/* ---- bounded per-mode sizes (quick = in-wave budget; soak = per round) -- */

#define CHURN_TOTAL 64 /* create/join cycles per group run */
#define CHURN_LIVE_QUICK 8
#define CHURN_LIVE_SOAK 16
#define CHURN_ITERS 24
#define CHURN_DEADLINE_MS 30000UL /* total create-effort budget per run */
#define FDPASS_ITERS_QUICK 16
#define FDPASS_ITERS_SOAK 32
#define MMAP_ITERS_QUICK 24
#define MMAP_ITERS_SOAK 32
#define EXEC_CYCLES_QUICK 8
#define EXEC_CYCLES_SOAK 12
#define EXEC_DEADLINE_MS 20000UL /* fork/exec effort budget per group run */

/* ---- counters and memory high-water ------------------------------------ */

static int fails;
static long thr_created, thr_rejected;
static long fdpass_bad_rounds;
static long exec_rejected, exec_ok;
static long rounds_done;
static uint64_t mem_total, mem_free0, mem_min;
static int mem_samples;
static uint64_t suite_t0;

/* ---- small helpers ------------------------------------------------------ */

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int streq_prefix(const char *a, const char *b) {
  while (*b && *a == *b) {
    a++;
    b++;
  }
  return *b == 0;
}

static int atoi_local(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (*s++ - '0');
  return v;
}

static uint64_t now_ms(void) {
#ifdef HOST_TEST
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#else
  return (uint64_t)sysinfo(1, 0, 0);
#endif
}

static void check(const char *name, int ok) {
  print_console("  TORTURE ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok)
    fails++;
}

static void note(const char *msg) {
  print_console("[TORTURE] note: ");
  print_console(msg);
  print_console("\n");
}

static void label_num(const char *label, long v) {
  /* Local decimal formatter: compat.c's print_dec goes through buffered
   * printf while print_console writes raw, which reorders the host log;
   * this is uniform on both builds. */
  char buf[24];
  int i = (int)sizeof buf;
  unsigned long u;
  buf[--i] = 0;
  u = (v < 0) ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v;
  if (u == 0)
    buf[--i] = '0';
  while (u) {
    buf[--i] = (char)('0' + u % 10);
    u /= 10;
  }
  if (v < 0)
    buf[--i] = '-';
  print_console(label);
  print_console(&buf[i]);
}

/* Sample the free-memory surface; keeps the running high-water minimum. */
static void mem_sample(void) {
  struct sys_meminfo m;
  if (sysinfo(2, &m, sizeof m) != 0)
    return;
  if (mem_total == 0)
    mem_total = m.total_bytes;
  if (mem_free0 == 0)
    mem_free0 = m.free_bytes;
  if (mem_min == 0 || m.free_bytes < mem_min)
    mem_min = m.free_bytes;
  mem_samples++;
}

/* ---- 1. 64 thread create/join cycles x mutex/condvar churn ------------ *
 *
 * "64 threads" is a create/join torture budget, not a 64-way table grab:
 * the wave shares the 64-slot PCB table, and a full-width grab starves
 * every sibling suite's spawns (bring-up smoke: table 63/63, the wave's
 * spawn-heavy tests blocked for minutes).  Each burst runs CHURN_LIVE
 * concurrent threads whose whole life is bounded; the bursts complete the
 * 64-cycle budget.  The turnstile release is a STICKY flag broadcast right
 * after the create loop, so neither side waits on the other.
 */

static pthread_mutex_t churn_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t churn_cv = PTHREAD_COND_INITIALIZER;
static volatile long churn_counter;
static volatile int churn_ready, churn_go, churn_bad, churn_woken;
static volatile int churn_timedout_waiters;

static void *churn_fn(void *arg) {
  (void)arg;
  int bad = 0;
  for (int i = 0; i < CHURN_ITERS; i++) {
    if (pthread_mutex_lock(&churn_mu) != 0) {
      bad = 1;
      break;
    }
    churn_counter++;
    /* trylock while held must report EBUSY (owner = this thread). */
    if (pthread_mutex_trylock(&churn_mu) != EBUSY)
      bad = 1;
    if ((i & 7) == 0)
      sched_yield();
    pthread_mutex_unlock(&churn_mu);
  }
  /* Condvar turnstile: publish readiness, then park until the (sticky)
   * broadcast.  The wait is bounded (5 s absolute chunks, give up after 3)
   * so a lost wakeup fails the check instead of hanging the suite. */
  pthread_mutex_lock(&churn_mu);
  churn_ready++;
  pthread_cond_signal(&churn_cv);
  {
    int timeouts = 0;
    while (!churn_go) {
      struct timespec ts;
      if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        bad = 1;
        break;
      }
      ts.tv_sec += 5;
      int rc = pthread_cond_timedwait(&churn_cv, &churn_mu, &ts);
      if (rc == ETIMEDOUT) {
        if (++timeouts >= 3) {
          churn_timedout_waiters++;
          bad = 1;
          break;
        }
      } else if (rc != 0) {
        bad = 1;
        break;
      }
    }
  }
  if (churn_go)
    churn_woken++;
  if (bad)
    churn_bad = 1;
  pthread_mutex_unlock(&churn_mu);
  return (void *)0;
}

/* One burst of up to n threads.  Returns 1 = all created threads churned
 * correctly, 0 = churn mismatch (real failure), -1 = every launch rejected
 * (slot pressure, tolerated), -2 = non-EAGAIN create error (real). */
static int churn_burst(int n, int *created_out) {
  pthread_t th[CHURN_LIVE_SOAK];
  int created = 0, rejected = 0;
  churn_counter = 0;
  churn_ready = 0;
  churn_go = 0;
  churn_bad = 0;
  churn_woken = 0;
  churn_timedout_waiters = 0;
  for (int i = 0; i < n; i++) {
    int rc = pthread_create(&th[i], 0, churn_fn, (void *)(long)i);
    if (rc == 0) {
      created++;
      continue;
    }
    if (rc == EAGAIN) {
      rejected++;
      continue;
    }
    /* Non-EAGAIN from pthread_create is a real defect, not pressure. */
    label_num("[TORTURE] pthread_create rc=", rc);
    print_console(" (non-EAGAIN)\n");
    *created_out = created;
    thr_created += created;
    thr_rejected += rejected;
    return -2;
  }
  *created_out = created;
  thr_created += created;
  thr_rejected += rejected;
  if (created == 0)
    return -1;
  /* Sticky release: a thread that parks after this sees go=1 immediately,
   * one already parked wakes on the broadcast (the wait snapshots the
   * cond sequence before parking, so no lost wakeup).  Nobody waits on
   * anybody -- the join bounds the stragglers. */
  pthread_mutex_lock(&churn_mu);
  churn_go = 1;
  pthread_cond_broadcast(&churn_cv);
  pthread_mutex_unlock(&churn_mu);
  int joined_all = 1;
  for (int i = 0; i < created; i++) {
    void *r = 0;
    if (pthread_join(th[i], &r) != 0)
      joined_all = 0;
  }
  int ok = joined_all && churn_ready == created && churn_woken == created &&
           churn_timedout_waiters == 0 && churn_bad == 0 &&
           churn_counter == (long)created * CHURN_ITERS;
  return ok ? 1 : 0;
}

/* Runs CHURN_TOTAL create/join cycles in bursts of `live` concurrent
 * threads.  Returns 1 = ran and passed, 0 = ran and failed, -1 = skipped
 * (every launch rejected: tolerated, noted, never a FAIL). */
static int thread_churn(void) {
  int live = soak_mode ? CHURN_LIVE_SOAK : CHURN_LIVE_QUICK;
  uint64_t t0 = now_ms();
  int bursts = (CHURN_TOTAL + live - 1) / live;
  int created_total = 0, rejected_total = 0, skipped = 0, bad = 0;
  for (int b = 0; b < bursts; b++) {
    int n = CHURN_TOTAL - b * live;
    int created = 0;
    int rc;
    if (n > live)
      n = live;
    if (now_ms() - t0 > CHURN_DEADLINE_MS) {
      skipped += n;
      continue;
    }
    rc = churn_burst(n, &created);
    if (rc == -2) {
      check("threads-churn-64", 0);
      return 0;
    }
    if (rc == -1) {
      skipped += n;
      continue;
    }
    created_total += created;
    rejected_total += n - created;
    if (rc == 0)
      bad = 1;
  }
  if (rejected_total > 0) {
    label_num("[TORTURE] note: thread launch rejected (slot pressure), "
              "created=", created_total);
    print_console("/64\n");
  }
  if (skipped > 0) {
    label_num("[TORTURE] note: churn cycles skipped (create budget), "
              "skipped=", skipped);
    print_console("\n");
  }
  label_num("[TORTURE] churn: cycles=", CHURN_TOTAL);
  label_num(" live=", live);
  label_num(" created=", created_total);
  print_console("\n");
  if (created_total == 0)
    return -1;
  check("threads-churn-64", !bad);
  return bad ? 0 : 1;
}

/* ---- 2. socketpair + SCM_RIGHTS fd-pass loops -------------------------- */

static size_t mk_fd_cmsg(char *buf, size_t bufsz, int fd) {
  struct cmsghdr *c = (struct cmsghdr *)buf;
  (void)bufsz;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(c), &fd, sizeof fd);
  return CMSG_SPACE(sizeof(int));
}

static int cmsg_fd(const struct msghdr *m) {
  const struct cmsghdr *c = CMSG_FIRSTHDR(m);
  if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
      c->cmsg_len < CMSG_LEN(sizeof(int)))
    return -1;
  int fd = -1;
  memcpy(&fd, CMSG_DATA(c), sizeof fd);
  return fd;
}

/* One in-process round: socketpair -> sendmsg(pipe write end) ->
 * recvmsg(CMSG_CLOEXEC) -> write/read through the RECEIVED fd -> close. */
static int fdpass_round(void) {
  int sv[2] = {-1, -1}, pp[2] = {-1, -1};
  int got = -1;
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
    return 0;
  if (pipe(pp) != 0) {
    close(sv[0]);
    close(sv[1]);
    return 0;
  }
  char ctl[64];
  size_t cl = mk_fd_cmsg(ctl, sizeof ctl, pp[1]);
  char payload[2] = {'o', 'k'};
  struct iovec iov = {payload, 2};
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = ctl;
  m.msg_controllen = cl;
  int ok = (sendmsg(sv[0], &m, 0) == 2);
  if (ok) {
    /* the original write end is dropped; only the transferred fd keeps
     * the pipe writable now */
    close(pp[1]);
    pp[1] = -1;
    char rctl[64];
    char rpayload[2] = {0, 0};
    struct iovec riov = {rpayload, 2};
    struct msghdr rm;
    memset(&rm, 0, sizeof rm);
    rm.msg_iov = &riov;
    rm.msg_iovlen = 1;
    rm.msg_control = rctl;
    rm.msg_controllen = sizeof rctl;
    ssize_t r = recvmsg(sv[1], &rm, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (r != 2 || rpayload[0] != 'o' || rpayload[1] != 'k')
      ok = 0;
    got = (r == 2) ? cmsg_fd(&rm) : -1;
    if (got < 0)
      ok = 0;
    if (ok) {
      if (write(got, "z", 1) != 1)
        ok = 0;
      char rb = 0;
      if (read(pp[0], &rb, 1) != 1 || rb != 'z')
        ok = 0;
    }
  }
  if (got >= 0)
    close(got);
  if (pp[1] >= 0)
    close(pp[1]);
  close(pp[0]);
  close(sv[0]);
  close(sv[1]);
  return ok;
}

static int fdpass_loop(int iters) {
  for (int i = 0; i < iters; i++)
    if (!fdpass_round())
      return 0;
  return 1;
}

/* Cross-fork round: parent sends the pipe write end to a forked child over
 * a socketpair; the child answers through the received fd.  Bounded polls
 * on both sides.  Returns 1 ok, 0 bad payload, -1 fork rejected. */
static int fdpass_fork_round(void) {
  int sv[2] = {-1, -1}, pp[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
    return 0;
  if (pipe(pp) != 0) {
    close(sv[0]);
    close(sv[1]);
    return 0;
  }
  int pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    close(pp[0]);
    close(pp[1]);
    return -1;
  }
  if (pid == 0) {
    /* child: keep sv[1]; drop everything else */
    close(sv[0]);
    close(pp[0]);
    close(pp[1]);
    struct pollfd pf = {sv[1], POLLIN, 0};
    if (poll(&pf, 1, 5000) != 1)
      exit(80);
    char rctl[64];
    char rpayload[2] = {0, 0};
    struct iovec riov = {rpayload, 2};
    struct msghdr rm;
    memset(&rm, 0, sizeof rm);
    rm.msg_iov = &riov;
    rm.msg_iovlen = 1;
    rm.msg_control = rctl;
    rm.msg_controllen = sizeof rctl;
    if (recvmsg(sv[1], &rm, 0) != 2)
      exit(81);
    int got = cmsg_fd(&rm);
    if (got < 0)
      exit(82);
    if (write(got, "Q", 1) != 1)
      exit(83);
    close(got);
    exit(0);
  }
  close(sv[1]);
  sv[1] = -1;
  char ctl[64];
  size_t cl = mk_fd_cmsg(ctl, sizeof ctl, pp[1]);
  char payload[2] = {'f', 'k'};
  struct iovec iov = {payload, 2};
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = ctl;
  m.msg_controllen = cl;
  int ok = (sendmsg(sv[0], &m, 0) == 2);
  close(pp[1]); /* the child's received fd is the only writer now */
  pp[1] = -1;
  char rb = 0;
  if (ok) {
    struct pollfd pf = {pp[0], POLLIN, 0};
    if (poll(&pf, 1, 5000) != 1 || read(pp[0], &rb, 1) != 1 || rb != 'Q')
      ok = 0;
  }
  int st = 0;
  if (waitpid(pid, &st, 0) != pid)
    ok = 0;
  else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    ok = 0;
  close(sv[0]);
  close(pp[0]);
  return ok;
}

/* ---- 3. mmap/fault/free storms ----------------------------------------- */

#define MMAP_CHUNK (256u * 1024u) /* 64 pages */
#define MMAP_PAGES 64

static int mmap_storm(int iters) {
  int ok = 1;
  for (int i = 0; i < iters && ok; i++) {
    unsigned char *p = mmap(0, MMAP_CHUNK, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      return 0;
    /* fault-in: every page demand-zeroes on first touch */
    for (int pg = 0; pg < MMAP_PAGES; pg++) {
      if (p[pg * 4096] != 0 || p[pg * 4096 + 4095] != 0) {
        ok = 0;
        break;
      }
      p[pg * 4096] = (unsigned char)(pg + i);
    }
    /* fault storm: zap to PROT_NONE then re-arm RW; the re-touched pages
     * must fault, re-demand and read zero again */
    if (mprotect(p, MMAP_CHUNK, PROT_NONE) != 0)
      ok = 0;
    if (mprotect(p, MMAP_CHUNK, PROT_READ | PROT_WRITE) != 0)
      ok = 0;
    for (int pg = 0; pg < MMAP_PAGES && ok; pg++) {
#ifdef HOST_TEST
      /* glibc/Linux keeps the data across the mprotect round trip. */
      if (p[pg * 4096] != (unsigned char)(pg + i))
        ok = 0;
#else
      /* HobbyOS v2 zaps resident leaves on the PROT_NONE flip: the
       * re-armed page must fault, re-demand and read zero again. */
      if (p[pg * 4096] != 0)
        ok = 0;
#endif
      p[pg * 4096 + 4095] = (unsigned char)(pg * 3 + i);
    }
    /* free storm: DONTNEED, then a last touch re-demands zero */
    if (ok && madvise(p, MMAP_CHUNK, MADV_DONTNEED) != 0)
      ok = 0;
    if (ok && p[4095] != 0)
      ok = 0;
    if (munmap(p, MMAP_CHUNK) != 0)
      ok = 0;
  }
  return ok;
}

/* A forked child writing an armed PROT_NONE page must die with SIGSEGV
 * (status low byte 11) while the parent and its mapping stay usable.
 * Returns 1 ok, 0 wrong status, -1 fork rejected. */
static int mmap_fault_kill(void) {
  unsigned char *p = mmap(0, 8192, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return 0;
  p[0] = 1;
  p[4096] = 1;
  if (mprotect(p + 4096, 4096, PROT_NONE) != 0) {
    munmap(p, 8192);
    return 0;
  }
  int pid = fork();
  if (pid < 0) {
    munmap(p, 8192);
    return -1;
  }
  if (pid == 0) {
    p[4096] = 2; /* must fault: PROT_NONE page */
    exit(0);     /* reached only if the arm failed */
  }
  int st = 0;
  int ok = 1;
  if (waitpid(pid, &st, 0) != pid)
    ok = 0;
  else if (WIFEXITED(st) || WTERMSIG(st) != 11)
    ok = 0;
  p[0] = 3; /* the parent's mapping is still RW */
  if (p[0] != 3)
    ok = 0;
  munmap(p, 8192);
  return ok;
}

/* ---- 4. exec/wait cycles ------------------------------------------------ */

/* Returns 0 ok, -1 wrong status (real), -2 fork rejected, -3 exec rejected
 * inside the child (the child exits 97 when execve fails). */
static int exec_cycle(int seq) {
  int want = (seq % 40) + 10;
  int pid = fork();
  if (pid < 0)
    return -2;
  if (pid == 0) {
    char code[8];
    int v = want, i = 0;
    char tmp[8];
    while (v > 0) {
      tmp[i++] = (char)('0' + v % 10);
      v /= 10;
    }
    int j = 0;
    while (i > 0)
      code[j++] = tmp[--i];
    code[j] = 0;
    char *argv[4];
    argv[0] = "TORTURE.BIN";
    argv[1] = "child";
    argv[2] = code;
    argv[3] = 0;
#ifdef HOST_TEST
    execv("/proc/self/exe", argv);
#else
    execv("/TORTURE.BIN", argv);
#endif
    exit(97); /* exec rejected */
  }
  int st = 0;
  if (waitpid(pid, &st, 0) != pid)
    return -1;
  if (WIFEXITED(st) && WEXITSTATUS(st) == 97)
    return -3;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != want)
    return -1;
  return 0;
}

/* Returns 1 = ran (all-rejected stragglers noted), 0 = real failure,
 * -1 = every cycle skipped under slot pressure (tolerated). */
static int exec_cycles(int cycles) {
  int ran = 0, rejected = 0, bad = 0;
  uint64_t t0 = now_ms();
  for (int i = 0; i < cycles; i++) {
    int rc = exec_cycle(i);
    if (rc != 0 && now_ms() - t0 < EXEC_DEADLINE_MS) {
      usleep(20000); /* transient pressure: one bounded retry */
      rc = exec_cycle(i);
    }
    if (rc == 0) {
      ran++;
      exec_ok++;
    } else if (rc == -1) {
      bad++;
    } else {
      rejected++;
      exec_rejected++;
    }
  }
  if (rejected > 0) {
    label_num("[TORTURE] note: exec cycle rejected and skipped, count=",
              rejected);
    print_console(" (process-slot pressure)\n");
  }
  if (bad > 0)
    return 0;
  if (ran == 0)
    return -1;
  return 1;
}

/* ---- 5. poll on many fds ------------------------------------------------ */

#define POLL_MAX_PIPES 16

static int poll_many(void) {
  int rd[POLL_MAX_PIPES], wr[POLL_MAX_PIPES];
  int n = 0;
  while (n < POLL_MAX_PIPES) {
    int pp[2];
    if (pipe(pp) != 0)
      break; /* 32-fd table full (or close to it): use what we have */
    rd[n] = pp[0];
    wr[n] = pp[1];
    n++;
  }
  if (n < 4) {
    for (int i = 0; i < n; i++) {
      close(rd[i]);
      close(wr[i]);
    }
    return -1; /* fd-table pressure: tolerated, no check */
  }
  struct pollfd pf[2 * POLL_MAX_PIPES];
  int ok = 1;
  /* Read ends request POLLIN; write ends sit in the set with events=0
   * (always-writable pipes would otherwise mask the readiness paths). */
  for (int i = 0; i < 2 * n; i++) {
    pf[i].fd = (i < n) ? rd[i] : wr[i];
    pf[i].events = (i < n) ? POLLIN : 0;
    pf[i].revents = 0;
  }
  /* Timeout path: nothing ready -> 0. */
  int r = poll(pf, 2 * n, 30);
  if (r != 0)
    ok = 0;
  /* One byte into one pipe -> exactly one readable fd, correct one. */
  if (ok) {
    int k = n / 2;
    if (write(wr[k], "x", 1) != 1)
      ok = 0;
    r = poll(pf, 2 * n, 2000);
    if (r != 1 || pf[k].revents != POLLIN)
      ok = 0;
    char rb;
    if (ok && (read(rd[k], &rb, 1) != 1 || rb != 'x'))
      ok = 0;
  }
  /* Burst: every read end gets a byte -> exactly n readable. */
  if (ok) {
    for (int i = 0; i < n; i++)
      if (write(wr[i], "b", 1) != 1)
        ok = 0;
    r = poll(pf, 2 * n, 2000);
    if (r != n)
      ok = 0;
    if (ok) {
      int seen = 0;
      for (int i = 0; i < n; i++)
        if (pf[i].revents == POLLIN)
          seen++;
      if (seen != n)
        ok = 0;
      for (int i = 0; i < n; i++) {
        char rb;
        if (read(rd[i], &rb, 1) != 1 || rb != 'b')
          ok = 0;
      }
      /* All quiet again -> timeout 0. */
      r = poll(pf, 2 * n, 30);
      if (r != 0)
        ok = 0;
    }
  }
  for (int i = 0; i < n; i++) {
    close(rd[i]);
    close(wr[i]);
  }
  return ok;
}

/* ---- suite -------------------------------------------------------------- */

static void print_mem_numbers(void) {
  label_num("[TORTURE] mem: total_kb=", (long)(mem_total / 1024));
  label_num(" free0_kb=", (long)(mem_free0 / 1024));
  label_num(" min_free_kb=", (long)(mem_min / 1024));
  label_num(" highwater_kb=", (long)((mem_free0 - mem_min) / 1024));
  label_num(" samples=", (long)mem_samples);
  print_console("\n");
}

static void run_group_common(int fdpass_iters, int mmap_iters, int exec_n) {
  mem_sample();
  if (thread_churn() < 0)
    note("threads-churn-64 skipped: every launch rejected");
  mem_sample();
  if (!fdpass_loop(fdpass_iters)) {
    fdpass_bad_rounds++;
    check("socketpair-fdpass-loop", 0);
  } else {
    check("socketpair-fdpass-loop", 1);
  }
  {
    int rc = fdpass_fork_round();
    if (rc < 0)
      note("socketpair-fdpass-fork skipped: fork rejected (slot pressure)");
    else
      check("socketpair-fdpass-fork", rc);
  }
  check("mmap-fault-storm", mmap_storm(mmap_iters));
  {
    int rc = mmap_fault_kill();
    if (rc < 0)
      note("mmap-fault-kill skipped: fork rejected (slot pressure)");
    else
      check("mmap-fault-kill", rc);
  }
  {
    int rc = exec_cycles(exec_n);
    if (rc < 0)
      note("exec-wait-cycles skipped: every fork rejected (slot pressure)");
    else
      check("exec-wait-cycles", rc);
  }
  {
    int rc = poll_many();
    if (rc < 0)
      note("poll-many-fds skipped: fd table full");
    else
      check("poll-many-fds", rc);
  }
  mem_sample();
  check("mem-highwater-recorded",
        mem_total > 0 && mem_free0 > 0 && mem_min > 0 && mem_min <= mem_total);
}

static void run_quick(void) {
  run_group_common(FDPASS_ITERS_QUICK, MMAP_ITERS_QUICK, EXEC_CYCLES_QUICK);
}

/* ---- P8.2 soak ---------------------------------------------------------- */

static int soak_round(void) {
  int before = fails;
  if (thread_churn() < 0)
    note("soak round: thread churn skipped (slot pressure)");
  if (!fdpass_loop(FDPASS_ITERS_SOAK)) {
    fdpass_bad_rounds++;
    check("soak-fdpass", 0);
  }
  check("soak-mmap-storm", mmap_storm(MMAP_ITERS_SOAK));
  {
    int rc = exec_cycles(EXEC_CYCLES_SOAK);
    if (rc < 0)
      note("soak round: exec cycles all rejected (slot pressure)");
    else
      check("soak-exec-cycles", rc);
  }
  {
    int rc = poll_many();
    if (rc < 0)
      note("soak round: poll skipped (fd table full)");
    else
      check("soak-poll", rc);
  }
  mem_sample();
  return fails - before;
}

static void run_soak(void) {
  uint64_t t0 = now_ms();
  uint64_t end = t0 + soak_cap_ms;
  label_num("[TORTURE] soak: combined round loop, cap_ms=", (long)soak_cap_ms);
  print_console("\n");
  int violations = 0;
  for (;;) {
    rounds_done++;
    violations += soak_round();
    label_num("[SOAK] round=", rounds_done);
    label_num(" min_free_kb=", (long)(mem_min / 1024));
    label_num(" hiwater_kb=", (long)((mem_free0 - mem_min) / 1024));
    label_num(" thr_created=", thr_created);
    label_num(" thr_rejected=", thr_rejected);
    label_num(" fdpass_bad=", fdpass_bad_rounds);
    label_num(" exec_ok=", exec_ok);
    label_num(" exec_rejected=", exec_rejected);
    label_num(" violations=", (long)violations);
    print_console("\n");
    mem_sample();
    if (now_ms() >= end || violations > 0 || rounds_done >= 20000)
      break; /* bounded: wall cap, first violation (stop early), hard cap */
  }
  label_num("[SOAK] done rounds=", rounds_done);
  label_num(" violations=", (long)violations);
  print_console("\n");
  check("soak-completed", violations == 0 && rounds_done > 0);
  /* Leak bound: after the rounds and a settle, free memory must not have
   * dropped more than 32 MiB below the pre-soak high-water baseline. */
  usleep(200000);
  mem_sample();
  label_num("[SOAK] memory drift_kb=",
            (long)((mem_free0 - mem_min) / 1024));
  print_console("\n");
  check("soak-memory-bounded",
        mem_min > 0 && mem_total > 0 && mem_free0 - mem_min <= (32UL << 20));
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (streq(argv[i], "child")) {
      /* exec/wait helper image: report and exit with the requested code */
      int code = (i + 1 < argc) ? atoi_local(argv[i + 1]) : 7;
      print_console("[TORTURE.CHILD] ok\n");
      exit(code & 0xff);
    }
    if (streq(argv[i], "soak")) {
      soak_mode = 1;
    } else if (streq_prefix(argv[i], "soak=")) {
      soak_mode = 1;
      soak_cap_ms = (unsigned long)atoi_local(argv[i] + 5) * 1000UL;
      if (soak_cap_ms < 5000UL)
        soak_cap_ms = 5000UL;
      if (soak_cap_ms > 3600UL * 1000UL)
        soak_cap_ms = 3600UL * 1000UL;
    }
  }

  suite_t0 = now_ms();
  print_console("[TORTURE] POSIX torture suite (P8.1)");
  print_console(soak_mode ? " — SOAK\n" : " — quick\n");

  mem_sample();
  if (soak_mode)
    run_soak();
  else
    run_quick();

  mem_sample();
  print_mem_numbers();
  {
    uint64_t t1 = now_ms();
    label_num("[TORTURE] counts: thr_created=", thr_created);
    label_num(" thr_rejected=", thr_rejected);
    label_num(" fdpass_bad=", fdpass_bad_rounds);
    label_num(" exec_ok=", exec_ok);
    label_num(" exec_rejected=", exec_rejected);
    label_num(" suite_ms=", (long)(t1 - suite_t0));
    print_console("\n");
  }

#ifdef HOST_TEST
  /* Exercise the high-water recorder against the compat mock: lower the
   * mock free value twice; the recorder must keep the minimum. */
  mock_sysinfo_mem_enabled = 1;
  mock_sysinfo_mem_total = 64ULL << 20;
  mock_sysinfo_mem_free = 40ULL << 20;
  mem_sample();
  mock_sysinfo_mem_free = 24ULL << 20;
  mem_sample();
  check("mem-highwater-monotonic", mem_min == (24ULL << 20));
  mock_sysinfo_mem_enabled = 0;
#endif

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    return 0;
  }
  print_console("TORTURE FAILED: ");
  label_num("", fails);
  print_console("\n");
  return 1;
}
