/*
 * src/user/linux_probe_ntsync.c -- /dev/ntsync semantics (C2-ntsync,
 * docs/cp-ntsync.md).
 *
 * ORDINARY musl Linux program: the same binary runs on a Linux 6.14+ box with
 * the ntsync module loaded and in the HobbyOS guest.  Rows "[LP] ...", last
 * "[LP] done rc=<fails>".  The cases are ported (by hand, from the upstream
 * kernel selftest tools/testing/selftests/drivers/ntsync/ntsync.c) and
 * extended with the paths Wine 11.0 uses (fds passed over SCM_RIGHTS to a
 * forked "client", signals, the alert event, stress).
 *
 *   N0  open /dev/ntsync; the UAPI numbers (static-asserted against
 *       <linux/ntsync.h> when the header exists)
 *   S*  semaphore state          (selftest semaphore_state)
 *   M*  mutex state, abandonment (mutex_state)
 *   E*  manual / auto events     (manual_event_state, auto_event_state)
 *   A*  WAIT_ANY                 (test_wait_any)
 *   L*  WAIT_ALL                 (test_wait_all)
 *   I*  invalid objects / args   (invalid_objects)
 *   W*  wakeups of a parked WAIT_ANY (wake_any), timeouts both clocks,
 *       close while waiting, fan-out, FIFO, wake latency (W17)
 *   B*  wakeups of a parked WAIT_ALL (wake_all)
 *   R*  the alert event          (alert_any, alert_all)
 *   G*  a handled signal ends a wait (G4: and how fast, against a pipe read)
 *   F*  Wine's shape: device + object fds over SCM_RIGHTS to a forked child
 *   X*  stress (stress_wait_any / stress_wait_all), a ping-pong latency, and
 *       the cost of CREATE_EVENT + close (the wineserver's per-object price)
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
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- the UAPI (include/uapi/linux/ntsync.h, Linux 6.14) ---------------- */
struct nts_sem_args { uint32_t count, max; };
struct nts_mutex_args { uint32_t owner, count; };
struct nts_event_args { uint32_t manual, signaled; };
#define NTSYNC_WAIT_REALTIME 0x1
struct nts_wait_args {
  uint64_t timeout, objs;
  uint32_t count, index, flags, owner, alert, pad;
};
#define NTSYNC_MAX_WAIT_COUNT 64
#define NT_IOC_CREATE_SEM _IOW('N', 0x80, struct nts_sem_args)
#define NT_IOC_WAIT_ANY _IOWR('N', 0x82, struct nts_wait_args)
#define NT_IOC_WAIT_ALL _IOWR('N', 0x83, struct nts_wait_args)
#define NT_IOC_CREATE_MUTEX _IOW('N', 0x84, struct nts_mutex_args)
#define NT_IOC_CREATE_EVENT _IOW('N', 0x87, struct nts_event_args)
#define NT_IOC_SEM_RELEASE _IOWR('N', 0x81, uint32_t)
#define NT_IOC_MUTEX_UNLOCK _IOWR('N', 0x85, struct nts_mutex_args)
#define NT_IOC_MUTEX_KILL _IOW('N', 0x86, uint32_t)
#define NT_IOC_EVENT_SET _IOR('N', 0x88, uint32_t)
#define NT_IOC_EVENT_RESET _IOR('N', 0x89, uint32_t)
#define NT_IOC_EVENT_PULSE _IOR('N', 0x8a, uint32_t)
#define NT_IOC_SEM_READ _IOR('N', 0x8b, struct nts_sem_args)
#define NT_IOC_MUTEX_READ _IOR('N', 0x8c, struct nts_mutex_args)
#define NT_IOC_EVENT_READ _IOR('N', 0x8d, struct nts_event_args)
#if defined(__has_include)
#if __has_include(<linux/ntsync.h>)
#include <linux/ntsync.h>
_Static_assert(NT_IOC_CREATE_SEM == NTSYNC_IOC_CREATE_SEM, "CREATE_SEM");
_Static_assert(NT_IOC_WAIT_ANY == NTSYNC_IOC_WAIT_ANY, "WAIT_ANY");
_Static_assert(NT_IOC_WAIT_ALL == NTSYNC_IOC_WAIT_ALL, "WAIT_ALL");
_Static_assert(NT_IOC_CREATE_MUTEX == NTSYNC_IOC_CREATE_MUTEX, "CREATE_MUTEX");
_Static_assert(NT_IOC_CREATE_EVENT == NTSYNC_IOC_CREATE_EVENT, "CREATE_EVENT");
_Static_assert(NT_IOC_SEM_RELEASE == NTSYNC_IOC_SEM_RELEASE, "SEM_RELEASE");
_Static_assert(NT_IOC_MUTEX_UNLOCK == NTSYNC_IOC_MUTEX_UNLOCK, "MUTEX_UNLOCK");
_Static_assert(NT_IOC_MUTEX_KILL == NTSYNC_IOC_MUTEX_KILL, "MUTEX_KILL");
_Static_assert(NT_IOC_EVENT_SET == NTSYNC_IOC_EVENT_SET, "EVENT_SET");
_Static_assert(NT_IOC_EVENT_RESET == NTSYNC_IOC_EVENT_RESET, "EVENT_RESET");
_Static_assert(NT_IOC_EVENT_PULSE == NTSYNC_IOC_EVENT_PULSE, "EVENT_PULSE");
_Static_assert(NT_IOC_SEM_READ == NTSYNC_IOC_SEM_READ, "SEM_READ");
_Static_assert(NT_IOC_MUTEX_READ == NTSYNC_IOC_MUTEX_READ, "MUTEX_READ");
_Static_assert(NT_IOC_EVENT_READ == NTSYNC_IOC_EVENT_READ, "EVENT_READ");
#include <stddef.h>
_Static_assert(sizeof(struct nts_wait_args) == sizeof(struct ntsync_wait_args), "wait args");
_Static_assert(offsetof(struct nts_wait_args, index) == offsetof(struct ntsync_wait_args, index), "index");
_Static_assert(offsetof(struct nts_wait_args, alert) == offsetof(struct ntsync_wait_args, alert), "alert");
_Static_assert(offsetof(struct nts_mutex_args, count) == offsetof(struct ntsync_mutex_args, count), "mutex");
_Static_assert(offsetof(struct nts_sem_args, max) == offsetof(struct ntsync_sem_args, max), "sem");
_Static_assert(offsetof(struct nts_event_args, signaled) == offsetof(struct ntsync_event_args, signaled), "event");
#define HAVE_REAL_HEADER 1
#endif
#endif

static int fails;
static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[LP] ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}
static void verdict(const char *id, const char *what, int ok, const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (!ok) fails++;
  row("%s %-46s %s -> %s", id, what, b, ok ? "PASS" : "FAIL");
}

static int dev;

