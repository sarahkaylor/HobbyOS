/*
 * rwmain.c — AD-9: the RimWorld executable, rebuilt as a HobbyOS compat PIE.
 *
 * The shipped RimWorldLinux is a 14 KB ET_EXEC stub linked at 0x200000 whose
 * main is literally `jmp PlayerMain@plt`; HobbyOS's compat loader refuses an
 * ET_EXEC below its 64 GiB user window (docs/rimworld-launch-log.md §4), and
 * the v2 page tables cannot represent 0x200000.  This program is the same
 * program at a legal address: it dlopen()s UnityPlayer.so, dlsyms the mangled
 * PlayerMain symbol the stub imports, and calls it with the arguments it was
 * given.  Nothing about the game's own code changes.
 *
 * It must be installed as `<game dir>/RimWorldLinux` because Unity derives its
 * data folder from /proc/self/exe and opens `<dir>/RimWorldLinux_Data`; the
 * real stub is kept next to it as RimWorldLinux.orig in the NFS staging copy.
 *
 * AD-10 (DSOs staged locally, data on NFS): UnityPlayer.so is loaded through
 * this launcher (RW_UNITYPLAYER, else next to the executable), and the Mono /
 * plugin DSOs Unity loads later are redirected from their NFS paths to the
 * local staging directory by the dlopen() wrapper below.  mmap of an NFS file
 * is -ENOTSUP in HobbyOS (M5b), which is the whole reason for the staging.
 * The redirect is by basename inside RW_DSO_DIR, so it does not depend on how
 * Unity spells the data path.
 *
 * Diagnostics go to stderr, not stdout: when the desktop launches this
 * program as a menu app, fd 1 is the desktop's ESC-protocol window pipe
 * (src/user/xcompat/libx11.c reads the same fd), and a [RWMAIN] text row
 * would be parsed as window protocol.  The launcher (rwlaunch.c) maps the
 * child's fd 2 to /dev/console, which is where these rows land.
 *
 * The wrapper is a normal dynamic symbol of this executable (linked
 * -rdynamic).  glibc resolves UnityPlayer.so's dlopen@GLIBC_2.2.5 through the
 * global scope, and the main executable is first in it; the real dlopen is
 * reached with dlsym(RTLD_NEXT, "dlopen").
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#ifndef RW_UNITYPLAYER_DEFAULT
#define RW_UNITYPLAYER_DEFAULT "/RWDSO/UnityPlayer.so"
#endif
#define RW_PLAYERMAIN "_Z10PlayerMainiPPc"

static void *(*next_dlopen)(const char *, int);
static void *(*next_dlsym)(void *, const char *);
static const char *dso_dir;

/* ---- dlsym observation ---------------------------------------------------
 * SDL loads libX11.so.6 with dlopen() and resolves the entry points it uses
 * through dlsym(); a NULL return disables the whole X11 video driver and SDL
 * reports only "No available video device" (Unity's log says "Video
 * subsystem has not been initialized").  Recording a failed dlsym is the way
 * to name the missing entry point.  The real dlsym is reached through
 * dlvsym(), which this program does not export, so there is no recursion.
 */
void *dlsym(void *handle, const char *name) {
  if (!next_dlsym) {
    void *(*real_dlvsym)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    next_dlsym = real_dlvsym
                     ? (void *(*)(void *, const char *))real_dlvsym(
                           RTLD_NEXT, "dlsym", "GLIBC_2.2.5")
                     : 0;
    if (!next_dlsym) {
      fprintf(stderr, "[RWMAIN] FATAL: cannot resolve the real dlsym\n");
      return 0;
    }
  }
  void *r = next_dlsym(handle, name);
  if (!r) {
    const char *e = dlerror();
    fprintf(stderr, "[RWMAIN] dlsym FAILED %s: %s\n",
            name ? name : "(null)", e ? e : "(no dlerror)");
  } else if (getenv("RW_TRACE_DLOPEN") && name && name[0] == 'X') {
    fprintf(stderr, "[RWMAIN] dlsym ok %s -> %p\n", name, r);
  }
  return r;
}

static const char *base_of(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* Map a DSO name Unity asks for onto the local staging copy, if one exists.
 * Only the basename is used: Unity asks for
 *   <data>/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so
 *   <data>/Plugins/lib_burst_generated.so
 * and the local copy is /RWDSO/<basename> regardless of where the data tree
 * lives.  Mono's native loader first probes the name without ".so" (measured
 * on the host: .../libmono-native then .../libmono-native.so), so that form is
 * mapped too. */
