/*
 * gl_stubs.c - the libc surface Mesa's GL frontend needs and HobbyOS lacks.
 *
 * Lane L5 (M4).  Two link flavours consume this file:
 *
 *   HB_MESA_SELF_CONTAINED=0  (the shipped libGL.so.1, "compat-resolved")
 *     The DSO is loaded by the compat class, whose global symbol scope
 *     already contains the pinned glibc 2.31 (libc/libm/libpthread/libdl/
 *     librt) and lane L4's libX11.so.6.  Mesa's 242 undefined symbols
 *     resolve there at load time.  Only the handful glibc does *not* export
 *     as dynamic symbols are provided here: atexit (glibc keeps it in
 *     libc_nonshared.a) and _assert_fail (a HobbyOS libc name, not glibc's
 *     __assert_fail).  Everything else in this file is #if'd out so the DSO
 *     does not shadow glibc with a worse implementation.
 *
 *   HB_MESA_SELF_CONTAINED=1  (the "no-glibc" variant, build with
 *     --self-contained)
 *     libGL links HobbyOS's own libc (rebuilt -fPIC, since obj/intel/libc.a
 *     is -mcmodel=large and cannot go into a DSO) and this file supplies
 *     every symbol that libc.a does not define.  That is the residual of the
 *     72-symbol gap list in docs/../MESA-NOTES.md §6.2 after the header work:
 *     the entries below, plus the float-math wrappers at the end.
 *
 * Ground rules for this file:
 *   - No new state that libc also owns (no second malloc, no second errno).
 *   - Fail closed, never abort: Mesa treats a failed "optional" call as
 *     "feature unavailable" and keeps rendering.
 *   - Everything here is documented with the Mesa caller that needs it, so
 *     the next person can tell whether it is still needed.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <nl_types.h>
#include <syslog.h>
#include <link.h>

/* ------------------------------------------------------------------ */
/* Always linked (both flavours)                                       */
/* ------------------------------------------------------------------ */

/* Mesa's assert() goes through HobbyOS's <assert.h>, which calls
 * _assert_fail (glibc's assert calls __assert_fail).  In the compat build
 * glibc knows nothing about this name, so define it here.  Called 655 times
 * in the host build's hot paths only when a Mesa invariant actually breaks -
 * print and die, like libc's. */
void
_assert_fail(const char *file, int line, const char *func, const char *expr)
{
   fprintf(stderr, "Mesa: %s:%d: %s: Assertion `%s' failed.\n",
           file ? file : "?", line, func ? func : "?", expr ? expr : "?");
   fflush(stderr);
   abort();
}

/* glibc keeps atexit() in libc_nonshared.a (a __cxa_atexit wrapper), so the
 * dynamic symbol is absent from libc.so.6.  Mesa registers a few cleanup
 * handlers (u_debug, the shader cache). */
extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);

int
atexit(void (*function)(void))
{
   return __cxa_atexit((void (*)(void *))function, NULL, NULL);
}

/* ---- libdrm / Android sync: not in glibc either ---- */
/* Nineteen DRM entry points and four sync-fence ones.  None of them exists
 * in glibc (they live in libdrm and libsync), and softpipe has no DRM device,
 * so these are linked in *both* flavours: fail closed, once. */
int
drmIoctl(int fd, unsigned long request, void *arg)
{
   (void)fd;
   (void)request;
   (void)arg;
   return -1;
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   (void)fd;
   (void)capability;
   (void)value;
   return -1;
}

int
drmClose(int fd)
{
   (void)fd;
   return -1;
}

void *
drmGetVersion(int fd)
{
   (void)fd;
   return NULL;
}

void
drmFreeVersion(void *version)
{
   (void)version;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd;
   (void)handle;
   (void)flags;
   (void)prime_fd;
   return -1;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   (void)prime_fd;
   (void)handle;
   return -1;
}

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{ (void)fd; (void)flags; (void)handle; return -1; }
int drmSyncobjDestroy(int fd, uint32_t handle)
{ (void)fd; (void)handle; return -1; }
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{ (void)fd; (void)handle; (void)obj_fd; return -1; }
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{ (void)fd; (void)obj_fd; (void)handle; return -1; }
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; return -1; }
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; return -1; }
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point, uint32_t flags)
{ (void)fd; (void)dst_handle; (void)dst_point; (void)src_handle;
  (void)src_point; (void)flags; return -1; }