/* ---- helpers (the selftest's) -------------------------------------------- */
static int create_sem(int d, uint32_t count, uint32_t max) {
  struct nts_sem_args a = {count, max};
  return ioctl(d, NT_IOC_CREATE_SEM, &a);
}
static int create_mutex(int d, uint32_t owner, uint32_t count) {
  struct nts_mutex_args a = {owner, count};
  return ioctl(d, NT_IOC_CREATE_MUTEX, &a);
}
static int create_event(int d, uint32_t manual, uint32_t signaled) {
  struct nts_event_args a = {manual, signaled};
  return ioctl(d, NT_IOC_CREATE_EVENT, &a);
}
/* return 0 or the errno */
static int E(int r) { return r < 0 ? errno : 0; }
static int release_sem(int s, uint32_t n, uint32_t *prev) {
  uint32_t v = n;
  int r = E(ioctl(s, NT_IOC_SEM_RELEASE, &v));
  if (prev) *prev = v;
  return r;
}
static int read_sem(int s, uint32_t *count, uint32_t *max) {
  struct nts_sem_args a = {0xdead, 0xbeef};
  int r = E(ioctl(s, NT_IOC_SEM_READ, &a));
  *count = a.count;
  *max = a.max;
  return r;
}
static int unlock_mutex(int m, uint32_t owner, uint32_t *prev) {
  struct nts_mutex_args a = {owner, 0xdead};
  int r = E(ioctl(m, NT_IOC_MUTEX_UNLOCK, &a));
  if (prev) *prev = a.count;
  return r;
}
static int kill_mutex(int m, uint32_t owner) {
  uint32_t v = owner;
  return E(ioctl(m, NT_IOC_MUTEX_KILL, &v));
}
static int read_mutex(int m, uint32_t *count, uint32_t *owner) {
  struct nts_mutex_args a = {0xdead, 0xbeef};
  int r = E(ioctl(m, NT_IOC_MUTEX_READ, &a));
  *count = a.count;
  *owner = a.owner;
  return r;
}
static int ev_op(int e, unsigned long req, uint32_t *prev) {
  uint32_t v = 0xdead;
  int r = E(ioctl(e, req, &v));
  if (prev) *prev = v;
  return r;
}
static int read_event(int e, uint32_t *signaled, uint32_t *manual) {
  struct nts_event_args a = {0xdead, 0xbeef};
  int r = E(ioctl(e, NT_IOC_EVENT_READ, &a));
  *signaled = a.signaled;
  *manual = a.manual;
  return r;
}
static uint64_t now_ns(clockid_t c) {
  struct timespec ts;
  clock_gettime(c, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* wait with timeout = now (the selftest's "check, do not block") */
static int wait_objs(int d, unsigned long req, uint32_t count, const int *objs,
                     uint32_t owner, int alert, uint32_t *index) {
  struct nts_wait_args a;
  memset(&a, 0, sizeof a);
  a.timeout = now_ns(CLOCK_MONOTONIC);
  a.count = count;
  a.objs = (uintptr_t)objs;
  a.owner = owner;
  a.index = 0xdeadbeef;
  a.alert = (uint32_t)alert;
  int r = E(ioctl(d, req, &a));
  if (index) *index = a.index;
  return r;
}
static int wait_any(uint32_t n, const int *o, uint32_t owner, uint32_t *idx) {
  return wait_objs(dev, NT_IOC_WAIT_ANY, n, o, owner, 0, idx);
}
static int wait_all(uint32_t n, const int *o, uint32_t owner, uint32_t *idx) {
  return wait_objs(dev, NT_IOC_WAIT_ALL, n, o, owner, 0, idx);
}
static const char *en(int e) {
  static char b[8][24];
  static int k;
  char *s = b[k++ & 7];
  switch (e) {
  case 0: return "0";
  case EINVAL: return "EINVAL";
  case EPERM: return "EPERM";
  case EOVERFLOW: return "EOVERFLOW";
  case ETIMEDOUT: return "ETIMEDOUT";
  case EOWNERDEAD: return "EOWNERDEAD";
  case EFAULT: return "EFAULT";
  case ENOTTY: return "ENOTTY";
  case EINTR: return "EINTR";
  case EBADF: return "EBADF";
  case ENOENT: return "ENOENT";
  default: snprintf(s, 24, "errno%d", e); return s;
  }
}

/* ---- a thread that does one blocking wait -------------------------------- */
struct waiter {
  unsigned long req;
  uint32_t count;
  int objs[8];
  uint32_t owner;
  int alert;
  uint64_t timeout_ns; /* absolute, or ~0 */
  uint32_t flags;
  volatile int done;
  int err;
  uint32_t index;
  uint64_t t0, t1;
  pthread_t th;
};
static void *waiter_fn(void *arg) {
  struct waiter *w = arg;
  struct nts_wait_args a;
  memset(&a, 0, sizeof a);
  a.timeout = w->timeout_ns;
  a.objs = (uintptr_t)w->objs;
  a.count = w->count;
  a.owner = w->owner;
  a.alert = (uint32_t)w->alert;
  a.flags = w->flags;
  a.index = 0xdeadbeef;
  w->t0 = now_ns(CLOCK_MONOTONIC);
  int r = ioctl(dev, w->req, &a);
  w->err = r < 0 ? errno : 0;
  w->index = a.index;
  w->t1 = now_ns(CLOCK_MONOTONIC);
  __atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
  return 0;
}
static void start_waiter(struct waiter *w, unsigned long req, uint32_t count,
                         const int *objs, uint32_t owner, int alert, int timeout_ms) {
  w->req = req;
  w->count = count;
  for (uint32_t i = 0; i < count && i < 8; i++) w->objs[i] = objs[i];
  w->owner = owner;
  w->alert = alert;
  w->flags = 0;
  w->timeout_ns = timeout_ms < 0 ? ~0ull
                                 : now_ns(CLOCK_MONOTONIC) + (uint64_t)timeout_ms * 1000000ull;
  w->done = 0;
  w->err = -1;
  pthread_create(&w->th, 0, waiter_fn, w);
}
/* has it finished within ms? */
static int wait_done(struct waiter *w, int ms) {
  for (int i = 0; i < ms; i++) {
    if (__atomic_load_n(&w->done, __ATOMIC_SEQ_CST)) return 1;
    usleep(1000);
  }
  return __atomic_load_n(&w->done, __ATOMIC_SEQ_CST);
}
static void join_waiter(struct waiter *w) { pthread_join(w->th, 0); }

/* ---- S: semaphore state -------------------------------------------------- */
static void t_sem(void) {
  uint32_t c, m, idx, prev;
  int r = create_sem(dev, 3, 2);
  verdict("S1", "create count 3 > max 2", r < 0 && errno == EINVAL, "%d %s", r, en(r < 0 ? errno : 0));
  int s = create_sem(dev, 2, 2);
  verdict("S2", "create sem 2/2 -> fd", s >= 0, "fd=%d", s);
  int e = read_sem(s, &c, &m);
  verdict("S3", "SEM_READ", !e && c == 2 && m == 2, "%s %u/%u", en(e), c, m);
  e = release_sem(s, 0, &prev);
  read_sem(s, &c, &m);
  verdict("S4", "release 0 (fits)", !e && prev == 2 && c == 2, "%s prev=%u count=%u", en(e), prev, c);
  e = release_sem(s, 1, &prev);
  read_sem(s, &c, &m);
  verdict("S5", "release 1 past max -> EOVERFLOW, unchanged", e == EOVERFLOW && c == 2, "%s count=%u", en(e), c);
  int e1 = wait_any(1, &s, 123, &idx);
  read_sem(s, &c, &m);
  verdict("S6", "wait takes one", !e1 && idx == 0 && c == 1, "%s idx=%u count=%u", en(e1), idx, c);
  e1 = wait_any(1, &s, 123, &idx);
  read_sem(s, &c, &m);
  verdict("S7", "wait takes the last", !e1 && idx == 0 && c == 0, "%s idx=%u count=%u", en(e1), idx, c);
  e1 = wait_any(1, &s, 123, &idx);
  verdict("S8", "empty -> ETIMEDOUT", e1 == ETIMEDOUT, "%s", en(e1));
  e = release_sem(s, 3, &prev);
  read_sem(s, &c, &m);
  verdict("S9", "release 3 > max -> EOVERFLOW", e == EOVERFLOW && c == 0, "%s count=%u", en(e), c);
  e = release_sem(s, 2, &prev);
  read_sem(s, &c, &m);
  verdict("S10", "release 2 -> prev 0, count 2", !e && prev == 0 && c == 2, "%s prev=%u count=%u", en(e), prev, c);
  e = release_sem(s, 1, &prev);
  verdict("S11", "release 1 at max -> EOVERFLOW", e == EOVERFLOW, "%s", en(e));
  wait_any(1, &s, 123, &idx);
  wait_any(1, &s, 123, &idx);
  e = release_sem(s, 1, &prev);
  read_sem(s, &c, &m);
  verdict("S12", "drain, release 1 -> prev 0, count 1", !e && prev == 0 && c == 1, "%s prev=%u count=%u", en(e), prev, c);
  e = release_sem(s, 0xffffffffu, &prev);
  read_sem(s, &c, &m);
  verdict("S13", "release UINT_MAX (u32 overflow) -> EOVERFLOW", e == EOVERFLOW && c == 1, "%s count=%u", en(e), c);
  e = unlock_mutex(s, 123, 0);
  int e2 = ev_op(s, NT_IOC_EVENT_SET, 0);
  uint32_t a1, a2;
  int e3 = read_mutex(s, &a1, &a2);
  verdict("S14", "mutex/event ops on a sem -> EINVAL", e == EINVAL && e2 == EINVAL && e3 == EINVAL,
          "%s %s %s", en(e), en(e2), en(e3));
  int e4 = E(ioctl(s, NT_IOC_SEM_RELEASE, (void *)0));
  verdict("S15", "SEM_RELEASE(NULL) -> EFAULT", e4 == EFAULT, "%s", en(e4));
  close(s);
}

/* ---- M: mutex state ------------------------------------------------------ */
static void t_mutex(void) {
  uint32_t c, o, idx, prev;
  int r1 = create_mutex(dev, 123, 0);
  int e1 = r1 < 0 ? errno : 0;
  int r2 = create_mutex(dev, 0, 2);
  int e2 = r2 < 0 ? errno : 0;
  verdict("M1", "create owner!=0,count 0 / owner 0,count 2", e1 == EINVAL && e2 == EINVAL, "%s %s", en(e1), en(e2));
  int m = create_mutex(dev, 123, 2);
  int e = read_mutex(m, &c, &o);
  verdict("M2", "create 123/2, MUTEX_READ", m >= 0 && !e && c == 2 && o == 123, "%s count=%u owner=%u", en(e), c, o);
  e = wait_any(1, &m, 456, &idx);
  verdict("M3", "other owner waits -> ETIMEDOUT", e == ETIMEDOUT, "%s", en(e));
  e = wait_any(1, &m, 123, &idx);
  read_mutex(m, &c, &o);
  verdict("M4", "owner re-acquires (recursive)", !e && idx == 0 && c == 3 && o == 123, "%s count=%u", en(e), c);
  e = unlock_mutex(m, 0, 0);
  e1 = unlock_mutex(m, 456, 0);
  read_mutex(m, &c, &o);
  verdict("M5", "unlock owner 0 -> EINVAL, 456 -> EPERM", e == EINVAL && e1 == EPERM && c == 3, "%s %s count=%u", en(e), en(e1), c);
  uint32_t p1 = 0, p2 = 0, p3 = 0;
  e = unlock_mutex(m, 123, &p1);
  e1 = unlock_mutex(m, 123, &p2);
  e2 = unlock_mutex(m, 123, &p3);
  read_mutex(m, &c, &o);
  verdict("M6", "unlock x3 -> prev 3,2,1; unowned", !e && !e1 && !e2 && p1 == 3 && p2 == 2 && p3 == 1 && c == 0 && o == 0,
          "prev=%u,%u,%u count=%u owner=%u", p1, p2, p3, c, o);
  e = unlock_mutex(m, 123, &prev);
  verdict("M7", "unlock unowned -> EPERM", e == EPERM, "%s", en(e));
  e = wait_any(1, &m, 456, &idx);
  read_mutex(m, &c, &o);
  verdict("M8", "456 acquires the free mutex", !e && c == 1 && o == 456, "%s count=%u owner=%u", en(e), c, o);
  e = kill_mutex(m, 0);
  e1 = kill_mutex(m, 123);
  e2 = kill_mutex(m, 456);
  verdict("M9", "kill 0 -> EINVAL, 123 -> EPERM, 456 -> 0", e == EINVAL && e1 == EPERM && !e2, "%s %s %s", en(e), en(e1), en(e2));
  e = read_mutex(m, &c, &o);
  verdict("M10", "MUTEX_READ of abandoned -> EOWNERDEAD", e == EOWNERDEAD && c == 0 && o == 0, "%s count=%u owner=%u", en(e), c, o);
  e = wait_any(1, &m, 123, &idx);
  int er = read_mutex(m, &c, &o);
  verdict("M11", "wait on abandoned -> EOWNERDEAD idx 0, owned", e == EOWNERDEAD && idx == 0 && !er && c == 1 && o == 123,
          "%s idx=%u read=%s count=%u owner=%u", en(e), idx, en(er), c, o);
  e = unlock_mutex(m, 123, &prev);
  e1 = kill_mutex(m, 123);
  verdict("M12", "unlock, then kill unowned -> EPERM", !e && prev == 1 && e1 == EPERM, "%s prev=%u %s", en(e), prev, en(e1));
  wait_any(1, &m, 123, &idx);
  e = kill_mutex(m, 123);
  e1 = wait_any(1, &m, 123, &idx);
  er = read_mutex(m, &c, &o);
  verdict("M13", "kill by owner, re-acquire -> EOWNERDEAD", !e && e1 == EOWNERDEAD && !er && c == 1 && o == 123,
          "%s %s count=%u owner=%u", en(e), en(e1), c, o);
  close(m);
  m = create_mutex(dev, 0, 0);
  e = read_mutex(m, &c, &o);
  e1 = wait_any(1, &m, 123, &idx);
  read_mutex(m, &c, &o);
  verdict("M14", "create unowned 0/0, acquire", m >= 0 && !e && !e1 && c == 1 && o == 123, "%s %s count=%u owner=%u", en(e), en(e1), c, o);
  close(m);
  m = create_mutex(dev, 123, 0xffffffffu);
  e = wait_any(1, &m, 123, &idx);
  read_mutex(m, &c, &o);
  verdict("M15", "count UINT_MAX: owner cannot recurse", e == ETIMEDOUT && c == 0xffffffffu, "%s count=%u", en(e), c);
  close(m);
}

/* ---- E: events ----------------------------------------------------------- */
static void t_events(void) {
  uint32_t s, mn, prev, idx;
  int ev = create_event(dev, 1, 0);
  int e = read_event(ev, &s, &mn);
  verdict("E1", "manual event create, EVENT_READ", ev >= 0 && !e && s == 0 && mn == 1, "%s sig=%u manual=%u", en(e), s, mn);
  int e1 = ev_op(ev, NT_IOC_EVENT_SET, &prev);
  read_event(ev, &s, &mn);
  verdict("E2", "set -> prev 0, signaled", !e1 && prev == 0 && s == 1, "%s prev=%u sig=%u", en(e1), prev, s);
  e1 = ev_op(ev, NT_IOC_EVENT_SET, &prev);
  verdict("E3", "set again -> prev 1", !e1 && prev == 1, "%s prev=%u", en(e1), prev);
  e1 = wait_any(1, &ev, 123, &idx);
  read_event(ev, &s, &mn);
  verdict("E4", "wait on manual keeps it signaled", !e1 && idx == 0 && s == 1, "%s sig=%u", en(e1), s);
  e1 = ev_op(ev, NT_IOC_EVENT_RESET, &prev);
  read_event(ev, &s, &mn);
  int e2 = ev_op(ev, NT_IOC_EVENT_RESET, &idx);
  verdict("E5", "reset -> prev 1; again -> prev 0", !e1 && prev == 1 && s == 0 && !e2 && idx == 0, "prev=%u,%u sig=%u", prev, idx, s);
  e1 = wait_any(1, &ev, 123, &idx);
  verdict("E6", "wait on reset event -> ETIMEDOUT", e1 == ETIMEDOUT, "%s", en(e1));
  ev_op(ev, NT_IOC_EVENT_SET, &prev);
  e1 = ev_op(ev, NT_IOC_EVENT_PULSE, &prev);
  read_event(ev, &s, &mn);
  verdict("E7", "pulse a set manual event -> prev 1, reset", !e1 && prev == 1 && s == 0, "%s prev=%u sig=%u", en(e1), prev, s);
  e1 = ev_op(ev, NT_IOC_EVENT_PULSE, &prev);
  read_event(ev, &s, &mn);
  verdict("E8", "pulse with no waiter -> prev 0, stays reset", !e1 && prev == 0 && s == 0, "%s prev=%u sig=%u", en(e1), prev, s);
  close(ev);

  ev = create_event(dev, 0, 1);
  e = read_event(ev, &s, &mn);
  verdict("E9", "auto event create signaled", ev >= 0 && !e && s == 1 && mn == 0, "%s sig=%u manual=%u", en(e), s, mn);
  e1 = wait_any(1, &ev, 123, &idx);
  read_event(ev, &s, &mn);
  verdict("E10", "wait on auto resets it", !e1 && idx == 0 && s == 0, "%s sig=%u", en(e1), s);
  e1 = wait_any(1, &ev, 123, &idx);
  verdict("E11", "again -> ETIMEDOUT", e1 == ETIMEDOUT, "%s", en(e1));
  e1 = ev_op(ev, NT_IOC_EVENT_SET, &prev);
  read_event(ev, &s, &mn);
  e2 = ev_op(ev, NT_IOC_EVENT_RESET, &idx);
  uint32_t s2;
  read_event(ev, &s2, &mn);
  verdict("E12", "auto set -> prev 0 sig 1; reset -> prev 1", !e1 && prev == 0 && s == 1 && !e2 && idx == 1 && s2 == 0,
          "prev=%u sig=%u prev=%u sig=%u", prev, s, idx, s2);
  e1 = ev_op(ev, NT_IOC_EVENT_PULSE, &prev);
  read_event(ev, &s, &mn);
  verdict("E13", "auto pulse, no waiter -> prev 0, reset", !e1 && prev == 0 && s == 0, "%s prev=%u sig=%u", en(e1), prev, s);
  e1 = create_event(dev, 7, 9);
  read_event(e1, &s, &mn);
  verdict("E14", "create {manual 7, signaled 9} reads as booleans", e1 >= 0 && s == 1 && mn == 1, "sig=%u manual=%u", s, mn);
  close(e1);
  close(ev);
}

/* ---- A: WAIT_ANY ---------------------------------------------------------- */
static void t_wait_any(void) {
  uint32_t c, o, idx, prev;
  int objs[2];
  objs[0] = create_sem(dev, 2, 3);
  objs[1] = create_mutex(dev, 0, 0);
  int e = wait_any(2, objs, 123, &idx);
  read_sem(objs[0], &c, &o);
  verdict("A1", "[sem 2, mutex free] -> sem first", !e && idx == 0 && c == 1, "%s idx=%u sem=%u", en(e), idx, c);
  e = wait_any(2, objs, 123, &idx);
  read_sem(objs[0], &c, &o);
  verdict("A2", "again -> sem", !e && idx == 0 && c == 0, "%s idx=%u sem=%u", en(e), idx, c);
  e = wait_any(2, objs, 123, &idx);
  read_mutex(objs[1], &c, &o);
  verdict("A3", "sem empty -> mutex", !e && idx == 1 && c == 1 && o == 123, "%s idx=%u mutex=%u/%u", en(e), idx, c, o);
  e = wait_any(2, objs, 123, &idx);
  read_mutex(objs[1], &c, &o);
  verdict("A4", "owner again -> mutex (count 2)", !e && idx == 1 && c == 2, "%s idx=%u count=%u", en(e), idx, c);
  e = wait_any(2, objs, 456, &idx);
  verdict("A5", "456: sem empty, mutex owned -> ETIMEDOUT", e == ETIMEDOUT, "%s", en(e));
  release_sem(objs[0], 1, &prev);
  e = wait_any(2, objs, 456, &idx);
  verdict("A6", "456 after release -> sem", !e && idx == 0, "%s idx=%u", en(e), idx);
  e = wait_any(2, objs, 456, &idx);
  verdict("A7", "456 again -> ETIMEDOUT", e == ETIMEDOUT, "%s", en(e));
  int dup[2] = {objs[0], objs[0]};
  e = wait_any(2, dup, 123, &idx);
  release_sem(objs[0], 1, &prev);
  int e2 = wait_any(2, dup, 123, &idx);
  read_sem(objs[0], &c, &o);
  verdict("A8", "duplicate fds are allowed in WAIT_ANY", e == ETIMEDOUT && !e2 && idx == 0 && c == 0, "%s %s idx=%u sem=%u", en(e), en(e2), idx, c);
  e = wait_any(0, objs, 123, &idx);
  verdict("A9", "count 0 -> ETIMEDOUT", e == ETIMEDOUT, "%s", en(e));
  kill_mutex(objs[1], 123);
  e = wait_any(2, objs, 456, &idx);
  read_mutex(objs[1], &c, &o);
  verdict("A10", "abandoned mutex at index 1 -> EOWNERDEAD idx 1", e == EOWNERDEAD && idx == 1 && c == 1 && o == 456,
          "%s idx=%u mutex=%u/%u", en(e), idx, c, o);
  close(objs[0]);
  close(objs[1]);
}

/* ---- L: WAIT_ALL ---------------------------------------------------------- */
static void t_wait_all(void) {
  uint32_t c, o, idx, prev, s, mn;
  int objs[2];
  objs[0] = create_sem(dev, 2, 3);
  objs[1] = create_mutex(dev, 0, 0);
  int man = create_event(dev, 1, 1), aut = create_event(dev, 0, 1);
  int e = wait_all(2, objs, 123, &idx);
  uint32_t sc, mc;
  read_sem(objs[0], &sc, &o);
  read_mutex(objs[1], &mc, &o);
  verdict("L1", "[sem 2, mutex free] -> both taken", !e && idx == 0 && sc == 1 && mc == 1 && o == 123,
          "%s idx=%u sem=%u mutex=%u/%u", en(e), idx, sc, mc, o);
  e = wait_all(2, objs, 123, &idx);
  read_sem(objs[0], &sc, &o);
  read_mutex(objs[1], &mc, &o);
  verdict("L2", "again -> sem 0, mutex 2", !e && sc == 0 && mc == 2, "%s sem=%u mutex=%u", en(e), sc, mc);
  e = wait_all(2, objs, 123, &idx);
  read_sem(objs[0], &sc, &o);
  read_mutex(objs[1], &mc, &o);
  verdict("L3", "sem empty -> ETIMEDOUT, mutex untouched", e == ETIMEDOUT && sc == 0 && mc == 2, "%s sem=%u mutex=%u", en(e), sc, mc);
  release_sem(objs[0], 1, &prev);
  e = wait_all(2, objs, 456, &idx);
  read_sem(objs[0], &sc, &o);
  verdict("L4", "456: mutex owned -> ETIMEDOUT, sem NOT taken", e == ETIMEDOUT && sc == 1, "%s sem=%u", en(e), sc);
  int o2[2] = {objs[0], man};
  e = wait_all(2, o2, 123, &idx);
  read_sem(objs[0], &sc, &o);
  read_event(man, &s, &mn);
  verdict("L5", "[sem, manual set] -> sem taken, event stays", !e && sc == 0 && s == 1, "%s sem=%u ev=%u", en(e), sc, s);
  release_sem(objs[0], 1, &prev);
  o2[1] = aut;
  e = wait_all(2, o2, 123, &idx);
  read_sem(objs[0], &sc, &o);
  read_event(aut, &s, &mn);
  verdict("L6", "[sem, auto set] -> both consumed", !e && sc == 0 && s == 0, "%s sem=%u ev=%u", en(e), sc, s);
  int dup[2] = {objs[0], objs[0]};
  e = wait_all(2, dup, 123, &idx);
  verdict("L7", "duplicate fds in WAIT_ALL -> EINVAL", e == EINVAL, "%s", en(e));
  unlock_mutex(objs[1], 123, &prev);
  unlock_mutex(objs[1], 123, &prev);
  wait_any(1, &objs[1], 123, &idx);
  kill_mutex(objs[1], 123);
  release_sem(objs[0], 1, &prev);
  e = wait_all(2, objs, 456, &idx);
  read_mutex(objs[1], &mc, &o);
  verdict("L8", "abandoned mutex in a set -> EOWNERDEAD, taken", e == EOWNERDEAD && mc == 1 && o == 456, "%s idx=%u mutex=%u/%u", en(e), idx, mc, o);
  (void)c;
  close(objs[0]);
  close(objs[1]);
  close(man);
  close(aut);
}

/* ---- I: invalid objects / arguments -------------------------------------- */
static void t_invalid(void) {
  uint32_t idx;
  int s = create_sem(dev, 1, 1);
  int p[2];
  if (pipe(p)) p[0] = p[1] = -1;
  int bad[2] = {dev, s};
  int e = wait_any(2, bad, 123, &idx);
  int e2 = wait_all(2, bad, 123, &idx);
  verdict("I1", "the device fd as an object -> EINVAL", e == EINVAL && e2 == EINVAL, "%s %s", en(e), en(e2));
  bad[0] = p[0];
  e = wait_any(2, bad, 123, &idx);
  verdict("I2", "a pipe fd as an object -> EINVAL", e == EINVAL, "%s", en(e));
  bad[0] = -1;
  e = wait_any(2, bad, 123, &idx);
  verdict("I3", "fd -1 -> EINVAL", e == EINVAL, "%s", en(e));
  struct nts_wait_args a;
  memset(&a, 0, sizeof a);
  a.objs = (uintptr_t)&s;
  a.count = 1;
  a.owner = 1;
  a.pad = 1;
  e = E(ioctl(dev, NT_IOC_WAIT_ANY, &a));
  a.pad = 0;
  a.flags = 2;
  e2 = E(ioctl(dev, NT_IOC_WAIT_ANY, &a));
  a.flags = 0;
  a.count = NTSYNC_MAX_WAIT_COUNT + 1;
  int e3 = E(ioctl(dev, NT_IOC_WAIT_ANY, &a));
  verdict("I4", "pad != 0 / flags 2 / count 65 -> EINVAL", e == EINVAL && e2 == EINVAL && e3 == EINVAL, "%s %s %s", en(e), en(e2), en(e3));
  a.count = 1;
  a.objs = 0;
  e = E(ioctl(dev, NT_IOC_WAIT_ANY, &a));
  e2 = E(ioctl(dev, NT_IOC_WAIT_ANY, (void *)0));
  verdict("I5", "objs NULL / args NULL -> EFAULT", e == EFAULT && e2 == EFAULT, "%s %s", en(e), en(e2));
  uint32_t v = 0;
  e = E(ioctl(dev, NT_IOC_SEM_READ, &v));
  struct nts_sem_args sa = {1, 1};
  e2 = E(ioctl(s, NT_IOC_CREATE_SEM, &sa));
  verdict("I6", "object ioctl on device / create on object -> ENOTTY", e == ENOTTY && e2 == ENOTTY, "%s %s", en(e), en(e2));
  int dev2 = open("/dev/ntsync", O_RDONLY | O_CLOEXEC);
  int s2 = create_sem(dev2, 1, 1);
  int mix[2] = {s, s2};
  e = wait_any(2, mix, 123, &idx);
  e2 = wait_objs(dev2, NT_IOC_WAIT_ANY, 1, &s, 123, 0, &idx);
  int e4 = wait_objs(dev2, NT_IOC_WAIT_ANY, 1, &s2, 123, 0, &idx);
  verdict("I7", "an object of another device -> EINVAL", e == EINVAL && e2 == EINVAL && !e4, "%s %s own=%s", en(e), en(e2), en(e4));
  int d = dup(s);
  close(s);
  uint32_t prev;
  e = release_sem(d, 0, &prev);
  e2 = wait_any(1, &d, 123, &idx);
  verdict("I8", "a dup keeps the object after close", !e && prev == 1 && !e2, "%s prev=%u %s", en(e), prev, en(e2));
  close(d);
  close(s2);
  close(dev2);
  close(p[0]);
  close(p[1]);
}

/* ---- W: wake a parked WAIT_ANY ------------------------------------------- */
static void t_wake_any(void) {
  uint32_t c, o, prev, s, mn;
  int objs[2];
  objs[0] = create_sem(dev, 0, 3);
  objs[1] = create_mutex(dev, 123, 1);
  struct waiter w;
  start_waiter(&w, NT_IOC_WAIT_ANY, 2, objs, 456, 0, 3000);
  int early = wait_done(&w, 100);
  release_sem(objs[0], 1, &prev);
  int done = wait_done(&w, 1000);
  join_waiter(&w);
  read_sem(objs[0], &c, &o);
  verdict("W1", "sem release wakes the waiter (takes it)", !early && done && !w.err && w.index == 0 && c == 0,
          "early=%d err=%s idx=%u sem=%u", early, en(w.err), w.index, c);
  start_waiter(&w, NT_IOC_WAIT_ANY, 2, objs, 456, 0, 3000);
  early = wait_done(&w, 100);
  unlock_mutex(objs[1], 123, &prev);
  done = wait_done(&w, 1000);
  join_waiter(&w);
  read_mutex(objs[1], &c, &o);
  verdict("W2", "mutex unlock hands it to the waiter", !early && done && !w.err && w.index == 1 && c == 1 && o == 456,
          "early=%d err=%s idx=%u mutex=%u/%u", early, en(w.err), w.index, c, o);
  /* the waiter now owns it; abandon test: 123 waits, main kills 456 */
  start_waiter(&w, NT_IOC_WAIT_ANY, 2, objs, 123, 0, 3000);
  early = wait_done(&w, 100);
  int ek = kill_mutex(objs[1], 456);
  done = wait_done(&w, 1000);
  join_waiter(&w);
  verdict("W3", "MUTEX_KILL wakes the waiter with EOWNERDEAD", !early && !ek && done && w.err == EOWNERDEAD && w.index == 1,
          "early=%d kill=%s err=%s idx=%u", early, en(ek), en(w.err), w.index);
  close(objs[1]);

  int man = create_event(dev, 1, 0), aut = create_event(dev, 0, 0);
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &man, 1, 0, 3000);
  early = wait_done(&w, 100);
  ev_op(man, NT_IOC_EVENT_SET, &prev);
  done = wait_done(&w, 1000);
  join_waiter(&w);
  read_event(man, &s, &mn);
  verdict("W4", "manual set wakes, stays set", !early && done && !w.err && s == 1, "early=%d err=%s sig=%u", early, en(w.err), s);
  ev_op(man, NT_IOC_EVENT_RESET, &prev);
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &man, 1, 0, 3000);
  early = wait_done(&w, 100);
  ev_op(man, NT_IOC_EVENT_PULSE, &prev);
  done = wait_done(&w, 1000);
  join_waiter(&w);
  read_event(man, &s, &mn);
  verdict("W5", "manual pulse wakes the queued waiter, ends reset", !early && done && !w.err && s == 0,
          "early=%d err=%s sig=%u", early, en(w.err), s);
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &aut, 1, 0, 3000);
  early = wait_done(&w, 100);
  ev_op(aut, NT_IOC_EVENT_SET, &prev);
  done = wait_done(&w, 1000);
  join_waiter(&w);
  read_event(aut, &s, &mn);
  verdict("W6", "auto set wakes and is consumed", !early && done && !w.err && s == 0, "early=%d err=%s sig=%u", early, en(w.err), s);
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &aut, 1, 0, 3000);
  early = wait_done(&w, 100);
  ev_op(aut, NT_IOC_EVENT_PULSE, &prev);
  done = wait_done(&w, 1000);
  join_waiter(&w);
  verdict("W7", "auto pulse wakes the queued waiter", !early && done && !w.err, "early=%d err=%s", early, en(w.err));

  /* timeouts */
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &aut, 1, 0, 100);
  done = wait_done(&w, 2000);
  join_waiter(&w);
  uint64_t el = (w.t1 - w.t0) / 1000;
  verdict("W8", "monotonic 100 ms timeout", done && w.err == ETIMEDOUT && el >= 99000 && el < 600000,
          "err=%s elapsed=%lluus", en(w.err), (unsigned long long)el);
  w.req = NT_IOC_WAIT_ANY;
  w.count = 1;
  w.objs[0] = aut;
  w.owner = 1;
  w.alert = 0;
  w.flags = NTSYNC_WAIT_REALTIME;
  w.timeout_ns = now_ns(CLOCK_REALTIME) + 100000000ull;
  w.done = 0;
  pthread_create(&w.th, 0, waiter_fn, &w);
  done = wait_done(&w, 2000);
  join_waiter(&w);
  el = (w.t1 - w.t0) / 1000;
  verdict("W9", "REALTIME absolute 100 ms timeout", done && w.err == ETIMEDOUT && el >= 95000 && el < 600000,
          "err=%s elapsed=%lluus", en(w.err), (unsigned long long)el);
  {
    struct nts_wait_args a;
    memset(&a, 0, sizeof a);
    a.objs = (uintptr_t)&aut;
    a.count = 1;
    a.owner = 1;
    a.flags = NTSYNC_WAIT_REALTIME;
    a.timeout = now_ns(CLOCK_REALTIME) - 1000000000ull;
    uint64_t t0 = now_ns(CLOCK_MONOTONIC);
    int e = E(ioctl(dev, NT_IOC_WAIT_ANY, &a));
    uint64_t us = (now_ns(CLOCK_MONOTONIC) - t0) / 1000;
    verdict("W10", "a past REALTIME deadline returns at once", e == ETIMEDOUT && us < 50000, "%s %lluus", en(e), (unsigned long long)us);
  }

  /* close the objects while a thread waits on them */
  int sem2 = create_sem(dev, 0, 1), mx2 = create_mutex(dev, 7, 1);
  int o3[2] = {sem2, mx2};
  start_waiter(&w, NT_IOC_WAIT_ANY, 2, o3, 456, 0, 300);
  early = wait_done(&w, 100);
  close(sem2);
  close(mx2);
  done = wait_done(&w, 2000);
  join_waiter(&w);
  verdict("W11", "close while waited on: wait still times out", !early && done && w.err == ETIMEDOUT, "early=%d err=%s", early, en(w.err));

  /* fan-out: 3 waiters on a manual event / an auto event / a sem */
  struct waiter ws[3];
  ev_op(man, NT_IOC_EVENT_RESET, &prev);
  for (int i = 0; i < 3; i++) start_waiter(&ws[i], NT_IOC_WAIT_ANY, 1, &man, 10 + i, 0, 3000);
  usleep(100000);
  ev_op(man, NT_IOC_EVENT_SET, &prev);
  int nd = 0;
  for (int i = 0; i < 3; i++) nd += wait_done(&ws[i], 1000) && !ws[i].err;
  for (int i = 0; i < 3; i++) join_waiter(&ws[i]);
  verdict("W12", "manual set wakes all 3 waiters", nd == 3, "woken=%d", nd);
  ev_op(man, NT_IOC_EVENT_RESET, &prev);
  for (int i = 0; i < 3; i++) start_waiter(&ws[i], NT_IOC_WAIT_ANY, 1, &man, 10 + i, 0, 3000);
  usleep(100000);
  ev_op(man, NT_IOC_EVENT_PULSE, &prev);
  nd = 0;
  for (int i = 0; i < 3; i++) nd += wait_done(&ws[i], 1000) && !ws[i].err;
  for (int i = 0; i < 3; i++) join_waiter(&ws[i]);
  read_event(man, &s, &mn);
  verdict("W13", "manual pulse wakes all 3, ends reset", nd == 3 && s == 0, "woken=%d sig=%u", nd, s);
  for (int i = 0; i < 3; i++) start_waiter(&ws[i], NT_IOC_WAIT_ANY, 1, &aut, 10 + i, 0, 1500);
  usleep(100000);
  ev_op(aut, NT_IOC_EVENT_SET, &prev);
  usleep(150000);
  int n1 = 0;
  for (int i = 0; i < 3; i++) n1 += __atomic_load_n(&ws[i].done, __ATOMIC_SEQ_CST) && !ws[i].err;
  ev_op(aut, NT_IOC_EVENT_SET, &prev);
  usleep(150000);
  int n2 = 0;
  for (int i = 0; i < 3; i++) n2 += __atomic_load_n(&ws[i].done, __ATOMIC_SEQ_CST) && !ws[i].err;
  for (int i = 0; i < 3; i++) join_waiter(&ws[i]);
  int nt = 0;
  for (int i = 0; i < 3; i++) nt += ws[i].err == ETIMEDOUT;
  verdict("W14", "auto set wakes exactly one at a time", n1 == 1 && n2 == 2 && nt == 1, "after1=%d after2=%d timedout=%d", n1, n2, nt);
  int sm = create_sem(dev, 0, 5);
  for (int i = 0; i < 3; i++) start_waiter(&ws[i], NT_IOC_WAIT_ANY, 1, &sm, 10 + i, 0, 800);
  usleep(100000);
  release_sem(sm, 2, &prev);
  for (int i = 0; i < 3; i++) join_waiter(&ws[i]);
  nd = 0;
  nt = 0;
  for (int i = 0; i < 3; i++) {
    nd += !ws[i].err;
    nt += ws[i].err == ETIMEDOUT;
  }
  read_sem(sm, &c, &o);
  verdict("W15", "release 2 with 3 waiters: 2 woken, 1 times out", nd == 2 && nt == 1 && c == 0, "woken=%d timedout=%d sem=%u", nd, nt, c);
  /* FIFO: the first queued waiter is served first */
  start_waiter(&ws[0], NT_IOC_WAIT_ANY, 1, &sm, 21, 0, 2000);
  usleep(60000);
  start_waiter(&ws[1], NT_IOC_WAIT_ANY, 1, &sm, 22, 0, 2000);
  usleep(60000);
  release_sem(sm, 1, &prev);
  usleep(100000);
  int first = __atomic_load_n(&ws[0].done, __ATOMIC_SEQ_CST), second = __atomic_load_n(&ws[1].done, __ATOMIC_SEQ_CST);
  release_sem(sm, 1, &prev);
  join_waiter(&ws[0]);
  join_waiter(&ws[1]);
  verdict("W16", "FIFO: the earlier waiter gets the first release", first && !second && !ws[0].err && !ws[1].err,
          "first=%d second=%d", first, second);
  /* W17: EVENT_SET -> the parked waiter running again */
  {
    enum { N = 100 };
    uint64_t lat[N];
    int bad = 0;
    for (int i = 0; i < N; i++) {
      start_waiter(&ws[0], NT_IOC_WAIT_ANY, 1, &aut, 31, 0, 3000);
      usleep(3000);
      uint64_t t0 = now_ns(CLOCK_MONOTONIC);
      ev_op(aut, NT_IOC_EVENT_SET, &prev);
      join_waiter(&ws[0]);
      if (ws[0].err) bad++;
      lat[i] = ws[0].t1 > t0 ? (ws[0].t1 - t0) / 1000 : 0;
    }
    for (int i = 1; i < N; i++)
      for (int j = i; j > 0 && lat[j - 1] > lat[j]; j--) {
        uint64_t t = lat[j];
        lat[j] = lat[j - 1];
        lat[j - 1] = t;
      }
    verdict("W17", "EVENT_SET -> parked waiter returns (latency)", !bad && lat[N / 2] < 2000,
            "p50=%lluus p90=%lluus max=%lluus bad=%d", (unsigned long long)lat[N / 2],
            (unsigned long long)lat[N * 9 / 10], (unsigned long long)lat[N - 1], bad);
  }
  close(sm);
  close(man);
  close(aut);
  close(objs[0]);
}

