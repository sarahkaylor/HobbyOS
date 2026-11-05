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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef RW_UNITYPLAYER_DEFAULT
#define RW_UNITYPLAYER_DEFAULT "/RWDSO/UnityPlayer.so"
#endif
#define RW_PLAYERMAIN "_Z10PlayerMainiPPc"

static void *(*next_dlopen)(const char *, int);
static void *(*next_dlsym)(void *, const char *);
static const char *dso_dir;
/* L48: threadpool accounting probe (defined before main). */
static void rw_tp_maybe_start(void *handle, const char *name);

/* Defined with the SIGSEGV dump below; refreshed after every dlopen so the
 * dump can tag a PC with <module>+offset (the handler must not walk the
 * module list itself: dl_iterate_phdr takes the loader lock, and the faults
 * worth dumping happen inside ld.so). */
static void rw_mods_collect(void);

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
    if (r) rw_mods_collect();
    if (!r) {
      /* Unity only says "Unable to load mono library"; the loader's reason
         (a missing NEEDED, an undefined symbol, a version) is the evidence. */
      const char *e = dlerror();
      fprintf(stderr, "[RWMAIN] dlopen FAILED %s: %s\n", local, e ? e : "(no dlerror)");
    }
    if (r) rw_tp_maybe_start(r, local);
    return r;
  }
  /* RW_TRACE_DLOPEN: every attempt and its result.  Unity/SDL disable a whole
     video backend silently when a dlopen() of libX11/libGL fails, and the
     engine's own log says only "Video subsystem has not been initialized";
     the failing soname and the loader's reason are the ladder.  Off by
     default; the M6 desktop harness turns it on. */
  int trace = getenv("RW_TRACE_DLOPEN") != 0;
  void *r = next_dlopen(file, mode);
  if (r) rw_mods_collect();
  if (r) rw_tp_maybe_start(r, file);
  if (!r) {
    const char *e = dlerror();
    fprintf(stderr, "[RWMAIN] dlopen FAILED %s: %s\n",
            file ? file : "(null)", e ? e : "(no dlerror)");
  } else if (trace) {
    /* The base address turns a runtime PC into a file offset: the M6 park's
       rip/stack words are otherwise unattributable (the guest has no
       /proc/self/maps).  RTLD_DI_LINKMAP gives l_addr, the load bias.
       M6/L30 also prints l_name and the (dev,ino) pair glibc uses to decide
       "this object is already loaded": two identical shims under different
       names MUST report distinct pairs, distinct handles and distinct bases. */
    struct link_map *lm = 0;
    unsigned long base = 0;
    if (dlinfo(r, RTLD_DI_LINKMAP, &lm) == 0 && lm) base = (unsigned long)lm->l_addr;
    unsigned long d0 = 0, i0 = 0;
    struct stat st;
    if (lm && lm->l_name && lm->l_name[0] && stat(lm->l_name, &st) == 0) {
      d0 = (unsigned long)st.st_dev;
      i0 = (unsigned long)st.st_ino;
    }
    fprintf(stderr, "[RWMAIN] dlopen ok %s -> %p base=0x%lx dev=%lu ino=%lu name=%s\n",
            file ? file : "(null)", r, base, d0, i0,
            (lm && lm->l_name && lm->l_name[0]) ? lm->l_name : "?");
  }
  return r;
}


/* ---- SIGSEGV/SIGBUS ucontext dump ---------------------------------------
 * M6/L29: after the window handshake the game dies on `memory fault VA=0
 * PC=0`, i.e. a jump through a NULL slot.  RIP=0 names the *target*, not the
 * caller, and a `jmp` pushes nothing, so the caller has to come from the
 * machine state: the ucontext's RIP/RSP, the GP registers (a dispatch-table
 * jump usually still has the table base and the index in rbx/rax/r??), and a
 * scan of the faulting stack for return addresses inside a mapped module.
 *
 * A signal handler on a broken stack must not call printf: deferred stdio
 * takes locks, and a fault inside it loses the dump.  Everything below is
 * write(2) over a hand-rolled hex/dec formatter, and every stack read is
 * probed with mincore() first (an unmapped stack page would fault inside the
 * handler and the kernel would kill the process before the row was flushed).
 *
 * RW_SIGDUMP=0 turns both handlers off; they are armed by default because a
 * launcher that dies at PC=0 has no other way to name its caller.
 */
#include <ucontext.h>
#include <sys/ucontext.h>
#include <sys/mman.h>

#define RW_MOD_MAX 16

struct rw_mod {
  unsigned long base, end;
  char name[56];
};
static struct rw_mod rw_mods[RW_MOD_MAX];
static int rw_mod_n;
static int rw_sigdumped;

static int rw_mods_cb(struct dl_phdr_info *info, size_t sz, void *data) {
  (void)sz; (void)data;
  if (rw_mod_n >= RW_MOD_MAX) return 1;
  unsigned long lo = ~0ul, hi = 0;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD) continue;
    unsigned long b = (unsigned long)info->dlpi_addr + ph->p_vaddr;
    if (b < lo) lo = b;
    if (b + ph->p_memsz > hi) hi = b + ph->p_memsz;
  }
  if (!hi) return 0;
  struct rw_mod *m = &rw_mods[rw_mod_n++];
  m->base = lo;
  m->end = hi;
  const char *nm = info->dlpi_name;
  if (!nm || !nm[0]) nm = "(main)";
  const char *sl = strrchr(nm, '/');
  if (sl) nm = sl + 1;
  size_t k = 0;
  for (; nm[k] && k < sizeof m->name - 1; k++) m->name[k] = nm[k];
  m->name[k] = 0;
  return 0;
}

