/*
 * kconsole.c — the line-atomic serial console (C0-console).
 *
 * The defect (integ7, docs/cp-console.md §1): the console was atomic per CALL,
 * not per line.  A row built from several uart_puts/print_int calls released
 * the lock between pieces, and the lock-free sinks (uart_putc_dbg, the raw fatal sink,
 * the watchdog's wd_puts) took no lock at all, so a stall dump on one CPU and
 * a [PUB] row on another alternated byte by byte ("cpu1:[ P0UxB0]0 0w0h...")
 * and a probe's "[P5] end d3d11" row was destroyed.
 *
 * The design:
 *   - Every kernel byte goes into a LINE SLOT owned by (this CPU, the running
 *     context).  A slot reaches the wire only when it holds a whole line (or
 *     overflows, or goes stale), and that write is one hold of the console
 *     lock.  Slots are touched only by their own CPU with IRQs masked, so they
 *     need no lock of their own.
 *   - A context is the running thread plus "is this an interrupt nested in
 *     it".  Nesting is recognised by BOTH signals: interrupts masked where the
 *     slot had them on (or the reverse) AND a stack pointer more than
 *     CONS_NEST_SP bytes away.  A row printed across an irqsave lock keeps its
 *     slot (same stack); a helper with a big frame keeps its slot (same IRQ
 *     state); an interrupt handler's dump gets its own slot (both differ).
 *   - The console lock is a private word, not a spinlock_t: the spinlock
 *     screams print through this file and must not recurse into the lock code.
 *     A waiter takes the lock over at once when the holder is a halted CPU,
 *     and after CONS_WAIT_US (CONS_WAIT_PANIC_US on a fatal path) writes
 *     anyway with a "[CONS!] " tag rather than wait for ever.
 *   - Re-entry on one CPU (a fault inside this file) writes raw: no deadlock.
 *   - The wire knows which writer left a line open; any other writer starts
 *     with a newline, so a row always begins at column 0.
 *
 * Freestanding: no libc, no allocation; arch hooks in arch/<arch>/uart.c.
 */
#include "console.h"

#ifdef CONS_HOST_TEST
#include "cons_host_shim.h"
#else
#include "process.h" /* MAX_CPUS */
extern uint32_t get_cpuid(void);
extern uint64_t timer_get_us(void);
extern int cpu_is_halted(uint32_t cpu);
#endif

#define CONS_NCPU MAX_CPUS
#define CONS_SLOTS 3
#define CONS_LINE 1024
#define CONS_NEST_SP 256
#define CONS_WAIT_US 200000ULL        /* normal writer: 200 ms, then tag */
#define CONS_WAIT_PANIC_US 20000ULL   /* fatal record: 20 ms */
#define CONS_SPIN_CAP (1ULL << 27)    /* backstop if the clock does not move */
#define CONS_KSTALE_US 50000ULL       /* kernel partial line pushed out */
#define CONS_USER_STALE_US 30000ULL   /* user partial line pushed out */
#define CONS_UPEND 16
#define CONS_UPEND_CAP 512
#define CONS_SUMMARY_US 60000000ULL   /* [CONS] row at most once a minute */

#ifndef CP_DIAG_DEFAULT
#define CP_DIAG_DEFAULT ""
#endif

struct cons_slot {
  const void *ctx; /* cons_hw_ctx() at the last append */
  const char *sp;  /* stack pointer at the last append */
  uint64_t t_us;  /* first byte of the pending piece */
  uint8_t used;   /* a context owns this slot */
  uint8_t irqon;  /* interrupts were enabled at the last append */
  uint8_t cont;   /* the line already has bytes on the wire */
  uint8_t drop;   /* the filter rejected this line */
  int n;
  char b[CONS_LINE + 1]; /* +1: the newline appended when the line ends */
};