/* ---- B: wake a parked WAIT_ALL ------------------------------------------- */
static void t_wake_all(void) {
  uint32_t c, o, prev, s, mn, s2;
  int objs[4];
  objs[0] = create_sem(dev, 0, 3);
  objs[1] = create_mutex(dev, 123, 1);
  objs[2] = create_event(dev, 1, 0);
  objs[3] = create_event(dev, 0, 0);
  struct waiter w;
  start_waiter(&w, NT_IOC_WAIT_ALL, 4, objs, 456, 0, 3000);
  int early = wait_done(&w, 100);
  release_sem(objs[0], 1, &prev);
  int d1 = wait_done(&w, 50);
  read_sem(objs[0], &c, &o);
  verdict("B1", "1 of 4 signaled: still waiting, sem not taken", !early && !d1 && c == 1, "early=%d done=%d sem=%u", early, d1, c);
  unlock_mutex(objs[1], 123, &prev);
  int d2 = wait_done(&w, 50);
  ev_op(objs[2], NT_IOC_EVENT_SET, &prev);
  int d3 = wait_done(&w, 50);
  read_mutex(objs[1], &c, &o);
  verdict("B2", "3 of 4: still waiting, mutex free", !d2 && !d3 && c == 0 && o == 0, "d2=%d d3=%d mutex=%u/%u", d2, d3, c, o);
  ev_op(objs[3], NT_IOC_EVENT_SET, &prev);
  int done = wait_done(&w, 1000);
  join_waiter(&w);
  uint32_t sc;
  read_sem(objs[0], &sc, &o);
  read_mutex(objs[1], &c, &o);
  read_event(objs[2], &s, &mn);
  read_event(objs[3], &s2, &mn);
  verdict("B3", "4th signal: all taken at once", done && !w.err && w.index == 0 && sc == 0 && c == 1 && o == 456 && s == 1 && s2 == 0,
          "err=%s idx=%u sem=%u mutex=%u/%u man=%u auto=%u", en(w.err), w.index, sc, c, o, s, s2);
  /* WAIT_ALL waiter beats a WAIT_ANY waiter when both are satisfiable */
  unlock_mutex(objs[1], 456, &prev);
  ev_op(objs[3], NT_IOC_EVENT_RESET, &prev);
  struct waiter wa, wy;
  int pair[2] = {objs[0], objs[3]};
  start_waiter(&wa, NT_IOC_WAIT_ALL, 2, pair, 1, 0, 2000);
  start_waiter(&wy, NT_IOC_WAIT_ANY, 1, &objs[0], 2, 0, 400);
  usleep(100000);
  ev_op(objs[3], NT_IOC_EVENT_SET, &prev); /* all: sem 0 still -> nothing */
  usleep(50000);
  release_sem(objs[0], 1, &prev);           /* both can take it: ALL first */
  int da = wait_done(&wa, 1000);
  join_waiter(&wa);
  join_waiter(&wy);
  verdict("B4", "a satisfiable WAIT_ALL is served before WAIT_ANY", da && !wa.err && wy.err == ETIMEDOUT,
          "all=%s any=%s", en(wa.err), en(wy.err));
  /* a WAIT_ANY steals the sem while the WAIT_ALL is unsatisfiable */
  ev_op(objs[3], NT_IOC_EVENT_RESET, &prev);
  start_waiter(&wa, NT_IOC_WAIT_ALL, 2, pair, 1, 0, 400);
  start_waiter(&wy, NT_IOC_WAIT_ANY, 1, &objs[0], 2, 0, 2000);
  usleep(100000);
  release_sem(objs[0], 1, &prev);
  int dy = wait_done(&wy, 1000);
  join_waiter(&wa);
  join_waiter(&wy);
  verdict("B5", "an unsatisfiable WAIT_ALL leaves the sem to WAIT_ANY", dy && !wy.err && wa.err == ETIMEDOUT,
          "all=%s any=%s", en(wa.err), en(wy.err));
  for (int i = 0; i < 4; i++) close(objs[i]);
}