static void rw_mods_collect(void) {
  rw_mod_n = 0;
  dl_iterate_phdr(rw_mods_cb, 0);
}

/* `<module>` when v is inside a mapped object, with its offset. */
static const char *rw_mod_of(unsigned long v, unsigned long *off) {
  for (int i = 0; i < rw_mod_n; i++)
    if (v >= rw_mods[i].base && v < rw_mods[i].end) {
      *off = v - rw_mods[i].base;
      return rw_mods[i].name;
    }
  return 0;
}

static int rw_readable(unsigned long a) {
  unsigned char vec[8];
  unsigned long page = a & ~0xFFFul;
  if (page < 0x10000ul) return 0;
  return mincore((void *)page, 4096, vec) == 0;
}

struct rb { char *start, *p, *e; };

static void rb_s(struct rb *b, const char *s) {
  while (*s && b->p < b->e) *b->p++ = *s++;
}
static void rb_c(struct rb *b, char c) {
  if (b->p < b->e) *b->p++ = c;
}
static void rb_hex(struct rb *b, unsigned long v) {
  char t[17];
  int n = 0;
  if (!v) { rb_c(b, '0'); return; }
  while (v) { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; }
  while (n) rb_c(b, t[--n]);
}
static void rb_dec(struct rb *b, long v) {
  char t[20];
  int n = 0, neg = 0;
  if (v < 0) { neg = 1; v = -v; }
  if (!v) { rb_c(b, '0'); return; }
  while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
  if (neg) rb_c(b, '-');
  while (n) rb_c(b, t[--n]);
}
static void rb_flush(struct rb *b) {
  if (b->p > b->start) {
    ssize_t w = write(2, b->start, (size_t)(b->p - b->start));
    (void)w;
  }
  b->p = b->start;
}
/* `name=0x<v>` plus `(<mod>+0x<off>)` when the value points into a module. */
static void rb_reg(struct rb *b, const char *name, unsigned long v, int tag) {
  rb_c(b, ' ');
  rb_s(b, name);
  rb_s(b, "=0x");
  rb_hex(b, v);
  if (tag) {
    unsigned long off = 0;
    const char *m = rw_mod_of(v, &off);
    if (m) {
      rb_c(b, '(');
      rb_s(b, m);
      rb_s(b, "+0x");
      rb_hex(b, off);
      rb_c(b, ')');
    }
  }
}

/* One dump per process; then the default action so the kernel's own
   `memory fault` row (and the exit status) still happen exactly once. */