static int map_local(const char *name, char *out, size_t cap) {
  if (!dso_dir || !dso_dir[0] || !name || !name[0]) return 0;
  const char *base = base_of(name);
  if (!base[0]) return 0;
  if (strncmp(name, dso_dir, strlen(dso_dir)) == 0) return 0; /* already local */
  if (snprintf(out, cap, "%s/%s", dso_dir, base) >= (int)cap) return 0;
  if (access(out, R_OK) == 0) return 1;
  if (!strchr(base, '.')) {
    if (snprintf(out, cap, "%s/%s.so", dso_dir, base) < (int)cap &&
        access(out, R_OK) == 0)
      return 1;
  }
  return 0;
}

void *dlopen(const char *file, int mode) {
  char local[512];
  if (!next_dlopen) {
    next_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
    if (!next_dlopen) {
      fprintf(stderr, "[RWMAIN] FATAL: cannot resolve the real dlopen via RTLD_NEXT\n");
      return 0;
    }
  }
  if (file && map_local(file, local, sizeof local)) {
    fprintf(stderr, "[RWMAIN] dlopen remap %s -> %s\n", file, local);
    void *r = next_dlopen(local, mode);
    if (!r) {
      /* Unity only says "Unable to load mono library"; the loader's reason
         (a missing NEEDED, an undefined symbol, a version) is the evidence. */
      const char *e = dlerror();
      fprintf(stderr, "[RWMAIN] dlopen FAILED %s: %s\n", local, e ? e : "(no dlerror)");
    }
    return r;
  }
  /* RW_TRACE_DLOPEN: every attempt and its result.  Unity/SDL disable a whole
     video backend silently when a dlopen() of libX11/libGL fails, and the
     engine's own log says only "Video subsystem has not been initialized";
     the failing soname and the loader's reason are the ladder.  Off by
     default; the M6 desktop harness turns it on. */
  int trace = getenv("RW_TRACE_DLOPEN") != 0;
  void *r = next_dlopen(file, mode);
  if (!r) {
    const char *e = dlerror();
    fprintf(stderr, "[RWMAIN] dlopen FAILED %s: %s\n",
            file ? file : "(null)", e ? e : "(no dlerror)");
  } else if (trace) {
    /* The base address turns a runtime PC into a file offset: the M6 park's
       rip/stack words are otherwise unattributable (the guest has no
       /proc/self/maps).  RTLD_DI_LINKMAP gives l_addr, the load bias. */
    struct link_map *lm = 0;
    unsigned long base = 0;
    if (dlinfo(r, RTLD_DI_LINKMAP, &lm) == 0 && lm) base = (unsigned long)lm->l_addr;
    fprintf(stderr, "[RWMAIN] dlopen ok %s -> %p base=0x%lx\n",
            file ? file : "(null)", r, base);
  }
  return r;
}


/* ---- pthread symbol shadowing (A/B knob) --------------------------------
 * This launcher is linked -rdynamic so that dlopen()/dlsym() are exported
 * (AD-10's redirect needs them: the executable is first in the global scope
 * and that is what makes the interposition reach UnityPlayer.so).  The same
 * rule applies to every other symbol this file defines -- including the
 * pthread_mutex_* / sem_* / pthread_cond_* wrappers below.  So the question
 * "who holds the stuck mutex" has a cheaper discriminator than a watchpoint:
 * does the park even survive when the shadowing is removed?
 *
 *   -DRW_NO_SHADOW_PTHREAD omits every pthread/sem definition (and its
 *   recorder) from the link, while dlopen/dlsym stay exported.  The game then
 *   binds glibc's own entry points from libpthread and the only difference in
 *   the boot is the interposition itself.  Build with:
 *       RW_NO_SHADOW=1 tools/rw/run-rimworld-desktop.sh
 * which passes the define; the default build is byte-for-byte the shipping
 * launcher.
 */
#ifndef RW_NO_SHADOW_PTHREAD

/* ---- mutex flight recorder ----------------------------------------------
 * Printing every sync call floods the console (the allocator loops over three
 * mutexes hundreds of times) and printing none loses the one lock that
 * matters.  So every call is recorded into a fixed ring and a watchdog thread
 * dumps it after 20 s -- by then the guest has been parked for ~15 s, and the
 * ring still holds the lock/unlock history that names the holder.
 * Off unless RW_TRACE_MUTEX is set; the shipping path records nothing.
 */