/* ---- R: the alert event -------------------------------------------------- */
static void t_alert(void) {
  uint32_t idx, prev, s, mn;
  int sem = create_sem(dev, 0, 2), alert = create_event(dev, 0, 1);
  int e = wait_objs(dev, NT_IOC_WAIT_ANY, 1, &sem, 123, alert, &idx);
  read_event(alert, &s, &mn);
  verdict("R1", "ANY: signaled alert -> index count, consumed", !e && idx == 1 && s == 0, "%s idx=%u alert=%u", en(e), idx, s);
  ev_op(alert, NT_IOC_EVENT_SET, &prev);
  release_sem(sem, 1, &prev);
  e = wait_objs(dev, NT_IOC_WAIT_ANY, 1, &sem, 123, alert, &idx);
  read_event(alert, &s, &mn);
  verdict("R2", "ANY: object and alert -> the object wins", !e && idx == 0 && s == 1, "%s idx=%u alert=%u", en(e), idx, s);
  e = wait_objs(dev, NT_IOC_WAIT_ANY, 0, &sem, 123, alert, &idx);
  verdict("R3", "ANY count 0 + alert -> index 0 (== count)", !e && idx == 0, "%s idx=%u", en(e), idx);
  e = wait_objs(dev, NT_IOC_WAIT_ANY, 1, &sem, 123, alert, &idx);
  verdict("R4", "nothing signaled -> ETIMEDOUT", e == ETIMEDOUT, "%s", en(e));
  int objs[2] = {sem, create_mutex(dev, 0, 0)};
  ev_op(alert, NT_IOC_EVENT_SET, &prev);
  e = wait_objs(dev, NT_IOC_WAIT_ALL, 2, objs, 123, alert, &idx);
  verdict("R5", "ALL: sem empty + alert -> index count", !e && idx == 2, "%s idx=%u", en(e), idx);
  ev_op(alert, NT_IOC_EVENT_SET, &prev);
  release_sem(sem, 1, &prev);
  e = wait_objs(dev, NT_IOC_WAIT_ALL, 2, objs, 123, alert, &idx);
  read_event(alert, &s, &mn);
  verdict("R6", "ALL: all signaled + alert -> the set wins", !e && idx == 0 && s == 1, "%s idx=%u alert=%u", en(e), idx, s);
  ev_op(alert, NT_IOC_EVENT_RESET, &prev);
  struct waiter w;
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &sem, 123, alert, 3000);
  int early = wait_done(&w, 100);
  ev_op(alert, NT_IOC_EVENT_SET, &prev);
  int done = wait_done(&w, 1000);
  join_waiter(&w);
  verdict("R7", "a parked ANY is ended by the alert (index count)", !early && done && !w.err && w.index == 1,
          "early=%d err=%s idx=%u", early, en(w.err), w.index);
  start_waiter(&w, NT_IOC_WAIT_ALL, 2, objs, 456, alert, 3000);
  early = wait_done(&w, 100);
  ev_op(alert, NT_IOC_EVENT_SET, &prev);
  done = wait_done(&w, 1000);
  join_waiter(&w);
  verdict("R8", "a parked ALL is ended by the alert (index count)", !early && done && !w.err && w.index == 2,
          "early=%d err=%s idx=%u", early, en(w.err), w.index);
  int dupa[2] = {sem, alert};
  e = wait_objs(dev, NT_IOC_WAIT_ALL, 1, dupa, 123, sem, &idx);
  verdict("R9", "ALL: the alert repeating an object -> EINVAL", e == EINVAL, "%s", en(e));
  close(objs[1]);
  close(sem);
  close(alert);
}