static void rw_sig_handler(int sig, siginfo_t *si, void *ucv) {
  if (rw_sigdumped) {
    signal(sig, SIG_DFL);
    return;
  }
  rw_sigdumped = 1;
  ucontext_t *uc = (ucontext_t *)ucv;
  greg_t *g = uc->uc_mcontext.gregs;

  char buf[2048];
  struct rb b = {buf, buf, buf + sizeof buf - 8};

  rb_s(&b, "[SIGDUMP] ");
  rb_s(&b, sig == SIGSEGV ? "SIGSEGV" : (sig == SIGBUS ? "SIGBUS" : "SIGother"));
  rb_s(&b, " code=");
  rb_dec(&b, si ? si->si_code : -1);
  rb_s(&b, " addr=0x");
  rb_hex(&b, (unsigned long)(si ? si->si_addr : 0));
  rb_s(&b, "\n");
  rb_flush(&b);

  /* The four fields the ladder asked for first, each tagged with the module
     it points into (a dispatch-table base shows up as `U+0x...`). */
  rb_s(&b, "[SIGDUMP] uctx");
  rb_reg(&b, "rip", (unsigned long)g[REG_RIP], 1);
  rb_reg(&b, "rsp", (unsigned long)g[REG_RSP], 0);
  rb_reg(&b, "rax", (unsigned long)g[REG_RAX], 1);
  rb_reg(&b, "rdi", (unsigned long)g[REG_RDI], 1);
  rb_reg(&b, "rsi", (unsigned long)g[REG_RSI], 1);
  rb_reg(&b, "rdx", (unsigned long)g[REG_RDX], 1);
  rb_s(&b, "\n");
  rb_flush(&b);

  rb_s(&b, "[SIGDUMP] greg");
  rb_reg(&b, "rbx", (unsigned long)g[REG_RBX], 1);
  rb_reg(&b, "rcx", (unsigned long)g[REG_RCX], 1);
  rb_reg(&b, "rbp", (unsigned long)g[REG_RBP], 1);
  rb_reg(&b, "r8", (unsigned long)g[REG_R8], 1);
  rb_reg(&b, "r9", (unsigned long)g[REG_R9], 1);
  rb_reg(&b, "r10", (unsigned long)g[REG_R10], 1);
  rb_reg(&b, "r11", (unsigned long)g[REG_R11], 1);
  rb_reg(&b, "r12", (unsigned long)g[REG_R12], 1);
  rb_reg(&b, "r13", (unsigned long)g[REG_R13], 1);
  rb_reg(&b, "r14", (unsigned long)g[REG_R14], 1);
  rb_reg(&b, "r15", (unsigned long)g[REG_R15], 1);
  rb_s(&b, "\n");
  rb_flush(&b);

  /* The first four qwords behind each pointer-ish register: a dispatch table
     (`call *0x18(%r14)`) or a vtable, i.e. the slot that was NULL and the
     entries next to it, become visible without a second boot. */
  {
    struct { const char *n; unsigned long v; } rv[8] = {
      {"rax", (unsigned long)g[REG_RAX]}, {"rbx", (unsigned long)g[REG_RBX]},
      {"rcx", (unsigned long)g[REG_RCX]}, {"rdx", (unsigned long)g[REG_RDX]},
      {"rsi", (unsigned long)g[REG_RSI]}, {"rdi", (unsigned long)g[REG_RDI]},
      {"r14", (unsigned long)g[REG_R14]}, {"r15", (unsigned long)g[REG_R15]},
    };
    rb_s(&b, "[SIGDUMP] mem");
    for (int i = 0; i < 8; i++) {
      unsigned long v = rv[i].v;
      if (!v || v >= 0x800000000000ul || !rw_readable(v)) continue;
      rb_c(&b, ' ');
      rb_s(&b, rv[i].n);
      rb_c(&b, '=');
      rb_c(&b, '[');
      for (int k = 0; k < 4; k++) {
        unsigned long a = v + (unsigned long)k * 8;
        if (k) rb_c(&b, ',');
        if (!rw_readable(a)) { rb_c(&b, '?'); break; }
        rb_hex(&b, *(volatile unsigned long *)a);
      }
      rb_c(&b, ']');
      if (b.p > b.e - 128) { rb_s(&b, "\n"); rb_flush(&b); rb_s(&b, "[SIGDUMP] mem"); }
    }
    rb_s(&b, "\n");
    rb_flush(&b);
  }

  /* 24 qwords at the faulting RSP: a `call`-then-`jmp` leaves the caller's
     return address at [rsp] (or one frame up, if the callee built a frame). */
  unsigned long sp = (unsigned long)g[REG_RSP];
  for (int i = 0; i < 24; i++) {
    unsigned long a = sp + (unsigned long)i * 8;
    if (i == 0) {
      rb_s(&b, "[SIGDUMP] stk");
    }
    if (!rw_readable(a)) {
      rb_s(&b, " [+unmapped]");
      break;
    }
    unsigned long v = *(volatile unsigned long *)a;
    rb_c(&b, ' ');
    rb_c(&b, '[');
    rb_hex(&b, (unsigned long)i * 8);
    rb_s(&b, "]=0x");
    rb_hex(&b, v);
    unsigned long off = 0;
    const char *m = rw_mod_of(v, &off);
    if (m) {
      rb_c(&b, '(');
      rb_s(&b, m);
      rb_s(&b, "+0x");
      rb_hex(&b, off);
      rb_c(&b, ')');
    }
    if ((i & 3) == 3) {
      rb_s(&b, "\n");
      rb_flush(&b);
      rb_s(&b, "[SIGDUMP] stk");
    }
  }
  rb_s(&b, "\n");
  rb_flush(&b);

  /* Every word of the first 2 KiB of stack that lands inside a module, in
     stack order (so frame order): the caller chain, with offsets that
     objdump -d resolves without the guest's missing /proc/self/maps. */
  rb_s(&b, "[SIGDUMP] chain");
  int hits = 0;
  for (unsigned long i = 0; i < 0x800 && hits < 20; i += 8) {
    unsigned long a = sp + i;
    if (!rw_readable(a)) break;
    unsigned long v = *(volatile unsigned long *)a;
    unsigned long off = 0;
    const char *m = rw_mod_of(v, &off);
    if (!m) continue;
    rb_s(&b, " [+0x");
    rb_hex(&b, i);
    rb_s(&b, "]->");
    rb_s(&b, m);
    rb_s(&b, "+0x");
    rb_hex(&b, off);
    hits++;
  }
  rb_s(&b, "\n");
  rb_flush(&b);

  signal(sig, SIG_DFL);
}

/* ---- who else installs SIGSEGV? -----------------------------------------
 * Measured (L29, first instrumented run): the launcher armed its handler
 * ("sigdump armed mods=4"), the game then died at PC=0 -- and not one
 * [SIGDUMP] row appeared, while a SECOND fault followed inside UnityPlayer
 * with every register reading 0xB.  That is somebody else's handler running
 * and crashing.  Mono installs SIGSEGV for null-check translation and Unity
 * installs a crash handler after it; a launcher-level dump is only useful if
 * it survives both.  So the disposition is watched: every non-default
 * SIGSEGV/SIGBUS/SIGILL install is logged with the installer's return
 * address, and the launcher's dump stays installed (RW_SIGKEEP=0 restores
 * the old "let them have it" behaviour for an A/B).
 */
static int (*next_sigaction)(int, const struct sigaction *, struct sigaction *);

static int rw_sig_keep(void) {
  static int on = -1;
  if (on < 0) {
    const char *v = getenv("RW_SIGKEEP");
    on = (v && v[0] == '0') ? 0 : 1;
  }
  return on;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
  if (!next_sigaction)
    next_sigaction = (int (*)(int, const struct sigaction *, struct sigaction *))
        dlvsym(RTLD_NEXT, "sigaction", "GLIBC_2.2.5");
  if (act && act->sa_sigaction == rw_sig_handler)
    return next_sigaction ? next_sigaction(sig, act, old) : -1; /* re-arming ours */
  if (act && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) &&
      act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
    fprintf(stderr, "[RWMAIN] sigaction sig=%d handler=%p flags=0x%x from=%p %s\n",
            sig, (void *)act->sa_handler, (unsigned)act->sa_flags,
            __builtin_return_address(0), rw_sig_keep() ? "KEEP-OURS" : "replaced");
    if (rw_sig_keep()) {
      if (old) {
        memset(old, 0, sizeof *old);
        old->sa_sigaction = rw_sig_handler;
        old->sa_flags = SA_SIGINFO;
      }
      return 0;
    }
  } else if (act && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL)) {
    fprintf(stderr, "[RWMAIN] sigaction sig=%d handler=%s from=%p\n", sig,
            act->sa_handler == SIG_DFL ? "SIG_DFL" : "SIG_IGN",
            __builtin_return_address(0));
  }
  return next_sigaction ? next_sigaction(sig, act, old) : -1;
}

