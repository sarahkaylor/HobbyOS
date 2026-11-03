/*
 * src/kernel/linux_compat_proc.c — the Linux-ABI process / identity / stat /
 * signal subsystem (docs/linux-abi.md §6/§8, gaming.md §2.2 #2).
 *
 * WHAT THIS FILE OWNS
 * -------------------
 * Exactly one array, l_tab_proc[], whose entries answer the numbers a Linux
 * program asks about *itself*: signal dispositions and masks, identity of its
 * files, resource limits, and the kernel's notion of time and memory.  The
 * dispatch contract (return 1 after writing tf->regs[0], 0 = "not mine") lives
 * in src/include/linux_compat.h; the numbers come from linux_syscall.h.
 *
 * Nothing here allocates, blocks, or reaches into another process's address
 * space.  Every user pointer goes through l_copy_from_user / l_copy_to_user /
 * l_copy_cstr, so a bogus pointer is -EFAULT, never a kernel fault.
 *
 * SIGNALS — WHAT IS REAL AND WHAT IS NOT (read this before trusting a row)
 * -----------------------------------------------------------------------
 * The kernel already has a signal implementation (process_sigaction,
 * process_signal, process_signal_deliver, process_sigreturn in process.c, used
 * by SIGTEST.BIN).  This file does NOT reimplement it; it MARSHALS the Linux
 * syscall's 32-byte struct sigaction (NOT the 152-byte userspace one — see the
 * layout note above l_proc_rt_sigaction) into the kernel's 40-byte struct
 * ho_sigaction and calls the native path.  That gives us, today, for free:
 *
 *   - disposition storage per process group (rt_sigaction) including the
 *     sa_restorer the native delivery engine needs to return from a handler;
 *   - real delivery: a raised SIGINT/SIGTERM/SIGPIPE/SIGHUP/SIGCHLD is planted
 *     as the native HOS3 frame on the guest stack, the guest handler runs with
 *     rdi = signum, and the musl restorer's `mov $15,%rax; syscall` lands in
 *     this file's rt_sigreturn, which calls process_sigreturn() and resumes the
 *     interrupted context.  The probe proves that round trip.
 *
 *   - the native engine can only DELIVER a few numbers (sig_deliverable() in
 *     process.c excludes SIGILL/ABRT/BUS/SEGV/USR1/USR2/STOP); the rest are
 *     stored and silently deferred.  Documented, not hidden.
 *
 * What is *storage only* (honest TODO, needs kernel work outside this lane's
 * file set — process.h has no field for either):
 *
 *   - the signal MASK (rt_sigprocmask).  It round-trips correctly here but the
 *     delivery engine does not consult it, so a blocked signal can still be
 *     delivered.  Single-threaded programs (Doom) block nothing that they then
 *     rely on being deferred; a program that does would see an early handler.
 *   - sigaltstack.  Stored and returned; delivery always uses the normal stack
 *     (SA_ONSTACK is not honoured).
 *   - SA_SIGINFO.  The native frame carries signum in rdi only; the
 *     (signo, siginfo_t*, ucontext_t*) form receives garbage in rsi/rdx.  A
 *     handler that reads siginfo must not be installed until the frame engine
 *     grows a Linux-shaped ucontext.
 *   - per-thread masks (docs §8 asks for per-thread): the table below is keyed
 *     by pid, and a thread has its own pid, so it is per-thread by accident of
 *     the native clone model — but the delivery engine delivers per GROUP.
 *
 * STAT — why it is not a FAT16 re-implementation
 * ----------------------------------------------
 * fs.c already resolves both an fd and a path to FAT16 metadata and fills a
 * native struct k_stat (st_ino synthesized from the dirent's sector/offset,
 * mode from the attribute byte, REAL size, real FAT mtime).  This file
 * translates that into the 144-byte Linux layout, so the two ABIs can never
 * disagree about a file's size — and Doom's WAD is opened, seeked and read by
 * size, while libX11 fstats its sockets and musl's stdio probes its files.
 *
 * JOB CONTROL / NICE / AFFINITY — WHAT IS MODELLED
 * -----------------------------------------------
 * The PCB has a pid, a parent_pid and a group anchor (tgid); it has no pgid,
 * no session, no nice value and no CPU mask, and this lane may not add fields
 * to process.h.  Each handler below therefore translates onto the anchor and
 * reports what is TRUE of this kernel instead of inventing state:
 *
 *   getpgid/getsid     the group anchor's pid (sid == pgid here).
 *   setpgid(0,0)       success — every process is already its own anchor.
 *   setpgid(other)     -EPERM; joining a group is not expressible.
 *   setsid             -EPERM; Linux's own refusal for a group leader.
 *   getpriority        nice 0 (raw 20) for every target.
 *   setpriority        non-negative nice accepted and ignored (a one-class
 *                      scheduler cannot honour it and it can never be a
 *                      correctness failure); negative nice is -EPERM.
 *   sched_getaffinity  the online CPU set, 8 bytes written, the caller's whole
 *                      128-byte buffer zeroed first (see the handler).
 *   sched_setaffinity  accepted, not enforced: the scheduler has no per-process
 *                      mask, and affinity is a hint.
 *   prctl              PR_SET/GET_NAME, PR_SET/GET_DUMPABLE, PR_SET/GET_PDEATHSIG
 *                      (see the handler for exactly how much each one does).
 */
#include "linux_compat.h"

#include <stdint.h>

#include "arch/trap.h"
#include "errno.h"
#include "frame.h"
#include "fs.h"
#include "kline.h"
#include "linux_layout.h"
#include "linux_syscall.h"
#include "lock.h"
#include "process.h"
#include "timer.h"
#include "vm.h"

/* The layouts this file writes into guest memory.  A drift here corrupts a
   struct the guest then trusts, so lock them at compile time (docs §6, and the
   measurements on this workstation that the probe's differential run repeats).
   linux_sigaction_t is the USERSPACE struct (what libX11's <signal.h> declares,
   152 B); rt_sigaction's own argument is the 32-byte kernel form declared just
   above its handler — both are asserted, because getting either wrong is
   silent. */
typedef char l_proc_stat_size_assert[sizeof(linux_stat_t) == 144 ? 1 : -1];
typedef char l_proc_sigaction_size_assert[sizeof(linux_sigaction_t) == 152 ? 1 : -1];
typedef char l_proc_stack_size_assert[sizeof(linux_stack_t) == 24 ? 1 : -1];

/* ---- helpers ------------------------------------------------------------ */