static struct cons_slot cons_slot[CONS_NCPU][CONS_SLOTS];
static volatile uint8_t cons_nest[CONS_NCPU];
static volatile uint8_t cons_dirty[CONS_NCPU];

struct cons_upend {
  uint64_t who;
  uint64_t t_us;
  int n;
  char b[CONS_UPEND_CAP];
};
static struct cons_upend cons_upend[CONS_UPEND]; /* under the wire lock */

/* The wire lock: a ticket lock, so a CPU that wants the console gets it in
   arrival order (an unfair test-and-set let one CPU lose the race for 200 ms
   under the console probe's load).  wire_holder names the owner's CPU so a
   waiter can pass a halted owner's turn on. */
static volatile uint32_t wire_next;    /* next ticket to hand out */
static volatile uint32_t wire_serving; /* the ticket that owns the wire */
static volatile uint32_t wire_holder;  /* owner's cpu + 1, 0 = none */
static uint32_t wire_my[CONS_NCPU];    /* each CPU's ticket while it holds */
static uint64_t wire_open;             /* id of the writer whose line is open */

volatile uint32_t cons_diag_mask;
struct cons_stats cons_stats;

static int cons_eq(const char *a, const char *b, int n);

/* ---- the wire --------------------------------------------------------- */

/* 1: this CPU owns the wire.  0: it owns it by TAKEOVER -- the owner made no
   progress for bound_us (a live CPU holds the wire for one line, so this means
   a wedged or descheduled owner): write, tagged, rather than wait for ever.
   -1: trylock (bound_us == 0) and the wire is busy.  Every 0/1 is paired with
   wire_unlock(). */