void (*signal(int sig, void (*h)(int)))(int) {
  /* glibc's signal() is not routed through the exported sigaction(), so it
     needs its own watch.  Reaching the real one through dlvsym keeps this
     definition from recursing into itself. */
  typedef void (*sigh_t)(int);
  static sigh_t (*real_signal)(int, sigh_t);
  if (!real_signal)
    real_signal = (sigh_t (*)(int, sigh_t))
        dlvsym(RTLD_NEXT, "signal", "GLIBC_2.2.5");
  if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
    fprintf(stderr, "[RWMAIN] signal() sig=%d handler=%p from=%p %s\n",
            sig, (void *)h, __builtin_return_address(0),
            (h != SIG_DFL && h != SIG_IGN && rw_sig_keep()) ? "KEEP-OURS" : "");
    if (h != SIG_DFL && h != SIG_IGN && rw_sig_keep()) {
      return SIG_IGN; /* pretend it took; ours stays armed */
    }
  }
  return real_signal ? real_signal(sig, h) : SIG_ERR;
}

static void rw_sig_install(void) {
  const char *v = getenv("RW_SIGDUMP");
  if (v && v[0] == '0') return;
  rw_mods_collect();
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = rw_sig_handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  /* The interposed sigaction() above is this file's own definition, so
     calling it here would take the "KEEP-OURS" branch and never reach the
     kernel -- measured: the process died at PC=0 with no [SIGDUMP] row at
     all.  Install through the real entry point. */
  if (!next_sigaction)
    next_sigaction = (int (*)(int, const struct sigaction *, struct sigaction *))
        dlvsym(RTLD_NEXT, "sigaction", "GLIBC_2.2.5");
  if (!next_sigaction || next_sigaction(SIGSEGV, &sa, 0) != 0 ||
      next_sigaction(SIGBUS, &sa, 0) != 0) {
    char e[96];
    struct rb eb = {e, e, e + sizeof e - 2};
    rb_s(&eb, "[RWMAIN] sigdump INSTALL FAILED\n");
    rb_flush(&eb);
    return;
  }
  char line[128];
  struct rb b = {line, line, line + sizeof line - 2};
  rb_s(&b, "[RWMAIN] sigdump armed mods=");
  rb_dec(&b, rw_mod_n);
  rb_s(&b, "\n");
  rb_flush(&b);
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

/* libGL.so.1 is linked with NO DT_NEEDED (tools/mesa/build-hobbyos.sh) but
 * imports libX11's private hooks (_XLockMutex_fn/_XUnlockMutex_fn/_Xglobal_lock)
 * plus XDrawString16/XGetImage/XFreeFontInfo.  Without a NEEDED edge the loader
 * searches only the already-loaded GLOBAL scope, so Unity's
 * dlopen("libGL.so.1") failed with "undefined symbol: _XLockMutex_fn" and the
 * engine exited with "Forced renderer 17 is not supported" (measured, wave 10).
 * A real link would have made libX11 a global dependency; do the same thing
 * explicitly, before anything asks for GL.  Runtime overrides let an experiment
 * change the set without a rebuild. */
static void preload_gl_deps(void) {
  static const char *defaults[] = {"/LIB64/libX11.so.6", "/LIB64/libXext.so.6", 0};
  const char *env = getenv("RW_PRELOAD_GLOBAL");
  if (env && env[0] == '0') return;
  if (env && env[0]) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", env);
    for (char *p = buf; p && *p;) {
      char *comma = strchr(p, ',');
      if (comma) *comma = 0;
      void *h = next_dlopen ? next_dlopen(p, RTLD_NOW | RTLD_GLOBAL) : dlopen(p, RTLD_NOW | RTLD_GLOBAL);
      fprintf(stderr, "[RWMAIN] preload %s -> %s\n", p, h ? "ok" : "FAILED");
      p = comma ? comma + 1 : 0;
    }
    return;
  }
  for (int i = 0; defaults[i]; i++) {
    void *h = next_dlopen ? next_dlopen(defaults[i], RTLD_NOW | RTLD_GLOBAL)
                          : dlopen(defaults[i], RTLD_NOW | RTLD_GLOBAL);
    if (h) fprintf(stderr, "[RWMAIN] preload %s -> ok\n", defaults[i]);
  }
}

/* ---- L48: the ThreadPool's work-item accounting -------------------------
 * The stall (docs/rimworld-launch-log.md §34) is "one work item never
 * completes while the pool that would run it is empty", and the native
 * /TPTRACE rows cannot see an *item* at all: in Mono 6.12 every managed
 * enqueue goes through three icalls and nothing else --
 *
 *   ThreadPoolWorkQueue.Enqueue(cb)   -> ThreadPool.NotifyWorkItemQueued()
 *   ThreadPoolWorkQueue.Dispatch()    -> ThreadPool.NotifyWorkItemComplete()
 *   ThreadPoolWorkQueue.EnsureThreadRequested() -> RequestWorkerThread()
 *
 * `ve_icall_..._NotifyWorkItemQueued` is a no-op in this runtime (it only
 * bumps a perfcounter), so hooking it is a free, exact enqueue counter.
 * NotifyWorkItemComplete is called after every executed item, so
 * enq - comp is the number of items queued and not yet run.
 *
 * mono_add_internal_call() replaces an existing registration (g_hash_table
 * _insert), and the JIT binds an icall's address when it compiles the
 * calling method, so the registration must land before the loader first
 * touches ThreadPool.  A watchdog thread therefore polls for the root
 * domain, attaches, and re-registers until the game's loading begins; the
 * re-registration is idempotent and the self-test row proves it took.
 *
 * The managed caller of the enqueue is resolved with mono_jit_info_table_find
 * on the icall's return address, so the row carries "Class::Method" -- the
 * identity of the enqueuer, not just its tid.
 *
 * Gated on /TPTRACE (the same marker that turns on Mono's own threadpool
 * trace), so an ordinary run executes none of this.
 *
 * NOTE: this file is compiled -nostdinc against the pinned glibc 2.31
 * headers, and <fcntl.h> drags in <linux/falloc.h> which that set does not
 * carry; open()/syscall() are therefore declared here directly. */
