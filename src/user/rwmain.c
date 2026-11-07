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

/* M6/L61: the handle Unity dlopen()ed for libmonobdwgc.  The runtime is
 * loaded RTLD_LOCAL (Unity's own load), so dlsym(RTLD_DEFAULT, ...) cannot see
 * `mono_use_interpreter` -- measured: "undefined symbol".  The interposer is
 * the only place that sees the handle, so it keeps it. */
static void *rw_mono_handle;

static void rw_note_mono_handle(void *h, const char *path) {
  if (!h || !path) return;
  const char *b = base_of(path);
  if (strstr(b, "libmonobdwgc")) rw_mono_handle = h;
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
    rw_note_mono_handle(r, local);
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
  rw_note_mono_handle(r, file);
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

/* ---- L59: the live thread count, without the kernel ----------------------
 * The SIGSEGV dump has to carry the thread count at the instant of the fault
 * (§45.8's death followed a climb, and "how many were live" is what separates
 * a pool burst from a leak).  The kernel's [THRS] row samples every 5 s and
 * [SIGX] gives the exact value at delivery; this counter is the launcher's own
 * view, incremented in the pthread_create interposer (inside the shadow block
 * below) and decremented by a cleanup handler so pthread_exit is counted too.
 * Both are plain atomics: a creator and an exiter race by design.
 */
static volatile int rw_live_threads;
static volatile int rw_created_threads;

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
  if (page < 0x10000ul || page >= 0x1800000000ul) return 0; /* USER_VA_BASE..TOP */
  /* mincore answers 0 when the RANGE IS COVERED BY A REGION, and bit 0 says
     whether a leaf is installed.  A PROT_NONE guard region is covered but has
     no leaf, so returning mincore's status alone made this say "readable" for
     the page at the very top of the user window (0x10FFFFF000) and the stack
     scan died reading 0x10FFFFFFFE, i.e. inside rwmain itself. */
  if (mincore((void *)page, 4096, vec) != 0) return 0;
  return (vec[0] & 1) != 0;
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
  size_t n = (size_t)(b->p - b->start);
  size_t off = 0;
  /* A console write can be SHORT: the crash dump is ~2 KiB and the previous
     version dropped the write's return value, so a partial write silently lost
     the head of the record (measured: only the last line of an 8-line dump
     reached the serial).  Loop until every byte is out or the write fails. */
  while (off < n) {
    ssize_t w = write(2, b->start + off, n - off);
    if (w <= 0)
      break;
    off += (size_t)w;
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

/* One dump per process; then chain (L59) or default action.

   M6/L59: §45.8's 600 s run died at ~410 s with `status=11` and NO dump at
   all.  Two ways that happens, and both are answered here:
     - the fault was a stack overflow: the kernel cannot push a signal frame
       onto a stack pointer that is already below the mapping, so no handler
       runs, whatever is installed.  `SA_ONSTACK` + a real sigaltstack is the
       only way a handler can speak from there (measured: Unity's own SIGSEGV
       install asks for SA_ONSTACK, ours did not);
     - the fault happened inside a handler, where a second dump is impossible
       (the kernel answers `default action`, and [LCOMPAT] fault-in-handler
       now names it).
   The dump is ONE write(2) of a fully-built record: per-line writes are what
   the serial capture loses under load, and the crash moment is the one moment
   the log must not lose.

   Chaining: the interposer below records every handler it refuses to install
   (Unity's and Mono's SIGSEGV/SIGBUS handlers, and the one septic case where
   Unity's is the only thing that knows how to unwind).  After the dump,
   Unity's is called with the same siginfo/ucontext, so its own crash report
   and its own exit path still happen -- ours first, theirs second. */
static void (*rw_chain_handler)(int, siginfo_t *, void *);

static void rw_sig_handler(int sig, siginfo_t *si, void *ucv) {
  if (rw_sigdumped) {
    /* A second fault: the first one's dump is out.  Default action, exactly
       once, so the kernel's own exit record still happens. */
    signal(sig, SIG_DFL);
    return;
  }
  rw_sigdumped = 1;
  ucontext_t *uc = (ucontext_t *)ucv;
  greg_t *g = uc->uc_mcontext.gregs;

  static char buf[4096];
  struct rb b = {buf, buf, buf + sizeof buf - 8};

  rb_s(&b, "[SIGDUMP] ");
  rb_s(&b, sig == SIGSEGV ? "SIGSEGV" : (sig == SIGBUS ? "SIGBUS" : "SIGother"));
  rb_s(&b, " code=");
  rb_dec(&b, si ? si->si_code : -1);
  rb_s(&b, " addr=0x");
  rb_hex(&b, (unsigned long)(si ? si->si_addr : 0));
  rb_s(&b, " live=");
  rb_dec(&b, (long)rw_live_threads);
  rb_s(&b, " created=");
  rb_dec(&b, (long)rw_created_threads);
  rb_s(&b, " tid=");
  rb_dec(&b, (long)gettid());
  rb_s(&b, "\n");
  /* The summary line goes out FIRST and on its own: it carries every field the
     verdict needs (signal, code, si_addr, live threads, tid), so a lossy
     console can cost the detail but never the answer. */
  rb_flush(&b);

  /* The complete interrupted context: every GPR plus the fault record.  A
     managed JIT frame keeps arguments in r8-r15 as often as in rdi/rsi, and
     `cr2` is the address the *hardware* refused while `addr` is what the
     kernel chose to report (they differ for a non-fault-generated signal). */
  rb_s(&b, "[SIGDUMP] rip=0x");
  rb_hex(&b, (unsigned long)g[REG_RIP]);
  {
    unsigned long off = 0;
    const char *m = rw_mod_of((unsigned long)g[REG_RIP], &off);
    if (m) {
      rb_s(&b, " (");
      rb_s(&b, m);
      rb_s(&b, "+0x");
      rb_hex(&b, off);
      rb_s(&b, ")");
    } else {
      rb_s(&b, " (no module: JIT chunk, stack, or freed mapping)");
    }
  }
  rb_s(&b, " rsp=0x");
  rb_hex(&b, (unsigned long)g[REG_RSP]);
  rb_s(&b, " rbp=0x");
  rb_hex(&b, (unsigned long)g[REG_RBP]);
  rb_s(&b, " rflags=0x");
  rb_hex(&b, (unsigned long)g[REG_EFL]);
  rb_s(&b, " err=0x");
  rb_hex(&b, (unsigned long)g[REG_ERR]);
  rb_s(&b, " trapno=0x");
  rb_hex(&b, (unsigned long)g[REG_TRAPNO]);
  rb_s(&b, " cr2=0x");
  rb_hex(&b, (unsigned long)g[REG_CR2]);
  rb_s(&b, "\n");

  rb_s(&b, "[SIGDUMP] greg");
  rb_reg(&b, "rax", (unsigned long)g[REG_RAX], 1);
  rb_reg(&b, "rbx", (unsigned long)g[REG_RBX], 1);
  rb_reg(&b, "rcx", (unsigned long)g[REG_RCX], 1);
  rb_reg(&b, "rdx", (unsigned long)g[REG_RDX], 1);
  rb_reg(&b, "rsi", (unsigned long)g[REG_RSI], 1);
  rb_reg(&b, "rdi", (unsigned long)g[REG_RDI], 1);
  rb_s(&b, "\n[SIGDUMP] greg");
  rb_reg(&b, "r8", (unsigned long)g[REG_R8], 1);
  rb_reg(&b, "r9", (unsigned long)g[REG_R9], 1);
  rb_reg(&b, "r10", (unsigned long)g[REG_R10], 1);
  rb_reg(&b, "r11", (unsigned long)g[REG_R11], 1);
  rb_reg(&b, "r12", (unsigned long)g[REG_R12], 1);
  rb_reg(&b, "r13", (unsigned long)g[REG_R13], 1);
  rb_reg(&b, "r14", (unsigned long)g[REG_R14], 1);
  rb_reg(&b, "r15", (unsigned long)g[REG_R15], 1);
  rb_s(&b, "\n");

  /* The module map as of the last dlopen: the last 16 objects, with base and
     end, so a rip/return address that is NOT in any of them is provably JIT
     code or a freed mapping -- the JIT-vs-corruption verdict in one row. */
  rb_s(&b, "[SIGDUMP] mods=");
  rb_dec(&b, rw_mod_n);
  for (int i = 0; i < rw_mod_n; i++) {
    rb_s(&b, " ");
    rb_s(&b, rw_mods[i].name);
    rb_s(&b, "@0x");
    rb_hex(&b, rw_mods[i].base);
    rb_s(&b, "-0x");
    rb_hex(&b, rw_mods[i].end);
  }
  rb_s(&b, "\n");

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
    int any = 0;
    for (int i = 0; i < 8; i++) {
      unsigned long v = rv[i].v;
      if (!v || v >= 0x800000000000ul || !rw_readable(v)) continue;
      if (!any) {
        rb_s(&b, "[SIGDUMP] mem");
        any = 1;
      }
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
    }
    if (any) rb_s(&b, "\n");
  }

  /* 24 qwords at the faulting RSP: a `call`-then-`jmp` leaves the caller's
     return address at [rsp] (or one frame up, if the callee built a frame). */
  {
    unsigned long sp = (unsigned long)g[REG_RSP];
    int printed = 0;
    for (int i = 0; i < 24; i++) {
      unsigned long a = sp + (unsigned long)i * 8;
      if (!rw_readable(a)) {
        if (i == 0) {
          rb_s(&b, "[SIGDUMP] stk [+unmapped at rsp]\n");
          printed = 1;
        }
        break;
      }
      if (i == 0) rb_s(&b, "[SIGDUMP] stk");
      unsigned long v = *(volatile unsigned long *)a;
      rb_c(&b, ' ');
      rb_c(&b, '[');
      rb_hex(&b, (unsigned long)i * 8);
      rb_s(&b, "]=");
      rb_hex(&b, v);
      printed = 1;
    }
    if (printed) rb_s(&b, "\n");
  }

  /* Every word of the first 2 KiB of stack that lands inside a module, in
     stack order (so frame order): the caller chain, with offsets that
     objdump -d resolves without the guest's missing /proc/self/maps. */
  {
    unsigned long sp = (unsigned long)g[REG_RSP];
    int hits = 0;
    rb_s(&b, "[SIGDUMP] chain");
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
  }

  rb_flush(&b); /* ONE write(2) with the whole record */

  /* Chain: Unity's handler runs second, with the untouched ucontext, so its
     crash report/exit still happens.  If there is none, the instruction is
     re-executed against SIG_DFL so the kernel's own record is produced. */
  if (rw_chain_handler) {
    void (*h)(int, siginfo_t *, void *) = rw_chain_handler;
    rw_chain_handler = 0;
    h(sig, si, ucv);
  }
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
  if (act && sig == SIGUSR2)
    return 0; /* L49: the stack-dump signal stays ours; the client is told it
                 was installed and never learns otherwise */
  if (act && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) &&
      act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
    fprintf(stderr, "[RWMAIN] sigaction sig=%d handler=%p flags=0x%x from=%p %s\n",
            sig, (void *)act->sa_handler, (unsigned)act->sa_flags,
            __builtin_return_address(0), rw_sig_keep() ? "KEEP-OURS" : "replaced");
    if (rw_sig_keep()) {
      /* L59: remember who asked, so the dump can chain to it after printing
         ours -- ours FIRST, Unity's second.  Last install wins, which is what
         Linux would have done had we not refused it.  A SA_SIGINFO handler is
         the only kind that can be called with our (sig, siginfo, ucontext). */
      if ((act->sa_flags & SA_SIGINFO) && act->sa_sigaction &&
          act->sa_sigaction != rw_sig_handler)
        rw_chain_handler = act->sa_sigaction;
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

/* ---- L49: the managed stack of a PARKED thread -------------------------
 *
 * The wait site is a managed WaitHandle.WaitOne, and L48 could not name its
 * caller: Mono's own SIGQUIT dump dies in its global-suspend handshake, and an
 * icall hook on Wait_internal never binds because the JIT had already cached
 * the call (the icall table holds our pointer -- verify=HOOK -- and the body
 * still calls something else).
 *
 * So take the stack from the thread itself: SIGUSR2 is delivered to the
 * parked thread, the handler (which is async-signal-safe: it copies words and
 * sets a flag, nothing else) records [rsp, rsp+N) from the interrupted
 * ucontext, and the watchdog thread resolves those words with
 * mono_jit_info_table_find.  Frame names, not addresses.
 */
#define RW_STK_WORDS 2048
static volatile int rw_stk_ready;
static volatile int rw_stk_busy;
static unsigned long rw_stk_base;
static volatile int rw_stk_n;
static unsigned long rw_stk_buf[RW_STK_WORDS];
/* L54: the interrupted thread's register file is the only place that carries
 * the futex key the thread is ACTUALLY parked on (syscall arg 0 = rdi).  The
 * stack alone can only offer handles that happen to be spilled in it, which is
 * how the L53 scan came to blame a signalled EVENT that the thread was not
 * waiting on.  Copy the whole greg set; the sampler reads it immediately. */
static unsigned long rw_stk_regs[NGREG];
static volatile int rw_stk_regs_ok;

static void rw_stk_handler(int sig, siginfo_t *si, void *ucv) {
  (void)sig;
  (void)si;
  ucontext_t *uc = (ucontext_t *)ucv;
  unsigned long sp = (unsigned long)uc->uc_mcontext.gregs[REG_RSP];
  for (int r = 0; r < NGREG; r++)
    rw_stk_regs[r] = (unsigned long)uc->uc_mcontext.gregs[r];
  rw_stk_regs_ok = 1;
  rw_stk_base = sp;
  int i = 0;
  while (i < RW_STK_WORDS) {
    unsigned long a = sp + (unsigned long)i * 8;
    if (!rw_readable(a)) break;              /* per-page, mincore-backed */
    rw_stk_buf[i] = *(volatile unsigned long *)a;
    i++;
  }
  rw_stk_n = i;
  rw_stk_busy = 0;
  rw_stk_ready = 1;
}

static void rw_sig_install(void) {
  const char *v = getenv("RW_SIGDUMP");
  if (v && v[0] == '0') return;
  rw_mods_collect();
  /* L59: our own alternate signal stack.  A stack overflow leaves rsp below
     the mapping, and the kernel cannot push a signal frame there: the process
     dies with status 11 and the handler never runs, whatever is installed.
     SA_ONSTACK + this stack is the only way the dump can speak from an
     overflowed thread.  Unity's own SIGSEGV install asks for SA_ONSTACK
     (flags 0x8000004); ours did not, which is why §45.8's death had no row. */
  {
    static void *alt;
    if (!alt)
      alt = mmap(0, 64 * 1024, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (alt && alt != MAP_FAILED) {
      stack_t ss;
      ss.ss_sp = alt;
      ss.ss_size = 64 * 1024;
      ss.ss_flags = 0;
      char line[128];
      struct rb b = {line, line, line + sizeof line - 2};
      int rc = sigaltstack(&ss, 0);
      rb_s(&b, "[RWMAIN] sigaltstack sp=0x");
      rb_hex(&b, (unsigned long)alt);
      rb_s(&b, " size=65536 rc=");
      rb_dec(&b, rc);
      rb_s(&b, "\n");
      rb_flush(&b);
    }
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = rw_sig_handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  /* The interposed sigaction() above is this file's own definition, so
     calling it here would take the "KEEP-OURS" branch and never reach the
     kernel -- measured: the process died at PC=0 with no [SIGDUMP] row at
     all.  Install through the real entry point. */
  if (!next_sigaction)
    next_sigaction = (int (*)(int, const struct sigaction *, struct sigaction *))
        dlvsym(RTLD_NEXT, "sigaction", "GLIBC_2.2.5");
  /* L49: our own stack-dump signal.  SA_RESTART must be CLEAR: the target is
     parked in a futex/cond wait and the -EINTR resume is what runs the handler
     on that thread's stack. */
  {
    struct sigaction sa2;
    memset(&sa2, 0, sizeof sa2);
    sa2.sa_sigaction = rw_stk_handler;
    sa2.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa2.sa_mask);
    if (next_sigaction) (void)next_sigaction(SIGUSR2, &sa2, 0);
  }
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

/* ---- L59: pthread_create, counted, with an altstack per thread -----------
 * Two jobs, both in service of the SIGSEGV dump:
 *   1. count live threads, so the dump carries `live=` at the fault instant
 *      without asking the kernel (the kernel's [THRS]/[SIGX] rows are the
 *      authoritative cross-check);
 *   2. arm an alternate signal stack in every new thread.  A stack overflow
 *      is the one fault a handler can only report from an altstack, and Linux
 *      does not inherit the main thread's across pthread_create -- §45.8's
 *      death at ~410 s is exactly the shape a thread-stack overflow takes
 *      when it has none (no frame, so no handler, so no dump at all).
 *
 * The altstacks come from a fixed pool (64 x 32 KiB, BSS) with a free list, so
 * a thread-pool that churns hundreds of workers costs a bounded 2 MiB rather
 * than a leak per thread.  Everything fails open: if the wrapper or the stack
 * cannot be had, the caller still gets its thread.
 */
#define RW_ALT_N 64
#define RW_ALT_SZ (32 * 1024)
static char rw_alt_mem[RW_ALT_N][RW_ALT_SZ] __attribute__((aligned(16)));
static unsigned char rw_alt_used[RW_ALT_N];
static volatile int rw_alt_lock;

struct rw_thr_start { void *(*fn)(void *); void *arg; };

static void *rw_alt_take(void) {
  while (__atomic_exchange_n(&rw_alt_lock, 1, __ATOMIC_ACQUIRE)) { }
  void *p = 0;
  for (int i = 0; i < RW_ALT_N; i++)
    if (!rw_alt_used[i]) { rw_alt_used[i] = 1; p = rw_alt_mem[i]; break; }
  __atomic_store_n(&rw_alt_lock, 0, __ATOMIC_RELEASE);
  return p;
}

static void rw_alt_give(void *p) {
  if (!p) return;
  int idx = (int)(((char *)p - (char *)rw_alt_mem) / RW_ALT_SZ);
  if (idx < 0 || idx >= RW_ALT_N) return;
  while (__atomic_exchange_n(&rw_alt_lock, 1, __ATOMIC_ACQUIRE)) { }
  rw_alt_used[idx] = 0;
  __atomic_store_n(&rw_alt_lock, 0, __ATOMIC_RELEASE);
}

static void rw_thr_cleanup(void *p) {
  if (p) {
    stack_t d;
    d.ss_sp = 0;
    d.ss_size = 0;
    d.ss_flags = SS_DISABLE;
    sigaltstack(&d, 0);
    rw_alt_give(p);
  }
  __atomic_sub_fetch(&rw_live_threads, 1, __ATOMIC_SEQ_CST);
}

static void *rw_thr_tramp(void *v) {
  struct rw_thr_start *s = (struct rw_thr_start *)v;
  void *(*fn)(void *) = s->fn;
  void *arg = s->arg;
  free(s);
  void *alt = rw_alt_take();
  if (alt) {
    stack_t ss;
    ss.ss_sp = alt;
    ss.ss_size = RW_ALT_SZ;
    ss.ss_flags = 0;
    sigaltstack(&ss, 0);
  }
  void *r = 0; /* declared outside the push block: the macro opens a scope */
  pthread_cleanup_push(rw_thr_cleanup, alt);
  r = fn(arg);
  pthread_cleanup_pop(1); /* runs the cleanup: count + altstack back to pool */
  return r;
}
int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*fn)(void *), void *arg) {
  typedef int (*cr_t)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
  static cr_t real;
  if (!real)
    real = (cr_t)dlvsym(RTLD_NEXT, "pthread_create", "GLIBC_2.2.5");
  if (!real) {
    fprintf(stderr, "[RWMAIN] FATAL: pthread_create not resolvable via RTLD_NEXT\n");
    return 11;
  }
  struct rw_thr_start *s = (struct rw_thr_start *)malloc(sizeof *s);
  if (!s) return real(t, a, fn, arg); /* uncounted, but the thread still runs */
  s->fn = fn;
  s->arg = arg;
  __atomic_add_fetch(&rw_live_threads, 1, __ATOMIC_SEQ_CST);
  int created = __atomic_add_fetch(&rw_created_threads, 1, __ATOMIC_SEQ_CST);
  int rc = real(t, a, rw_thr_tramp, s);
  if (rc != 0) {
    __atomic_sub_fetch(&rw_live_threads, 1, __ATOMIC_SEQ_CST);
    free(s);
    fprintf(stderr, "[RWMAIN] threads: pthread_create rc=%d live=%d created=%d\n",
            rc, (int)rw_live_threads, created);
    return rc;
  }
  /* One row every 8th live crossing: a 300+ thread climb is visible in the
     serial without printing a line per create. */
  int live = __atomic_load_n(&rw_live_threads, __ATOMIC_RELAXED);
  {
    static int last;
    if (live != last && (live > 64 || (live & 7) == 0)) {
      last = live;
      fprintf(stderr, "[RWMAIN] threads live=%d created=%d\n", live, created);
    }
  }
  return 0;
}

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

/* L54: pthread_cond_wait/timedwait live in tools/rw/mono-probe/condtrace.c.
 * The two definitions that used to be here resolved
 *   dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.2.5")
 * which is libpthread's `__pthread_cond_wait_2_0` compat entry: it reads
 * *(void**)cond and, when the first word is zero (every freshly calloc'd
 * MonoW32Handle cond is), calloc()s a private 0x30-byte "2.0" cond, stores
 * that pointer into the REAL cond's first word and parks on the private one.
 * Mono's pthread_cond_signal/broadcast are not interposed, so they operate on
 * the real cond -- the waiter and the waker end up on two different futex
 * keys, and an infinite wait never returns.  condtrace uses GLIBC_2.3.2 (the
 * ABI this pinned libpthread exports as default) for all four functions, so
 * both sides see the same cond. */

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

static int tp_ofd = 2; /* stderr; /TPLATE redirects to a file (see below) */
static int tp_late_only; /* /TPLATE: minimal sampler, no Mono object walks */
static void tp_w(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  if (n) (void)!write(tp_ofd, s, n);
}
static void tp_wi(long v) {
  char t[24], b[24];
  int i = 0, j = 0;
  if (v < 0) { tp_w("-"); v = -v; }
  if (!v) t[j++] = '0';
  while (v) { t[j++] = (char)('0' + (v % 10)); v /= 10; }
  while (j) b[i++] = t[--j];
  (void)!write(tp_ofd, b, (size_t)i);
}
static void tp_wx(unsigned long v) {
  char t[20], b[22];
  int i = 0, j = 0;
  b[i++] = '0'; b[i++] = 'x';
  if (!v) t[j++] = '0';
  while (v) { int d = (int)(v & 15); t[j++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
  while (j) b[i++] = t[--j];
  (void)!write(tp_ofd, b, (size_t)i);
}

/* M6/L72: THE PROBE'S OWN FILE.
 *
 * Everything the launcher writes to fd 2 lands in Player.log, and Player.log's
 * tail is exactly where L69's leftover write defect bites: one FAT sector
 * (657766 in this run) reads back as zeros on every RMW, so the log is a NUL
 * hole after ~183 KiB and `[FATW] ZERORD` repeats once a second.  The park's
 * stack samples are written at t>=400 s, i.e. always past the hole, and the
 * only copy of the three-thread table was lost that way in two runs.  /TPLATE
 * therefore redirects the probe to its own file, opened once, written with
 * plain write() and never interleaved with Unity's buffered log writer. */
static void tp_out_open(void) {
  if (tp_ofd != 2)
    return;
  int f = open("/RW_HOME/l72.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (f >= 0)
    tp_ofd = f;
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
  void *compile;
  void *(*m_get_class)(void *);
  void *(*invoke)(void *, void *, void **, void **);
  const char *(*c_get_name)(void *);
  const char *(*m_get_name)(void *);
  void *orig_q, *orig_c, *orig_r;
  void *m_q, *m_c, *m_r;
  long enq, comp, req, reqf, seq, wtn, setn;
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
/* L49: the REAL signature.  mscorlib declares
 *   [MethodImpl(InternalCall)] static extern int Wait_internal(IntPtr* handles,
 *                                       int numHandles, bool waitAll, int ms);
 * so the first argument is a POINTER to an array of handles, not a handle.
 * L48's hook declared (long handle, unsigned ms, long, long, void*) -- it read
 * `h` as the handle and `ms` as numHandles, i.e. it would have printed garbage
 * on the very first call.  This one is a pure pass-through: six registers in,
 * rax out. */
static long (*tp_orig_w)(void *h, long n, long all, long ms, void *a5, void *a6);
static int (*tp_orig_s)(void *h);
static long tp_w_ret, tp_w_fin;
static volatile unsigned long tp_w_key;

/* A poor man's managed stack: scan the raw stack for return addresses that
 * Mono's JIT info table resolves to a method, keeping only words preceded by
 * a `call rel32` (0xE8) -- the same filter that turned a page of stack
 * garbage into a real chain in §33/L46.  This is what the broken SIGQUIT dump
 * cannot give us: the managed caller of a wait, in-process, on demand. */
static int tp_bt_n(const char *tag, int tid, long extra, int depth) {
  unsigned long sp = 0;
  __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
  if (!tp.jit_find || !tp.domain_get) return 0;
  void *dom = tp.domain_get();
  if (!dom) return 0;
  int printed = 0;
  unsigned long *p = (unsigned long *)sp;
  for (int i = 0; i < 1536 && printed < depth; i++) {
    if (!rw_readable((unsigned long)&p[i])) break;
    unsigned long v = p[i];
    if (v < 0x100000 || v > 0x7fffffffffffUL) continue;
    /* The E8 filter is NOT enough here: this JIT reaches managed methods
       through indirect calls (`call *%rax`, FF /2), so a real return address
       is preceded by 0xD0..0xD7 or a disp8 form, and L48's E8-only scan found
       0 frames on the game's own stack while it reported 7 enqueues.  The JIT
       info table IS the filter: it answers only for addresses inside a JIT'd
       method. */
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
static int tp_bt(const char *tag, int tid, long extra) __attribute__((unused));
static int tp_bt(const char *tag, int tid, long extra) { return tp_bt_n(tag, tid, extra, 6); }

static int tp_hook_w(void *h, long nh, long all, long ms, void *a5, void *a6) {
  int tid = tp_tid();
  int n = (int)__sync_fetch_and_add(&tp.wtn, 1);
  /* Handles[0]: wait_one passes &waitableSafeHandle, i.e. a one-element array. */
  void *h0 = 0;
  if (h && rw_readable((unsigned long)h)) h0 = *(void **)h;
  if (n == 0) {
    tp_w("[TPQW] FIRST Wait_internal h=0x");
    tp_wx((unsigned long)h0);
    tp_w(" n=");
    tp_wi(nh);
    tp_w(" all=");
    tp_wi(all);
    tp_w(" ms=");
    tp_wi(ms);
    tp_w(" tid=");
    tp_wi(tid);
    tp_w(" key=0x");
    tp_wx((unsigned long)h0 + 0x38);
    tp_w("\n");
  }
  int infinite = (ms < 0 || ms == 0xFFFFFFFFL);
  /* Only the interesting waits pay for a stack walk: infinite ones (the
     stall) and the first few of everything else. */
  if (infinite && tp.wbt_inf < 8) {
    tp.wbt_inf++;
    tp_w("[TPQW] park tid=");
    tp_wi(tid);
    tp_w(" n=");
    tp_wi(nh);
    tp_w(" h0=0x");
    tp_wx((unsigned long)h0);
    tp_w(" key=0x");
    tp_wx((unsigned long)h0 + 0x38);
    tp_w(" seq=");
    tp_wi(n);
    tp_w("\n");
    tp_bt_n("[TPQW]", tid, -1, 12);
  } else if (infinite) {
    tp_w("[TPQW] park tid=");
    tp_wi(tid);
    tp_w(" h0=0x");
    tp_wx((unsigned long)h0);
    tp_w(" seq=");
    tp_wi(n);
    tp_w("\n");
  }
  if (h0) tp_w_key = (unsigned long)h0 + 0x38;
  long r = tp_orig_w ? tp_orig_w(h, nh, all, ms, a5, a6) : 0;
  tp_w_ret++;
  if (infinite) {
    tp_w_fin++;
    tp_w("[TPQW] woke tid=");
    tp_wi(tid);
    tp_w(" ms=");
    tp_wi(ms);
    tp_w(" r=");
    tp_wi(r);
    tp_w("\n");
  }
  /* One frame above the patched call site inside WaitHandle.WaitOne. */
  void *ra1 = __builtin_return_address(1);
  int slot = -1;
  for (int i = 0; i < tp_w_n && i < TPW_MAX; i++)
    if (tp_waits[i].ra1 == ra1) { slot = i; break; }
  if (slot < 0 && tp_w_n < TPW_MAX) { slot = tp_w_n; tp_w_n = slot + 1; tp_waits[slot].ra1 = ra1; tp_waits[slot].n = 0; }
  if (slot >= 0) {
    tp_waits[slot].n++;
    tp_waits[slot].last_ms = (unsigned)ms;
    tp_waits[slot].last_tid = tid;
  }
  return r;
}

/* Every managed EventWaitHandle.Set() lands here (NativeEventCalls.SetEvent ->
 * SetEvent_internal -> mono_w32handle_set).  This is the "did the completion
 * signal get ISSUED at all" half of the question, in the waiter's own terms:
 * the row carries the handle value, so it can be compared with the wait key
 * (handle + 0x38) without decoding any native frame. */
static int tp_hook_s(void *h) {
  long n = __sync_fetch_and_add(&tp.setn, 1);
  if (n < 32 || (n % 128) == 0) {
    tp_w("[TPQS] Set h=0x");
    tp_wx((unsigned long)h);
    tp_w(" key=0x");
    tp_wx((unsigned long)h + 0x38);
    tp_w(" tid=");
    tp_wi(tp_tid());
    tp_w(" seq=");
    tp_wi(n);
    tp_w("\n");
  }
  return tp_orig_s ? tp_orig_s(h) : 1;
}

/* Rewrite every call site in `code` that targets `orig` so it targets `hook`.
 * Mono's amd64 JIT reaches an icall either by `call rel32` (E8) or by
 * `movabs $imm64,%r11; call *%r11`; both forms carry the address literally,
 * which is why the JIT's cached binding is patchable even when it was made
 * long before mono_add_internal_call().  Returns the number of sites. */
static int tp_patch_sites(const char *tag, void *code, void *orig, void *hook) {
  if (!code || !orig || !hook) return 0;
  unsigned char *c = (unsigned char *)code;
  int sites = 0;
  for (int i = 0; i + 10 < 4096; i++) {
    if (c[i] == 0xE8) {
      int32_t rel;
      memcpy(&rel, c + i + 1, 4);
      unsigned long tgt = (unsigned long)(c + i + 5) + (long)rel;
      if (tgt == (unsigned long)orig) {
        int32_t nrel = (int32_t)((long)((unsigned char *)hook - (c + i + 5)));
        memcpy(c + i + 1, &nrel, 4);
        sites++;
      }
    } else if ((c[i] == 0x48 || c[i] == 0x49) && c[i + 1] >= 0xB8 && c[i + 1] <= 0xBF) {
      unsigned long imm;
      memcpy(&imm, c + i + 2, 8);
      if (imm == (unsigned long)orig) {
        memcpy(c + i + 2, &hook, 8);
        sites++;
      }
    }
  }
  tp_w("[TPQP] tag=");
  tp_w(tag);
  tp_w(" code=0x");
  tp_wx((unsigned long)code);
  tp_w(" orig=0x");
  tp_wx((unsigned long)orig);
  tp_w(" hook=0x");
  tp_wx((unsigned long)hook);
  tp_w(" sites=");
  tp_wi(sites);
  tp_w(" calls=");
  {
    /* Every direct call in the body, so an icall reached by an encoding this
       patcher does not know is still visible as "call -> 0x...". */
    int shown = 0;
    for (int i = 0; i + 5 <= 512 && shown < 8; i++) {
      if (c[i] != 0xE8) continue;
      int32_t rel;
      memcpy(&rel, c + i + 1, 4);
      tp_w("+");
      tp_wi(i);
      tp_w(":0x");
      tp_wx((unsigned long)(c + i + 5) + (long)rel);
      tp_w(" ");
      shown++;
    }
  }
  tp_w(" bytes=");
  for (int i = 0; i < 64; i++) { tp_wx((unsigned long)c[i]); tp_w(" "); }
  tp_w("\n");
  return sites;
}

static void *tp_patch_method(void *klass, const char *name, int arity,
                             void *orig, void *hook, const char *tag) {
  if (!klass || !tp.compile) return 0;
  void *m = tp.method_from_name(klass, name, arity);
  if (!m) return 0;
  void *code = ((void *(*)(void *))tp.compile)(m);
  if (!code) return 0;
  char full[64];
  int i = 0;
  for (const char *s = tag; *s && i < 40; s++) full[i++] = *s;
  full[i++] = '/';
  for (const char *s = name; *s && i < 58; s++) full[i++] = *s;
  full[i++] = (char)('0' + (arity % 10));
  full[i] = 0;
  tp_patch_sites(full, code, orig, hook);
  return code;
}

/* ---- L50: the managed state of an awaited Task, read in place -----------
 *
 * 36.6 named the *wait* (`Task.SpinThenBlockingWait` on a completion
 * ManualResetEventSlim that is never set) but not the task it waits for.
 * Status, scheduler and continuation list are private managed fields, so they
 * are read through Mono's own metadata API rather than at guessed offsets:
 * mono_class_get_field_from_name + mono_field_get_value, i.e. the fields this
 * mscorlib really has (tools/rw/mono-probe/taskinv.cs prints the inventory).
 *
 * The object is found WITHOUT unwinding: the parked thread's stack still holds
 * `this` for Task.SpinThenBlockingWait and the SetOnInvokeMres local.  A word
 * is a Task iff *(void**)word == the Task MonoVTable -- an exact type test --
 * and every deref is guarded by rw_readable(), the same mincore-backed test the
 * stack capture already depends on.
 *
 * The same scan on a Thread Pool Worker's stack answers the other open row of
 * 36.8: `Thread.Join`'s target.  The Thread object on that stack carries
 * internal_thread.thread_id / state / name, so "the target exited", "the target
 * never started" and "the target is running" are different rows.
 */
/* Mono metadata entry points used by the L50 task/thread introspection.
 * Kept separate from `tp` because they are pure reads and stay available even
 * when the icall hooks above failed to bind. */
static struct {
  void *(*class_vtable)(void *, void *);
  void *(*field_from_name)(void *, const char *);
  void (*field_get_value)(void *, void *, void *);
  void (*field_static_get_value)(void *, void *, void *);
  int (*array_length)(void *);
  char *(*array_addr)(void *, int, unsigned long);
  void *(*obj_get_class)(void *);
  void *(*nested)(void *, void **);
  const char *(*class_namespace)(void *);
} mo;

static int tp_bt_words(const char *tag, unsigned long base, int n);

struct tpcls {
  void *task, *f_id, *f_flags, *f_sched, *f_cont, *f_cp, *f_action;
  void *cp, *f_ev, *f_countdown;
  void *mres, *f_comb, *f_lock;
  void *monitor;
  void *thr, *f_it;
  void *it, *f_tid, *f_state, *f_nchars, *f_nlen, *f_mid, *f_started;
  void *vt_task, *vt_thr, *vt_mres;
  int state; /* 1 = ready, -1 = gave up */
};
static struct tpcls tpc;

static void *tp_nested_cls(void *klass, const char *name) {
  if (!klass || !mo.nested) return 0;
  void *iter = 0;
  for (int i = 0; i < 32; i++) {
    void *k = mo.nested(klass, &iter);
    if (!k) break;
    const char *n = tp.c_get_name(k);
    if (n && !strcmp(n, name)) return k;
  }
  return 0;
}

static long tp_f_i4(void *f, void *o) {
  int v = 0;
  if (!f || !o) return 0;
  mo.field_get_value(o, f, &v);
  return (long)v;
}
static long tp_f_i8(void *f, void *o) {
  long v = 0;
  if (!f || !o) return 0;
  mo.field_get_value(o, f, &v);
  return v;
}
static void *tp_f_p(void *f, void *o) {
  void *v = 0;
  if (!f || !o) return 0;
  mo.field_get_value(o, f, &v);
  return v;
}
static const char *tp_obj_cls(void *o) {
  if (!o || !mo.obj_get_class) return 0;
  void *k = mo.obj_get_class(o);
  return k ? tp.c_get_name(k) : 0;
}

static int tp_cls_init(void) {
  if (tpc.state) return tpc.state > 0;
  tpc.state = -1;
  if (!tp.class_from_name || !mo.field_from_name || !mo.field_get_value || !mo.obj_get_class) return 0;
  void *img = tp.image_loaded("mscorlib");
  if (!img) return 0;
  void *task = tp.class_from_name(img, "System.Threading.Tasks", "Task");
  if (!task) return 0;
  tpc.task = task;
  tpc.f_id = mo.field_from_name(task, "m_taskId");
  tpc.f_flags = mo.field_from_name(task, "m_stateFlags");
  tpc.f_sched = mo.field_from_name(task, "m_taskScheduler");
  tpc.f_cont = mo.field_from_name(task, "m_continuationObject");
  tpc.f_cp = mo.field_from_name(task, "m_contingentProperties");
  tpc.f_action = mo.field_from_name(task, "m_action");
  tpc.cp = tp_nested_cls(task, "ContingentProperties");
  if (tpc.cp) {
    tpc.f_ev = mo.field_from_name(tpc.cp, "m_completionEvent");
    tpc.f_countdown = mo.field_from_name(tpc.cp, "m_completionCountdown");
  }
  void *mres = tp.class_from_name(img, "System.Threading", "ManualResetEventSlim");
  if (mres) {
    tpc.mres = mres;
    tpc.f_comb = mo.field_from_name(mres, "m_combinedState");
    tpc.f_lock = mo.field_from_name(mres, "m_lock");
  }
  tpc.monitor = tp.class_from_name(img, "System.Threading", "Monitor");
  void *thr = tp.class_from_name(img, "System.Threading", "Thread");
  if (thr) {
    tpc.thr = thr;
    tpc.f_it = mo.field_from_name(thr, "internal_thread");
    tpc.f_started = mo.field_from_name(thr, "started");
  }
  void *it = tp.class_from_name(img, "System.Threading", "InternalThread");
  if (it) {
    tpc.it = it;
    tpc.f_tid = mo.field_from_name(it, "thread_id");
    tpc.f_state = mo.field_from_name(it, "state");
    tpc.f_nchars = mo.field_from_name(it, "name_chars");
    tpc.f_nlen = mo.field_from_name(it, "name_length");
    tpc.f_mid = mo.field_from_name(it, "managed_id");
  }
  void *dom = tp.domain_get ? tp.domain_get() : 0;
  if (dom && mo.class_vtable) {
    tpc.vt_task = mo.class_vtable(dom, task);
    if (thr) tpc.vt_thr = mo.class_vtable(dom, thr);
    void *mres_sub = tp_nested_cls(task, "SetOnInvokeMres");
    if (mres_sub) tpc.vt_mres = mo.class_vtable(dom, mres_sub);
  }
  tpc.state = (tpc.f_flags && tpc.f_sched && tpc.vt_task) ? 1 : -1;
  /* One inventory row: a field name that does not resolve here would make
     every later row read "sched=null", i.e. look like an answer. */
  tp_w("[TPWI] Task id=");
  tp_wx((unsigned long)tpc.f_id);
  tp_w(" flags=");
  tp_wx((unsigned long)tpc.f_flags);
  tp_w(" sched=");
  tp_wx((unsigned long)tpc.f_sched);
  tp_w(" cont=");
  tp_wx((unsigned long)tpc.f_cont);
  tp_w(" cp=");
  tp_wx((unsigned long)tpc.f_cp);
  tp_w(" action=");
  tp_wx((unsigned long)tpc.f_action);
  tp_w(" CP.ev=");
  tp_wx((unsigned long)tpc.f_ev);
  tp_w(" CP.cd=");
  tp_wx((unsigned long)tpc.f_countdown);
  tp_w(" Thread.it=");
  tp_wx((unsigned long)tpc.f_it);
  tp_w(" IT.tid=");
  tp_wx((unsigned long)tpc.f_tid);
  tp_w(" vt_task=0x");
  tp_wx((unsigned long)tpc.vt_task);
  tp_w(" vt_thr=0x");
  tp_wx((unsigned long)tpc.vt_thr);
  tp_w(" ok=");
  tp_wi(tpc.state > 0);
  tp_w("\n");
  return tpc.state > 0;
}

/* ---- L53: the managed ThreadPoolWorkQueue's OWN queues -------------------
 *
 * The L52 dump exonerated the pool by its trace rows (14 retired workers,
 * 0 failed requests, all CPUs idle) but not by its *state*: Mono's pool is a
 * per-thread work-stealing deque (WorkStealingQueue) plus one global
 * QueueSegment chain, and a continuation that was LocalPush()ed into a
 * worker's deque is only discoverable there.  A worker that exits without
 * draining its deque orphans exactly those items -- no global request is
 * made for them, which is what "empty pool, 0 failed requests" would look
 * like.
 *
 * The classes are read with the same metadata API as the Task dump (rule 102)
 * and the field names below are this mscorlib's own (measured on the host
 * under the game's libmonobdwgc with tools/rw/mono-probe/tpq*.cs):
 *
 *   ThreadPoolWorkQueue: queueHead, queueTail (QueueSegment),
 *                        allThreadQueues (SparseArray<WorkStealingQueue>),
 *                        numOutstandingThreadRequests
 *   ThreadPoolWorkQueue+WorkStealingQueue:
 *                        m_array, m_mask, m_headIndex, m_tailIndex
 *   ThreadPoolWorkQueue+QueueSegment:
 *                        nodes, indexes (lo16=head, hi16=tail), Next
 *
 * Semantics (validated by tpq3.cs: three Task enqueues from a worker moved
 * tail 0->3 and the steals moved head 0->3):
 *   used = tail - head        items currently in that worker's deque
 *   m_headIndex               cumulative STEALS (and TryDequeue) from it
 *   m_tailIndex               LocalPush count minus LocalPop count
 *
 * A queue with used>0 and no live owner (owner=-1, matched by finding the
 * ThreadPoolWorkQueueThreadLocals object on that worker's stack) is an
 * orphan.  numOutstandingThreadRequests is the other half: a counter stuck
 * above 0 means EnsureThreadRequested() will never ask for another worker.
 */
static struct {
  void *wpq, *f_allq, *f_qhead, *f_qtail, *f_numout, *vt_wpq;
  void *wsq, *f_arr, *f_mask, *f_head, *f_tail;
  void *sa, *f_sa_arr;
  void *seg, *f_nodes, *f_indexes, *f_next;
  void *tl, *f_tl_wsq, *vt_tl;
  void *globals, *f_workqueue, *vt_globals;
  int state;
  int nq;
  void *q[16];
  int own[16], qhead[16], qtail[16], qused[16], qcap[16];
  int prev_ok[16], prev_head[16], prev_tail[16];
  void *prev_q[16];
} twq;

static void *twq_arr_get(void *arr, int i) {
  if (!arr || !mo.array_addr) return 0;
  char *p = mo.array_addr(arr, 8, (unsigned long)i);
  return p ? *(void **)p : 0;
}

static int twq_init(void) {
  if (twq.state) return twq.state > 0;
  twq.state = -1;
  if (!tp.class_from_name || !tp.image_loaded || !mo.field_static_get_value ||
      !mo.array_length || !mo.array_addr)
    return 0;
  void *img = tp.image_loaded("mscorlib");
  if (!img) return 0;
  void *wpq = tp.class_from_name(img, "System.Threading", "ThreadPoolWorkQueue");
  if (!wpq) return 0;
  twq.wpq = wpq;
  twq.f_allq = mo.field_from_name(wpq, "allThreadQueues");
  twq.f_qhead = mo.field_from_name(wpq, "queueHead");
  twq.f_qtail = mo.field_from_name(wpq, "queueTail");
  twq.f_numout = mo.field_from_name(wpq, "numOutstandingThreadRequests");
  twq.wsq = tp_nested_cls(wpq, "WorkStealingQueue");
  if (twq.wsq) {
    twq.f_arr = mo.field_from_name(twq.wsq, "m_array");
    twq.f_mask = mo.field_from_name(twq.wsq, "m_mask");
    twq.f_head = mo.field_from_name(twq.wsq, "m_headIndex");
    twq.f_tail = mo.field_from_name(twq.wsq, "m_tailIndex");
  }
  twq.seg = tp_nested_cls(wpq, "QueueSegment");
  if (twq.seg) {
    twq.f_nodes = mo.field_from_name(twq.seg, "nodes");
    twq.f_indexes = mo.field_from_name(twq.seg, "indexes");
    twq.f_next = mo.field_from_name(twq.seg, "Next");
  }
  twq.sa = tp_nested_cls(wpq, "SparseArray`1");
  if (twq.sa) twq.f_sa_arr = mo.field_from_name(twq.sa, "m_array");
  void *tl = tp.class_from_name(img, "System.Threading", "ThreadPoolWorkQueueThreadLocals");
  if (tl) {
    twq.tl = tl;
    twq.f_tl_wsq = mo.field_from_name(tl, "workStealingQueue");
    void *dom = tp.domain_get ? tp.domain_get() : 0;
    if (dom && mo.class_vtable) twq.vt_tl = mo.class_vtable(dom, tl);
  }
  void *g = tp.class_from_name(img, "System.Threading", "ThreadPoolGlobals");
  if (g) {
    twq.globals = g;
    twq.f_workqueue = mo.field_from_name(g, "workQueue");
  }
  twq.state = (twq.f_allq && twq.f_qhead && twq.f_qtail && twq.wsq && twq.f_arr &&
               twq.f_head && twq.f_tail && twq.seg && twq.f_indexes && twq.f_next)
                  ? 1 : -1;
  void *dom = tp.domain_get ? tp.domain_get() : 0;
  if (dom && mo.class_vtable && twq.state > 0) {
    twq.vt_wpq = mo.class_vtable(dom, wpq);
    if (g) twq.vt_globals = mo.class_vtable(dom, g);
  }
  if (twq.state > 0 && (!twq.vt_wpq || (g && !twq.vt_globals)))
    twq.state = -1;
  if (twq.state > 0) {
    tp_w("[TPWP] inventory wpq=1 wsq=1 seg=1 allq=1 head/tail=1 tl=");
    tp_wi(tl != 0);
    tp_w(" globals=");
    tp_wi(g != 0);
    tp_w("\n");
  } else {
    tp_w("[TPWP] inventory INCOMPLETE (state=-1)\n");
  }
  return twq.state > 0;
}

/* One snapshot of every registered local deque.  Owner is filled later by
 * twq_owner_scan() from each thread's captured stack. */
static void twq_scan(void) {
  twq.nq = 0;
  if (!twq_init()) return;
  void *sparse = 0;
  mo.field_static_get_value(twq.vt_wpq, twq.f_allq, &sparse);
  if (!sparse) { tp_w("[TPWP] allThreadQueues=null\n"); return; }
  /* The field is declared on SparseArray<T>; ask the INSTANCE's class so the
     generic instantiation resolves the field, not just the type definition. */
  void *scls = mo.obj_get_class ? mo.obj_get_class(sparse) : 0;
  void *farr = scls ? mo.field_from_name(scls, "m_array") : twq.f_sa_arr;
  if (!farr) farr = twq.f_sa_arr;
  void *arr = farr ? tp_f_p(farr, sparse) : 0;
  int len = arr ? mo.array_length(arr) : 0;
  if (len > 16) len = 16;
  for (int i = 0; i < len; i++) {
    void *q = twq_arr_get(arr, i);
    if (!q) continue;
    int k = twq.nq++;
    twq.q[k] = q;
    twq.own[k] = -1;
    twq.qhead[k] = (int)tp_f_i4(twq.f_head, q);
    twq.qtail[k] = (int)tp_f_i4(twq.f_tail, q);
    twq.qused[k] = twq.qtail[k] - twq.qhead[k];
    void *qa = tp_f_p(twq.f_arr, q);
    twq.qcap[k] = qa ? mo.array_length(qa) : -1;
  }
}

/* Map queue -> owner tid by finding the ThreadPoolWorkQueueThreadLocals object
 * on the worker's own captured stack (exact vtable test, rule 102). */
static void twq_owner_scan(int tid) {
  if (!twq_init() || !twq.vt_tl || !twq.f_tl_wsq) return;
  for (int i = 0; i < rw_stk_n; i++) {
    unsigned long v = rw_stk_buf[i];
    if (v < 0x10000000UL || v > 0x1800000000UL || (v & 7)) continue;
    if (!rw_readable(v)) continue;
    if (*(volatile void **)v != twq.vt_tl) continue;
    void *w = tp_f_p(twq.f_tl_wsq, (void *)v);
    if (!w) continue;
    int hit = 0;
    for (int k = 0; k < twq.nq; k++)
      if (twq.q[k] == w) { twq.own[k] = tid; hit = 1; }
    tp_w("[TPWO] tid=");
    tp_wi(tid);
    tp_w(" tloc=0x");
    tp_wx(v);
    tp_w(" q=0x");
    tp_wx((unsigned long)w);
    tp_w(" match=");
    tp_wi(hit);
    tp_w("\n");
  }
}

static void twq_dump(const char *why) {
  if (!twq_init()) return;
  int nonempty = 0, items = 0, unowned_items = 0;
  for (int k = 0; k < twq.nq; k++) {
    if (twq.qused[k] > 0) {
      nonempty++;
      items += twq.qused[k];
      if (twq.own[k] < 0) unowned_items += twq.qused[k];
    }
  }
  /* Global QueueSegment chain.  queueHead/queueTail/numOutstandingThreadRequests
     are INSTANCE fields of ThreadPoolWorkQueue; the instance is the static
     ThreadPoolGlobals.workQueue (mono_field_static_get_value aborts on an
     instance field -- measured object.c:3874). */
  void *wq = 0;
  if (twq.globals && twq.f_workqueue)
    mo.field_static_get_value(twq.vt_globals, twq.f_workqueue, &wq);
  int gdepth = 0, segs = 0;
  void *s = wq ? tp_f_p(twq.f_qhead, wq) : 0;
  for (int guard = 0; s && guard < 64; guard++) {
    int idx = (int)tp_f_i4(twq.f_indexes, s);
    int lo = idx & 0xffff, hi = (idx >> 16) & 0xffff;
    int d = hi - lo;
    if (d > 0) { gdepth += d; segs++; }
    void *nodes = tp_f_p(twq.f_nodes, s);
    tp_w("[TPWG] i=");
    tp_wi(guard);
    tp_w(" seg=0x");
    tp_wx((unsigned long)s);
    tp_w(" lo=");
    tp_wi(lo);
    tp_w(" hi=");
    tp_wi(hi);
    tp_w(" d=");
    tp_wi(d);
    tp_w(" nodes=");
    tp_wx((unsigned long)nodes);
    tp_w("\n");
    s = tp_f_p(twq.f_next, s);
  }
  int outreq = -1;
  if (wq && twq.f_numout) outreq = (int)tp_f_i4(twq.f_numout, wq);
  tp_w("[TPWP] why=");
  tp_w(why);
  tp_w(" n=");
  tp_wi(twq.nq);
  tp_w(" nonempty=");
  tp_wi(nonempty);
  tp_w(" items=");
  tp_wi(items);
  tp_w(" unowned_items=");
  tp_wi(unowned_items);
  tp_w(" glob=");
  tp_wi(gdepth);
  tp_w(" outreq=");
  tp_wi(outreq);
  tp_w("\n");
  for (int k = 0; k < twq.nq; k++) {
    int dh = 0, dt = 0, have = 0;
    for (int p = 0; p < 16; p++)
      if (twq.prev_ok[p] && twq.prev_q[p] == twq.q[k]) {
        dh = twq.qhead[k] - twq.prev_head[p];
        dt = twq.qtail[k] - twq.prev_tail[p];
        have = 1;
      }
    tp_w("[TPWL] i=");
    tp_wi(k);
    tp_w(" q=0x");
    tp_wx((unsigned long)twq.q[k]);
    tp_w(" head=");
    tp_wi(twq.qhead[k]);
    tp_w(" tail=");
    tp_wi(twq.qtail[k]);
    tp_w(" used=");
    tp_wi(twq.qused[k]);
    tp_w(" cap=");
    tp_wi(twq.qcap[k]);
    tp_w(" owner=");
    tp_wi(twq.own[k]);
    if (have) {
      tp_w(" dhead=");
      tp_wi(dh);
      tp_w(" dtail=");
      tp_wi(dt);
    }
    if (twq.qused[k] > 0) {
      void *qa = tp_f_p(twq.f_arr, twq.q[k]);
      int mask = (int)tp_f_i4(twq.f_mask, twq.q[k]);
      int shown = 0;
      tp_w(" itemcls=");
      for (int i = twq.qhead[k]; i < twq.qtail[k] && shown < 4; i++) {
        void *it = twq_arr_get(qa, i & mask);
        const char *cn = it ? tp_obj_cls(it) : 0;
        tp_w(shown ? "," : "");
        tp_w(cn ? cn : (it ? "?" : "null"));
        shown++;
      }
    }
    tp_w("\n");
  }
  /* This snapshot becomes the baseline for the next [TPWL] dhead/dtail. */
  for (int k = 0; k < 16; k++) twq.prev_ok[k] = 0;
  for (int k = 0; k < twq.nq && k < 16; k++) {
    twq.prev_ok[k] = 1;
    twq.prev_q[k] = twq.q[k];
    twq.prev_head[k] = twq.qhead[k];
    twq.prev_tail[k] = twq.qtail[k];
  }
}

/* ---- L53: the reverse monitor scan -------------------------------------
 *
 * [TPWM] dumps the monitor of the ManualResetEventSlim's m_lock, and at the
 * stall that monitor has nwl=0 while the thread is plainly inside
 * Monitor.ObjWait.  Either the waiter is on a DIFFERENT monitor (nested PLINQ
 * has several) or the dumped object is not the waited-on one.  So find the
 * monitor by its OWN wait_list: walk the captured stack, and for each word V
 * that is a managed object with an INFLATED lock word, read its
 * MonoThreadsSync and print it only when wait_list != NULL.  That is the
 * monitor a parked thread is actually queued on -- no guessing which MRES is
 * live.
 *
 * The same pass prints the native return addresses at the top of the stack
 * ([TPWR]), which is what distinguishes mono_monitor_wait from
 * mono_w32handle_wait_one/mono_wait_uninterrupted: both end in the same
 * pthread_cond_wait, but their callers are different libmono offsets. */
static void tp_native_first(int tid, int n) {
  if (!rw_mod_n) rw_mods_collect();
  int shown = 0;
  for (int i = 0; i < rw_stk_n && i < n; i++) {
    unsigned long v = rw_stk_buf[i];
    if (v < 0x100000UL) continue;
    unsigned long off = 0;
    const char *m = rw_mod_of(v, &off);
    if (!m) continue;
    tp_w("[TPWR] tid=");
    tp_wi(tid);
    tp_w(" i=");
    tp_wi(i);
    tp_w(" ");
    tp_w(m);
    tp_w("+0x");
    tp_wx(off);
    tp_w("\n");
    shown++;
  }
  if (!shown) {
    tp_w("[TPWR] tid=");
    tp_wi(tid);
    tp_w(" no module address in the first ");
    tp_wi(n);
    tp_w(" words\n");
  }
}

static void tp_scan_locks(int tid) {
  if (!mo.obj_get_class || !tpc.state) return;
  int shown = 0;
  for (int i = 0; i < rw_stk_n && shown < 8; i++) {
    unsigned long v = rw_stk_buf[i];
    if (v < 0x10000000UL || v > 0x1800000000UL || (v & 7)) continue;
    if (!rw_readable(v + 16)) continue;
    unsigned long vt = *(volatile unsigned long *)v;
    if (vt < 0x10000000UL || vt > 0x1800000000UL || !rw_readable(vt + 16)) continue;
    unsigned long sy = *(volatile unsigned long *)(v + 8);
    if ((sy & 3) != 2) continue;
    unsigned long mon = sy & ~3UL;
    if (!mon || !rw_readable(mon + 40)) continue;
    unsigned long wl = *(volatile unsigned long *)((char *)mon + 16);
    if (!wl || !rw_readable(wl + 16)) continue;
    int nwl = 0;
    unsigned long node = wl, ev = 0;
    while (node && nwl < 8 && rw_readable(node + 16)) {
      if (nwl == 0) ev = *(volatile unsigned long *)node;
      node = *(volatile unsigned long *)((char *)node + 8);
      nwl++;
    }
    if (nwl <= 0) continue;
    unsigned int st = *(volatile unsigned int *)mon;
    unsigned int nest = *(volatile unsigned int *)((char *)mon + 4);
    tp_w("[TPWX] tid=");
    tp_wi(tid);
    tp_w(" obj=0x");
    tp_wx(v);
    tp_w(" vt=0x");
    tp_wx(vt);
    tp_w(" mon=0x");
    tp_wx(mon);
    tp_w(" status=0x");
    tp_wx(st);
    tp_w(" owner=");
    tp_wi(st & 0xffff);
    tp_w(" ent=");
    tp_wi((st >> 16) & 0x7fff);
    tp_w(" nest=");
    tp_wi(nest);
    tp_w(" wl=0x");
    tp_wx(wl);
    tp_w(" nwl=");
    tp_wi(nwl);
    if (ev && rw_readable(ev + 0x40)) {
      tp_w(" ev=0x");
      tp_wx(ev);
      tp_w(" etype=");
      tp_wi(*(volatile int *)ev);
      tp_w(" sig=");
      tp_wi(*(volatile int *)((char *)ev + 8));
      tp_w(" cond=0x");
      tp_wx(ev + 0x38);
    }
    tp_w("\n");
    shown++;
  }
  if (!shown) {
    tp_w("[TPWX] tid=");
    tp_wi(tid);
    tp_w(" no inflated monitor with a non-empty wait_list on the stack\n");
  }
}

static void tp_mods_dump(void) {
  if (!rw_mod_n) rw_mods_collect();
  for (int i = 0; i < rw_mod_n; i++) {
    tp_w("[RWMOD] ");
    tp_w(rw_mods[i].name);
    tp_w(" base=0x");
    tp_wx(rw_mods[i].base);
    tp_w(" end=0x");
    tp_wx(rw_mods[i].end);
    tp_w("\n");
  }
}

/* ------------------------------------------------------------------------
 * L53: read the MonoW32Handle a parked waiter is actually blocked on.
 *
 * The native loop at libmono+0x20dd00 is mono_w32handle_wait_one: it loads
 * handle->type at +0, calls the type's wait op, then re-checks
 * handle->signalled at +8 and loops while it is 0.  The handle pointer is a
 * stack local (mov 0x8(%rsp),%rdi), so it is IN the captured stack.  A
 * plausible MonoW32Handle is {int type; u32 ref; int signalled; int in_use;}
 * with small values; print every candidate with its signalled flag.  This
 * separates "the producer never set the event" (sig=0) from "the event was
 * set and the wake was lost" (sig=1) below Mono.
 */
/* ------------------------------------------------------------------------
 * L54: the handle the parked thread is ACTUALLY blocked on.
 *
 * A glibc 2.31 pthread_cond_wait parks on the futex word
 * `cond + 0x28 + 4*g` (disassembled from the pinned libpthread: f1f5
 * `lea 0x28(%rbp,%r9,1),%r15` then the futex syscall with rdi=r15).  The
 * thread's interrupted register file carries that key in rdi, so the key no
 * longer has to be inferred from a stack scan.
 *
 * Mono's MonoW32Handle puts the cond at handle+0x38 and the mutex at
 * handle+0x10 (libmonobdwgc mono_w32handle_wait_one, 0x20dec8:
 * `lea 0x38(%rbp),%rdi; add $0x10,%rbp; mov %rbp,%rsi`).  Therefore a key
 * that belongs to a w32handle event must satisfy
 *
 *     handle = key - 0x60 - 4*g        (g = 0 or 1)
 *
 * and the 16-byte header at `handle` must decode as
 * {int type; u32 ref; int signalled; int in_use} with small sane values.
 * That is the identity test; everything else on the stack is a candidate.
 */
static int tp_hdr(unsigned long h, int *type, unsigned int *ref, int *sig,
                  int *inuse) {
  if (h < 0x1000000000UL || h > 0x1400000000UL || (h & 7)) return 0;
  if (!rw_readable(h + 0x6f)) return 0;
  *type = *(volatile int *)h;
  *ref = *(volatile unsigned int *)((char *)h + 4);
  *sig = *(volatile int *)((char *)h + 8);
  *inuse = *(volatile int *)((char *)h + 12);
  return (*type >= 1 && *type <= 16 && *ref > 0 && *ref <= 0x100000 &&
          (*sig == 0 || *sig == 1) && (*inuse == 0 || *inuse == 1));
}

static void tp_hdr_row(const char *tag, int tid, unsigned long key,
                       unsigned long h, int g) {
  int type = -1, sig = -1, inuse = -1;
  unsigned int ref = 0;
  int ok = tp_hdr(h, &type, &ref, &sig, &inuse);
  tp_w(tag);
  tp_w(" tid=");
  tp_wi(tid);
  tp_w(" key=0x");
  tp_wx(key);
  tp_w(" g=");
  tp_wi(g);
  tp_w(" h=0x");
  tp_wx(h);
  tp_w(" valid=");
  tp_wi(ok);
  tp_w(" type=");
  tp_wi(type);
  tp_w(" ref=");
  tp_wi((long)ref);
  tp_w(" sig=");
  tp_wi(sig);
  tp_w(" in_use=");
  tp_wi(inuse);
  if (ok) {
    tp_w(" mutex=0x");
    tp_wx(h + 0x10);
    tp_w(" cond=0x");
    tp_wx(h + 0x38);
    tp_w(" hdr=");
    for (int w = 0; w < 2; w++) {
      tp_w(" 0x");
      tp_wx(*(volatile unsigned long *)((char *)h + w * 8));
    }
  }
  tp_w("\n");
}

/* Print the register-derived park identity, then every exact-handle candidate
 * that the same thread's stack still points at.  `tid` is the sampled thread
 * (pid 4 for the game's main thread). */
static void tp_park_ident(int tid) {
  if (!rw_stk_regs_ok) return;
  unsigned long rip = rw_stk_regs[REG_RIP];
  unsigned long rsp = rw_stk_regs[REG_RSP];
  unsigned long rax = rw_stk_regs[REG_RAX];
  unsigned long rdi = rw_stk_regs[REG_RDI];
  unsigned long rsi = rw_stk_regs[REG_RSI];
  unsigned long rdx = rw_stk_regs[REG_RDX];
  unsigned long r10 = rw_stk_regs[REG_R10];
  unsigned long rbp = rw_stk_regs[REG_RBP];
  tp_w("[TPWF] tid=");
  tp_wi(tid);
  tp_w(" rip=0x");
  tp_wx(rip);
  tp_w(" rsp=0x");
  tp_wx(rsp);
  tp_w(" rax=0x");
  tp_wx(rax);
  tp_w(" rdi=0x");
  tp_wx(rdi);
  tp_w(" rsi=0x");
  tp_wx(rsi);
  tp_w(" rdx=0x");
  tp_wx(rdx);
  tp_w(" r10=0x");
  tp_wx(r10);
  tp_w(" rbp=0x");
  tp_wx(rbp);
  tp_w("\n");

  /* The futex key is only rdi when the thread is inside the futex syscall.
     rip == libpthread+0xf376 is the `cmp` right after `syscall` in
     pthread_cond_wait's wait loop; the syscall args are still live there. */
  unsigned long key = rdi;
  if (key < 0x1000000000UL || key > 0x1400000000UL) return;
  /* An idle thread's rdi is not a futex key.  Reading it unguarded would
     fault the launcher itself, so the mincore-backed probe gates the row. */
  if (!rw_readable(key)) return;
  unsigned long cond = key - 0x28;
  tp_w("[TPWFK] tid=");
  tp_wi(tid);
  tp_w(" key=0x");
  tp_wx(key);
  tp_w(" cond=0x");
  tp_wx(cond);
  tp_w(" word=0x");
  tp_wx(*(volatile unsigned int *)key);
  tp_w("\n");
  tp_hdr_row("[TPWH0]", tid, key, key - 0x60, 0);   /* cond = h+0x38, key = cond+0x28 */
  tp_hdr_row("[TPWH1]", tid, key, key - 0x64, 1);   /* group 1: key = cond+0x2c */
  {
    extern void ct_report(const void *cond, const char *tag) __attribute__((weak));
    if (ct_report) {
      ct_report((const void *)cond, "parked");
      if (tp_hdr(key - 0x60, &(int){0}, &(unsigned int){0}, &(int){0}, &(int){0}))
        ct_report((const void *)(key - 0x60 + 0x38), "parked-h");
    }
  }

  /* Identity, part 2: is that handle pointer in THIS thread's captured frame?
     And does any other plausible MonoW32Handle on the stack produce this same
     key?  A second handle with the same key would be an aliasing bug. */
  int h_hit = -1, m_hit = -1, c_hit = -1, k_hit = -1, n_same = 0;
  unsigned long same = 0;
  for (int i = 0; i < rw_stk_n; i++) {
    unsigned long v = rw_stk_buf[i];
    if (v == key - 0x60) h_hit = i;
    if (v == key - 0x50) m_hit = i;       /* handle+0x10 */
    if (v == cond) c_hit = i;
    if (v == key) k_hit = i;
    int t2, s2, u2; unsigned int r2;
    if (v >= 0x1000000000UL && v <= 0x1400000000UL && !(v & 7) &&
        tp_hdr(v, &t2, &r2, &s2, &u2)) {
      if (v + 0x60 == key || v + 0x64 == key) { n_same++; same = v; }
    }
  }
  tp_w("[TPWFS] tid=");
  tp_wi(tid);
  tp_w(" h_at=");
  tp_wi(h_hit);
  tp_w(" mutex_at=");
  tp_wi(m_hit);
  tp_w(" cond_at=");
  tp_wi(c_hit);
  tp_w(" key_at=");
  tp_wi(k_hit);
  tp_w(" same_key_handles=");
  tp_wi(n_same);
  if (n_same) {
    tp_w(" other=0x");
    tp_wx(same);
  }
  tp_w("\n");
  /* The stack handle that L53 blamed: say where it is and what its own cond's
     futex key would be, so "signalled event on the stack" and "event this
     thread waits on" are different rows even when both exist. */
  for (int i = 0; i < rw_stk_n && i < 256; i++) {
    unsigned long v = rw_stk_buf[i];
    int t2, s2, u2; unsigned int r2;
    if (v < 0x1000000000UL || v > 0x1400000000UL || (v & 7)) continue;
    if (!tp_hdr(v, &t2, &r2, &s2, &u2)) continue;
    if (v == key - 0x60) continue;         /* the parked one, already printed */
    tp_w("[TPWFC] tid=");
    tp_wi(tid);
    tp_w(" at=");
    tp_wi(i);
    tp_w(" h=0x");
    tp_wx(v);
    tp_w(" type=");
    tp_wi(t2);
    tp_w(" ref=");
    tp_wi((long)r2);
    tp_w(" sig=");
    tp_wi(s2);
    tp_w(" in_use=");
    tp_wi(u2);
    tp_w(" key0=0x");
    tp_wx(v + 0x60);
    tp_w(" key1=0x");
    tp_wx(v + 0x64);
    tp_w("\n");
    break;
  }
}

/* Every plausible MonoW32Handle in the captured stack, with the CORRECT
 * handle-relative cond/mutex addresses (handle+0x38 / handle+0x10) and the
 * futex key glibc would use for it.  L53's version printed `cond=handle+0x38`
 * but never the key, and the reader had to guess which of these handles the
 * thread was in. */
static void tp_scan_handles(int tid) {
  int shown = 0;
  for (int i = 0; i < rw_stk_n && i < 160 && shown < 10; i++) {
    unsigned long v = rw_stk_buf[i];
    int type, sig, inuse; unsigned int ref;
    if (!tp_hdr(v, &type, &ref, &sig, &inuse)) continue;
    tp_w("[TPWW] tid=");
    tp_wi(tid);
    tp_w(" i=");
    tp_wi(i);
    tp_w(" handle=0x");
    tp_wx(v);
    tp_w(" type=");
    tp_wi(type);
    tp_w(" ref=");
    tp_wi((long)ref);
    tp_w(" sig=");
    tp_wi(sig);
    tp_w(" in_use=");
    tp_wi(inuse);
    tp_w(" cond=0x");
    tp_wx(v + 0x38);
    tp_w(" key0=0x");
    tp_wx(v + 0x60);
    tp_w(" key1=0x");
    tp_wx(v + 0x64);
    tp_w("\n");
    shown++;
  }
}

/* Name the native target of a managed extern: lookup_icall is exactly what
 * the JIT binds, and rw_mod_of turns it into a file offset we can disassemble
 * offline.  This is how mono_monitor_wait vs mono_w32handle_wait_one stops
 * being a guess. */
static void tp_one_icall(void *img, const char *cls, const char *meth, int arity,
                         const char *tag) {
  void *k = tp.class_from_name(img, "System.Threading", cls);
  void *m = 0;
  int a = 0;
  if (k) for (a = arity; a >= 2; a--) { m = tp.method_from_name(k, meth, a); if (m) break; }
  void *f = (m && tp.lookup_icall) ? tp.lookup_icall(m) : 0;
  unsigned long off = 0;
  const char *mod = f ? rw_mod_of((unsigned long)f, &off) : 0;
  tp_w("[TPIC] ");
  tp_w(tag);
  tp_w(" arity=");
  tp_wi(a);
  tp_w(" fn=0x");
  tp_wx((unsigned long)f);
  if (mod) {
    tp_w(" ");
    tp_w(mod);
    tp_w("+0x");
    tp_wx(off);
  }
  tp_w("\n");
}

static void tp_icall_rows(void) {
  if (!tp.image_loaded || !tp.class_from_name || !tp.method_from_name || !tp.lookup_icall) return;
  void *img = tp.image_loaded("mscorlib");
  if (!img) return;
  if (!rw_mod_n) rw_mods_collect();
  tp_one_icall(img, "Monitor", "Monitor_wait", 3, "Monitor.Monitor_wait");
  tp_one_icall(img, "Monitor", "ObjWait", 4, "Monitor.ObjWait");
  tp_one_icall(img, "WaitHandle", "Wait_internal", 5, "WaitHandle.Wait_internal");
  tp_one_icall(img, "NativeEventCalls", "SetEvent_internal", 1, "NativeEventCalls.SetEvent_internal");
}

/* TASK_STATE_* as this mscorlib defines them (Task.cs). */
static void tp_state_words(long f) {
  int any = 0;
#define TPST(bit, name) do { if (f & (bit)) { tp_w(any ? "|" name : name); any = 1; } } while (0)
  TPST(0x10000, "started");
  TPST(0x20000, "delegate");
  TPST(0x40000, "disposed");
  TPST(0x80000, "excobserved");
  TPST(0x100000, "cancelack");
  TPST(0x200000, "faulted");
  TPST(0x400000, "canceled");
  TPST(0x800000, "children");
  TPST(0x1000000, "ran");
  TPST(0x2000000, "waitactivation");
  TPST(0x4000000, "completionreserved");
  TPST(0x8000000, "threadaborted");
#undef TPST
  if (!any) tp_w("created");
}

static void tp_task_row(int tid, void *t) {
  if (!tpc.state || !t) return;
  long id = tp_f_i4(tpc.f_id, t);
  long fl = tp_f_i4(tpc.f_flags, t);
  void *sch = tp_f_p(tpc.f_sched, t);
  void *cont = tp_f_p(tpc.f_cont, t);
  void *cp = tp_f_p(tpc.f_cp, t);
  void *act = tp_f_p(tpc.f_action, t);
  void *ev = 0;
  long comb = 0, cd = -1;
  if (cp) {
    if (tpc.f_countdown) cd = tp_f_i4(tpc.f_countdown, cp);
    if (tpc.f_ev) ev = tp_f_p(tpc.f_ev, cp);
  }
  if (ev && tpc.f_comb) comb = tp_f_i4(tpc.f_comb, ev);
  tp_w("[TPWT] tid=");
  tp_wi(tid);
  tp_w(" task=0x");
  tp_wx((unsigned long)t);
  tp_w(" id=");
  tp_wi(id);
  tp_w(" flags=0x");
  tp_wx((unsigned long)(fl & 0xffffffffL));
  tp_w(" st=");
  tp_state_words(fl);
  tp_w(" sched=0x");
  tp_wx((unsigned long)sch);
  tp_w("(");
  {
    const char *s = sch ? tp_obj_cls(sch) : 0;
    tp_w(s ? s : "null");
  }
  tp_w(") action=0x");
  tp_wx((unsigned long)act);
  tp_w(" cont=0x");
  tp_wx((unsigned long)cont);
  tp_w("(");
  {
    const char *s = cont ? tp_obj_cls(cont) : 0;
    tp_w(s ? s : "null");
  }
  tp_w(") cp=0x");
  tp_wx((unsigned long)cp);
  tp_w(" cd=");
  tp_wi(cd);
  tp_w(" ev=0x");
  tp_wx((unsigned long)ev);
  tp_w(" set=");
  tp_wi((comb >> 31) & 1);
  tp_w(" waiters=");
  tp_wi(comb & 0x7ffff);
  tp_w("\n");
}

static void tp_thr_row(int tid, void *th) {
  void *it = tp_f_p(tpc.f_it, th);
  long itid = tp_f_i8(tpc.f_tid, it);
  long st = tp_f_i4(tpc.f_state, it);
  long mid = tp_f_i4(tpc.f_mid, it);
  long started = tp_f_i4(tpc.f_started, th);
  void *nc = tp_f_p(tpc.f_nchars, it);
  long nl = tp_f_i4(tpc.f_nlen, it);
  tp_w("[TPWY] tid=");
  tp_wi(tid);
  tp_w(" thread=0x");
  tp_wx((unsigned long)th);
  tp_w(" started=");
  tp_wi(started);
  tp_w(" itid=");
  tp_wi(itid);
  tp_w(" mid=");
  tp_wi(mid);
  tp_w(" tstate=");
  tp_wi(st);
  tp_w(" name=");
  if (nc && nl > 0 && nl < 64 && rw_readable((unsigned long)nc)) {
    for (long i = 0; i < nl && i < 32; i++) {
      unsigned short c = ((volatile unsigned short *)nc)[i];
      if (c < 0x20 || c > 0x7e) { tp_w("?"); continue; }
      char s[2] = {(char)c, 0};
      tp_w(s);
    }
  } else {
    tp_w("(none)");
  }
  tp_w("\n");
}

static unsigned long tp_seen[32];
static int tp_seen_n;
static void *tp_poke_lock;   /* m_lock of the SetOnInvokeMres on the main stack */
static void *tp_poke_mres;
static volatile int tp_poke_n;

static int tp_poke_on(void) {
  static int on = -1;
  if (on < 0) {
    int f = open("/TPPOKE", TP_O_RDONLY);
    on = (f >= 0);
    if (f >= 0) close(f);
  }
  return on;
}

/* Rule 24(b): a one-shot spurious wake on the object the thread is parked on.
 * MRES.Wait's loop is `while (!IsSet) Monitor.Wait(m_lock)` and MRES.Set has
 * already set its signalled bit (m_combinedState bit 31), so a correct consumer
 * re-checks and returns.  If this un-stalls the loader, the wait object was
 * signalled and the pulse never reached the waiter -- which is a lost wake,
 * not a missing task. */
static void tp_poke(const char *why) {
  if (!tp_poke_lock || !tpc.monitor || !tp.method_from_name || !tp.invoke) return;
  void *m_enter = tp.method_from_name(tpc.monitor, "Enter", 1);
  void *m_pulse = tp.method_from_name(tpc.monitor, "PulseAll", 1);
  void *m_exit = tp.method_from_name(tpc.monitor, "Exit", 1);
  tp_poke_n++;
  if (!m_enter || !m_pulse || !m_exit) {
    tp_w("[TPWK] no Monitor.Enter/PulseAll/Exit on this mscorlib\n");
    return;
  }
  void *dom = tp.domain_get ? tp.domain_get() : 0;
  if (dom && tp.attach && !tp.attached) {
    tp.attach(dom);
    tp.attached = 1;
  }
  void *args[1];
  args[0] = tp_poke_lock;
  void *e1 = 0, *e2 = 0, *e3 = 0;
  tp.invoke(m_enter, 0, args, &e1);
  tp.invoke(m_pulse, 0, args, &e2);
  tp.invoke(m_exit, 0, args, &e3);
  tp_w("[TPWK] n=");
  tp_wi(tp_poke_n);
  tp_w(" lock=0x");
  tp_wx((unsigned long)tp_poke_lock);
  tp_w(" mres=0x");
  tp_wx((unsigned long)tp_poke_mres);
  tp_w(" exc=");
  tp_wx((unsigned long)e1);
  tp_w(" ");
  tp_wx((unsigned long)e2);
  tp_w(" ");
  tp_wx((unsigned long)e3);
  tp_w(" why=");
  tp_w(why);
  tp_w("\n");
}

/* Decode the monitor of a managed object in place, with Mono 6.12's own
 * encoding (mono/metadata/monitor.h):
 *
 *   MonoObject        = { MonoVTable *vtable; MonoThreadsSync *synchronisation; }
 *   LockWord status    bits 0..1: 0 LOCK_WORD_FLAT, 1 HAS_HASH, 2 INFLATED
 *   MonoThreadsSync   = { u32 status; u32 nest; GSList *wait_list; void *data;
 *                         MonoCoopMutex *entry_mutex; MonoCoopCond *entry_cond; }
 *   status             bits 0..15 owner small-id, 16..31 entry count,
 *                      bit 31 ENTRY_COUNT_WAITERS
 *   MonoW32Handle     = { int type; u32 ref; int signalled; int in_use;
 *                         MonoCoopMutex signal_mutex;   // +0x10, 0x28 bytes
 *                         MonoCoopCond signal_cond;     // +0x38
 *                         void *specific; }
 *
 * A monitor wait creates one private auto-reset EVENT handle per wait, appends
 * it to mon->wait_list and blocks on event->signal_cond; Pulse walks
 * wait_list and calls mono_w32event_set(handle), i.e. pthread_cond_signal on
 * that same cond.  So `sig` (the handle's signalled flag), `cond` and the
 * kernel's parked futex key in the same log decide the lost wake in one row:
 *
 *   nwl=0                     -- the waiter was not on the pulser's list
 *   nwl>0 sig=1               -- the event WAS set, and the waiter is still
 *                                parked on a signalled event (the wake was
 *                                lost below Mono)
 *   nwl>0 sig=0, cond=PARKKEY -- the event was never set
 *   cond != PARKKEY           -- the waiter is not on this sync block at all
 */
static void tp_dump_sync(int tid, void *lockobj) {
  if (!lockobj || !rw_readable((unsigned long)lockobj + 16)) return;
  unsigned long vt = *(volatile unsigned long *)lockobj;
  unsigned long sy = *(volatile unsigned long *)((char *)lockobj + 8);
  tp_w("[TPWM] tid=");
  tp_wi(tid);
  tp_w(" lock=0x");
  tp_wx((unsigned long)lockobj);
  tp_w(" vtable=0x");
  tp_wx(vt);
  tp_w(" sync=0x");
  tp_wx(sy);
  tp_w(" cls=");
  tp_w((sy & 3) == 2 ? "inflated" : (sy & 3) == 0 ? (sy ? "flat" : "free") : "hash");
  unsigned long mon = sy & ~3UL;
  if ((sy & 3) != 2 || !mon || !rw_readable(mon + 40)) {
    tp_w("\n");
    return;
  }
  unsigned int st = *(volatile unsigned int *)mon;
  unsigned int nest = *(volatile unsigned int *)((char *)mon + 4);
  /* Unity's Mono 6.12 is built with HAVE_MOVING_COLLECTOR: an extra i32
     hash_code sits between nest and wait_list, so wait_list is +16, data +24,
     entry_mutex +32, entry_cond +40.  Measured: data held 0x14 (a small
     MonoGCHandle) where a pointer would be -- the +8 "wait_list" of the
     non-moving layout is always 0 here, which is why every earlier dump read
     nwl=0 on a monitor that certainly had a waiter. */
  unsigned long wl = *(volatile unsigned long *)((char *)mon + 16);
  unsigned long em = *(volatile unsigned long *)((char *)mon + 32);
  unsigned long ec = *(volatile unsigned long *)((char *)mon + 40);
  tp_w(" mon=0x");
  tp_wx(mon);
  tp_w(" status=0x");
  tp_wx(st);
  tp_w(" owner=");
  tp_wi(st & 0xffff);
  tp_w(" nest=");
  tp_wi(nest);
  tp_w(" ent=");
  tp_wi((st >> 16) & 0x7fff);
  tp_w(" wbit=");
  tp_wi((st >> 31) & 1);
  tp_w(" entcond=0x");
  tp_wx(ec);
  tp_w(" entmutex=0x");
  tp_wx(em);
  int nwl = 0;
  unsigned long node = wl, ev = 0;
  while (node && nwl < 8 && rw_readable(node + 16)) {
    if (nwl == 0) ev = *(volatile unsigned long *)node;   /* GSList.data */
    node = *(volatile unsigned long *)((char *)node + 8); /* GSList.next */
    nwl++;
  }
  tp_w(" nwl=");
  tp_wi(nwl);
  tp_w("\n");
  if (ev && rw_readable(ev + 0x40)) {
    int type = *(volatile int *)ev;
    int ref = *(volatile int *)((char *)ev + 4);
    int sig = *(volatile int *)((char *)ev + 8);
    tp_w("[TPWH] tid=");
    tp_wi(tid);
    tp_w(" ev=0x");
    tp_wx(ev);
    tp_w(" type=");
    tp_wi(type);
    tp_w(" ref=");
    tp_wi(ref);
    tp_w(" sig=");
    tp_wi(sig);
    tp_w(" cond=0x");
    tp_wx(ev + 0x38);
    tp_w(" mutex=0x");
    tp_wx(ev + 0x10);
    tp_w("\n");
  }
}

/* kind: 0 = Task, 1 = Thread, 2 = SetOnInvokeMres.  Deduped, because the same
 * `this` is spilled at several slots of one frame and the first three hits used
 * to be the same completed Task. */
static int tp_scan_obj(int tid, int kind, int maxout) {
  void *vt = kind == 1 ? tpc.vt_thr : kind == 2 ? tpc.vt_mres : tpc.vt_task;
  int found = 0;
  if (!tpc.state || !vt) return 0;
  for (int i = 0; i < rw_stk_n && found < maxout; i++) {
    unsigned long v = rw_stk_buf[i];
    if (v < 0x10000000UL || v > 0x1800000000UL || (v & 7)) continue;
    if (!rw_readable(v)) continue;
    if (*(volatile void **)v != vt) continue;
    int dup = 0;
    for (int k = 0; k < tp_seen_n; k++) if (tp_seen[k] == v) dup = 1;
    if (dup) continue;
    if (tp_seen_n < 32) tp_seen[tp_seen_n++] = v;
    if (kind == 1) {
      tp_thr_row(tid, (void *)v);
    } else if (kind == 2) {
      long comb = tp_f_i4(tpc.f_comb, (void *)v);
      tp_w("[TPWQ] tid=");
      tp_wi(tid);
      tp_w(" mres=0x");
      tp_wx(v);
      tp_w(" combined=0x");
      tp_wx((unsigned long)comb);
      tp_w(" set=");
      tp_wi((comb >> 31) & 1);
      tp_w(" waiters=");
      tp_wi(comb & 0x7ffff);
      if (tid == (int)getpid()) {
        tp_poke_mres = (void *)v;
        tp_poke_lock = tp_f_p(tpc.f_lock, (void *)v);
        /* MonoObject = { MonoVTable *vtable; MonoThreadsSync *synchronization; }
           so `lock+8` is the monitor's own word: a small tagged value for a
           THIN lock, a heap pointer for an INFLATED (fat) one.  The waiter's
           park key is printed by the kernel ([FUTXW] NOW / [PARK]); a cond
           inside `sync` sits a few tens of bytes past it, so the pair decides
           whether mono_monitor_pulse_all and mono_monitor_wait are on the same
           sync block. */
        if (tp_poke_lock && rw_readable((unsigned long)tp_poke_lock + 16)) {
          tp_dump_sync(tid, tp_poke_lock);
        }
      }
      tp_w("\n");
    } else {
      tp_task_row(tid, (void *)v);
    }
    found++;
  }
  return found;
}

/* A thread whose stack resolves to no JIT frame: print the words themselves, so
 * "native thread" and "the resolver failed" are different rows. */
static void tp_raw_words(int tid) {
  tp_w("[TPWD] tid=");
  tp_wi(tid);
  tp_w(" sp=0x");
  tp_wx(rw_stk_base);
  tp_w(" w=");
  int shown = 0;
  for (int i = 0; i < rw_stk_n && shown < 14; i++) {
    unsigned long v = rw_stk_buf[i];
    if (v < 0x100000UL) continue;
    tp_w(" ");
    tp_wx(v);
    shown++;
  }
  tp_w("\n");
}

/* Deliver SIGUSR2 to one tid and wait for the handler to copy its stack.
 * Returns the word count, 0 on a timeout, -1 when the tid does not exist. */
static int tp_capture_tid(int tid, int wait_ms) {
  rw_stk_ready = 0;
  rw_stk_busy = 1;
  long r = syscall(234 /* tgkill */, (long)getpid(), (long)tid, (long)SIGUSR2);
  if (r != 0) return -1;
  for (int i = 0; i < wait_ms / 5; i++) {
    if (rw_stk_ready) return rw_stk_n;
    usleep(5000);
  }
  return 0;
}

/* Sample every live thread once: its managed frames, then any Task / Thread
 * object its stack still points at.  This is the stall's whole wait graph in
 * one place -- who is joining whom, and who (if anyone) is running the task
 * the main thread waits for. */
static void tp_probe_stall(int at) {
  tp_cls_init();
  {
    static int icall_done;
    if (!icall_done) { icall_done = 1; tp_icall_rows(); }
  }
  int self = (int)gettid();
  tp_w("[TPWT] sample t=");
  tp_wi(at);
  tp_w("s pid=");
  tp_wi((long)getpid());
  tp_w(" self=");
  tp_wi(self);
  tp_w(" nt=");
  tp_wi(tpc.state);
  tp_w(" vt_mres=0x");
  tp_wx((unsigned long)tpc.vt_mres);
  tp_w("\n");
  int live = 0;
  twq_scan();
  for (int tid = 1; tid <= 120; tid++) {
    if (tid == self) continue;
    int n = tp_capture_tid(tid, 400);
    if (n <= 0) continue;
    live++;
    tp_seen_n = 0;
    twq_owner_scan(tid);
    if (tid == (int)getpid()) {
      tp_native_first(tid, 48);
      tp_park_ident(tid);
      tp_scan_locks(tid);
      tp_scan_handles(tid);
    }
    tp_w("[TPWA] tid=");
    tp_wi(tid);
    tp_w(" sp=0x");
    tp_wx(rw_stk_base);
    tp_w(" words=");
    tp_wi(n);
    int before = tp_bt_words("[TPWA]", rw_stk_base, n);
    if (before <= 0) tp_raw_words(tid);
    tp_scan_obj(tid, 0, 6);
    tp_scan_obj(tid, 2, 2);
    tp_scan_obj(tid, 1, 2);
  }
  tp_w("[TPWT] sample done live=");
  tp_wi(live);
  tp_w("\n");
  twq_dump("stall");
  if (tp_poke_on() && tp_poke_lock) tp_poke("stall");
}

/* Resolve the words the SIGUSR2 handler copied out of the parked thread's
 * stack.  Runs on the watchdog thread, i.e. outside any signal context. */
static int tp_bt_words(const char *tag, unsigned long base, int n) {
  if (!tp.jit_find || !tp.domain_get) return -1;
  void *dom = tp.domain_get();
  if (!dom) return -1;
  int printed = 0;
  for (int i = 0; i < n && printed < 24; i++) {
    unsigned long v = rw_stk_buf[i];
    if (v < 0x100000 || v > 0x7fffffffffffUL) continue;
    void *ji = tp.jit_find(dom, (void *)v);
    if (!ji) continue;
    void *m = tp.jit_method(ji);
    if (!m) continue;
    void *k = tp.m_get_class(m);
    const char *mn = tp.m_get_name(m);
    const char *kn = k ? tp.c_get_name(k) : 0;
    tp_w(printed ? " <- " : " bt=");
    tp_w(kn ? kn : "?");
    tp_w(".");
    tp_w(mn ? mn : "?");
    printed++;
  }
  if (printed) { tp_w("\n"); return printed; }
  tp_w(" bt=(no JIT frame in ");
  tp_wi(n);
  tp_w(" words from 0x");
  tp_wx(base);
  tp_w(")\n");
  return 0;
}

static int tp_register_w(void) {
  if (!tp.class_from_name || !tp.method_from_name || !tp.lookup_icall || !tp.image_loaded) return 0;
  void *img = tp.image_loaded("mscorlib");
  if (!img) { tp_w("[TPQW] no mscorlib image\n"); return 0; }
  void *k = tp.class_from_name(img, "System.Threading", "WaitHandle");
  if (!k) { tp_w("[TPQW] WaitHandle class lookup failed\n"); return 0; }
  /* Wait_internal(IntPtr*, int, bool, int) is the only handle wait in this
   * mscorlib: WaitOne/WaitAny/WaitAll all funnel into it. */
  void *m = 0;
  int arity = 0;
  for (arity = 5; arity >= 3; arity--) {
    m = tp.method_from_name(k, "Wait_internal", arity);
    if (m) break;
  }
  if (!m) { tp_w("[TPQW] Wait_internal not found\n"); return 0; }
  if (!tp_orig_w) tp_orig_w = (long (*)(void *, long, long, long, void *, void *))tp.lookup_icall(m);
  if (!tp_orig_w) { tp_w("[TPQW] Wait_internal icall lookup failed\n"); return 0; }
  tp.add_icall("System.Threading.WaitHandle::Wait_internal", (const void *)tp_hook_w);
  /* Proof that the table now holds OUR pointer for the key the JIT will use. */
  void *ver = tp.lookup_icall(m);
  tp_w("[TPQW] bind arity=");
  tp_wi(arity);
  tp_w(" orig=0x");
  tp_wx((unsigned long)tp_orig_w);
  tp_w(" hook=0x");
  tp_wx((unsigned long)tp_hook_w);
  tp_w(" verify=");
  tp_w(ver == (void *)tp_hook_w ? "HOOK" : ver == tp_orig_w ? "ORIG(unchanged)" : "OTHER");
  tp_w("\n");
  /* The JIT had already compiled WaitHandle.WaitOne before this registration
   * (L48's hook never fired in 200 s), so the cached call site must be
   * rewritten.  Compile-then-patch also covers a body compiled after us: the
   * table entry alone would already bind our hook, and the patch finds 0
   * sites -- either way the hook runs. */
  /* The icall call site is NOT in WaitOne: WaitHandle.WaitOne/WaitAny/WaitAll
   * all funnel through the private managed helpers WaitOneNative(SafeHandle,
   * uint, bool, bool) and WaitMultiple(WaitHandle[], int, bool, bool), and
   * THOSE are the bodies carrying the call.  Measured: WaitOne/0..2 and
   * WaitAny/1 compile with sites=0. */
  tp_patch_method(k, "WaitOneNative", 4, tp_orig_w, (void *)tp_hook_w, "WaitHandle");
  tp_patch_method(k, "WaitMultiple", 4, tp_orig_w, (void *)tp_hook_w, "WaitHandle");
  /* The set side, for the "signal issued vs wake dropped" question. */
  void *k2 = tp.class_from_name(img, "System.Threading", "NativeEventCalls");
  if (k2) {
    void *ms = tp.method_from_name(k2, "SetEvent_internal", 1);
    if (ms) {
      if (!tp_orig_s) tp_orig_s = (int (*)(void *))tp.lookup_icall(ms);
      if (tp_orig_s) {
        tp.add_icall("System.Threading.NativeEventCalls::SetEvent_internal", (const void *)tp_hook_s);
        void *v2 = tp.lookup_icall(ms);
        tp_w("[TPQS] bind SetEvent orig=0x");
        tp_wx((unsigned long)tp_orig_s);
        tp_w(" verify=");
        tp_w(v2 == (void *)tp_hook_s ? "HOOK" : "unchanged");
        tp_w("\n");
        tp_patch_method(k2, "SetEvent", 1, tp_orig_s, (void *)tp_hook_s, "NativeEventCalls");
      }
    }
  }
  return 1;
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
  /* L49: the managed chain that CREATED the item.  L48 resolved only the icall's
     own caller (ThreadPoolTaskScheduler.QueueTask); the game method that asked
     for the task is two frames above that, and this walk names it in place. */
  if (n <= 8) tp_bt_n("[TPQE]", tp_tid(), n, 12);
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

/* M6/L72: /TPLATE uses THIS sampler only.
 *
 * The rich stall probe (twq_scan / tp_scan_obj / tp_dump) walks Mono's internal
 * objects from a foreign thread and, measured in two consecutive runs, it
 * stopped the watchdog between t=100 s and t=105 s: no heartbeat, no sample,
 * nothing from that thread for the rest of the process's life.  The park
 * samples live at t>=400 s, so the probe has to survive 400 s first.  This
 * path touches exactly two Mono calls (`mono_jit_info_table_find` and
 * `mono_domain_get`) on words the thread itself copied out of its own stack --
 * the async-signal-safe SIGUSR2 capture -- and nothing else. */
static void tp_late_sample(int at) {
  tp_w("[TPWT] late t=");
  tp_wi(at);
  tp_w("s\n");
  int self = (int)gettid();
  int live = 0;
  unsigned long t0 = (unsigned long)time(0);
  for (int tid = 1; tid <= 96; tid++) {
    if (tid == self) continue;
    if ((unsigned long)time(0) - t0 > 20) break; /* bounded: never stall the loop */
    int n = tp_capture_tid(tid, 300);
    if (n <= 0) continue;
    live++;
    tp_w("[TPWA] tid=");
    tp_wi(tid);
    tp_w(" sp=0x");
    tp_wx(rw_stk_base);
    tp_w(" words=");
    tp_wi(n);
    tp_bt_words("[TPWA]", rw_stk_base, n);
  }
  tp_w("[TPWT] late done live=");
  tp_wi(live);
  tp_w("\n");
}

static void *tp_watchdog(void *arg) {
  (void)arg;
  long prev = -1;
  int quiet = 0;
  int late_attach_tried = 0;
  for (int i = 0; i < 42000; i++) { /* <= 2100 s at 50 ms: the post-load
                                       park arrives at ~420 s, so the old
                                       240 s window ended before the wall */
    /* Phase 1 must not run during mono_init: a class lookup from this thread
       aborted the runtime (object.c:1939 `klass').  4 s after the DSO load
       the runtime is executing managed code, and mscorlib is on the loaded
       image list, so the name-only registration below is safe. */
    /* M6/L72: /TPLATE needs none of the pool bookkeeping, and every one of
       these is a Mono class lookup from a foreign thread during/after init --
       the exact shape that aborts the runtime at object.c:1939 (`klass`).
       Measured: two runs with /TPLATE died at startup on it.  Late-only mode
       touches nothing but mono_jit_info_table_find. */
    if (!tp_late_only && i >= 40 && !tp.registered_q && tp.image_loaded &&
        tp.add_icall) {
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
    if (!tp_late_only && !tp.registered_w && tp.attached && i >= 50) {
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
    /* L49: the wait site.  Ask the main thread for its own stack twice, well
       after the loader's stall begins (~30 s), via a signal that the kernel
       delivers to THAT thread (tgkill to the leader == the main thread). */
    if (tp.on && (i == 900 || i == 1500) && tp.jit_find) {
      rw_stk_ready = 0;
      rw_stk_busy = 1;
      long r = syscall(234 /* tgkill */, (long)getpid(), (long)getpid(),
                       (long)SIGUSR2);
      tp_w("[TPWS] tgkill SIGUSR2 t=");
      tp_wi(i / 20);
      tp_w("s rc=");
      tp_wi(r);
      tp_w("\n");
    }
    if (rw_stk_ready) {
      rw_stk_ready = 0;
      tp_w("[TPWS] stack sp=0x");
      tp_wx(rw_stk_base);
      tp_w(" words=");
      tp_wi(rw_stk_n);
      tp_bt_words("[TPWS]", rw_stk_base, rw_stk_n);
    }
    /* L50: the stall's whole wait graph, twice.  Every live thread's managed
       frames plus any Task / Thread object its stack still points at: the task
       the main thread awaits, its scheduler, and the target of any
       Thread.Join (36.8's one contradicting row). */
    if (tp.on && i >= 700 && (i % 300) == 100 && tp.jit_find && !tp_late_only)
      tp_probe_stall(i / 20);
    /* L72: a heartbeat, so "no sample at 480 s" is distinguishable from "the
       sampler died at 125 s" (which is what happened while its output was
       interleaved into Player.log's NUL hole). */
    if (tp.on && (i % 1200) == 0) {
      tp_w("[TPWL] beat t=");
      tp_wi(i / 20);
      tp_w("s dumps=");
      tp_wi(tp.dumps);
      tp_w(" enq=");
      tp_wi(tp.enq);
      tp_w(" comp=");
      tp_wi(tp.comp);
      tp_w("\n");
    }
    /* L72: THE POST-LOAD PARK.  Presents stop at ~420 s and the process then
       ping-pongs three threads through futexes for the rest of the window, so
       a 240 s sample window (above) ends before the wall exists.  Sample every
       120 s from t=400 s: each sample is the whole wait graph (every live
       thread's managed frames), which is what names the three threads in the
       cyclic handshake and says which one should have completed. */
    if (tp.on && i >= 7600 && (i % 1200) == 400 && tp.jit_find) {
      if (tp_late_only)
        tp_late_sample(i / 20);
      else
        tp_probe_stall(i / 20);
    }
    /* 3 s without a single event, at least 10 s in: dump the tail.  Up to
       six times, because the first quiet window is often just the gap before
       the loader starts. */
    if (quiet == 60 && i >= 200 && tp.dumps < 6 && !tp_late_only) { tp.dumps++; tp_dump("quiet3s"); }
    usleep(50000);
  }
  tp_dump("exit");
  return 0;
}

static void rw_tp_maybe_start(void *handle, const char *name) {
  if (tp.on) return;
  if (!name || !strstr(name, "libmonobdwgc")) return;
  {
    /* /TPTRACE also turns on Mono's threadpool trace in the child.  /TPLATE
       turns on the launcher's OWN sampling only (no game-side logging): the
       post-load park runs at ~112 futex ops/s, and a threadpool log line per
       op would both drown the console and change the rate being measured. */
    int f = open("/TPTRACE", TP_O_RDONLY);
    if (f < 0) {
      f = open("/TPLATE", TP_O_RDONLY);
      if (f >= 0) tp_late_only = 1;
    }
    if (f < 0) { tp.on = 0; return; }
    close(f);
  }
  tp.on = 1;
  tp_out_open(); /* /TPLATE: own file, see tp_out_open */
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
  tp.compile = dlsym(handle, "mono_compile_method");
  tp.invoke = (void *(*)(void *, void *, void **, void **))dlsym(handle, "mono_runtime_invoke");
  tp.jit_method = (void *(*)(void *))dlsym(handle, "mono_jit_info_get_method");
  tp.m_get_class = (void *(*)(void *))dlsym(handle, "mono_method_get_class");
  tp.c_get_name = (const char *(*)(void *))dlsym(handle, "mono_class_get_name");
  tp.m_get_name = (const char *(*)(void *))dlsym(handle, "mono_method_get_name");
  /* L50: metadata reads for the Task / Thread dumps (no JIT needed). */
  mo.class_vtable = (void *(*)(void *, void *))dlsym(handle, "mono_class_vtable");
  mo.field_from_name = (void *(*)(void *, const char *))dlsym(handle, "mono_class_get_field_from_name");
  mo.field_get_value = (void (*)(void *, void *, void *))dlsym(handle, "mono_field_get_value");
  mo.field_static_get_value = (void (*)(void *, void *, void *))dlsym(handle, "mono_field_static_get_value");
  mo.array_length = (int (*)(void *))dlsym(handle, "mono_array_length");
  mo.array_addr = (char *(*)(void *, int, unsigned long))dlsym(handle, "mono_array_addr_with_size");
  mo.obj_get_class = (void *(*)(void *))dlsym(handle, "mono_object_get_class");
  mo.nested = (void *(*)(void *, void **))dlsym(handle, "mono_class_get_nested_types");
  mo.class_namespace = (const char *(*)(void *))dlsym(handle, "mono_class_get_namespace");
  tp_w("[TPQ] probe armed (libmonobdwgc);");
  tp_w(" get_root_domain=");
  tp_wx((unsigned long)tp.get_root_domain);
  tp_w(" add_icall=");
  tp_wx((unsigned long)tp.add_icall);
  tp_w("\n");
  tp_mods_dump();
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

/* ---- M6/L61: interpreter control evidence -------------------------------
 * The launcher stages /INTERP -> MONO_ENV_OPTIONS=--interp, but Mono reads
 * that inside mono_jit_init, i.e. after this process has started and long
 * after any startup print.  The runtime exports `mono_use_interpreter` (int),
 * so polling it is the difference between "we asked for the interpreter" and
 * "the interpreter is what ran".  If the flag never becomes 1, the control is
 * void and the run must be discarded, not interpreted.
 */
static void *rw_interp_watch(void *arg) {
  (void)arg;
  int last = -1;
  const char *asked = getenv("MONO_ENV_OPTIONS");
  for (int i = 0; i < 1200; i++) {
    int *p = rw_mono_handle ? (int *)dlsym(rw_mono_handle, "mono_use_interpreter") : 0;
    if (p) {
      int v = __atomic_load_n(p, __ATOMIC_RELAXED);
      if (v != last) {
        last = v;
        fprintf(stderr, "[RWMAIN] mono_use_interpreter=%d asked=%s\n", v,
                asked && asked[0] ? asked : "(none)");
      }
    }
    sleep(1);
  }
  return 0;
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
  {
    int (*pc)(void *, void *, void *, void *) =
        (int (*)(void *, void *, void *, void *))dlsym(RTLD_DEFAULT, "pthread_create");
    void *th = 0;
    if (pc) pc(&th, 0, (void *)rw_interp_watch, 0);
  }
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
  /* M6/L61: interpreter control.  MONO_ENV_OPTIONS is not parsed by Unity's
     embedded runtime (measured: mono_use_interpreter stayed 0 while the
     variable was set), so the launcher loads libmonobdwgc itself and parses
     --interp before UnityPlayer initialises Mono.  mono_jit_parse_options()
     is the runtime's own option parser; mono_use_interpreter is read back
     immediately, so a run that asked for the interpreter and got the JIT says
     so in one row instead of being mistaken for a control. */
  {
    const char *mi = getenv("RW_MONO_INTERP");
    if (mi && mi[0] == '1') {
      void *mh = dlopen("/RWDSO/libmonobdwgc-2.0.so", RTLD_NOW | RTLD_GLOBAL);
      if (!mh) {
        fprintf(stderr, "[RWMAIN] INTERP: preload FAILED: %s\n",
                dlerror() ? dlerror() : "(no dlerror)");
      } else {
        void (*po)(int, char **) =
            (void (*)(int, char **))dlsym(mh, "mono_jit_parse_options");
        int *flag = (int *)dlsym(mh, "mono_use_interpreter");
        if (po) {
          char *av[2];
          av[0] = (char *)"mono";
          av[1] = (char *)"--interp";
          po(2, av);
        }
        /* mono_jit_parse_options() is the embedder's parser and it does NOT
           accept --interp (measured: the flag stayed 0 and its own "Available
           options" list has no interp entry); the standalone driver's mono_main
           sets the global directly.  Do the same, before mono_jit_init, so
           mono_init runs mono_ee_interp_init and every method is interpreted. */
        if (flag && getenv("RW_MONO_INTERP") && getenv("RW_MONO_INTERP")[0] == '1') {
          /* Flip the runtime's own global before mono_jit_init, so mono_init
             runs mono_ee_interp_init and managed methods are interpreted.
             Measured: with the flag alone the interpreter starts and then
             faults in interp frame setup (libmonobdwgc+0x116e57, a class
             pointer read out of a frame the native->interp transition had not
             built); adding the driver's second global (0x5b9cb8) instead
             overflows the unmanaged stack.  The switch is kept because it is
             the only way to run this runtime without the amd64 JIT, but it is
             NOT yet a path to the menu -- see docs §48. */
          *flag = 1;
        }
        fprintf(stderr, "[RWMAIN] INTERP: handle=%p parse=%p mono_use_interpreter=%d\n",
                mh, (void *)po, flag ? *flag : -1);
      }
    }
  }
  void *h = dlopen(chosen, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    fprintf(stderr, "[RWMAIN] FAIL dlopen: %s\n", dlerror());
    return 2;
  }
  fprintf(stderr, "[RWMAIN] UnityPlayer.so handle=%p\n", h);

  /* M6/L60: IFUNC provenance, measured instead of argued.  Nothing in the
     game's own DSOs defines an IFUNC (audited: UnityPlayer, libmonobdwgc,
     libmono-native, lib_burst = 0 symbols, 0 IRELATIVE relocations), but
     glibc's libc does -- 58 of them, including memcpy/strlen/memcmp.  Their
     GOT slots are resolved by the interpreter glibc's ld.so at load time;
     this kernel's ELF loader only ever applies R_*_RELATIVE to the main PIE
     and to the interpreter itself and REFUSES anything else, so "our loader
     never ran the resolver" can only be true if one of these prints a libc
     resolver's st_value (a `__memcpy_*` resolver entry) instead of the
     selected implementation.  A `__memcpy_avx_unaligned_erms` /
     `__strlen_avx2` / `__memcmp_avx2_movbe` target means it ran. */
  {
    static const char *const ifn[] = {"memcpy", "strlen", "memcmp", "memmove"};
    for (unsigned i = 0; i < sizeof ifn / sizeof ifn[0]; i++) {
      void *p = dlsym(RTLD_DEFAULT, ifn[i]);
      fprintf(stderr, "[IFUNC] %s=%p\n", ifn[i], p);
    }
  }

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