static int mtx_on(void);
/* fcntl.h drags in linux/falloc.h, which the pinned glibc-dev tree does not
   ship; the three flags and the prototype are all this file needs. */
#ifndef O_WRONLY
#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000
#endif
extern int open(const char *path, int flags, ...);

static int mtx_fd = -1; /* /RW_HOME/mtx.log: survives console interleaving */

#define MTX_RING 8192
struct mtx_ev { unsigned long m, from; int tid; int op; };
static struct mtx_ev mtx_ring[MTX_RING];
static unsigned mtx_at;
static int mtx_dumped;

/* L25: return-value instrumentation.  Separate counters from mtx_rows so the
   six-row "first few" budget below can never hide a failure.  Declared up
   here because the alarm dump prints their totals. */
#define MTX_FAIL_MAX 160
static unsigned mtx_lock_n, mtx_lock_bad;
static unsigned mtx_try_n, mtx_try_bad;
static unsigned mtx_timed_n, mtx_timed_bad;
static unsigned mtx_unlock_n, mtx_unlock_bad;
static unsigned mtx_fail_rows;
static unsigned mtx_init_rows;
static unsigned mtx_init_total;
static unsigned mtx_kind_init[64]; /* __kind histogram at pthread_mutex_init */
static unsigned mtx_kind_fail[64]; /* __kind of a mutex whose call failed  */

static char *mtx_put(char *p, char *end, const char *s);
static char *mtx_putx(char *p, char *end, unsigned long v);
static char *mtx_puti(char *p, char *end, long v);

static void mtx_rec(int op, const void *m, void *from) {
  if (!mtx_on()) return;
  unsigned i = __atomic_fetch_add(&mtx_at, 1u, __ATOMIC_RELAXED) % MTX_RING;
  mtx_ring[i].op = op;
  mtx_ring[i].m = (unsigned long)m;
  mtx_ring[i].from = (unsigned long)from;
  mtx_ring[i].tid = (int)gettid();
  /* Live rows for everything except the two hot loops; the ring keeps the
     full history in case a line is missed. */
  {
    unsigned long a = (unsigned long)m;
    if ((a >= 0x1090000000UL && a < 0x1091000000UL) ||
        (a >= 0x108218cc00UL && a < 0x108218cd00UL))
      return;
    /* L24: the log keeps the arena's history plus every init/destroy (rare,
       and the only evidence of a lock pool).  Everything else (61k condvar
       ops) would bury it. */
    int hot = (a >= 0x10C0000000UL && a < 0x10C0400000UL);
    static unsigned printed;
    printed++;
    char buf[160]; char *p = buf, *end = buf + sizeof buf - 8;
    p = mtx_put(p, end, "[MTR] ");
    p = mtx_put(p, end, (op == 0) ? "lock" : (op == 1) ? "unlock" :
                          (op == 2) ? "trylock" : (op == 3) ? "timedlock" :
                          (op == 4) ? "sem_wait" : (op == 5) ? "sem_post" :
                          (op == 7) ? "init" : (op == 8) ? "destroy" :
                                      "cond_wait");
    p = mtx_put(p, end, " m=");
    p = mtx_putx(p, end, a);
    p = mtx_put(p, end, " pid=");
    p = mtx_puti(p, end, (long)mtx_ring[i].tid);
    p = mtx_put(p, end, " from=");
    p = mtx_putx(p, end, (unsigned long)from);
    p = mtx_put(p, end, "\n");
    if (hot && printed <= 6) { ssize_t w = write(2, buf, (size_t)(p - buf)); (void)w; }
    if (mtx_fd >= 0 && (hot || op >= 7)) {
      ssize_t w = write(mtx_fd, buf, (size_t)(p - buf)); (void)w;
    }
  }
}

static const char *mtx_opname(int op) {
  switch (op) {
  case 0: return "lock";
  case 1: return "unlock";
  case 2: return "trylock";
  case 3: return "timedlock";
  case 4: return "sem_wait";
  case 5: return "sem_post";
  case 6: return "cond_wait";
  case 7: return "init";
  case 8: return "destroy";
  default: return "?";
  }
}