/* ---- G: signals ----------------------------------------------------------- */
static volatile int g_hits;
static void g_handler(int sig) { (void)sig; g_hits++; }
struct sigw {
  int kind, fd, ev;
  volatile int done;
  int err;
  uint64_t t1;
};
static void *g_sigw_fn(void *a) {
  struct sigw *x = a;
  int r;
  if (x->kind == 0) {
    struct nts_wait_args wa;
    memset(&wa, 0, sizeof wa);
    wa.objs = (uintptr_t)&x->ev;
    wa.count = 1;
    wa.owner = 1;
    wa.timeout = ~0ull;
    r = ioctl(dev, NT_IOC_WAIT_ANY, &wa);
  } else {
    char c;
    r = (int)read(x->fd, &c, 1);
  }
  x->err = r < 0 ? errno : 0;
  x->t1 = now_ns(CLOCK_MONOTONIC);
  __atomic_store_n(&x->done, 1, __ATOMIC_SEQ_CST);
  return 0;
}
/* G5: a thread spinning in user mode; how long until its handler runs */
static volatile int g5_stop;
static volatile uint64_t g5_hit_ns;
static void g5_handler(int sig) {
  (void)sig;
  g5_hit_ns = now_ns(CLOCK_MONOTONIC);
}
static void *g5_spin(void *a) {
  (void)a;
  while (!g5_stop) __asm__ volatile("" ::: "memory");
  return 0;
}
static void t_signals(void) {
  int ev = create_event(dev, 1, 0);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = g_handler;
  sigaction(SIGUSR1, &sa, 0);
  struct waiter w;
  g_hits = 0;
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &ev, 1, 0, -1);
  usleep(100000);
  pthread_kill(w.th, SIGUSR1);
  int done = wait_done(&w, 1000);
  if (!done) {
    uint32_t prev;
    ev_op(ev, NT_IOC_EVENT_SET, &prev);
  }
  join_waiter(&w);
  verdict("G1", "a handled signal (no SA_RESTART) -> EINTR", done && w.err == EINTR && g_hits == 1, "done=%d err=%s hits=%d", done, en(w.err), g_hits);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGUSR1, &sa, 0);
  uint32_t prev;
  ev_op(ev, NT_IOC_EVENT_RESET, &prev);
  g_hits = 0;
  start_waiter(&w, NT_IOC_WAIT_ANY, 1, &ev, 1, 0, -1);
  usleep(100000);
  pthread_kill(w.th, SIGUSR1);
  done = wait_done(&w, 300);
  ev_op(ev, NT_IOC_EVENT_SET, &prev);
  join_waiter(&w);
  /* Linux restarts the wait (-ERESTARTSYS + SA_RESTART); HobbyOS answers
     EINTR (docs/cp-ntsync.md §2.3).  Both are correct for Wine, which loops. */
  verdict("G2", "SA_RESTART handler: restarted (Linux) or EINTR", g_hits == 1 && ((!done && !w.err) || (done && w.err == EINTR)),
          "hits=%d %s", g_hits, done ? "EINTR-before-set" : "restarted-then-woken");
  /* a signal while the object is already granted: success wins */
  ev_op(ev, NT_IOC_EVENT_RESET, &prev);
  sa.sa_flags = 0;
  sigaction(SIGUSR1, &sa, 0);
  int ok = 0;
  for (int i = 0; i < 20; i++) {
    start_waiter(&w, NT_IOC_WAIT_ANY, 1, &ev, 1, 0, 2000);
    usleep(50000);
    ev_op(ev, NT_IOC_EVENT_PULSE, &prev);
    pthread_kill(w.th, SIGUSR1);
    join_waiter(&w);
    ok += (w.err == 0 || w.err == EINTR);
  }
  verdict("G3", "pulse + signal races: only 0 or EINTR", ok == 20, "ok=%d/20", ok);
  /* G4/G5: how long a signal takes to end a parked wait, against a
     blocking pipe read (the control: the kernel's ordinary rewound park).
     Wine 11 + ntsync interrupts a thread's inproc wait with SIGUSR1 for every
     system APC (server/thread.c queue_apc), so this is on Wine's hot path. */
  sa.sa_flags = 0;
  sigaction(SIGUSR1, &sa, 0);
  {
    enum { N = 40 };
    uint64_t lat[2][N];
    int bad = 0;
    for (int kind = 0; kind < 2; kind++) {
      int pp[2] = {-1, -1};
      if (kind && pipe(pp)) bad++;
      for (int i = 0; i < N; i++) {
        struct sigw sw = {kind, pp[0], ev, 0, 0, 0};
        pthread_t th;
        pthread_create(&th, 0, g_sigw_fn, &sw);
        usleep(20000);
        uint64_t t0 = now_ns(CLOCK_MONOTONIC);
        pthread_kill(th, SIGUSR1);
        for (int k = 0; k < 3000 && !__atomic_load_n(&sw.done, __ATOMIC_SEQ_CST); k++) usleep(1000);
        if (!sw.done) {
          bad++;
          if (kind == 0) ev_op(ev, NT_IOC_EVENT_SET, &prev);
          else if (write(pp[1], "x", 1) != 1) bad++;
        }
        pthread_join(th, 0);
        if (kind == 0) ev_op(ev, NT_IOC_EVENT_RESET, &prev);
        if (sw.err != EINTR) bad++;
        lat[kind][i] = sw.t1 > t0 ? (sw.t1 - t0) / 1000 : 0;
      }
      if (kind) {
        close(pp[0]);
        close(pp[1]);
      }
    }
    for (int kind = 0; kind < 2; kind++)
      for (int i = 1; i < N; i++)
        for (int j = i; j > 0 && lat[kind][j - 1] > lat[kind][j]; j--) {
          uint64_t t = lat[kind][j];
          lat[kind][j] = lat[kind][j - 1];
          lat[kind][j - 1] = t;
        }
    verdict("G4", "signal -> EINTR latency, ntsync wait", !bad && lat[0][N / 2] < 2000 && lat[0][N - 1] < 100000,
            "p50=%lluus p90=%lluus max=%lluus (pipe read: %llu/%llu/%lluus) bad=%d",
            (unsigned long long)lat[0][N / 2], (unsigned long long)lat[0][N * 9 / 10], (unsigned long long)lat[0][N - 1],
            (unsigned long long)lat[1][N / 2], (unsigned long long)lat[1][N * 9 / 10], (unsigned long long)lat[1][N - 1], bad);
  }
  {
    /* G5 (informational): the cost of Wine 11 + ntsync's SIGUSR1 for a
       system APC when the target thread is RUNNING in user mode, not
       parked: Linux kicks its CPU at once (kick_process). */
    struct sigaction s5;
    memset(&s5, 0, sizeof s5);
    s5.sa_handler = g5_handler;
    sigaction(SIGUSR2, &s5, 0);
    enum { N = 40 };
    uint64_t lat[N];
    g5_stop = 0;
    pthread_t th;
    pthread_create(&th, 0, g5_spin, 0);
    usleep(20000);
    for (int i = 0; i < N; i++) {
      g5_hit_ns = 0;
      uint64_t t0 = now_ns(CLOCK_MONOTONIC);
      pthread_kill(th, SIGUSR2);
      for (int k = 0; k < 2000 && !g5_hit_ns; k++) usleep(100);
      lat[i] = g5_hit_ns > t0 ? (g5_hit_ns - t0) / 1000 : 999999;
      usleep(3000);
    }
    g5_stop = 1;
    pthread_join(th, 0);
    signal(SIGUSR2, SIG_DFL);
    for (int i = 1; i < N; i++)
      for (int j = i; j > 0 && lat[j - 1] > lat[j]; j--) {
        uint64_t t = lat[j];
        lat[j] = lat[j - 1];
        lat[j - 1] = t;
      }
    row("G5 info: signal -> handler of a thread running in user mode: p50=%lluus p90=%lluus max=%lluus",
        (unsigned long long)lat[N / 2], (unsigned long long)lat[N * 9 / 10], (unsigned long long)lat[N - 1]);
  }
  signal(SIGUSR1, SIG_DFL);
  close(ev);
}