int drmSyncobjQuery2(int fd, const uint32_t *handles, uint64_t *points,
                     uint32_t handle_count, uint32_t flags)
{ (void)fd; (void)handles; (void)points; (void)handle_count; (void)flags;
  return -1; }
int drmSyncobjTimelineWait(int fd, const uint32_t *handles,
                           const uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags,
                           uint32_t *first_signaled)
{ (void)fd; (void)handles; (void)points; (void)num_handles;
  (void)timeout_nsec; (void)flags; (void)first_signaled; return -1; }
int drmSyncobjWait(int fd, const uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled)
{ (void)fd; (void)handles; (void)num_handles; (void)timeout_nsec;
  (void)flags; (void)first_signaled; return -1; }
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{ (void)fd; (void)handles; (void)handle_count; return -1; }
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{ (void)fd; (void)handles; (void)handle_count; return -1; }
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                             const uint64_t *points, uint32_t handle_count)
{ (void)fd; (void)handles; (void)points; (void)handle_count; return -1; }

/* ---- Android sync fences (u_tests.c only) ---- */
int sync_wait(int fd, int timeout) { (void)fd; (void)timeout; return -1; }
int sync_merge(const char *name, int fd1, int fd2)
{ (void)name; (void)fd1; (void)fd2; return -1; }
int sync_inc(int fd) { (void)fd; return -1; }
int sync_dec(int fd) { (void)fd; return -1; }

/* ------------------------------------------------------------------ */
/* Self-contained flavour only                                        */
/* ------------------------------------------------------------------ */
#if HB_MESA_SELF_CONTAINED

/* ---- allocation ---- */
/* src/util/os_memory_aligned.h, src/mesa/main/bufferobj.c (13 refs).
 * aligned_alloc() exists in libc.a; posix_memalign() is the POSIX spelling
 * Mesa uses and returns an errno value rather than NULL. */
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
   void *p;

   if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
      return EINVAL;

   p = aligned_alloc(alignment, size);
   if (p == NULL)
      return ENOMEM;

   *memptr = p;
   return 0;
}

/* ---- string ---- */
/* glcpp-parse.c, gl_nir_lower_xfb_varying.c, util/u_printf.c. */
char *
strndup(const char *s, size_t n)
{
   size_t len = strnlen(s, n);
   char *p = malloc(len + 1);

   if (p == NULL)
      return NULL;
   memcpy(p, s, len);
   p[len] = '\0';
   return p;
}

/* ---- sorting ---- */
/* src/util/u_qsort.h (nir.c, nir_opt_vectorize.c, nir_opt_reassociate.c,
 * u_qsort.cpp).  glibc's GNU signature: the context is the LAST argument. */
struct qsort_r_ctx {
   int (*compar)(const void *, const void *, void *);
   void *arg;
};

static struct qsort_r_ctx *qsort_r_active;

static int
qsort_r_trampoline(const void *a, const void *b)
{
   return qsort_r_active->compar(a, b, qsort_r_active->arg);
}

void
qsort_r(void *base, size_t nmemb, size_t size,
        int (*compar)(const void *, const void *, void *), void *arg)
{
   struct qsort_r_ctx ctx = { compar, arg };
   struct qsort_r_ctx *saved = qsort_r_active;

   /* Single-threaded by construction: Mesa only sorts while compiling a
   * shader on the calling thread, and HobbyOS's qsort is not reentrant. */
   qsort_r_active = &ctx;
   qsort(base, nmemb, size, qsort_r_trampoline);
   qsort_r_active = saved;
}

/* ---- environment ---- */
/* src/util/os_misc.c.  No setuid model, so secure_getenv == getenv. */
char *
secure_getenv(const char *name)
{
   return getenv(name);
}

/* src/util/u_process.c reads these two globals.  libc.a has neither; set
 * them from argv[0] lazily through the constructor-free route (Mesa falls
 * back to "unknown" when they are NULL, which is what we want until the
 * frontend calls util_set_process_name). */
char *program_invocation_name;
char *program_invocation_short_name;

/* ---- threads ---- */
/* HobbyOS has pthread_mutex/cond/once/rwlock/barrier, but not these five.
 * softpipe is single-threaded; util_queue only starts workers when
 * util_cpu_detect() sees >1 CPU, and the HobbyOS port reports one. */