static char *mtx_put(char *p, char *end, const char *s) {
  while (*s && p < end) *p++ = *s++;
  return p;
}
static char *mtx_putx(char *p, char *end, unsigned long v) {
  p = mtx_put(p, end, "0x");
  int st = 0;
  for (int sh = 60; sh >= 0; sh -= 4) {
    unsigned d = (unsigned)((v >> sh) & 0xF);
    if (!d && !st && sh) continue;
    st = 1;
    if (p < end) *p++ = "0123456789abcdef"[d];
  }
  return p;
}
static char *mtx_puti(char *p, char *end, long v) {
  char t[24]; int i = 0, neg = v < 0; unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
  if (!u) t[i++] = '0';
  while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
  if (neg && p < end) *p++ = '-';
  while (i && p < end) *p++ = t[--i];
  return p;
}

/* L25: the totals, independent of every per-row cap.  Printed periodically
   from pthread_mutex_init (alarm(2) is ENOSYS on this kernel, so a SIGALRM
   dump never arrives) and once more by the dump if it ever does.
   "lock=N bad=M" is the PI hypothesis in one row. */
static void mtx_summary(const char *why) {
  if (!mtx_on()) return;
  char sb[768]; char *q = sb, *qe = sb + sizeof sb - 8;
  q = mtx_put(q, qe, "[MTXR] summary ");
  q = mtx_put(q, qe, why ? why : "");
  q = mtx_put(q, qe, " lock=");
  q = mtx_puti(q, qe, (long)mtx_lock_n);
  q = mtx_put(q, qe, " bad=");
  q = mtx_puti(q, qe, (long)mtx_lock_bad);
  q = mtx_put(q, qe, " try=");
  q = mtx_puti(q, qe, (long)mtx_try_n);
  q = mtx_put(q, qe, " bad=");
  q = mtx_puti(q, qe, (long)mtx_try_bad);
  q = mtx_put(q, qe, " timed=");
  q = mtx_puti(q, qe, (long)mtx_timed_n);
  q = mtx_put(q, qe, " bad=");
  q = mtx_puti(q, qe, (long)mtx_timed_bad);
  q = mtx_put(q, qe, " unlock=");
  q = mtx_puti(q, qe, (long)mtx_unlock_n);
  q = mtx_put(q, qe, " bad=");
  q = mtx_puti(q, qe, (long)mtx_unlock_bad);
  q = mtx_put(q, qe, "\n[MTXR] init-kinds");
  for (int k = 0; k < 64; k++)
    if (mtx_kind_init[k]) {
      q = mtx_put(q, qe, " ");
      q = mtx_puti(q, qe, k);
      q = mtx_put(q, qe, ":");
      q = mtx_puti(q, qe, (long)mtx_kind_init[k]);
    }
  q = mtx_put(q, qe, "\n[MTXR] fail-kinds");
  for (int k = 0; k < 64; k++)
    if (mtx_kind_fail[k]) {
      q = mtx_put(q, qe, " ");
      q = mtx_puti(q, qe, k);
      q = mtx_put(q, qe, ":");
      q = mtx_puti(q, qe, (long)mtx_kind_fail[k]);
    }
  q = mtx_put(q, qe, "\n");
  ssize_t w = write(2, sb, (size_t)(q - sb)); (void)w;
}

/* SIGALRM (armed for 20 s) dumps the ring.  A helper thread would need
   -lpthread in the launcher link and would add a 26th guest thread; a signal
   costs neither and is delivered to this same (parked) main thread -- the
   dump runs before the interrupted futex is re-entered. */