/* ---- F: Wine's shape ------------------------------------------------------ */
static int send_fds(int sock, const int *fds, int n) {
  char c = 'x';
  struct iovec iov = {&c, 1};
  char cbuf[CMSG_SPACE(sizeof(int) * 4)];
  memset(cbuf, 0, sizeof cbuf);
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  mh.msg_control = cbuf;
  mh.msg_controllen = CMSG_SPACE(sizeof(int) * n);
  struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int) * n);
  memcpy(CMSG_DATA(cm), fds, sizeof(int) * n);
  return sendmsg(sock, &mh, 0) == 1 ? 0 : -1;
}
static int recv_fds(int sock, int *fds, int n) {
  char c;
  struct iovec iov = {&c, 1};
  char cbuf[CMSG_SPACE(sizeof(int) * 4)];
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  mh.msg_control = cbuf;
  mh.msg_controllen = sizeof cbuf;
  if (recvmsg(sock, &mh, 0) != 1) return -1;
  struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
  if (!cm || cm->cmsg_type != SCM_RIGHTS) return -1;
  memcpy(fds, CMSG_DATA(cm), sizeof(int) * n);
  return 0;
}
static void t_scm(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
    verdict("F1", "socketpair", 0, "errno=%d", errno);
    return;
  }
  /* the "server": a fresh device, a sem and an auto event; the "client" (a
     forked child that closed its inherited copies) gets them over SCM_RIGHTS,
     waits on the sem, then sets the event */
  int d = open("/dev/ntsync", O_RDONLY | O_CLOEXEC);
  int sem = create_sem(d, 0, 1), ev = create_event(d, 0, 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(d);
    close(sem);
    close(ev);
    int f[3];
    if (recv_fds(sv[1], f, 3)) _exit(10);
    struct nts_wait_args a;
    memset(&a, 0, sizeof a);
    a.objs = (uintptr_t)&f[1];
    a.count = 1;
    a.owner = 77;
    a.timeout = now_ns(CLOCK_MONOTONIC) + 3000000000ull;
    if (ioctl(f[0], NT_IOC_WAIT_ANY, &a) != 0) _exit(11);
    uint32_t prev;
    if (ioctl(f[2], NT_IOC_EVENT_SET, &prev) != 0) _exit(12);
    _exit(0);
  }
  int f[3] = {d, sem, ev};
  int se = send_fds(sv[0], f, 3);
  usleep(100000);
  uint32_t prev;
  release_sem(sem, 1, &prev);
  uint32_t idx;
  struct nts_wait_args a;
  memset(&a, 0, sizeof a);
  a.objs = (uintptr_t)&ev;
  a.count = 1;
  a.owner = 1;
  a.timeout = now_ns(CLOCK_MONOTONIC) + 3000000000ull;
  int e = E(ioctl(d, NT_IOC_WAIT_ANY, &a));
  idx = a.index;
  int st = -1;
  waitpid(pid, &st, 0);
  verdict("F1", "fds over SCM_RIGHTS: child waits, parent signals", !se && !e && idx == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "send=%d wait=%s child=%d", se, en(e), WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  /* a forked child inherits the fds and shares the objects */
  int m = create_mutex(d, 0, 0);
  pid = fork();
  if (pid == 0) {
    uint32_t i2;
    struct nts_wait_args b;
    memset(&b, 0, sizeof b);
    b.objs = (uintptr_t)&m;
    b.count = 1;
    b.owner = 99;
    b.timeout = now_ns(CLOCK_MONOTONIC);
    int r = ioctl(d, NT_IOC_WAIT_ANY, &b);
    (void)i2;
    _exit(r == 0 ? 0 : 20);
  }
  waitpid(pid, &st, 0);
  uint32_t c, o;
  read_mutex(m, &c, &o);
  verdict("F2", "a forked child acquires the shared mutex", WIFEXITED(st) && WEXITSTATUS(st) == 0 && c == 1 && o == 99,
          "child=%d mutex=%u/%u", WIFEXITED(st) ? WEXITSTATUS(st) : -1, c, o);
  /* wineserver's job at a client's death: MUTEX_KILL its tid */
  int ek = kill_mutex(m, 99);
  e = wait_objs(d, NT_IOC_WAIT_ANY, 1, &m, 1, 0, &idx);
  verdict("F3", "server abandons the dead client's mutex", !ek && e == EOWNERDEAD, "kill=%s wait=%s", en(ek), en(e));
  /* a thread killed while parked must never be granted anything */
  {
    int sm = create_sem(d, 0, 1);
    pid = fork();
    if (pid == 0) {
      struct nts_wait_args b;
      memset(&b, 0, sizeof b);
      b.objs = (uintptr_t)&sm;
      b.count = 1;
      b.owner = 5;
      b.timeout = ~0ull;
      ioctl(d, NT_IOC_WAIT_ANY, &b);
      _exit(30);
    }
    usleep(150000);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    release_sem(sm, 1, &prev);
    uint32_t sc, sx;
    read_sem(sm, &sc, &sx);
    e = wait_objs(d, NT_IOC_WAIT_ANY, 1, &sm, 1, 0, &idx);
    verdict("F4", "SIGKILLed waiter: the release is not lost", WIFSIGNALED(st) && sc == 1 && !e, "sem=%u wait=%s", sc, en(e));
    close(sm);
  }
  close(m);
  close(sem);
  close(ev);
  close(d);
  close(sv[0]);
  close(sv[1]);
}