int
pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime)
{
   (void)abstime;
   return pthread_mutex_lock(mutex);
}

int
pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
   (void)attr;
   return clock_id == CLOCK_MONOTONIC ? 0 : EINVAL;
}

int
pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id)
{
   (void)thread;
   *clock_id = CLOCK_PROCESS_CPUTIME_ID;
   return 0;
}

int
pthread_setname_np(pthread_t thread, const char *name)
{
   (void)thread;
   (void)name;
   return 0; /* no per-thread names in the kernel yet */
}

int
pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t *cpuset)
{
   (void)thread;
   if (cpuset != NULL && cpusetsize >= sizeof(*cpuset))
      memset(cpuset, 0, sizeof(*cpuset));
   return 0;
}

int
pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
                       const cpu_set_t *cpuset)
{
   (void)thread;
   (void)cpusetsize;
   (void)cpuset;
   return 0; /* the scheduler has no affinity mask */
}

int
sched_getcpu(void)
{
   return 0; /* one CPU is visible to a process today */
}

/* ---- time ---- */
/* src/util/os_time.c uses the absolute form for its sleep-until helper. */
int
clock_nanosleep(clockid_t clk_id, int flags, const struct timespec *req,
                struct timespec *rem)
{
   if (req == NULL)
      return EINVAL;

   if (flags & TIMER_ABSTIME) {
      struct timespec now;
      struct timespec rel;

      if (clock_gettime(clk_id, &now) != 0)
         return errno;
      rel.tv_sec = req->tv_sec - now.tv_sec;
      rel.tv_nsec = req->tv_nsec - now.tv_nsec;
      if (rel.tv_nsec < 0) {
         rel.tv_sec -= 1;
         rel.tv_nsec += 1000000000L;
      }
      if (rel.tv_sec < 0)
         return 0; /* already due */
      return nanosleep(&rel, rem) == 0 ? 0 : errno;
   }

   return nanosleep(req, rem) == 0 ? 0 : errno;
}

void
sync(void)
{
}

/* ---- process ---- */
/* src/gallium/auxiliary/util/u_queue.c lowers worker nice levels. */
int
getpriority(int which, id_t who)
{
   (void)which;
   (void)who;
   return 0;
}

int
setpriority(int which, id_t who, int prio)
{
   (void)which;
   (void)who;
   (void)prio;
   return 0;
}

/* ---- stdio extras ---- */
/* src/util/memstream.c and glsl_parser_extras.cpp build error strings in
 * memory.  HobbyOS's stdio has no open_memstream; returning NULL makes
 * u_memstream fall back to "no buffer", which loses *error text* only
 * (shader compilation still succeeds or fails identically). */
FILE *
open_memstream(char **bufp, size_t *sizep)
{
   if (bufp != NULL)
      *bufp = NULL;
   if (sizep != NULL)
      *sizep = 0;
   errno = ENOSYS;
   return NULL;
}

/* The ddebug gallium driver pipes an external parser; no shell in-guest. */
FILE *
popen(const char *command, const char *type)
{
   (void)command;
   (void)type;
   errno = ENOSYS;
   return NULL;
}

int
pclose(FILE *stream)
{
   (void)stream;
   errno = ENOSYS;
   return -1;
}

/* ---- message catalogs / syslog ---- */
nl_catd
catopen(const char *name, int flag)
{
   (void)name;
   (void)flag;
   return (nl_catd)-1;
}

char *
catgets(nl_catd catalog, int set_number, int message_number,
        const char *message)
{
   (void)catalog;
   (void)set_number;
   (void)message_number;
   return (char *)message;
}

int
catclose(nl_catd catalog)
{
   (void)catalog;
   return 0;
}

void
openlog(const char *ident, int option, int facility)
{
   (void)ident;
   (void)option;
   (void)facility;
}

void
syslog(int priority, const char *format, ...)
{
   va_list ap;

   (void)priority;
   va_start(ap, format);
   vfprintf(stderr, format, ap);
   va_end(ap);
   fputc('\n', stderr);
}

void
closelog(void)
{
}

/* ---- SysV IPC / eventfd / inotify (all fail closed) ---- */
int
shmget(key_t key, size_t size, int shmflg)
{
   (void)key;
   (void)size;
   (void)shmflg;
   errno = ENOSYS;
   return -1;
}