static void mtx_alarm_handler(int sig) {
  (void)sig;
  if (mtx_dumped) return;
  mtx_dumped = 1;
  unsigned total = __atomic_load_n(&mtx_at, __ATOMIC_RELAXED);
  unsigned n = total < MTX_RING ? total : MTX_RING;
  char buf[4096];
  char *p = buf, *end = buf + sizeof buf - 200;
  p = mtx_put(p, end, "[MTR] dump ");
  p = mtx_puti(p, end, (long)n);
  p = mtx_put(p, end, " of ");
  p = mtx_puti(p, end, (long)total);
  p = mtx_put(p, end, "\n");
  for (unsigned i = 0; i < n; i++) {
    struct mtx_ev *e = &mtx_ring[(total - n + i) % MTX_RING];
    if ((e->m >= 0x1090000000UL && e->m < 0x1091000000UL) ||
        (e->m >= 0x108218cc00UL && e->m < 0x108218cd00UL))
      continue;
    /* L24: the dump is about the parked arena and the lock lifetime.  Rows
       for other addresses' condvars would evict the answer from the ring and
       the serial budget alike. */
    if (!(e->m >= 0x10C0000000UL && e->m < 0x10C0400000UL) && e->op < 7)
      continue;
    p = mtx_put(p, end, "[MTR] ");
    p = mtx_put(p, end, mtx_opname(e->op));
    p = mtx_put(p, end, " m=");
    p = mtx_putx(p, end, e->m);
    p = mtx_put(p, end, " pid=");
    p = mtx_puti(p, end, e->tid);
    p = mtx_put(p, end, " from=");
    p = mtx_putx(p, end, e->from);
    p = mtx_put(p, end, "\n");
    if (p > buf + sizeof buf - 256) {
      ssize_t w = write(2, buf, (size_t)(p - buf)); (void)w;
      p = buf;
    }
  }
  if (p > buf) { ssize_t w = write(2, buf, (size_t)(p - buf)); (void)w; }
  mtx_summary("alarm");
}

/* ---- mutex observation (RW_TRACE_MUTEX) ---------------------------------
 * The park that ends M6 is a futex WAIT with expected value 2 on a word in an
 * anonymous 4 MiB mapping -- i.e. a userspace lock, and glibc keeps no owner
 * tid for a normal mutex.  The kernel therefore cannot say WHO took it, only
 * that nobody ever released it (no WAKE on that key exists).  This tracer
 * names the other side: it logs contended lock attempts and every lock/unlock
 * of an object inside the arena the parked word lives in, with the caller's
 * return address and the tid.  Off unless RW_TRACE_MUTEX is set, so the
 * shipping run's code path is unchanged.
 *
 * L25: the RETURN VALUE of every mutex entry point is logged (uncapped, up to
 * MTX_FAIL_MAX rows) together with the mutex's __kind and its __lock word
 * before and after.  A PRIO_INHERIT mutex (__kind 34 = ERRORCHECK|PI) whose
 * contended lock hits a kernel without FUTEX_LOCK_PI is the hypothesis this
 * wave tests: if glibc returns an error that Unity ignores, the row says so;
 * if glibc instead aborts, the serial says that instead.  A summary line at
 * the alarm makes the totals independent of the row cap.
 *
 * The real functions are reached through dlvsym(RTLD_NEXT,...) -- the same
 * non-interposed route the dlsym wrapper below uses, so there is no recursion.
 */
static int (*next_mutex_lock)(pthread_mutex_t *);
static int (*next_mutex_trylock)(pthread_mutex_t *);
static int (*next_mutex_unlock)(pthread_mutex_t *);
static int (*next_mutex_init)(pthread_mutex_t *, const pthread_mutexattr_t *);
static unsigned mtx_rows;
static int mtx_main_tid; /* gettid() of the thread that calls PlayerMain */

/* glibc 2.31 x86-64 pthread_mutex_t: __lock(0) __count(4) __owner(8)
   __nusers(12) __kind(16) __spins(20) ... -- 0x30 bytes total.  __kind 34 is
   ERRORCHECK(2) | PRIO_INHERIT(32), the kind UnityPlayer+0xBA2E16 creates. */
static int mtx_kind(const pthread_mutex_t *m) {
  return m ? ((const volatile int *)m)[4] & 0x3f : -1;
}

static void mtx_fail_row(const char *what, const pthread_mutex_t *m,
                         long pre, long rv, void *from) {
  int k = mtx_kind(m);
  if (k >= 0 && k < 64) mtx_kind_fail[k]++;
  if (mtx_fail_rows >= MTX_FAIL_MAX) return;
  mtx_fail_rows++;
  fprintf(stderr, "[MTXR] %s m=%p kind=%d pre=%#lx rv=%ld from=%p tid=%d\n",
          what, (const void *)m, k, pre, rv, from, (int)gettid());
}


static int mtx_on(void) {
  static int on = -1;
  if (on < 0) {
    const char *v = getenv("RW_TRACE_MUTEX");
    on = (v && v[0] && v[0] != '0') ? 1 : 0;
  }
  return on;
}

/* The arena the stuck word lives in (measured: 0x10C00964A0 inside
   0x10C0000000+0x400000 kind=ANON).  Kept as a range, not one address, so a
   heap-layout shift does not silently blind the tracer. */