/* ---- X: stress ------------------------------------------------------------ */
static int x_lock;
static volatile long x_counter;
static int x_iters;
static unsigned long x_req;
static void *x_mutex_fn(void *arg) {
  uint32_t owner = (uint32_t)(uintptr_t)arg;
  for (int i = 0; i < x_iters; i++) {
    struct nts_wait_args a;
    memset(&a, 0, sizeof a);
    a.objs = (uintptr_t)&x_lock;
    a.count = 1;
    a.owner = owner;
    a.timeout = ~0ull;
    int r;
    do r = ioctl(dev, x_req, &a);
    while (r < 0 && errno == EINTR);
    if (r) {
      __atomic_add_fetch(&fails, 1, __ATOMIC_RELAXED);
      return 0;
    }
    long v = x_counter;
    x_counter = v + 1;
    struct nts_mutex_args m = {owner, 0};
    if (ioctl(x_lock, NT_IOC_MUTEX_UNLOCK, &m)) {
      __atomic_add_fetch(&fails, 1, __ATOMIC_RELAXED);
      return 0;
    }
  }
  return 0;
}
static void t_stress(void) {
  const int nth = 8;
  pthread_t th[8];
  x_iters = 20000;
  for (int pass = 0; pass < 2; pass++) {
    x_req = pass ? NT_IOC_WAIT_ALL : NT_IOC_WAIT_ANY;
    x_lock = create_mutex(dev, 0, 0);
    x_counter = 0;
    uint64_t t0 = now_ns(CLOCK_MONOTONIC);
    for (int i = 0; i < nth; i++) pthread_create(&th[i], 0, x_mutex_fn, (void *)(uintptr_t)(100 + i));
    for (int i = 0; i < nth; i++) pthread_join(th[i], 0);
    uint64_t ms = (now_ns(CLOCK_MONOTONIC) - t0) / 1000000;
    long want = (long)nth * x_iters;
    verdict(pass ? "X2" : "X1", pass ? "stress_wait_all: 8 threads x 20000, one mutex" : "stress_wait_any: 8 threads x 20000, one mutex",
            x_counter == want, "counter=%ld/%ld %llums (%.2f us/op)", x_counter, want, (unsigned long long)ms,
            (double)ms * 1000.0 / (double)want);
    close(x_lock);
  }
}
static int pp_a, pp_b;
static void *pp_fn(void *arg) {
  int n = (int)(intptr_t)arg;
  for (int i = 0; i < n; i++) {
    struct nts_wait_args a;
    memset(&a, 0, sizeof a);
    a.objs = (uintptr_t)&pp_a;
    a.count = 1;
    a.owner = 2;
    a.timeout = ~0ull;
    if (ioctl(dev, NT_IOC_WAIT_ANY, &a)) return 0;
    uint32_t prev;
    ioctl(pp_b, NT_IOC_EVENT_SET, &prev);
  }
  return 0;
}
static void t_pingpong(void) {
  pp_a = create_event(dev, 0, 0);
  pp_b = create_event(dev, 0, 0);
  const int n = 5000;
  pthread_t th;
  pthread_create(&th, 0, pp_fn, (void *)(intptr_t)n);
  int ok = 1;
  uint64_t t0 = now_ns(CLOCK_MONOTONIC);
  for (int i = 0; i < n && ok; i++) {
    uint32_t prev;
    ioctl(pp_a, NT_IOC_EVENT_SET, &prev);
    struct nts_wait_args a;
    memset(&a, 0, sizeof a);
    a.objs = (uintptr_t)&pp_b;
    a.count = 1;
    a.owner = 1;
    a.timeout = now_ns(CLOCK_MONOTONIC) + 2000000000ull;
    if (ioctl(dev, NT_IOC_WAIT_ANY, &a)) ok = 0;
  }
  uint64_t ns = now_ns(CLOCK_MONOTONIC) - t0;
  pthread_join(th, 0);
  verdict("X3", "event ping-pong round trip (2 threads)", ok, "%d trips, %.2f us/trip", n, (double)ns / 1000.0 / n);
  close(pp_a);
  close(pp_b);
}