static int wire_lock(uint32_t cpu, uint64_t bound_us) {
  if (bound_us == 0) {
    uint32_t s = __atomic_load_n(&wire_serving, __ATOMIC_ACQUIRE);
    uint32_t want = s;
    if (!__atomic_compare_exchange_n(&wire_next, &want, s + 1, 0, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
      return -1;
    wire_my[cpu] = s;
    wire_holder = cpu + 1;
    return 1;
  }
  uint32_t t = __atomic_fetch_add(&wire_next, 1, __ATOMIC_ACQ_REL);
  wire_my[cpu] = t;
  uint32_t last = __atomic_load_n(&wire_serving, __ATOMIC_ACQUIRE);
  uint64_t t0 = 0, spins = 0;
  int timed = 0;
  for (;;) {
    uint32_t s = __atomic_load_n(&wire_serving, __ATOMIC_ACQUIRE);
    if ((int32_t)(s - t) >= 0) {
      wire_holder = cpu + 1;
      return 1;
    }
    if (s != last) { /* the queue moves: restart the no-progress clock */
      last = s;
      timed = 0;
      spins = 0;
    }
    uint32_t h = wire_holder;
    if (h && h - 1 < CONS_NCPU && cpu_is_halted(h - 1)) {
      /* A halted CPU never writes again: pass its turn on. */
      uint32_t exp = s;
      if (__atomic_compare_exchange_n(&wire_serving, &exp, s + 1, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_RELAXED)) {
        wire_holder = 0;
        cons_stats.broken++;
        wire_open = 1; /* its line may be half written: start a new one */
      }
      continue;
    }
    cons_hw_relax();
    if ((++spins & 255) == 0) {
      /* The clock may not run yet (early boot: timer_get_us() can read 0 or
         stand still); then only the spin cap bounds the wait. */
      uint64_t now = timer_get_us();
      int expired = spins > CONS_SPIN_CAP;
      if (!timed) {
        t0 = now;
        timed = 1;
      } else if (now > t0 && now - t0 > bound_us) {
        expired = 1;
      }
      if (expired) {
        uint32_t exp = s;
        while ((int32_t)(t - exp) > 0 &&
               !__atomic_compare_exchange_n(&wire_serving, &exp, t, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED))
          ;
        wire_holder = cpu + 1;
        return 0;
      }
    }
  }
}

static void wire_unlock(uint32_t cpu) {
  uint32_t t = wire_my[cpu];
  wire_holder = 0;
  uint32_t s = __atomic_load_n(&wire_serving, __ATOMIC_RELAXED);
  while ((int32_t)(t + 1 - s) > 0 &&
         !__atomic_compare_exchange_n(&wire_serving, &s, t + 1, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_RELAXED))
    ;
}

/* Set by a writer that took the wire over (wire_lock() == 0): its next piece
   is tagged so the log says this line may have been written over another. */
static int wire_tag;

/* Write one piece for writer `id`.  `ends` = the piece ends its line. */
static void wire_piece(uint64_t id, const char *s, int n, int ends) {
  if (wire_open && wire_open != id)
    cons_hw_tx("\n", 1);
  if (wire_tag) {
    cons_hw_tx("[CONS!] ", 8);
    wire_tag = 0;
  }
  if (n > 0)
    cons_hw_tx(s, n);
  wire_open = ends ? 0 : id;
}

/* ---- the diagnostic filter ------------------------------------------- */

struct cons_dtag {
  const char *tag;  /* the row's tag, including the brackets */
  const char *rest; /* required text right after "tag ", or 0 */
  uint32_t cls;
};
static const struct cons_dtag cons_dtags[] = {
  {"[PUB]", 0, CONS_D_PUB},
  {"[MR]", "s=", CONS_D_MR},
  {"[FUTEX]", 0, CONS_D_FUTEX},
  {"[ALIVE-SET]", "slot=", CONS_D_ALIVE},
  {"[RESUME-RZ]", 0, CONS_D_RESUME},
  {"[RESUME-SPSR]", 0, CONS_D_RESUME},
  {"[RESUME-DIAG]", 0, CONS_D_RESUME},
  {"[BLKD]", "cc=", CONS_D_BLKD},
  {"[PECOW]", 0, CONS_D_PECOW},
  {"[ANONF]", 0, CONS_D_ANONF},
};
static const char *const cons_dnames[CONS_D_NCLASS] = {
  "pub", "mr", "futex", "alive", "resume", "blkd", "pecow", "anonf"};

static int cls_index(uint32_t cls) {
  int i = 0;
  while (cls > 1) { cls >>= 1; i++; }
  return i;
}

/* 1 = drop.  Only the first piece of a line is classified. */
static int cons_filter(const char *b, int n) {
  uint32_t mask = cons_diag_mask;
  if (mask == CONS_D_ALL)
    return 0;
  int i = 0;
  while (i < n && b[i] == ' ')
    i++;
  if (i >= n || b[i] != '[')
    return 0;
  for (unsigned t = 0; t < sizeof cons_dtags / sizeof cons_dtags[0]; t++) {
    const struct cons_dtag *d = &cons_dtags[t];
    if (mask & d->cls)
      continue;
    int k = 0;
    while (d->tag[k] && i + k < n && b[i + k] == d->tag[k])
      k++;
    if (d->tag[k])
      continue;
    int j = i + k;
    if (j >= n || b[j] != ' ')
      continue;
    if (d->rest) {
      j++;
      int r = 0;
      while (d->rest[r] && j + r < n && b[j + r] == d->rest[r])
        r++;
      if (d->rest[r])
        continue;
    }
    cons_stats.dropped[cls_index(d->cls)]++;
    return 1;
  }
  return 0;
}

uint32_t cons_diag_parse(const char *s, int n) {
  uint32_t m = 0;
  int i = 0;
  if (n < 0) {
    n = 0;
    while (s[n]) n++;
  }
  while (i < n) {
    while (i < n && (s[i] == ',' || s[i] == ' ' || s[i] == '\n' || s[i] == '\r'))
      i++;
    int b = i;
    while (i < n && s[i] != ',' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r')
      i++;
    int len = i - b;
    if (len <= 0)
      continue;
#define CONS_WORD(w) (len == (int)sizeof(w) - 1 && cons_eq(s + b, w, len))
    if (CONS_WORD("all") || CONS_WORD("1"))
      return CONS_D_ALL;
    for (int c = 0; c < CONS_D_NCLASS; c++) {
      const char *nm = cons_dnames[c];
      int k = 0;
      while (nm[k] && k < len && nm[k] == s[b + k])
        k++;
      if (!nm[k] && k == len)
        m |= 1u << c;
    }
#undef CONS_WORD
  }
  return m;
}

/* ---- kernel line slots ------------------------------------------------ */

/* A wire id per slot: 2 + its index (0 = no open line, 1 = "unknown", user
   ids have bit 63 set). */
static uint64_t slot_id(struct cons_slot *sl) {
  return 2 + (uint64_t)(sl - &cons_slot[0][0]);
}

/* Push a slot's bytes to the wire.  ends: they finish the line. */
static void slot_push(uint32_t cpu, struct cons_slot *sl, int ends) {
  if (ends)
    sl->b[sl->n++] = '\n';
  if (!sl->cont)
    sl->drop = (uint8_t)cons_filter(sl->b, sl->n);
  if (!sl->drop) {
    int got = wire_lock(cpu, cons_hw_in_panic() ? CONS_WAIT_PANIC_US : CONS_WAIT_US);
    if (got == 0) {
      cons_stats.forced++;
      wire_tag = 1;
    }
    wire_piece(slot_id(sl), sl->b, sl->n, ends);
    wire_unlock(cpu);
    if (ends)
      cons_stats.lines++;
  }
  sl->n = 0;
  if (ends) {
    sl->cont = 0;
    sl->drop = 0;
    sl->used = 0;
  } else {
    sl->cont = 1;
  }
}

static int sp_far(const char *a, const char *b) {
  long d = (long)(a - b);
  return (d < 0 ? -d : d) > CONS_NEST_SP;
}

static struct cons_slot *slot_for(uint32_t cpu, const void *ctx, int irqon, const char *sp) {
  struct cons_slot *s = cons_slot[cpu];
  struct cons_slot *free_s = 0, *idle = 0, *oldest = 0;
  for (int i = 0; i < CONS_SLOTS; i++) {
    struct cons_slot *x = &s[i];
    if (!x->used) {
      if (!free_s) free_s = x;
      continue;
    }
    if (x->ctx == ctx && !((x->irqon != (uint8_t)irqon) && sp_far(x->sp, sp)))
      return x;
    if (x->n == 0 && (!idle || x->t_us < idle->t_us))
      idle = x;
    if (!oldest || x->t_us < oldest->t_us)
      oldest = x;
  }
  struct cons_slot *x = free_s ? free_s : idle;
  if (!x) {
    x = oldest; /* all three hold text: push the oldest out as a piece */
    cons_stats.evicted++;
    slot_push(cpu, x, 0);
  }
  x->used = 1;
  x->n = 0;
  x->cont = 0;
  x->drop = 0;
  return x;
}

static void raw_write(const char *s, int n) {
  /* Re-entered (a fault inside this file) or an impossible CPU number: write
     straight through, tagged, and never touch the lock. */
  cons_stats.reentered++;
  cons_hw_tx("\n[CONS!] ", 9);
  cons_hw_tx(s, n);
}

void cons_kwrite(const char *s, int n) {
  if (!s)
    return;
  if (n < 0) {
    n = 0;
    while (s[n]) n++;
  }
  if (n == 0)
    return;
  uint64_t fl = cons_hw_irqsave();
  uint32_t cpu = get_cpuid();
  if (cpu >= CONS_NCPU || cons_nest[cpu]) {
    raw_write(s, n);
    cons_hw_irqrestore(fl);
    return;
  }
  cons_nest[cpu] = 1;
#ifdef CONS_HOST_TEST
  const char *sp = cons_host_sp;
#else
  volatile char here = 0;
  const char *sp = (const char *)&here;
#endif
  const void *ctx = cons_hw_ctx(cpu);
  int irqon = cons_hw_irqs_on(fl);
  struct cons_slot *sl = slot_for(cpu, ctx, irqon, sp);
  sl->ctx = ctx;
  sl->sp = sp;
  sl->irqon = (uint8_t)irqon;
  for (int i = 0; i < n; i++) {
    if (sl->n == 0)
      sl->t_us = timer_get_us();
    char c = s[i];
    if (c == '\n') {
      slot_push(cpu, sl, 1);
      if (i + 1 < n) {
        /* More text after the newline: the next line belongs to the same
           context; re-claim the slot just released. */
        sl->used = 1;
        sl->ctx = ctx;
        sl->sp = sp;
        sl->irqon = (uint8_t)irqon;
      }
      continue;
    }
    if (sl->n >= CONS_LINE) {
      cons_stats.split++;
      slot_push(cpu, sl, 0);
    }
    sl->b[sl->n++] = c;
  }
  int dirty = 0;
  for (int i = 0; i < CONS_SLOTS; i++)
    if (cons_slot[cpu][i].used && cons_slot[cpu][i].n)
      dirty = 1;
  cons_dirty[cpu] = (uint8_t)dirty;
  cons_nest[cpu] = 0;
  cons_hw_irqrestore(fl);
}

void cons_kputc(char c) { cons_kwrite(&c, 1); }

static void flush_slots(uint32_t cpu, uint64_t older_than_us, uint64_t now) {
  for (int i = 0; i < CONS_SLOTS; i++) {
    struct cons_slot *x = &cons_slot[cpu][i];
    if (!x->used || x->n == 0)
      continue;
    if (older_than_us && now - x->t_us < older_than_us)
      continue;
    cons_stats.stale++;
    slot_push(cpu, x, 0);
  }
}

void cons_flush_local(void) {
  uint64_t fl = cons_hw_irqsave();
  uint32_t cpu = get_cpuid();
  if (cpu < CONS_NCPU && !cons_nest[cpu]) {
    cons_nest[cpu] = 1;
    flush_slots(cpu, 0, 0);
    cons_dirty[cpu] = 0;
    cons_nest[cpu] = 0;
  }
  cons_hw_irqrestore(fl);
}

/* ---- user writes ------------------------------------------------------ */

#define CONS_USER_ID(who) ((1ULL << 63) | (who))

static void upend_out(struct cons_upend *e) {
  /* A held partial user line goes out as an open piece. */
  if (e->n)
    wire_piece(CONS_USER_ID(e->who), e->b, e->n, 0);
  e->n = 0;
  e->who = 0;
}

static struct cons_upend *upend_find(uint64_t who) {
  for (int i = 0; i < CONS_UPEND; i++)
    if (cons_upend[i].who == who && cons_upend[i].n)
      return &cons_upend[i];
  return 0;
}

static struct cons_upend *upend_new(uint64_t who) {
  struct cons_upend *o = 0;
  for (int i = 0; i < CONS_UPEND; i++) {
    struct cons_upend *e = &cons_upend[i];
    if (e->n == 0) {
      e->who = who;
      return e;
    }
    if (!o || e->t_us < o->t_us)
      o = e;
  }
  cons_stats.evicted++;
  upend_out(o);
  o->who = who;
  return o;
}

void cons_user_write(uint64_t who, const char *s, int n) {
  if (n <= 0)
    return;
  uint64_t fl = cons_hw_irqsave();
  uint32_t cpu = get_cpuid();
  if (cpu >= CONS_NCPU || cons_nest[cpu]) {
    raw_write(s, n);
    cons_hw_irqrestore(fl);
    return;
  }
  cons_nest[cpu] = 1;
  cons_stats.user_writes++;
  if (wire_lock(cpu, CONS_WAIT_US) == 0) {
    cons_stats.forced++;
    wire_tag = 1;
  }
  uint64_t id = CONS_USER_ID(who);
  struct cons_upend *e = upend_find(who);
  int last = -1;
  for (int i = n - 1; i >= 0; i--)
    if (s[i] == '\n') { last = i; break; }
  if (last < 0) {
    /* No newline: hold it if it fits, else write it all as an open piece. */
    if (!e)
      e = upend_new(who);
    if (e->n + n <= CONS_UPEND_CAP) {
      if (e->n == 0)
        e->t_us = timer_get_us();
      for (int i = 0; i < n; i++)
        e->b[e->n + i] = s[i];
      e->n += n;
    } else {
      wire_piece(id, e->b, e->n, 0);
      wire_piece(id, s, n, 0);
      e->n = 0;
      e->who = 0;
    }
  } else {
    if (e) {
      wire_piece(id, e->b, e->n, 0);
      e->n = 0;
      e->who = 0;
    }
    wire_piece(id, s, last + 1, 1);
    int rem = n - last - 1;
    if (rem > 0) {
      if (rem <= CONS_UPEND_CAP) {
        e = upend_new(who);
        e->t_us = timer_get_us();
        for (int i = 0; i < rem; i++)
          e->b[i] = s[last + 1 + i];
        e->n = rem;
      } else {
        wire_piece(id, s + last + 1, rem, 0);
      }
    }
  }
  wire_unlock(cpu);
  cons_nest[cpu] = 0;
  cons_hw_irqrestore(fl);
}

/* ---- the tick --------------------------------------------------------- */

static uint64_t cons_last_summary_us;
static uint64_t cons_summary_sig;

static void cons_kl_s(char *b, int *n, int cap, const char *s) {
  while (*s && *n < cap - 1) b[(*n)++] = *s++;
}
static void cons_kl_u(char *b, int *n, int cap, uint64_t v) {
  char t[24];
  int k = 0;
  if (v == 0) t[k++] = '0';
  while (v && k < 23) { t[k++] = (char)('0' + v % 10); v /= 10; }
  while (k && *n < cap - 1) b[(*n)++] = t[--k];
}

static void cons_summary(uint64_t now) {
  uint64_t sig = cons_stats.forced * 7 + cons_stats.broken * 13 + cons_stats.reentered * 17 +
                 cons_stats.evicted * 19 + cons_stats.split * 23;
  for (int c = 0; c < CONS_D_NCLASS; c++)
    sig = sig * 31 + cons_stats.dropped[c];
  if (sig == cons_summary_sig || now - cons_last_summary_us < CONS_SUMMARY_US)
    return;
  cons_summary_sig = sig;
  cons_last_summary_us = now;
  char b[400];
  int n = 0;
  cons_kl_s(b, &n, sizeof b, "[CONS] lines=");
  cons_kl_u(b, &n, sizeof b, cons_stats.lines);
  cons_kl_s(b, &n, sizeof b, " user=");
  cons_kl_u(b, &n, sizeof b, cons_stats.user_writes);
  cons_kl_s(b, &n, sizeof b, " forced=");
  cons_kl_u(b, &n, sizeof b, cons_stats.forced);
  cons_kl_s(b, &n, sizeof b, " broken=");
  cons_kl_u(b, &n, sizeof b, cons_stats.broken);
  cons_kl_s(b, &n, sizeof b, " raw=");
  cons_kl_u(b, &n, sizeof b, cons_stats.reentered);
  cons_kl_s(b, &n, sizeof b, " stale=");
  cons_kl_u(b, &n, sizeof b, cons_stats.stale);
  cons_kl_s(b, &n, sizeof b, " evicted=");
  cons_kl_u(b, &n, sizeof b, cons_stats.evicted);
  cons_kl_s(b, &n, sizeof b, " split=");
  cons_kl_u(b, &n, sizeof b, cons_stats.split);
  cons_kl_s(b, &n, sizeof b, " diag-off:");
  for (int c = 0; c < CONS_D_NCLASS; c++) {
    if (cons_diag_mask & (1u << c))
      continue;
    cons_kl_s(b, &n, sizeof b, " ");
    cons_kl_s(b, &n, sizeof b, cons_dnames[c]);
    cons_kl_s(b, &n, sizeof b, "=");
    cons_kl_u(b, &n, sizeof b, cons_stats.dropped[c]);
  }
  cons_kl_s(b, &n, sizeof b, " (CP_DIAG=all to print)\n");
  b[n] = 0;
  cons_kwrite(b, n);
}

void cons_tick(uint32_t cpu) {
  if (cpu >= CONS_NCPU || cons_nest[cpu])
    return;
  if (!cons_dirty[cpu] && cpu != 0)
    return;
  uint64_t now = timer_get_us();
  uint64_t fl = cons_hw_irqsave();
  cons_nest[cpu] = 1;
  if (cons_dirty[cpu]) {
    int left = 0;
    for (int i = 0; i < CONS_SLOTS; i++) {
      struct cons_slot *x = &cons_slot[cpu][i];
      if (!x->used || x->n == 0)
        continue;
      if (now - x->t_us >= CONS_KSTALE_US) {
        cons_stats.stale++;
        slot_push(cpu, x, 0);
      } else {
        left = 1;
      }
    }
    cons_dirty[cpu] = (uint8_t)left;
  }
  if (cpu == 0) {
    int any = 0;
    for (int i = 0; i < CONS_UPEND; i++)
      if (cons_upend[i].n && now - cons_upend[i].t_us >= CONS_USER_STALE_US)
        any = 1;
    if (any && wire_lock(cpu, 0) > 0) {
      for (int i = 0; i < CONS_UPEND; i++) {
        struct cons_upend *e = &cons_upend[i];
        if (e->n && now - e->t_us >= CONS_USER_STALE_US) {
          cons_stats.stale++;
          upend_out(e);
        }
      }
      wire_unlock(cpu);
    }
  }
  cons_nest[cpu] = 0;
  cons_hw_irqrestore(fl);
  if (cpu == 0)
    cons_summary(now);
}

#ifdef CONS_HOST_TEST
/* Make it look as if CPU owner-1 holds the wire (0: nobody). */
void cons_test_set_owner(uint32_t owner) {
  if (owner) {
    wire_holder = owner;
    wire_next = wire_serving + 1;
  } else {
    wire_holder = 0;
    wire_serving = wire_next;
  }
}
#endif

/* ---- init ------------------------------------------------------------- */

static int cons_eq(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

void cons_init(void) {
  cons_diag_mask = cons_diag_parse(CP_DIAG_DEFAULT, -1);
  char arg[96];
  int n = cons_hw_bootarg(arg, (int)sizeof arg - 1);
  if (n > 0) {
    arg[n] = 0;
    cons_diag_mask = cons_diag_parse(arg, n);
  }
  char b[200];
  int k = 0;
  cons_kl_s(b, &k, sizeof b, "[CONS] line-atomic console: diag=");
  if (cons_diag_mask == CONS_D_ALL) {
    cons_kl_s(b, &k, sizeof b, "all");
  } else {
    int any = 0;
    for (int c = 0; c < CONS_D_NCLASS; c++)
      if (cons_diag_mask & (1u << c)) {
        cons_kl_s(b, &k, sizeof b, any ? "," : "");
        cons_kl_s(b, &k, sizeof b, cons_dnames[c]);
        any = 1;
      }
    if (!any)
      cons_kl_s(b, &k, sizeof b, "off");
  }
  cons_kl_s(b, &k, sizeof b, n > 0 ? " (fw_cfg opt/hobbyos/diag)\n" : " (build default)\n");
  b[k] = 0;
  cons_kwrite(b, k);
}