static int mtx_hot(const void *m) {
  unsigned long v = (unsigned long)m;
  return v >= 0x10C0000000UL && v < 0x10C0400000UL;
}

/* The assembly-loader loop locks three mutexes in the 0x1090000000 arena
   hundreds of times before the hang; logging them buries everything else.
   They are not where the process stops (measured), so they are skipped. */
static int mtx_boring(const void *m) {
  unsigned long v = (unsigned long)m;
  return (v >= 0x1090000000UL && v < 0x1091000000UL) ||
         (v >= 0x108218cc00UL && v < 0x108218cd00UL);
}

static void mtx_row(const char *what, const void *m, long v, void *from) {
  if (mtx_rows >= 6) return;
  mtx_rows++;
  fprintf(stderr, "[MTX] %s m=%p pid=%d v=%ld from=%p\n", what, m,
          (int)gettid(), v, from);
}

int pthread_mutex_lock(pthread_mutex_t *m) {
  if (!next_mutex_lock)
    next_mutex_lock = (int (*)(pthread_mutex_t *))
        dlvsym(RTLD_NEXT, "pthread_mutex_lock", "GLIBC_2.2.5");
  static int first_few;
  int pre = m ? *(volatile int *)m : -1;
  mtx_rec(0, m, __builtin_return_address(0));
  if (first_few < 3) {
    first_few++;
    mtx_row("first-lock", m, pre, __builtin_return_address(0));
  }
  if (mtx_on() && !mtx_boring(m) && (pre != 0 || mtx_hot(m)))
    mtx_row("lock", m, pre, __builtin_return_address(0));
  int r = next_mutex_lock(m);
  if (mtx_on()) {
    mtx_lock_n++;
    if (r != 0) {
      mtx_lock_bad++;
      mtx_fail_row("lock-fail", m, pre, r, __builtin_return_address(0));
    }
  }
  return r;
}

/* Unity's Mutex code may use timed/clocked entry points instead of the plain
   one; they all funnel into __pthread_mutex_clocklock_common (which is where
   the guest's stuck main thread sits), so a tracer that only wraps
   pthread_mutex_lock can miss every call. */
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *t) {
  static int (*real)(pthread_mutex_t *, const struct timespec *);
  if (!real)
    real = (int (*)(pthread_mutex_t *, const struct timespec *))
        dlvsym(RTLD_NEXT, "pthread_mutex_timedlock", "GLIBC_2.2.5");
  int pre = m ? *(volatile int *)m : -1;
  mtx_rec(3, m, __builtin_return_address(0));
  if (mtx_on() && (pre != 0 || mtx_hot(m) || (int)gettid() == mtx_main_tid))
    mtx_row("timedlock", m, pre, __builtin_return_address(0));
  int r = real ? real(m, t) : -1;
  if (mtx_on()) {
    mtx_timed_n++;
    if (r != 0) {
      mtx_timed_bad++;
      mtx_fail_row("timedlock-fail", m, pre, r, __builtin_return_address(0));
    }
  }
  return r;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
  if (!next_mutex_trylock)
    next_mutex_trylock = (int (*)(pthread_mutex_t *))
        dlvsym(RTLD_NEXT, "pthread_mutex_trylock", "GLIBC_2.2.5");
  int pre = m ? *(volatile int *)m : -1;
  mtx_rec(2, m, __builtin_return_address(0));
  int r = next_mutex_trylock(m);
  if (mtx_on()) {
    mtx_try_n++;
    if (r != 0) {
      mtx_try_bad++;
      mtx_fail_row("trylock-fail", m, pre, r, __builtin_return_address(0));
    } else if (mtx_hot(m) || (int)gettid() == mtx_main_tid) {
      mtx_row("trylock", m, r, __builtin_return_address(0));
    }
  }
  return r;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
  if (!next_mutex_unlock)
    next_mutex_unlock = (int (*)(pthread_mutex_t *))
        dlvsym(RTLD_NEXT, "pthread_mutex_unlock", "GLIBC_2.2.5");
  int pre = m ? *(volatile int *)m : -1;
  mtx_rec(1, m, __builtin_return_address(0));
  if (mtx_on() && !mtx_boring(m) && mtx_hot(m))
    mtx_row("unlock", m, pre, __builtin_return_address(0));
  int r = next_mutex_unlock(m);
  if (mtx_on()) {
    mtx_unlock_n++;
    if (r != 0) {
      mtx_unlock_bad++;
      mtx_fail_row("unlock-fail", m, pre, r, __builtin_return_address(0));
    }
  }
  return r;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
  if (!next_mutex_init)
    next_mutex_init = (int (*)(pthread_mutex_t *, const pthread_mutexattr_t *))
        dlvsym(RTLD_NEXT, "pthread_mutex_init", "GLIBC_2.2.5");
  int hot = mtx_on() && mtx_hot(m);
  /* L24: init/destroy go into the ring for EVERY address, not only the hot
     arena: the question "was the parked word ever a mutex, and was it
     destroyed" is answered by their presence/absence, and a lock pool is
     identified by four consecutive inits. */
  mtx_rec(7, m, __builtin_return_address(0));
  if (hot) mtx_row("init", m, -1, __builtin_return_address(0));
  int r = next_mutex_init(m, a);
  /* __kind is the 5th int of a glibc pthread_mutex_t; logging it after the
     call avoids linking libpthread just for pthread_mutexattr_gettype. */
  if (mtx_on()) {
    int k = mtx_kind(m);
    if (k >= 0 && k < 64) mtx_kind_init[k]++;
    unsigned n = ++mtx_init_total;
    /* Every non-plain kind is logged, always (bounded): the PI pool is 23
       objects with kind 33 and their addresses are exactly the blocks found
       later in a 0x30 free-list chain.  A cap over ALL inits hid them once. */
    if (k != 1 && mtx_init_rows < 160) {
      mtx_init_rows++;
      fprintf(stderr, "[MTXR] init-odd m=%p kind=%d rv=%d from=%p tid=%d n=%u\n",
              (void *)m, k, r, __builtin_return_address(0), (int)gettid(), n);
    }
    if (mtx_fd >= 0) { /* the log keeps kind for EVERY init */
      char b[128]; char *q = b, *e = b + sizeof b - 8;
      q = mtx_put(q, e, "[MTRK] init m=");
      q = mtx_putx(q, e, (unsigned long)m);
      q = mtx_put(q, e, " kind=");
      q = mtx_puti(q, e, k);
      q = mtx_put(q, e, "\n");
      ssize_t w = write(mtx_fd, b, (size_t)(q - b)); (void)w;
    }
    if (n % 64 == 0) mtx_summary("inits");
  }
  return r;
}