/* X4: what the wineserver pays for every internal sync (each fd object,
   APC, async...): CREATE_EVENT + close, with a small and a large fd table */
static void t_create_close(void) {
  int keep[1500];
  int nk = 0;
  {
    /* as the wineserver does (server/main.c): soft RLIMIT_NOFILE = hard */
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) {
      rl.rlim_cur = rl.rlim_max;
      setrlimit(RLIMIT_NOFILE, &rl);
    }
  }
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1)
      for (nk = 0; nk < 1500; nk++)
        if ((keep[nk] = create_event(dev, 1, 0)) < 0) break;
    const int n = 5000;
    uint64_t t0 = now_ns(CLOCK_MONOTONIC);
    int bad = 0;
    for (int i = 0; i < n; i++) {
      int e = create_event(dev, 1, 0);
      if (e < 0) bad++;
      else close(e);
    }
    uint64_t ns = now_ns(CLOCK_MONOTONIC) - t0;
    verdict(pass ? "X5" : "X4", pass ? "CREATE_EVENT + close, 1500 objects open" : "CREATE_EVENT + close",
            !bad && (!pass || nk == 1500), "%.2f us per pair (kept %d, failed %d)", (double)ns / 1000.0 / n, nk, bad);
  }
  for (int i = 0; i < nk; i++) close(keep[i]);
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  row("linux_probe_ntsync: /dev/ntsync (UAPI %s)",
#ifdef HAVE_REAL_HEADER
      "checked against <linux/ntsync.h>"
#else
      "restated; no <linux/ntsync.h> at build"
#endif
  );
  dev = open("/dev/ntsync", O_RDONLY | O_CLOEXEC);
  int fdf = dev >= 0 ? fcntl(dev, F_GETFD) : -1;
  verdict("N0", "open(/dev/ntsync, O_RDONLY|O_CLOEXEC)", dev >= 0 && (fdf & FD_CLOEXEC), "fd=%d %s", dev, dev < 0 ? en(errno) : "");
  if (dev < 0) {
    row("done rc=%d", fails + 100);
    return 1;
  }
  {
    int s = create_sem(dev, 0, 1);
    int ff = s >= 0 ? fcntl(s, F_GETFD) : -1;
    int fl = s >= 0 ? fcntl(s, F_GETFL) : -1;
    verdict("N1", "object fds are O_RDWR | O_CLOEXEC", s >= 0 && (ff & FD_CLOEXEC) && (fl & O_ACCMODE) == O_RDWR, "fd=%d fd_flags=%d fl=0x%x", s, ff, fl);
    close(s);
  }
  t_sem();
  t_mutex();
  t_events();
  t_wait_any();
  t_wait_all();
  t_invalid();
  t_wake_any();
  t_wake_all();
  t_alert();
  t_signals();
  t_scm();
  t_stress();
  t_pingpong();
  t_create_close();
  close(dev);
  row("done rc=%d", fails);
  return fails ? 1 : 0;
}