extern int open(const char *path, int flags, ...);
extern long syscall(long nr, ...);
#define TP_O_RDONLY 0
#define TP_SYS_gettid 186

static void tp_w(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  if (n) (void)!write(2, s, n);
}
static void tp_wi(long v) {
  char t[24], b[24];
  int i = 0, j = 0;
  if (v < 0) { tp_w("-"); v = -v; }
  if (!v) t[j++] = '0';
  while (v) { t[j++] = (char)('0' + (v % 10)); v /= 10; }
  while (j) b[i++] = t[--j];
  (void)!write(2, b, (size_t)i);
}
static void tp_wx(unsigned long v) {
  char t[20], b[22];
  int i = 0, j = 0;
  b[i++] = '0'; b[i++] = 'x';
  if (!v) t[j++] = '0';
  while (v) { int d = (int)(v & 15); t[j++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
  while (j) b[i++] = t[--j];
  (void)!write(2, b, (size_t)i);
}

#define TPRING 64
struct tp_ev { long seq; int kind; int tid; void *ra; void *ra1; };

static struct {
  volatile int on, attached, registered_q, registered_w, dumps;
  void *h;
  void *(*get_root_domain)(void);
  void *(*attach)(void *);
  void *(*get_corlib)(void);
  void *(*image_loaded)(const char *);
  void *(*class_from_name)(void *, const char *, const char *);
  void *(*method_from_name)(void *, const char *, int);
  void *(*lookup_icall)(void *);
  void (*add_icall)(const char *, const void *);
  void *(*domain_get)(void);
  void *(*jit_find)(void *, void *);
  void *(*jit_method)(void *);
  void *(*m_get_class)(void *);
  const char *(*c_get_name)(void *);
  const char *(*m_get_name)(void *);
  void *orig_q, *orig_c, *orig_r;
  void *m_q, *m_c, *m_r;
  long enq, comp, req, reqf, seq, wtn;
  volatile int wbt_inf;
} tp;
static struct tp_ev tp_ring[TPRING];

static int tp_tid(void) {
#ifdef TP_SYS_gettid
  return (int)syscall(TP_SYS_gettid);
#else
  return 0;
#endif
}

static void tp_note(int kind, void *ra, void *ra1) {
  long s = __sync_fetch_and_add(&tp.seq, 1);
  int i = (int)(s % TPRING);
  /* No Mono API on this path: the icall runs on up to 8 threads at once and
     mono_jit_info_table_find takes the loader's JIT-table lock, so the
     return address is stored raw and resolved once, at dump time. */
  tp_ring[i].seq = s;
  tp_ring[i].kind = kind;
  tp_ring[i].tid = tp_tid();
  tp_ring[i].ra = ra;
  tp_ring[i].ra1 = ra1;
}

/* Phase 2 — resolve the other two icalls and their originals.  Called from
 * inside the NotifyWorkItemQueued hook, i.e. from a Mono thread with a fully
 * initialised runtime: a class lookup from the watchdog thread proved fatal
 * ("Assertion at object.c:1939, condition `klass' not met") because it ran
 * during mono_init, before mscorlib's defaults existed.  Nothing in phase 1
 * below touches the class loader for that reason. */
static int tp_hook_c(void *err);
static int tp_hook_r(void *err);

/* The waiter's identity.  WaitHandle.WaitOne parks in Mono's w32handle code,
 * so a native stack names mono_w32handle_wait_one but never the game method
 * that called it.  WaitOneNative is an icall: hooking it gives the managed
 * caller (return address one frame above WaitHandle.WaitOne) directly, which
 * is the fact the stall needs -- "who is waiting, and on what". */
#define TPW_MAX 24
static struct { void *ra1; long n; unsigned last_ms; int last_tid; } tp_waits[TPW_MAX];
static volatile int tp_w_n;
static int (*tp_orig_w)(long, unsigned, long, long, void *);

/* A poor man's managed stack: scan the raw stack for return addresses that
 * Mono's JIT info table resolves to a method, keeping only words preceded by
 * a `call rel32` (0xE8) -- the same filter that turned a page of stack
 * garbage into a real chain in §33/L46.  This is what the broken SIGQUIT dump
 * cannot give us: the managed caller of a wait, in-process, on demand. */
static int tp_bt(const char *tag, int tid, long extra) {
  unsigned long sp = 0;
  __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
  if (!tp.jit_find || !tp.domain_get) return 0;
  void *dom = tp.domain_get();
  if (!dom) return 0;
  int printed = 0;
  unsigned long *p = (unsigned long *)sp;
  for (int i = 0; i < 1536 && printed < 6; i++) {
    if (!rw_readable((unsigned long)&p[i])) break;
    unsigned long v = p[i];
    if (v < 0x100000 || v > 0x7fffffffffffUL) continue;
    if (!rw_readable(v - 1)) continue;
    unsigned char prev = *(unsigned char *)(v - 1);
    if (prev != 0xE8) continue;
    void *ji = tp.jit_find(dom, (void *)v);
    if (!ji) continue;
    void *m = tp.jit_method(ji);
    if (!m) continue;
    void *k = tp.m_get_class(m);
    const char *mn = tp.m_get_name(m);
    const char *kn = k ? tp.c_get_name(k) : 0;
    if (printed == 0) {
      tp_w(tag);
      tp_w(" tid=");
      tp_wi(tid);
      if (extra >= 0) { tp_w(" ms="); tp_wi(extra); }
      tp_w(" bt=");
    } else {
      tp_w(" <- ");
    }
    tp_w(kn ? kn : "?");
    tp_w(".");
    tp_w(mn ? mn : "?");
    printed++;
  }
  if (printed) { tp_w("\n"); return 1; }
  return 0;
}

static int tp_hook_w(long h, unsigned ms, long a3, long a4, void *err) {
  int infinite = (ms == 0xFFFFFFFFu);
  int n = (int)__sync_fetch_and_add(&tp.wtn, 1);
  if (n == 0) tp_w("[TPQW] first Wait_internal through the hook\n");
  /* Only the interesting waits pay for a stack walk: infinite ones (the
     stall) and the first few of everything else. */
  if ((infinite && tp.wbt_inf < 6) || n < 3) {
    if (infinite) tp.wbt_inf++;
    tp_bt("[TPQW]", tp_tid(), infinite ? -1 : (long)ms);
  }
  int r = tp_orig_w ? tp_orig_w(h, ms, a3, a4, err) : 0;
  void *ra1 = __builtin_return_address(1);
  int slot = -1;
  for (int i = 0; i < tp_w_n && i < TPW_MAX; i++)
    if (tp_waits[i].ra1 == ra1) { slot = i; break; }
  if (slot < 0 && tp_w_n < TPW_MAX) { slot = tp_w_n; tp_w_n = slot + 1; tp_waits[slot].ra1 = ra1; tp_waits[slot].n = 0; }
  if (slot >= 0) {
    tp_waits[slot].n++;
    tp_waits[slot].last_ms = ms;
    tp_waits[slot].last_tid = tp_tid();
  }
  return r;
}

static int tp_register_w(void) {
  if (!tp.class_from_name || !tp.method_from_name || !tp.lookup_icall || !tp.image_loaded) return 0;
  void *img = tp.image_loaded("mscorlib");
  if (!img) { tp_w("[TPQW] no mscorlib image\n"); return 0; }
  void *k = tp.class_from_name(img, "System.Threading", "WaitHandle");
  if (!k) { tp_w("[TPQW] WaitHandle class lookup failed\n"); return 0; }
  /* This runtime registers Wait_internal, not WaitOneNative: the mscorlib on
     disk has WaitOneNative as a *managed* wrapper (DangerousAddRef/ExitContext)
     around the icall, so the icall name that exists in libmonobdwgc's table is
     the only one that can be hooked. */
  static const char *names[] = {"Wait_internal", "WaitOneNative", "InternalWaitOne", 0};
  int hooked = 0;
  for (int ni = 0; names[ni]; ni++) {
    void *m = 0;
    int arity = 0;
    for (arity = 4; arity >= 1; arity--) {
      m = tp.method_from_name(k, names[ni], arity);
      if (m) break;
    }
    if (!m) continue;
    if (!tp_orig_w) tp_orig_w = (int (*)(long, unsigned, long, long, void *))tp.lookup_icall(m);
    if (!tp_orig_w) continue;
    char nm[64];
    int i = 0;
    for (const char *s = "System.Threading.WaitHandle::"; *s && i < 60; s++) nm[i++] = *s;
    for (const char *s = names[ni]; *s && i < 62; s++) nm[i++] = *s;
    nm[i] = 0;
    tp.add_icall(nm, (const void *)tp_hook_w);
    if (!hooked) {
      tp_w("[TPQW] hooked ");
      tp_w(nm);
      tp_w(" arity=");
      tp_wi(arity);
      tp_w(" orig=");
      tp_wx((unsigned long)tp_orig_w);
      tp_w("\n");
    }
    hooked++;
  }
  if (!hooked) tp_w("[TPQW] no WaitHandle icall could be hooked\n");
  return hooked;
}

static int tp_register_cr(void) {
  int ok = 0;
  if (!tp.add_icall || !tp.lookup_icall || !tp.image_loaded) return 0;
  void *img = tp.image_loaded("mscorlib");
  if (!img) return 0;
  void *klass = tp.class_from_name(img, "System.Threading", "ThreadPool");
  if (!klass) return 0;
  if (!tp.m_c) tp.m_c = tp.method_from_name(klass, "NotifyWorkItemComplete", 0);
  if (!tp.m_r) tp.m_r = tp.method_from_name(klass, "RequestWorkerThread", 0);
  if (!tp.orig_c && tp.m_c) tp.orig_c = tp.lookup_icall(tp.m_c);
  if (!tp.orig_r && tp.m_r) tp.orig_r = tp.lookup_icall(tp.m_r);
  if (tp.m_c && tp.orig_c) { tp.add_icall("System.Threading.ThreadPool::NotifyWorkItemComplete", (const void *)tp_hook_c); ok |= 2; }
  if (tp.m_r && tp.orig_r) { tp.add_icall("System.Threading.ThreadPool::RequestWorkerThread", (const void *)tp_hook_r); ok |= 4; }
  return ok;
}

static void tp_hook_q(void *err) {
  long n = __sync_fetch_and_add(&tp.enq, 1) + 1;
  if (n == 1) {
    tp_w("[TPQ] first enqueue through the hook (phase 2)\n");
    int ok = tp_register_cr();
    tp_w("[TPQ] phase2 c=");
    tp_w((ok & 2) ? "HOOK" : "no");
    tp_w(" r=");
    tp_w((ok & 4) ? "HOOK" : "no");
    tp_w(" orig_c=");
    tp_wx((unsigned long)tp.orig_c);
    tp_w(" orig_r=");
    tp_wx((unsigned long)tp.orig_r);
    tp_w("\n");
  }
  tp_note(0, __builtin_return_address(0), __builtin_return_address(1));
}
static int tp_hook_c(void *err) {
  int r = tp.orig_c ? ((int (*)(void *))tp.orig_c)(err) : 1;
  __sync_fetch_and_add(&tp.comp, 1);
  tp_note(1, __builtin_return_address(0), __builtin_return_address(1));
  return r;
}
static int tp_hook_r(void *err) {
  int r = tp.orig_r ? ((int (*)(void *))tp.orig_r)(err) : 0;
  if (r) __sync_fetch_and_add(&tp.req, 1);
  else __sync_fetch_and_add(&tp.reqf, 1);
  tp_note(r ? 2 : 3, __builtin_return_address(0), __builtin_return_address(1));
  return r;
}

static void tp_meth_at(void *ra, const char **cls, const char **meth) {
  *cls = 0;
  *meth = 0;
  if (!tp.jit_find || !ra) return;
  void *dom = tp.domain_get ? tp.domain_get() : 0;
  if (!dom) return;
  void *ji = tp.jit_find(dom, ra);
  if (!ji) return;
  void *m = tp.jit_method(ji);
  if (!m) return;
  void *k = tp.m_get_class(m);
  *meth = tp.m_get_name(m);
  if (k) *cls = tp.c_get_name(k);
}

static void tp_dump(const char *why) {
  tp_w("[TPQSUM] enq=");
  tp_wi(tp.enq);
  tp_w(" comp=");
  tp_wi(tp.comp);
  tp_w(" req=");
  tp_wi(tp.req);
  tp_w(" reqF=");
  tp_wi(tp.reqf);
  tp_w(" stranding=");
  tp_wi(tp.enq - tp.comp);
  tp_w(" why=");
  tp_w(why);
  tp_w("\n");
  /* The waiters: who called WaitHandle.WaitOne, how often, with what timeout.
     This is the row that names the stall's wait site in managed terms. */
  for (int i = 0; i < tp_w_n && i < TPW_MAX; i++) {
    const char *c1 = 0, *m1 = 0;
    tp_meth_at(tp_waits[i].ra1, &c1, &m1);
    tp_w("[TPQW] n=");
    tp_wi(tp_waits[i].n);
    tp_w(" tid=");
    tp_wi(tp_waits[i].last_tid);
    tp_w(" ms=");
    tp_wi((long)tp_waits[i].last_ms);
    tp_w(" caller=");
    tp_w(c1 ? c1 : "?");
    tp_w(".");
    tp_w(m1 ? m1 : "?");
    tp_w(" ra=");
    tp_wx((unsigned long)tp_waits[i].ra1);
    tp_w("\n");
  }
  long s0 = tp.seq;
  long first = s0 > 24 ? s0 - 24 : 0;
  for (long s = first; s < s0; s++) {
    const struct tp_ev *e = &tp_ring[s % TPRING];
    if (e->seq != s) continue;
    const char *c0 = 0, *m0 = 0, *c1 = 0, *m1 = 0;
    tp_meth_at(e->ra, &c0, &m0);
    tp_meth_at(e->ra1, &c1, &m1);
    tp_w("[TPQR] seq=");
    tp_wi(e->seq);
    tp_w(" kind=");
    tp_w(e->kind == 0 ? "enq" : e->kind == 1 ? "cmp" : e->kind == 2 ? "req" : "reqF");
    tp_w(" tid=");
    tp_wi(e->tid);
    tp_w(" icall=");
    tp_w(c0 ? c0 : "?");
    tp_w(".");
    tp_w(m0 ? m0 : "?");
    tp_w(" from=");
    tp_w(c1 ? c1 : "?");
    tp_w(".");
    tp_w(m1 ? m1 : "?");
    tp_w(" ra0=");
    tp_wx((unsigned long)e->ra);
    tp_w(" ra1=");
    tp_wx((unsigned long)e->ra1);
    tp_w("\n");
  }
}

/* Phase 1 — register the enqueue icall BY NAME ONLY.  It needs no class
 * lookup, and its original is a no-op in this runtime
 * (ves_icall_..._NotifyWorkItemQueued only bumps a perfcounter), so nothing
 * of the runtime's own work is lost by not calling through. */
static int tp_register_q(void) {
  if (!tp.add_icall) return 0;
  tp.add_icall("System.Threading.ThreadPool::NotifyWorkItemQueued", (const void *)tp_hook_q);
  return 1;
}

static void *tp_watchdog(void *arg) {
  (void)arg;
  long prev = -1;
  int quiet = 0;
  int late_attach_tried = 0;
  for (int i = 0; i < 4800; i++) { /* <= 240 s at 50 ms */
    /* Phase 1 must not run during mono_init: a class lookup from this thread
       aborted the runtime (object.c:1939 `klass').  4 s after the DSO load
       the runtime is executing managed code, and mscorlib is on the loaded
       image list, so the name-only registration below is safe. */
    if (i >= 40 && !tp.registered_q && tp.image_loaded && tp.add_icall) {
      void *img = tp.image_loaded("mscorlib");
      if (img) {
        tp.registered_q = 1;
        tp_register_q();
        tp_w("[TPQ] phase1: NotifyWorkItemQueued registered by name\n");
      }
    }
    /* The name resolution needs an attached thread; attach lazily and late,
       and only once (a second attach of the same thread is a no-op anyway). */
    if (!tp.attached && late_attach_tried == 0 && i >= 25 && tp.get_root_domain) {
      void *root = tp.get_root_domain();
      if (root) {
        late_attach_tried = 1;
        if (tp.attach) tp.attach(root);
        tp.attached = 1;
        tp_w("[TPQ] attached to the root domain\n");
      }
    }
    /* 10 s in: the runtime has been executing managed code for seconds, so a
       class lookup is safe here, and the loading wait starts before the pool's
       first enqueue. */
    if (!tp.registered_w && tp.attached && i >= 50) {
      tp.registered_w = tp_register_w();
      if (!tp.registered_w && i > 400) tp.registered_w = -1; /* give up, once */
    }
    long s = tp.enq + tp.comp + tp.req + tp.reqf;
    if (s != prev) { prev = s; quiet = 0; }
    else quiet++;
    /* Every 5 s: the timeline.  A single end-of-run total cannot tell a pool
       that never woke from one that woke and died. */
    if ((i % 100) == 0) {
      tp_w("[TPQT] t=");
      tp_wi(i / 20);
      tp_w("s enq=");
      tp_wi(tp.enq);
      tp_w(" comp=");
      tp_wi(tp.comp);
      tp_w(" req=");
      tp_wi(tp.req);
      tp_w(" reqF=");
      tp_wi(tp.reqf);
      tp_w("\n");
    }
    /* 3 s without a single event, at least 10 s in: dump the tail.  Up to
       six times, because the first quiet window is often just the gap before
       the loader starts. */
    if (quiet == 60 && i >= 200 && tp.dumps < 6) { tp.dumps++; tp_dump("quiet3s"); }
    usleep(50000);
  }
  tp_dump("exit");
  return 0;
}

static void rw_tp_maybe_start(void *handle, const char *name) {
  if (tp.on) return;
  if (!name || !strstr(name, "libmonobdwgc")) return;
  {
    int f = open("/TPTRACE", TP_O_RDONLY);
    if (f < 0) { tp.on = 0; return; }
    close(f);
  }
  tp.on = 1;
  tp.h = handle;
  tp.get_root_domain = (void *(*)(void))dlsym(handle, "mono_get_root_domain");
  tp.attach = (void *(*)(void *))dlsym(handle, "mono_thread_attach");
  tp.get_corlib = (void *(*)(void))dlsym(handle, "mono_get_corlib");
  tp.image_loaded = (void *(*)(const char *))dlsym(handle, "mono_image_loaded");
  tp.class_from_name = (void *(*)(void *, const char *, const char *))dlsym(handle, "mono_class_from_name");
  tp.method_from_name = (void *(*)(void *, const char *, int))dlsym(handle, "mono_class_get_method_from_name");
  tp.lookup_icall = (void *(*)(void *))dlsym(handle, "mono_lookup_internal_call");
  tp.add_icall = (void (*)(const char *, const void *))dlsym(handle, "mono_add_internal_call");
  tp.domain_get = (void *(*)(void))dlsym(handle, "mono_domain_get");
  tp.jit_find = (void *(*)(void *, void *))dlsym(handle, "mono_jit_info_table_find");
  tp.jit_method = (void *(*)(void *))dlsym(handle, "mono_jit_info_get_method");
  tp.m_get_class = (void *(*)(void *))dlsym(handle, "mono_method_get_class");
  tp.c_get_name = (const char *(*)(void *))dlsym(handle, "mono_class_get_name");
  tp.m_get_name = (const char *(*)(void *))dlsym(handle, "mono_method_get_name");
  tp_w("[TPQ] probe armed (libmonobdwgc);");
  tp_w(" get_root_domain=");
  tp_wx((unsigned long)tp.get_root_domain);
  tp_w(" add_icall=");
  tp_wx((unsigned long)tp.add_icall);
  tp_w("\n");
  if (!tp.get_root_domain || !tp.add_icall || !tp.lookup_icall || !tp.image_loaded) {
    tp_w("[TPQ] probe OFF: a needed Mono API symbol is missing\n");
    tp.on = 0;
    return;
  }
  int (*pc)(void *, void *, void *, void *) =
      (int (*)(void *, void *, void *, void *))dlsym(RTLD_DEFAULT, "pthread_create");
  if (!pc) {
    void *lp = dlopen("/LIB64/libpthread.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (lp) pc = (int (*)(void *, void *, void *, void *))dlsym(lp, "pthread_create");
  }
  if (!pc) {
    tp_w("[TPQ] probe OFF: no pthread_create\n");
    tp.on = 0;
    return;
  }
  void *th = 0;
  if (pc(&th, 0, (void *)tp_watchdog, 0) != 0) {
    tp_w("[TPQ] probe OFF: watchdog thread creation failed\n");
    tp.on = 0;
  }
}

int main(int argc, char **argv) {
  fprintf(stderr, "[RWMAIN] AD-9 PlayerMain launcher start argc=%d\n", argc);
  /* M6/L36: make the managed runtime's stdout UNBUFFERED.  fd 1 is Player.log
     (a regular file), so glibc block-buffers it and a printf issued just
     before an abort never reaches the log.  Mono's trampoline guard prints the
     MONO trampoline TYPE that tripped it (`printf("%d\n", tramp_type)` right
     before g_assert_not_reached at tramp-amd64.c:641) -- that number is the one
     datum that names the failing trampoline, and it is lost to buffering. */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  rw_sig_install();
  if (!next_dlopen) next_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
  preload_gl_deps();
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