void *
shmat(int shmid, const void *shmaddr, int shmflg)
{
   (void)shmid;
   (void)shmaddr;
   (void)shmflg;
   errno = ENOSYS;
   return (void *)-1;
}

int
shmdt(const void *shmaddr)
{
   (void)shmaddr;
   errno = ENOSYS;
   return -1;
}

int
shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
   (void)shmid;
   (void)cmd;
   (void)buf;
   errno = ENOSYS;
   return -1;
}

int
eventfd(unsigned int initval, int flags)
{
   (void)initval;
   (void)flags;
   errno = ENOSYS;
   return -1;
}

int
eventfd_read(int fd, uint64_t *value)
{
   (void)fd;
   (void)value;
   errno = ENOSYS;
   return -1;
}

int
eventfd_write(int fd, uint64_t value)
{
   (void)fd;
   (void)value;
   errno = ENOSYS;
   return -1;
}

int
inotify_init1(int flags)
{
   (void)flags;
   errno = ENOSYS;
   return -1;
}

int
inotify_add_watch(int fd, const char *pathname, uint32_t mask)
{
   (void)fd;
   (void)pathname;
   (void)mask;
   errno = ENOSYS;
   return -1;
}

int
inotify_rm_watch(int fd, int wd)
{
   (void)fd;
   (void)wd;
   errno = ENOSYS;
   return -1;
}

/* ---- ELF / auxv / dynamic loader helpers ---- */
/* src/util/u_cpu_detect.c only reads AT_HWCAP on non-x86 branches. */
unsigned long
getauxval(unsigned long type)
{
   (void)type;
   return 0;
}

/* src/util/build_id.c walks the loaded objects for the GNU build-id.  There
 * is no phdr list to walk in the native loader; "no objects" is honest. */
int
dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size,
                                void *data), void *data)
{
   (void)callback;
   (void)data;
   return 0;
}

/* ------------------------------------------------------------------ */
/* Float math                                                         */
/* ------------------------------------------------------------------ */
/* libc.a's <math.h> is double-only (MESA-NOTES §6.2 row 8): the GLSL/NIR
 * constant folders call the float forms ~30 times.  Correctness matters -
 * these evaluate shader constants at compile time - so they route through
 * the double implementations, which the tree's math.c already has, and are
 * correctly rounded by the hardware conversion. */
float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float asinf(float x) { return (float)asin((double)x); }
float acosf(float x) { return (float)acos((double)x); }
float atanf(float x) { return (float)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float logf(float x) { return (float)log((double)x); }
float log2f(float x) { return (float)log2((double)x); }
float log10f(float x) { return (float)log10((double)x); }
float expf(float x) { return (float)exp((double)x); }
float exp2f(float x) { return (float)exp2((double)x); }
float sqrtf(float x) { return (float)sqrt((double)x); }
float fabsf(float x) { return (float)fabs((double)x); }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x) { return (float)ceil((double)x); }
float truncf(float x) { return (float)trunc((double)x); }
float rintf(float x) { return (float)rint((double)x); }
float nearbyintf(float x) { return (float)nearbyint((double)x); }
float roundf(float x) { return (float)round((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
float ldexpf(float x, int e) { return (float)ldexp((double)x, e); }
float frexpf(float x, int *e) { return (float)frexp((double)x, e); }
float modff(float x, float *ip)
{
   double dip;
   float frac = (float)modf((double)x, &dip);
   *ip = (float)dip;
   return frac;
}
float fmaf(float x, float y, float z) { return (float)fma((double)x, (double)y, (double)z); }
float fmaxf(float x, float y) { return x > y ? x : y; }
float fminf(float x, float y) { return x < y ? x : y; }
float copysignf(float x, float y) { return (float)copysign((double)x, (double)y); }
float sinhf(float x) { return (float)sinh((double)x); }
float coshf(float x) { return (float)cosh((double)x); }
float tanhf(float x) { return (float)tanh((double)x); }
float cbrtf(float x) { return (float)cbrt((double)x); }
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
float erff(float x) { return (float)erf((double)x); }

void
sincosf(float x, float *s, float *c)
{
   *s = sinf(x);
   *c = cosf(x);
}

void
sincos(double x, double *s, double *c)
{
   *s = sin(x);
   *c = cos(x);
}

#endif /* HB_MESA_SELF_CONTAINED */