/* L24: the missing half of the lifetime question.  A lock that is DESTROYED
   and then locked is a use-after-free the allocator will happily rematerialise
   as a free-list link in its first qword; a lock that is freed WITHOUT destroy
   is a plain double allocation.  The two are told apart here, by rows. */
int pthread_mutex_destroy(pthread_mutex_t *m) {
  static int (*real)(pthread_mutex_t *);
  if (!real)
    real = (int (*)(pthread_mutex_t *))dlvsym(RTLD_NEXT, "pthread_mutex_destroy",
                                              "GLIBC_2.2.5");
  mtx_rec(8, m, __builtin_return_address(0));
  if (mtx_on() && mtx_hot(m))
    mtx_row("destroy", m, m ? *(volatile int *)m : -1, __builtin_return_address(0));
  return real ? real(m) : 0;
}

/* ---- semaphore / condvar observation ------------------------------------ */
int sem_wait(sem_t *s) {
  static int (*real)(sem_t *);
  if (!real) real = (int (*)(sem_t *))dlvsym(RTLD_NEXT, "sem_wait", "GLIBC_2.2.5");
  mtx_rec(4, s, __builtin_return_address(0));
  if (mtx_on() && ((int)gettid() == mtx_main_tid || mtx_hot(s)))
    mtx_row("sem_wait", s, s ? *(volatile int *)s : -1, __builtin_return_address(0));
  return real ? real(s) : -1;
}