static void l_zero(void *dst, uint64_t n) {
  uint8_t *b = (uint8_t *)dst;
  for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

/* The per-process slot for the compat-only state below.  Keyed by pid: a
   thread has its own pid, so a mask/sigaltstack set inside a thread stays
   there (Linux's per-thread semantics, by construction). */
static int l_slot(struct process *p) {
  if (!p) return -1;
  if (p->pid < 0 || p->pid >= MAX_PROCESSES) return -1;
  return p->pid;
}

static int l_ret(struct trap_frame *tf, int rc) {
  tf->regs[0] = (uint64_t)(int64_t)rc;
  return 1;
}

/* ---- compat-only per-process state --------------------------------------
 * process.h has no room for these (it is another lane's file), so they live
 * here as pid-indexed tables: 64 entries, no allocation, no lock needed for a
 * naturally-aligned 64-bit store on this target.  See the header comment for
 * exactly how much of this the delivery engine honours today. */
static uint64_t l_sigmask[MAX_PROCESSES];

/* sa_flags per (pid, signal).  The native PCB stores handler/mask/restorer
   but not the flag word, and the compat delivery engine needs SA_ONSTACK and
   SA_SIGINFO to build a correct frame.  Keyed like everything else here. */
static uint32_t l_sigflags[MAX_PROCESSES][HO_SIG_MAX];

struct l_altstack_state {
  uint64_t ss_sp;
  uint64_t ss_size;
  int32_t ss_flags;
  int32_t in_use;
};
static struct l_altstack_state l_altstack[MAX_PROCESSES];

/* --- accessors for linux_compat_thr.c's frame engine ---------------------
 * One file owns the state; the engine that builds Linux rt_sigframes asks. */
uint64_t linux_compat_sigmask_get(struct process *p) {
  int slot = l_slot(p);
  return slot < 0 ? 0 : l_sigmask[slot];
}

void linux_compat_sigmask_set(struct process *p, uint64_t mask) {
  int slot = l_slot(p);
  if (slot < 0) return;
  mask &= ~((1ull << (L_SIGKILL - 1)) | (1ull << (L_SIGSTOP - 1)));
  l_sigmask[slot] = mask;
}

int linux_compat_altstack_get(struct process *p, uint64_t *sp, uint64_t *size,
                              int *flags) {
  int slot = l_slot(p);
  if (slot < 0 || !l_altstack[slot].in_use) return -1;
  if (sp) *sp = l_altstack[slot].ss_sp;
  if (size) *size = l_altstack[slot].ss_size;
  if (flags) *flags = l_altstack[slot].ss_flags;
  return 0;
}

int linux_compat_sigflags_get(struct process *p, int sig) {
  /* Dispositions (and therefore their flags) are group state: a handler
     installed by one thread is entered by any thread, with that thread's
     stack rules.  Resolve the anchor before the pid lookup. */
  int slot = l_slot(process_group(p));
  if (slot < 0 || sig <= 0 || sig >= HO_SIG_MAX) return 0;
  return (int)l_sigflags[slot][sig];
}

/* The umask is process state in POSIX but a single kernel-wide value here:
 * every compat process is uid 0 and FAT16 carries no permission bits, so a
 * shared value changes no observable answer — it only has to round-trip. */
static uint32_t l_umask = 022;

/* Console budget for the "refused a handler install" diagnostic (below): one
   line per distinct refusal is evidence, an unbounded stream is a flood.  The
   record rides the [LCOMPAT] prefix so the probe gate prints it. */
static int l_sa_diag;

/* ---- stat --------------------------------------------------------------- */

static void l_stat_fill(const struct k_stat *ks, linux_stat_t *o) {
  l_zero(o, sizeof *o);
  /* One volume, mounted at /; the native ABI also reports 0.  A program that
     compares st_dev between two paths still gets a stable answer. */
  o->st_dev = 0;
  o->st_ino = ks->st_ino;
  o->st_nlink = ks->st_nlink > 0 ? (uint64_t)ks->st_nlink : 1;
  o->st_mode = ks->st_mode;
  o->st_uid = ks->st_uid;
  o->st_gid = ks->st_gid;
  o->st_rdev = ks->st_rdev;
  o->st_size = (int64_t)ks->st_size;
  o->st_blksize = (int64_t)ks->st_blksize;
  o->st_blocks = (int64_t)ks->st_blocks;
  /* FAT has no atime and no ctime; the dirent's real modify time is the only
     timestamp the volume carries, so all three report it (docs §6 leaves the
     nanoseconds at 0 — a FAT timestamp has 2-second resolution). */
  o->st_atim.tv_sec = (int64_t)ks->st_atime;
  o->st_mtim.tv_sec = (int64_t)ks->st_mtime;
  o->st_ctim.tv_sec = (int64_t)ks->st_ctime;
}

/* Resolve a user path and fill the Linux layout.  Returns 0 or -errno. */
static int l_stat_path(struct process *p, uint64_t upath, linux_stat_t *o) {
  char path[256];
  int r = l_copy_cstr(p, path, upath, sizeof path);
  if (r != 0) return r;
  struct k_stat ks;
  int err = 0;
  if (file_stat_path(p, path, &ks, &err) != 0) return -(err ? err : EIO);
  l_stat_fill(&ks, o);
  return 0;
}

static int l_stat_fd(struct process *p, int fd, linux_stat_t *o) {
  struct k_stat ks;
  int err = 0;
  if (file_stat_fd(p, fd, &ks, &err) != 0) return -(err ? err : EBADF);
  l_stat_fill(&ks, o);
  return 0;
}

static int l_proc_stat(struct process *p, struct trap_frame *tf) {
  uint64_t upath = l_arg(tf, 0);
  uint64_t ust = l_arg(tf, 1);
  if (!ust) return l_ret(tf, -EFAULT);
  linux_stat_t o;
  int rc = l_stat_path(p, upath, &o);
  if (rc != 0) return l_ret(tf, rc);
  if (l_copy_to_user(p, ust, &o, sizeof o) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

static int l_proc_fstat(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t ust = l_arg(tf, 1);
  if (!ust) return l_ret(tf, -EFAULT);
  linux_stat_t o;
  int rc = l_stat_fd(p, fd, &o);
  if (rc != 0) return l_ret(tf, rc);
  if (l_copy_to_user(p, ust, &o, sizeof o) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* lstat == stat: FAT16 has no symbolic links, so "do not follow" and "follow"
   name the same object.  Saying so in one place beats a second code path that
   would only ever be wrong in the same way. */
static int l_proc_lstat(struct process *p, struct trap_frame *tf) {
  return l_proc_stat(p, tf);
}

/* newfstatat(dirfd, path, st, flags).  Supported: absolute paths, AT_FDCWD
   relative paths, and (with AT_EMPTY_PATH) the dirfd itself.  A path relative
   to a NON-AT_FDCWD dirfd needs an fd -> path resolver the kernel does not
   have; refusing loudly is better than silently statting the wrong file. */
static int l_proc_newfstatat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  uint64_t upath = l_arg(tf, 1);
  uint64_t ust = l_arg(tf, 2);
  int flags = (int)l_argi(tf, 3);
  if (!ust) return l_ret(tf, -EFAULT);
  char path[256];
  int r = l_copy_cstr(p, path, upath, sizeof path);
  if (r != 0) return l_ret(tf, r);
  linux_stat_t o;
  int rc;
  if (path[0] == '\0') {
    if (!(flags & L_AT_EMPTY_PATH) || dirfd == L_AT_FDCWD) return l_ret(tf, -ENOENT);
    rc = l_stat_fd(p, dirfd, &o);
  } else if (path[0] == '/' || dirfd == L_AT_FDCWD) {
    rc = l_stat_path(p, upath, &o);
  } else {
    return l_ret(tf, -ENOTSUP);
  }
  if (rc != 0) return l_ret(tf, rc);
  if (l_copy_to_user(p, ust, &o, sizeof o) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* ---- readlink / readlinkat --------------------------------------------- */

/* The virtual symlinks a compat program actually reads.
 *
 * MEASURED on the host against the real RimWorld binary (tools/rw/proc-usage.sh,
 * 2026-11-02): readlinkat("/proc/self/exe") once with a 4096 buffer, then
 * readlink("/proc/self/exe") 6x with 99-byte buffers from the startup and
 * render threads; readlink("/proc/self") once (the kernel answers the pid as
 * a decimal string); and NO readlink of /proc/self/fd/N (the fds there are
 * re-opened by PATH, which is why the open path resolves that link, below).
 *
 * /proc/self/exe answers the executable the loader was asked to run, never
 * the interpreter it entered and never the truncated display name in
 * p->name.  That distinction is load-bearing: Unity's SDL derives its data
 * directory from this link, and with ld.so there it reports "There is no data
 * folder" (measured on the host; see docs/rimworld-launch-log.md).
 *
 * Return: > 0 length copied into `out` (never NUL-terminated, like
 * readlink(2)); 0 = "not a virtual link, decide from the volume" (the
 * caller's sentinel); < 0 = -errno.  (0 rather than 1 on purpose: a pid
 * string for pid < 10 is one character long and used to be mistaken for the
 * sentinel — measured on the first probe boot, /proc/self answered ENOENT.)
 */
static int l_streq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) { a++; b++; }
  return *a == '\0' && *b == '\0';
}

static int l_dec(char *out, unsigned v) {
  char tmp[12];
  int n = 0;
  if (v == 0) { out[0] = '0'; return 1; }
  while (v && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  return n;
}

/* "/proc/self/fd/<decimal>" exactly. */
static int l_self_fd_path(const char *path, int *fd_out) {
  static const char pfx[] = "/proc/self/fd/";
  int i = 0;
  for (; pfx[i]; i++)
    if (path[i] != pfx[i]) return 0;
  const char *n = path + i;
  if (!*n) return 0;
  unsigned v = 0;
  for (; *n; n++) {
    if (*n < '0' || *n > '9') return 0;
    v = v * 10 + (unsigned)(*n - '0');
    if (v > 100000u) return 0;
  }
  *fd_out = (int)v;
  return 1;
}

static int l_readlink_target(struct process *p, const char *path, char *out,
                             uint64_t cap) {
  if (path[0] != '/') return -ENOENT;
  struct process *g = p ? process_group(p) : 0;
  const char *src = 0;
  char buf[160];
  int fd = -1;

  if (l_streq(path, "/proc/self")) {
    int n = l_dec(buf, (unsigned)(g ? g->pid : 0));
    buf[n] = '\0';
    src = buf;
  } else if (l_streq(path, "/proc/self/exe")) {
    /* The loaded executable's path (absolute at load time).  Fall back to the
       display name for a process that predates the field / a kernel thread. */
    src = (g && g->exe_path[0]) ? g->exe_path : (p ? p->name : "/");
    if (src[0] != '/') {
      uint64_t n = 0;
      buf[n++] = '/';
      for (int i = 0; src[i] && n < sizeof buf - 1; i++) buf[n++] = src[i];
      buf[n] = '\0';
      src = buf;
    }
  } else if (l_streq(path, "/proc/self/cwd")) {
    src = (g && g->cwd[0]) ? g->cwd : "/";
  } else if (l_streq(path, "/proc/self/root")) {
    src = "/";
  } else if (l_self_fd_path(path, &fd)) {
    int r = file_path_of(g, fd, buf, (int)sizeof buf);
    if (r != 0) return r; /* -EBADF for a closed fd, -ENOENT for a nameless one */
    src = buf;
  } else {
    return 0; /* not a virtual link */
  }

  uint64_t len = 0;
  while (src[len]) len++;
  uint64_t n = len < cap ? len : cap; /* truncated, but no NUL terminator */
  for (uint64_t k = 0; k < n; k++) out[k] = src[k];
  return (int)n;
}

static int l_readlink_common(struct process *p, uint64_t upath, uint64_t ubuf,
                             int64_t bufsz) {
  if (bufsz <= 0) return -EINVAL;
  char path[256];
  int r = l_copy_cstr(p, path, upath, sizeof path);
  if (r != 0) return r;
  char target[160];
  int n = l_readlink_target(p, path, target, sizeof target);
  if (n == 0) {
    /* A real FAT16 path: readlink(2) on a non-symlink is -EINVAL, and on a
       missing path -ENOENT.  Decide from the volume, not by guessing. */
    struct k_stat ks;
    int err = 0;
    if (file_stat_path(p, path, &ks, &err) != 0) return -(err ? err : EIO);
    return -EINVAL;
  }
  if (n < 0) return n;
  if (l_copy_to_user(p, ubuf, target, (uint64_t)n) != 0) return -EFAULT;
  return n;
}

static int l_proc_readlink(struct process *p, struct trap_frame *tf) {
  int rc = l_readlink_common(p, l_arg(tf, 0), l_arg(tf, 1), l_argi(tf, 2));
  return l_ret(tf, rc);
}

static int l_proc_readlinkat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  uint64_t upath = l_arg(tf, 1);
  char path[256];
  int r = l_copy_cstr(p, path, upath, sizeof path);
  if (r != 0) return l_ret(tf, r);
  if (path[0] != '/' && dirfd != L_AT_FDCWD) return l_ret(tf, -ENOTSUP);
  int rc = l_readlink_common(p, upath, l_arg(tf, 2), l_argi(tf, 3));
  return l_ret(tf, rc);
}

/* ---- signals ----------------------------------------------------------- */

/* THE STRUCT rt_sigaction TAKES IS NOT THE ONE IN <signal.h>.
 *
 * docs/linux-abi.md §6 pins a 152-byte struct sigaction (handler 0, mask
 * 8..135, flags 136, restorer 144).  That is the USERSPACE shape a C program
 * declares; glibc and musl both CONVERT it to a 32-byte kernel struct in their
 * sigaction() wrapper before the syscall.  The raw ABI is:
 *
 *     sa_handler    0   (8 bytes)
 *     sa_flags      8   (8 bytes, unsigned long)
 *     sa_restorer  16   (8 bytes)
 *     sa_mask      24   (8 bytes when the caller's sigsetsize is 8; the
 *                        kernel writes exactly 32 bytes back into oldact)
 *
 * MEASURED, twice: on this workstation a raw rt_sigaction query after a libc
 * install returns {handler, 0x04000000 (SA_RESTORER), <code ptr>, 0} at
 * 0/8/16/24 with bytes 32+ untouched; on the guest, decoding the 152-byte
 * shape put 0x800000 (stack garbage) in sa_restorer and every handler install
 * failed EINVAL, which is exactly the failure libX11 reads as "no signals".
 * So: read and write the 32-byte form, and never fetch more than 32 bytes
 * (a caller's oldact is a 32-byte object; writing 152 would smash its stack).
 */
struct l_kernel_sigaction {
  uint64_t sa_handler;
  uint64_t sa_flags;
  uint64_t sa_restorer;
  uint64_t sa_mask;
};
typedef char l_ksa_size_assert[sizeof(struct l_kernel_sigaction) == 32 ? 1 : -1];

static int l_proc_rt_sigaction(struct process *p, struct trap_frame *tf) {
  int sig = (int)l_argi(tf, 0);
  uint64_t uact = l_arg(tf, 1);
  uint64_t uold = l_arg(tf, 2);
  uint64_t setsize = l_arg(tf, 3);
  /* The kernel only ever has an 8-byte sigset; musl passes _NSIG/8 == 8.  A
     different width is a caller bug and Linux answers EINVAL. */
  if (setsize != 8) return l_ret(tf, -EINVAL);
  if (l_slot(p) < 0) return l_ret(tf, -EINVAL);

  struct l_kernel_sigaction ka;
  struct ho_sigaction ha;
  struct ho_sigaction ho;
  l_zero(&ha, sizeof ha);
  l_zero(&ho, sizeof ho);
  int have_act = 0;
  if (uact) {
    if (l_copy_from_user(p, &ka, uact, sizeof ka) != 0) return l_ret(tf, -EFAULT);
    have_act = 1;
    ha.sa_handler = ka.sa_handler;
    ha.sa_mask[0] = ka.sa_mask;
    ha.sa_mask[1] = 0;
    ha.sa_flags = (int)(uint32_t)ka.sa_flags;
    ha.sa_restorer = ka.sa_restorer;
    /* A caller that installed a real handler without SA_RESTORER expects the
       KERNEL's trampoline (Linux has one).  We have none to hand out, so fall
       back to the restorer already stored for this group — musl always sets
       one, which is why the fallback is never exercised by the target set. */
    if (ha.sa_handler > 1 && ha.sa_restorer == 0) {
      struct process *grp = process_group(p);
      if (grp) ha.sa_restorer = grp->sig_restorer;
    }
  }
  int rc = process_sigaction(sig, have_act ? &ha : 0, uold ? &ho : 0);
  if (rc < 0) {
    /* Name the wall once instead of leaving a bare EINVAL in the guest's
       errno: the native path refuses a real handler whose restorer is not
       readable (or a signal outside the frozen set).  libX11 treats a failing
       sigaction as "this kernel cannot do signals". */
    if (rc == -EINVAL && have_act && ha.sa_handler > 1 && l_sa_diag < 4) {
      l_sa_diag++;
      struct process *grp = process_group(p);
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] sigaction refused sig=");
      kl_i(&kl, (long)sig);
      kl_s(&kl, " handler=");
      kl_hex(&kl, (unsigned long)ha.sa_handler);
      kl_s(&kl, " restorer=");
      kl_hex(&kl, (unsigned long)ha.sa_restorer);
      kl_s(&kl, " restorer_ok=");
      kl_i(&kl, grp ? (long)process_user_ok(grp, ha.sa_restorer, 4, 0) : -1);
      kl_s(&kl, "\n");
      kl_emit(&kl);
    }
    return l_ret(tf, rc);
  }
  /* Record the flag word: the native PCB keeps handler/mask/restorer but not
     flags, and the compat frame engine needs SA_ONSTACK/SA_SIGINFO. */
  if (have_act) {
    int slot = l_slot(process_group(p));
    if (slot >= 0 && sig > 0 && sig < HO_SIG_MAX)
      l_sigflags[slot][sig] = (uint32_t)ka.sa_flags;
  }
  if (uold) {
    /* Exactly the 32 bytes the ABI defines — see the layout note above. */
    struct l_kernel_sigaction ko;
    l_zero(&ko, sizeof ko);
    ko.sa_handler = ho.sa_handler;
    ko.sa_flags = (uint64_t)(uint32_t)ho.sa_flags;
    ko.sa_restorer = ho.sa_restorer;
    ko.sa_mask = ho.sa_mask[0];
    if (l_copy_to_user(p, uold, &ko, sizeof ko) != 0) return l_ret(tf, -EFAULT);
  }
  return l_ret(tf, 0);
}

/* rt_sigprocmask(how, set, oldset, sigsetsize): the 8-byte kernel sigset.
   SIGKILL/SIGSTOP cannot be blocked; Linux drops those bits silently rather
   than failing, and so do we. */
static int l_proc_rt_sigprocmask(struct process *p, struct trap_frame *tf) {
  int how = (int)l_argi(tf, 0);
  uint64_t uset = l_arg(tf, 1);
  uint64_t uold = l_arg(tf, 2);
  uint64_t setsize = l_arg(tf, 3);
  if (setsize != 8) return l_ret(tf, -EINVAL);
  int slot = l_slot(p);
  if (slot < 0) return l_ret(tf, -EINVAL);
  if (uset) {
    uint64_t set;
    if (l_copy_from_user(p, &set, uset, sizeof set) != 0) return l_ret(tf, -EFAULT);
    uint64_t old = l_sigmask[slot];
    if (how == L_SIG_BLOCK) old |= set;
    else if (how == L_SIG_UNBLOCK) old &= ~set;
    else if (how == L_SIG_SETMASK) old = set;
    else return l_ret(tf, -EINVAL);
    /* bit (signo-1): SIGKILL 9, SIGSTOP 19 */
    old &= ~((1ull << (L_SIGKILL - 1)) | (1ull << (L_SIGSTOP - 1)));
    l_sigmask[slot] = old;
  } else if (how != L_SIG_BLOCK && how != L_SIG_UNBLOCK && how != L_SIG_SETMASK) {
    return l_ret(tf, -EINVAL);
  }
  if (uold) {
    uint64_t cur = l_sigmask[slot];
    if (l_copy_to_user(p, uold, &cur, sizeof cur) != 0) return l_ret(tf, -EFAULT);
  }
  return l_ret(tf, 0);
}

/* rt_sigreturn: the guest's restorer has just executed `syscall` with rax=15.
   The active frame is the NATIVE HOS3 frame planted by
   process_signal_deliver(), so the native restore is exactly right — and it
   writes the restored rax itself, so this handler must NOT touch regs[0]
   afterwards (doing so clobbers the interrupted syscall's return value).
   process_sigreturn() writes -EINVAL into regs[0] on a stale call and the
   process continues, which is the documented defensive behaviour. */
static int l_proc_rt_sigreturn(struct process *p, struct trap_frame *tf) {
  /* Compat processes are entered through a Linux x86-64 rt_sigframe (built by
     linux_compat_thr.c), not the native HOS3 frame: the guest's restorer is
     glibc's __restore_rt and its handler reads a Linux ucontext_t.  The
     native restore would read the Linux frame as an HOS3 one. */
  extern int linux_compat_sigreturn(struct process *p, struct trap_frame *tf);
  return linux_compat_sigreturn(p, tf);
}

/* sigaltstack(ss, old_ss).  Stored and reported faithfully; the delivery
   engine always builds its frame on the normal stack (SA_ONSTACK is not
   honoured by the native engine — see the file header). */
static int l_proc_sigaltstack(struct process *p, struct trap_frame *tf) {
  uint64_t uss = l_arg(tf, 0);
  uint64_t uold = l_arg(tf, 1);
  int slot = l_slot(p);
  if (slot < 0) return l_ret(tf, -EINVAL);
  if (uss) {
    linux_stack_t st;
    if (l_copy_from_user(p, &st, uss, sizeof st) != 0) return l_ret(tf, -EFAULT);
    /* Only SS_DISABLE / SS_ONSTACK(->refused) / SS_AUTODISARM(ignored) exist. */
    if ((uint32_t)st.ss_flags & ~(uint32_t)(1 | 2 | 0x80000000u))
      return l_ret(tf, -EINVAL);
    if (st.ss_flags & 1) return l_ret(tf, -EINVAL); /* SS_ONSTACK on input */
    l_altstack[slot].ss_flags = (st.ss_flags & 2) ? 2 : 0;
    if (l_altstack[slot].ss_flags == 2) {
      l_altstack[slot].ss_sp = 0;
      l_altstack[slot].ss_size = 0;
    } else {
      l_altstack[slot].ss_sp = (uint64_t)(uintptr_t)st.ss_sp;
      l_altstack[slot].ss_size = st.ss_size;
    }
    l_altstack[slot].in_use = 1;
  }
  if (uold) {
    linux_stack_t st;
    l_zero(&st, sizeof st);
    if (l_altstack[slot].in_use) {
      st.ss_sp = (void *)(uintptr_t)l_altstack[slot].ss_sp;
      st.ss_flags = l_altstack[slot].ss_flags;
      st.ss_size = l_altstack[slot].ss_size;
    } else {
      st.ss_flags = 2; /* SS_DISABLE: nothing installed */
    }
    if (l_copy_to_user(p, uold, &st, sizeof st) != 0) return l_ret(tf, -EFAULT);
  }
  return l_ret(tf, 0);
}

/* kill(pid, sig) / tgkill(tgid, tid, sig).  sig == 0 is the existence probe
   (no signal); everything else goes through the native process_signal(), so
   dispositions and the default action are the same ones SIGTEST.BIN
   exercises.  If the default action just killed the CALLER's own group we must
   not return to a released image: schedule away (the native sys_kill does
   exactly this, and it never returns). */
static int l_signal_one(struct trap_frame *tf, int pid, int sig) {
  if (sig == 0) {
    /* The existence probe.  pid 0 means the caller's own group and a negative
       pid names the group -pid; neither sends anything, so resolve them to the
       process whose liveness is being asked about instead of treating pid 0 as
       "the pid 0 process" (which would answer about the idle task). */
    int probe = pid;
    if (probe == 0) return l_ret(tf, current_process() ? 0 : -ESRCH);
    if (probe < 0) probe = -probe;
    struct process *t = process_get_pcb(probe);
    if (t && t->state != PROC_STATE_FREE && t->state != PROC_STATE_EXITED &&
        t->state != PROC_STATE_THREAD_DONE)
      return l_ret(tf, 0);
    return l_ret(tf, -ESRCH);
  }
  if (sig < 0 || sig >= HO_SIG_MAX) return l_ret(tf, -EINVAL);
  int rc = process_signal(pid, sig);
  struct process *cur = current_process();
  if (cur && process_group(cur)->state == PROC_STATE_EXITED) {
    tf->regs[0] = (uint64_t)(int64_t)rc;
    schedule(tf, 0);
    return 1; /* not reached: schedule() never returns here */
  }
  return l_ret(tf, rc);
}

static int l_proc_kill(struct process *p, struct trap_frame *tf) {
  (void)p;
  return l_signal_one(tf, (int)l_argi(tf, 0), (int)l_argi(tf, 1));
}

/* tkill(tid, sig): the single-thread form musl's raise()/abort() actually
   use (measured: an ENOSYS on 200 makes raise() a silent no-op). */
static int l_proc_tkill(struct process *p, struct trap_frame *tf) {
  (void)p;
  int tid = (int)l_argi(tf, 0);
  int sig = (int)l_argi(tf, 1);
  if (tid <= 0) return l_ret(tf, -EINVAL);
  return l_signal_one(tf, tid, sig);
}

static int l_proc_tgkill(struct process *p, struct trap_frame *tf) {
  (void)p;
  int tid = (int)l_argi(tf, 1);
  int sig = (int)l_argi(tf, 2);
  if (tid <= 0) return l_ret(tf, -EINVAL);
  return l_signal_one(tf, tid, sig);
}

/* ---- resource limits --------------------------------------------------- */

#define L_RLIMIT_CPU 0
#define L_RLIMIT_FSIZE 1
#define L_RLIMIT_DATA 2
#define L_RLIMIT_CORE 4

/* Every limit this kernel can state truthfully.  RLIMIT_NOFILE is the fd
   table (64, MAX_OPEN_FDS); RLIMIT_STACK is the 8 MiB demand-paged main stack
   (vm.h USER_MAIN_STK_SIZE).  The two are hard limits, so cur == max.  A
   resource the kernel does not model reads back as RLIM_INFINITY except
   RLIMIT_CORE, which is genuinely 0 (no core dumps) — the alternative was
   inventing numbers, and a program that sizes a buffer from a made-up
   RLIMIT_NOFILE would fail somewhere unrelated. */
static int l_rlimit_get(int resource, linux_rlimit_t *rl) {
  switch (resource) {
  case L_RLIMIT_NOFILE:
    rl->rlim_cur = rl->rlim_max = (uint64_t)MAX_OPEN_FDS;
    return 0;
  case L_RLIMIT_STACK:
    rl->rlim_cur = rl->rlim_max = (uint64_t)USER_MAIN_STK_SIZE;
    return 0;
  case L_RLIMIT_CORE:
    rl->rlim_cur = rl->rlim_max = 0;
    return 0;
  case L_RLIMIT_CPU:
  case L_RLIMIT_FSIZE:
  case L_RLIMIT_DATA:
    rl->rlim_cur = rl->rlim_max = L_RLIM_INFINITY;
    return 0;
  default:
    if (resource >= 0 && resource < 16) {
      rl->rlim_cur = rl->rlim_max = L_RLIM_INFINITY;
      return 0;
    }
    return -EINVAL;
  }
}

/* setrlimit is ACCEPTED and not enforced (nothing in the kernel enforces a
   limit today; the fd table and stack are fixed).  Returning 0 keeps a program
   that lowers its own soft limit working; returning EPERM would break a
   perfectly reasonable caller for a limit it cannot observe. */
static int l_proc_setrlimit(struct process *p, struct trap_frame *tf) {
  (void)p;
  int resource = (int)l_argi(tf, 0);
  uint64_t url = l_arg(tf, 1);
  if (!url) return l_ret(tf, -EFAULT);
  linux_rlimit_t rl;
  if (l_copy_from_user(p, &rl, url, sizeof rl) != 0) return l_ret(tf, -EFAULT);
  linux_rlimit_t cur;
  int rc = l_rlimit_get(resource, &cur);
  if (rc != 0) return l_ret(tf, rc);
  return l_ret(tf, 0);
}

static int l_proc_getrlimit(struct process *p, struct trap_frame *tf) {
  int resource = (int)l_argi(tf, 0);
  uint64_t url = l_arg(tf, 1);
  if (!url) return l_ret(tf, -EFAULT);
  linux_rlimit_t rl;
  int rc = l_rlimit_get(resource, &rl);
  if (rc != 0) return l_ret(tf, rc);
  if (l_copy_to_user(p, url, &rl, sizeof rl) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* prlimit64(pid, resource, new, old).  pid 0 or self only: cross-process limit
   manipulation would need a permission model this kernel does not have, and
   -EPERM is the honest answer for another live pid. */
static int l_proc_prlimit64(struct process *p, struct trap_frame *tf) {
  int pid = (int)l_argi(tf, 0);
  int resource = (int)l_argi(tf, 1);
  uint64_t unew = l_arg(tf, 2);
  uint64_t uold = l_arg(tf, 3);
  int self = p ? p->pid : -1;
  if (pid != 0 && pid != self) {
    struct process *t = process_get_pcb(pid);
    if (!t || t->state == PROC_STATE_FREE || t->state == PROC_STATE_EXITED)
      return l_ret(tf, -ESRCH);
    return l_ret(tf, -EPERM);
  }
  linux_rlimit_t rl;
  int rc = l_rlimit_get(resource, &rl);
  if (rc != 0) return l_ret(tf, rc);
  if (uold) {
    if (l_copy_to_user(p, uold, &rl, sizeof rl) != 0) return l_ret(tf, -EFAULT);
  }
  if (unew) {
    linux_rlimit_t nr;
    if (l_copy_from_user(p, &nr, unew, sizeof nr) != 0) return l_ret(tf, -EFAULT);
    /* accepted, not enforced — see l_proc_setrlimit */
  }
  return l_ret(tf, 0);
}

/* ---- sysinfo / times / getrusage / umask ------------------------------- */

/* Linux's struct sysinfo is 112 bytes: uptime @0, loads[3] @8, totalram @32,
   freeram @40, sharedram @48, bufferram @56, totalswap @64, freeswap @72,
   procs (u16) @80, pad @82, totalhigh @88, freehigh @96, mem_unit (u32) @104,
   then 4 bytes of trailing padding.  mem_unit = 1 says "every size is in
   bytes", which is what we fill. */
struct l_proc_sysinfo {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint16_t pad;
  uint32_t pad2;
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
  uint32_t _f;
};
typedef char l_proc_sysinfo_size_assert[sizeof(struct l_proc_sysinfo) == 112 ? 1 : -1];

/* Scratch for the live-process count (see l_proc_sysinfo): static so a sysinfo
   call never puts 2.8 KiB on the syscall stack. */
static struct sys_procinfo l_sysinfo_procs[MAX_PROCESSES];
static spinlock_t l_sysinfo_lock;

static int l_proc_sysinfo(struct process *p, struct trap_frame *tf) {
  uint64_t usi = l_arg(tf, 0);
  if (!usi) return l_ret(tf, -EFAULT);
  struct l_proc_sysinfo si;
  l_zero(&si, sizeof si);
  si.uptime = (int64_t)(timer_get_ms() / 1000);
  /* Physical memory in 32 MiB blocks (frame.h FRAME_BLOCK_SIZE). */
  uint64_t blk = (uint64_t)FRAME_BLOCK_SIZE;
  int total = process_get_total_blocks();
  int freeb = phys_block_free_count();
  si.totalram = (total > 0 ? (uint64_t)total : 0) * blk;
  si.freeram = (freeb > 0 ? (uint64_t)freeb : 0) * blk;
  /* The live-process count.  A static buffer (64 * 44 B of BSS) rather than
     2.8 KiB of syscall stack; the lock is a leaf, and process_get_info_list
     takes proc_lock itself. */
  uint64_t pflags = spinlock_acquire_irqsave(&l_sysinfo_lock);
  int n = process_get_info_list(l_sysinfo_procs, MAX_PROCESSES);
  uint16_t procs = (uint16_t)(n < 0 ? 0 : n);
  spinlock_release_irqrestore(&l_sysinfo_lock, pflags);
  si.procs = procs;
  /* Load averages are not tracked: 0 is "no information", which is what a
     program reading /proc/loadavg tolerates; inventing a value would not. */
  si.mem_unit = 1;
  if (l_copy_to_user(p, usi, &si, sizeof si) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* times(2): returns the uptime in CLK_TCK ticks (100 Hz, the Linux x86_64
   value) and fills struct tms = 4 clock_t (32 bytes).  There is no per-process
   CPU accounting in this kernel, so the four counters are honestly 0; the
   return value — the only part anything in the target set reads — is real. */
static int l_proc_times(struct process *p, struct trap_frame *tf) {
  uint64_t utms = l_arg(tf, 0);
  if (utms) {
    int64_t tms[4];
    l_zero(tms, sizeof tms);
    if (l_copy_to_user(p, utms, tms, sizeof tms) != 0) return l_ret(tf, -EFAULT);
  }
  tf->regs[0] = (uint64_t)(int64_t)(timer_get_ms() / (1000 / L_CLK_TCK));
  return 1;
}

/* getrusage(who, struct rusage *) — 144 bytes, all-zero except the two
   timevals, which are 0 for the same reason times()' counters are.  who must
   be one of SELF / CHILDREN / THREAD. */
static int l_proc_getrusage(struct process *p, struct trap_frame *tf) {
  int who = (int)l_argi(tf, 0);
  uint64_t uru = l_arg(tf, 1);
  if (who != L_RUSAGE_SELF && who != L_RUSAGE_CHILDREN && who != 1)
    return l_ret(tf, -EINVAL);
  if (!uru) return l_ret(tf, -EFAULT);
  if (!l_user_ok(p, uru, sizeof(linux_rusage_t), 1)) return l_ret(tf, -EFAULT);
  uint8_t blob[144];
  l_zero(blob, sizeof blob);
  if (l_copy_to_user(p, uru, blob, sizeof blob) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* umask(mask): set and return the previous value (mode bits only). */
static int l_proc_umask(struct process *p, struct trap_frame *tf) {
  (void)p;
  uint32_t old = l_umask;
  l_umask = (uint32_t)l_argi(tf, 0) & 0777u;
  return l_ret(tf, (int)old);
}

/* ---- access / faccessat ------------------------------------------------ */

/* Every compat process is uid 0, so R_OK/W_OK always pass for an existing
   path; X_OK needs an execute bit, which a FAT16 file (mode 0644) does not
   have.  That is the same answer stat() gives, and the two must agree. */
static int l_access_check(struct process *p, uint64_t upath, int mode) {
  if (mode & ~7) return -EINVAL;
  char path[256];
  int r = l_copy_cstr(p, path, upath, sizeof path);
  if (r != 0) return r;
  struct k_stat ks;
  int err = 0;
  if (file_stat_path(p, path, &ks, &err) != 0) return -(err ? err : EIO);
  if ((mode & 1) && !(ks.st_mode & 0111)) return -EACCES;
  return 0;
}

static int l_proc_access(struct process *p, struct trap_frame *tf) {
  return l_ret(tf, l_access_check(p, l_arg(tf, 0), (int)l_argi(tf, 1)));
}

static int l_proc_faccessat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  uint64_t upath = l_arg(tf, 1);
  int mode = (int)l_argi(tf, 2);
  char path[256];
  int r = l_copy_cstr(p, path, upath, sizeof path);
  if (r != 0) return l_ret(tf, r);
  if (path[0] != '/' && dirfd != L_AT_FDCWD) return l_ret(tf, -ENOTSUP);
  return l_ret(tf, l_access_check(p, upath, mode));
}

/* ---- identity: prctl, nice, job control, affinity ----------------------- */

/* Resolve a pid argument the way Linux's getpgid/getsid/prlimit do: 0 is the
 * caller, a pid that names no live process is -ESRCH. */
static struct process *l_pcb_of(struct process *p, int pid, int *err) {
  *err = 0;
  if (pid == 0) return p;
  if (p && pid == p->pid) return p;
  struct process *t = process_get_pcb(pid);
  if (!t || t->state == PROC_STATE_FREE || t->state == PROC_STATE_EXITED) {
    *err = -ESRCH;
    return 0;
  }
  return t;
}

/* prctl(option, a2, a3, a4, a5).  glibc's pthread_create sets the 16-byte comm
 * (PR_SET_NAME) and reads it back for /proc/self/task/<tid>/comm-shaped
 * diagnostics; Mono's crash reporter asks for dumpability.  The answered set is
 * exactly the one the task/plan names:
 *
 *   PR_SET_NAME / PR_GET_NAME   round-trip through p->name, the native PCB's own
 *                               32-byte name.  Linux TRUNCATES a longer comm to
 *                               15 characters (16 bytes including NUL) rather
 *                               than refusing it — copied faithfully.  NOTE:
 *                               p->name is also what /proc/self/exe is
 *                               synthesized from (l_readlink_target), so naming
 *                               the MAIN process changes that path too; the
 *                               common caller, pthread_setname_np on a worker,
 *                               renames a thread PCB and leaves it alone.
 *   PR_SET_DUMPABLE             0 (accepted, recorded nowhere).  HobbyOS has no
 *                               core-dump path and no ptrace, so the flag has
 *                               nothing to gate; PR_GET_DUMPABLE reports the
 *                               default 1 a process that never set it reads.
 *   PR_SET_PDEATHSIG            0 (clearing a hook that was never armed) is
 *                               trivially true; a NON-ZERO signal is -ENOSYS,
 *                               because the PCB has no field and the reaper is
 *                               the parent's waitpid — promising a delivery
 *                               that can never happen would be worse.
 *   PR_GET_PDEATHSIG            writes 0 (nothing is ever armed).
 *   anything else               -EINVAL, which is Linux's answer for an option
 *                               the kernel does not know.
 */
static int l_proc_prctl(struct process *p, struct trap_frame *tf) {
  int op = (int)l_argi(tf, 0);
  uint64_t a2 = l_arg(tf, 1);

  switch (op) {
  case L_PR_SET_NAME: {
      char name[L_PR_NAME_LEN];
      int r = l_copy_cstr(p, name, a2, sizeof name);
      if (r == -EFAULT) return l_ret(tf, r);
      if (r != 0) {
        /* No NUL inside 16 bytes: Linux's truncation case, not an error. Read
           the 15 characters it would keep and terminate them here. */
        if (l_copy_from_user(p, name, a2, L_PR_NAME_LEN - 1) != 0)
          return l_ret(tf, -EFAULT);
        name[L_PR_NAME_LEN - 1] = '\0';
      }
      if (!p) return l_ret(tf, -ESRCH);
      for (int i = 0; i < (int)sizeof p->name; i++) p->name[i] = '\0';
      for (int i = 0; i < L_PR_NAME_LEN - 1 && name[i]; i++) p->name[i] = name[i];
      return l_ret(tf, 0);
    }
  case L_PR_GET_NAME: {
      char out[L_PR_NAME_LEN];
      l_zero(out, sizeof out);
      if (p)
        for (int i = 0; i < L_PR_NAME_LEN - 1 && p->name[i]; i++)
          out[i] = p->name[i];
      if (l_copy_to_user(p, a2, out, sizeof out) != 0) return l_ret(tf, -EFAULT);
      return l_ret(tf, 0);
    }
  case L_PR_SET_DUMPABLE:
    return l_ret(tf, 0);
  case L_PR_GET_DUMPABLE:
    return l_ret(tf, 1);
  case L_PR_SET_PDEATHSIG:
    return l_ret(tf, (int)l_argi(tf, 1) == 0 ? 0 : -ENOSYS);
  case L_PR_GET_PDEATHSIG: {
      int zero = 0;
      if (l_copy_to_user(p, a2, &zero, sizeof zero) != 0)
        return l_ret(tf, -EFAULT);
      return l_ret(tf, 0);
    }
  default:
    return l_ret(tf, -EINVAL);
  }
}

/* getpriority(which, who) / setpriority(which, who, prio).
 *
 * The native scheduler has ONE priority class.  The truthful answers are:
 *   - getpriority reports nice 0 for everything.  The RAW syscall returns
 *     20 - nice (range 1..40) so that a -1 return stays unambiguous, and every
 *     libc wrapper subtracts 20 again; L_PRIO_BASE is that 20.
 *   - setpriority ACCEPTS a non-negative nice (a request to be scheduled less
 *     aggressively): the one-class round-robin already treats every process
 *     equally, so running at default priority is never a correctness failure,
 *     and refusing would abort a program that only wanted to be polite.
 *     A NEGATIVE nice (raise priority above default) is -EPERM, which is also
 *     what unprivileged Linux answers: this kernel has no priority to raise
 *     and no capability model to grant one.
 * which/who are still validated (EINVAL / ESRCH) so a typo cannot read as a
 * real answer. */
static int l_proc_getpriority(struct process *p, struct trap_frame *tf) {
  int which = (int)l_argi(tf, 0);
  int who = (int)l_argi(tf, 1);
  int err = 0;

  if (which < L_PRIO_PROCESS || which > L_PRIO_USER) return l_ret(tf, -EINVAL);
  if (which == L_PRIO_USER) {
    /* Every compat process is uid 0; any other uid owns nothing here. */
    if (who != 0) return l_ret(tf, -ESRCH);
    return l_ret(tf, L_PRIO_BASE);
  }
  /* PRIO_PGRP's `who` is a PGID, and a pgid is the anchor's pid here, so the
     same resolver answers both.  The value is identical for every target. */
  (void)l_pcb_of(p, who, &err);
  if (err != 0) return l_ret(tf, err);
  return l_ret(tf, L_PRIO_BASE);
}

static int l_proc_setpriority(struct process *p, struct trap_frame *tf) {
  int which = (int)l_argi(tf, 0);
  int who = (int)l_argi(tf, 1);
  int prio = (int)l_argi(tf, 2);
  int err = 0;

  if (which < L_PRIO_PROCESS || which > L_PRIO_USER) return l_ret(tf, -EINVAL);
  if (prio < L_NICE_MIN) prio = L_NICE_MIN;
  if (prio > L_NICE_MAX) prio = L_NICE_MAX;
  if (which == L_PRIO_USER) {
    if (who != 0) return l_ret(tf, -ESRCH);
  } else {
    (void)l_pcb_of(p, who, &err);
    if (err != 0) return l_ret(tf, err);
  }
  if (prio < 0) return l_ret(tf, -EPERM);
  return l_ret(tf, 0);
}

/* setpgid / getpgid / setsid / getsid.
 *
 * There is no pgid or session FIELD in the PCB, and this lane may not add one
 * (process.h belongs to the process lane).  What exists is the group ANCHOR:
 * process_group() maps a thread to its leader, and process.c sets tgid = pid at
 * creation, so every process is its own anchor.  Translating onto that gives
 * answers that are true of this kernel rather than invented:
 *
 *   getpgid / getsid   the anchor's pid (session == group here; documented).
 *   setpgid(0,0)       "be your own group leader" — already true, success.
 *   setpgid(_, other)  joining another group is not expressible: -EPERM, which
 *                      is also what Linux answers for a cross-session join.
 *   setsid             -EPERM: Linux refuses exactly when the caller is already
 *                      a group leader, which every HobbyOS process is.
 *
 * A caller that needs real sessions (a daemon double-fork) will see the EPERM
 * and the file header's note; nothing silently pretends the session changed. */
static int l_proc_getpgid(struct process *p, struct trap_frame *tf) {
  int err = 0;
  struct process *t = l_pcb_of(p, (int)l_argi(tf, 0), &err);
  if (err != 0 || !t) return l_ret(tf, err ? err : -ESRCH);
  return l_ret(tf, process_group(t)->pid);
}

static int l_proc_getsid(struct process *p, struct trap_frame *tf) {
  return l_proc_getpgid(p, tf); /* no session layer: sid == group anchor */
}

static int l_proc_setpgid(struct process *p, struct trap_frame *tf) {
  int pgid = (int)l_argi(tf, 1);
  int err = 0;
  struct process *t = l_pcb_of(p, (int)l_argi(tf, 0), &err);
  if (err != 0 || !t) return l_ret(tf, err ? err : -ESRCH);
  if (pgid < 0) return l_ret(tf, -EINVAL);
  if (t != p) return l_ret(tf, -EPERM);
  if (pgid == 0 || pgid == p->pid) return l_ret(tf, 0);
  return l_ret(tf, -EPERM);
}

static int l_proc_setsid(struct process *p, struct trap_frame *tf) {
  (void)p;
  (void)tf;
  return l_ret(tf, -EPERM);
}

/* sched_getaffinity(pid, len, mask) / sched_setaffinity(pid, len, mask).
 *
 * THE SHAPE: a caller passes a 128-byte cpu_set_t (CPU_SETSIZE = 1024 bits),
 * x86_64's kernel mask is one 8-byte word (nr_cpu_ids <= 64), and the syscall
 * RETURNS THE NUMBER OF BYTES WRITTEN (8) — not 0.  len < 8 is -EINVAL, not a
 * short write.
 *
 * DEVIATION, deliberate and measured against glibc: Linux leaves the guest
 * bytes past the 8 it wrote UNTOUCHED, and glibc's sched_getaffinity does not
 * zero the cpu_set_t first, so a caller that CPU_COUNTs its full 128-byte set
 * counts its own stack garbage on a real kernel too.  We zero every byte the
 * caller declared, which makes CPU_COUNT deterministic; the bits at and above
 * the online CPU count stay 0 either way.
 *
 * setaffinity is ACCEPTED, NOT ENFORCED: the native scheduler runs any ready
 * process on whichever CPU idles first and has no per-process mask (adding one
 * is scheduler work outside this lane's files).  Success is safe because
 * affinity is a performance hint; a caller that verifies with getaffinity sees
 * the online set — the same thing it asked for in every program that pins to
 * "all CPUs" and a superset for one that pins to a subset. */
static int l_proc_sched_getaffinity(struct process *p, struct trap_frame *tf) {
  int pid = (int)l_argi(tf, 0);
  uint64_t len = l_arg(tf, 1);
  uint64_t umask = l_arg(tf, 2);
  int err = 0;

  if (len < L_CPUMASK_KERNEL_BYTES) return l_ret(tf, -EINVAL);
  if (!umask) return l_ret(tf, -EFAULT);
  (void)l_pcb_of(p, pid, &err);
  if (err != 0) return l_ret(tf, err);

  uint8_t buf[L_CPUMASK_BYTES];
  l_zero(buf, sizeof buf);
  /* smp_init_count() is the boot-time truth ("cores that ran
     trap_init_core_with_id", ids 0..n-1).  process_get_num_cpus() counts
     cores that have entered the IDLE loop, which lags AP bring-up and made a
     probe started early report a ONE-CPU machine; it is only the fallback. */
  int ncpu = (int)smp_init_count();
  if (ncpu <= 0) ncpu = process_get_num_cpus();
  if (ncpu < 1) ncpu = 1;
  if (ncpu > L_CPUMASK_MAX_CPUS) ncpu = L_CPUMASK_MAX_CPUS;
  for (int i = 0; i < ncpu; i++) buf[i / 8] |= (uint8_t)(1u << (i % 8));

  uint64_t n = len > sizeof buf ? (uint64_t)sizeof buf : len;
  if (l_copy_to_user(p, umask, buf, n) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, L_CPUMASK_KERNEL_BYTES);
}

static int l_proc_sched_setaffinity(struct process *p, struct trap_frame *tf) {
  int pid = (int)l_argi(tf, 0);
  uint64_t len = l_arg(tf, 1);
  uint64_t umask = l_arg(tf, 2);
  int err = 0;

  /* Linux wants len to be a multiple of sizeof(long); anything smaller than
     the kernel mask is EINVAL. */
  if (len < L_CPUMASK_KERNEL_BYTES || (len & (L_CPUMASK_KERNEL_BYTES - 1)) != 0)
    return l_ret(tf, -EINVAL);
  if (!umask) return l_ret(tf, -EFAULT);
  uint64_t mask = 0;
  if (l_copy_from_user(p, &mask, umask, sizeof mask) != 0)
    return l_ret(tf, -EFAULT);
  if (mask == 0) return l_ret(tf, -EINVAL); /* an empty mask is not a CPU set */

  struct process *t = l_pcb_of(p, pid, &err);
  if (err != 0) return l_ret(tf, err);
  if (t != p) return l_ret(tf, -EPERM);
  return l_ret(tf, 0);
}

/* statx(332) — deliberately -ENOSYS.  glibc 2.31's stat()/fstatat() TRY statx
 * and, on ENOSYS, fall back to newfstatat (262), which this table implements
 * against the real FAT16 metadata.  Implementing statx would mean a second,
 * 256-byte layout and stx_mask/timestamp semantics to keep in sync with the
 * 144-byte one, for callers that already have a working path. */
static int l_proc_statx(struct process *p, struct trap_frame *tf) {
  (void)p;
  (void)tf;
  return l_ret(tf, -ENOSYS);
}

/* ---- the table --------------------------------------------------------- */


/* ---- timer resolution + scheduler queries + rt_sigtimedwait (Mono) ------- */

/* clock_getres(229).  The kernel's tick is 100 Hz, so 10 ms is the honest
 * answer for every clock; a NULL resolver only validates the clock id.
 * Mono sizes its timer wheels from this. */
static int l_proc_clock_getres(struct process *p, struct trap_frame *tf) {
  int clockid = (int)l_argi(tf, 0);
  uint64_t ures = l_arg(tf, 1);
  if (clockid < 0 || clockid > 11) return l_ret(tf, -EINVAL);
  if (!ures) return l_ret(tf, 0);
  struct { int64_t sec; int64_t nsec; } ts = {0, 10000000};
  if (l_copy_to_user(p, ures, &ts, sizeof ts) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* sched_getparam(143).  Every process is SCHED_OTHER, whose sched_priority is
 * 0 on Linux — this is an exact answer, not a stub. */
static int l_proc_sched_getparam(struct process *p, struct trap_frame *tf) {
  uint64_t uparam = l_arg(tf, 1);
  if (!uparam) return l_ret(tf, -EFAULT);
  int prio = 0;
  if (l_copy_to_user(p, uparam, &prio, sizeof prio) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* sched_getscheduler(145): SCHED_OTHER (0) for everyone. */
static int l_proc_sched_getscheduler(struct process *p, struct trap_frame *tf) {
  (void)p;
  return l_ret(tf, 0);
}

/* rt_sigtimedwait(137).  The compat class has no queued-signal model (the
 * header of process.c says so), so a blocking wait would have nothing to wake
 * it: report "nothing pending" with -EAGAIN and let the caller retry.  glibc's
 * sigwait loops tolerate it; Mono reaches thread creation and the managed
 * runtime with this answer (measured). */
static int l_proc_rt_sigtimedwait(struct process *p, struct trap_frame *tf) {
  (void)p;
  (void)tf;
  return l_ret(tf, -EAGAIN);
}


/* statfs(137).  The compat class's processes see one filesystem; report the
 * FAT page size and Linux's 255-byte name limit with free space taken from a
 * zeroed counter (Mono only asks to decide whether it can write a temp file).
 * Layout is the 120-byte x86-64 struct statfs. */
struct l_statfs {
  int64_t f_type, f_bsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  int64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static int l_proc_statfs(struct process *p, struct trap_frame *tf) {
  uint64_t upath = l_arg(tf, 0);
  uint64_t uout = l_arg(tf, 1);
  char path[VFS_PATH_MAX];
  if (l_copy_cstr(p, path, upath, sizeof path) != 0) return l_ret(tf, -EFAULT);
  if (!uout) return l_ret(tf, -EFAULT);
  struct l_statfs st;
  for (unsigned i = 0; i < sizeof st; i++) ((uint8_t *)&st)[i] = 0;
  st.f_type = 0x4d44;          /* MSDOS_SUPER_MAGIC */
  st.f_bsize = 4096;
  st.f_namelen = 255;
  st.f_frsize = 4096;
  if (l_copy_to_user(p, uout, &st, sizeof st) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* sched_get_priority_max/min(146/147): SCHED_OTHER has the single priority 0,
 * so Linux's answer for the default policy is 0 for both. */
static int l_proc_sched_prio_max(struct process *p, struct trap_frame *tf) {
  (void)p; (void)tf; return l_ret(tf, 0);
}
static int l_proc_sched_prio_min(struct process *p, struct trap_frame *tf) {
  (void)p; (void)tf; return l_ret(tf, 0);
}

/* sched_rr_get_interval(148): no SCHED_RR class exists, so the interval is 0. */
static int l_proc_sched_rr_interval(struct process *p, struct trap_frame *tf) {
  uint64_t uout = l_arg(tf, 1);
  struct { int64_t sec, nsec; } ts = {0, 0};
  if (!uout) return l_ret(tf, -EFAULT);
  if (l_copy_to_user(p, uout, &ts, sizeof ts) != 0) return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

const struct l_sysent l_tab_proc[] = { {L_SYS_rt_sigaction, l_proc_rt_sigaction, "rt_sigaction"}, {L_SYS_rt_sigprocmask, l_proc_rt_sigprocmask, "rt_sigprocmask"}, {L_SYS_rt_sigreturn, l_proc_rt_sigreturn, "rt_sigreturn"}, {L_SYS_sigaltstack, l_proc_sigaltstack, "sigaltstack"}, {L_SYS_kill, l_proc_kill, "kill"}, {L_SYS_tkill, l_proc_tkill, "tkill"}, {L_SYS_tgkill, l_proc_tgkill, "tgkill"}, {L_SYS_stat, l_proc_stat, "stat"}, {L_SYS_fstat, l_proc_fstat, "fstat"}, {L_SYS_lstat, l_proc_lstat, "lstat"}, {L_SYS_newfstatat, l_proc_newfstatat, "newfstatat"}, {L_SYS_statx, l_proc_statx, "statx"}, {L_SYS_readlink, l_proc_readlink, "readlink"}, {L_SYS_readlinkat, l_proc_readlinkat, "readlinkat"}, {L_SYS_getrlimit, l_proc_getrlimit, "getrlimit"}, {L_SYS_setrlimit, l_proc_setrlimit, "setrlimit"}, {L_SYS_prlimit64, l_proc_prlimit64, "prlimit64"}, {L_SYS_sysinfo, l_proc_sysinfo, "sysinfo"}, {L_SYS_times, l_proc_times, "times"}, {L_SYS_getrusage, l_proc_getrusage, "getrusage"}, {L_SYS_umask, l_proc_umask, "umask"}, {L_SYS_access, l_proc_access, "access"}, {L_SYS_faccessat, l_proc_faccessat, "faccessat"}, {L_SYS_prctl, l_proc_prctl, "prctl"}, {L_SYS_getpriority, l_proc_getpriority, "getpriority"}, {L_SYS_setpriority, l_proc_setpriority, "setpriority"}, {L_SYS_setpgid, l_proc_setpgid, "setpgid"}, {L_SYS_getpgid, l_proc_getpgid, "getpgid"}, {L_SYS_setsid, l_proc_setsid, "setsid"}, {L_SYS_getsid, l_proc_getsid, "getsid"}, {L_SYS_sched_getaffinity, l_proc_sched_getaffinity, "sched_getaffinity"}, {L_SYS_sched_setaffinity, l_proc_sched_setaffinity, "sched_setaffinity"},
  {L_SYS_clock_getres, l_proc_clock_getres, "clock_getres"},
  {L_SYS_sched_getparam, l_proc_sched_getparam, "sched_getparam"},
  {L_SYS_sched_getscheduler, l_proc_sched_getscheduler, "sched_getscheduler"},
  {L_SYS_rt_sigtimedwait, l_proc_rt_sigtimedwait, "rt_sigtimedwait"},
  {L_SYS_statfs, l_proc_statfs, "statfs"},
  {L_SYS_sched_get_priority_max, l_proc_sched_prio_max, "sched_get_priority_max"},
  {L_SYS_sched_get_priority_min, l_proc_sched_prio_min, "sched_get_priority_min"},
  {L_SYS_sched_rr_get_interval, l_proc_sched_rr_interval, "sched_rr_get_interval"},
  L_SYSENT_END
};