int sem_post(sem_t *s) {
  static int (*real)(sem_t *);
  if (!real) real = (int (*)(sem_t *))dlvsym(RTLD_NEXT, "sem_post", "GLIBC_2.2.5");
  mtx_rec(5, s, __builtin_return_address(0));
  if (mtx_on() && ((int)gettid() == mtx_main_tid || mtx_hot(s)))
    mtx_row("sem_post", s, s ? *(volatile int *)s : -1, __builtin_return_address(0));
  return real ? real(s) : -1;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  static int (*real)(pthread_cond_t *, pthread_mutex_t *);
  if (!real)
    real = (int (*)(pthread_cond_t *, pthread_mutex_t *))
        dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.2.5");
  mtx_rec(6, c, __builtin_return_address(0));
  if (mtx_on())
    mtx_row("cond_wait", c, m ? *(volatile int *)m : -1,
            __builtin_return_address(0));
  return real ? real(c, m) : -1;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *t) {
  static int (*real)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
  if (!real)
    real = (int (*)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *))
        dlvsym(RTLD_NEXT, "pthread_cond_timedwait", "GLIBC_2.3.2");
  mtx_rec(6, c, __builtin_return_address(0));
  if (mtx_on())
    mtx_row("cond_timedwait", c, m ? *(volatile int *)m : -1,
            __builtin_return_address(0));
  return real ? real(c, m, t) : -1;
}

#endif /* !RW_NO_SHADOW_PTHREAD */

/* The executable path, and the directory Unity will append "_Data" to. */
static int exe_dir(char *out, size_t cap) {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return -1;
  exe[n] = 0;
  fprintf(stderr, "[RWMAIN] /proc/self/exe = %s\n", exe);
  char *slash = strrchr(exe, '/');
  if (!slash) return -1;
  *slash = 0;
  if (!exe[0]) strcpy(exe, "/");
  snprintf(out, cap, "%s", exe);
  return 0;
}

int main(int argc, char **argv) {
  fprintf(stderr, "[RWMAIN] AD-9 PlayerMain launcher start argc=%d\n", argc);
#ifndef RW_NO_SHADOW_PTHREAD
  mtx_main_tid = (int)gettid();
  fprintf(stderr, "[MTX] enabled=%d main_tid=%d\n", mtx_on(), mtx_main_tid);
  if (mtx_on()) {
    mtx_fd = open("/RW_HOME/mtx.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    signal(SIGALRM, mtx_alarm_handler);
    alarm(20);
  }
#else
  fprintf(stderr, "[RWMAIN] pthread shadowing compiled OUT (A/B)\n");
#endif

  char dir[4096];
  if (exe_dir(dir, sizeof dir) != 0) {
    fprintf(stderr, "[RWMAIN] FAIL readlink(/proc/self/exe)\n");
    return 1;
  }
  fprintf(stderr, "[RWMAIN] data dir = %s/RimWorldLinux_Data\n", dir);

  dso_dir = getenv("RW_DSO_DIR");
  fprintf(stderr, "[RWMAIN] RW_DSO_DIR = %s\n", dso_dir && dso_dir[0] ? dso_dir : "(unset)");

  char beside[4096], chosen[4096];
  snprintf(beside, sizeof beside, "%s/UnityPlayer.so", dir);

  const char *env = getenv("RW_UNITYPLAYER");
  if (env && env[0]) {
    snprintf(chosen, sizeof chosen, "%s", env);
    fprintf(stderr, "[RWMAIN] UnityPlayer.so from RW_UNITYPLAYER\n");
  } else if (access(beside, R_OK) == 0) {
    snprintf(chosen, sizeof chosen, "%s", beside);
    fprintf(stderr, "[RWMAIN] UnityPlayer.so next to the executable\n");
  } else {
    snprintf(chosen, sizeof chosen, "%s", RW_UNITYPLAYER_DEFAULT);
    fprintf(stderr, "[RWMAIN] UnityPlayer.so from the built-in default\n");
  }

  fprintf(stderr, "[RWMAIN] dlopen(%s, RTLD_NOW|RTLD_GLOBAL)\n", chosen);
  void *h = dlopen(chosen, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    fprintf(stderr, "[RWMAIN] FAIL dlopen: %s\n", dlerror());
    return 2;
  }
  fprintf(stderr, "[RWMAIN] UnityPlayer.so handle=%p\n", h);

  dlerror();
  void *sym = dlsym(h, RW_PLAYERMAIN);
  const char *err = dlerror();
  if (!sym) {
    fprintf(stderr, "[RWMAIN] FAIL dlsym(" RW_PLAYERMAIN "): %s\n", err ? err : "?");
    return 3;
  }
  fprintf(stderr, "[RWMAIN] PlayerMain=%p argc=%d — calling\n", sym, argc);

  int rc = ((int (*)(int, char **))sym)(argc, argv);
  fprintf(stderr, "[RWMAIN] PlayerMain returned %d\n", rc);
  return rc;
}
