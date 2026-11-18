/*
 * src/kernel/linux_compat_away.c -- [AWAY]: what a periodic waiter does while
 * it is away from its clock (H4-kglitch round 2, H5-05; docs/cp-kglitch.md §12).
 *
 * H5-audio2 found that the game's WASAPI client thread leaves its loop for
 * 31-260 ms at a time (97 of 97 audible lead-ins), while a waiting thread
 * answers its 10 ms event within ~5 ms.  This instrument names what the
 * thread is doing while it is away, without knowing its tid in advance:
 *
 *   1. A CLOCK is an ntsync event that is SET 50-200 times a second
 *      (winealsa's timer thread sets the client's event every 10 ms period).
 *      away_clock_set() counts sets per event in a 1 s window.
 *   2. A thread that WAITS on a clock is a periodic waiter (the audio client).
 *      away_wait_return() stamps the moment each of its waits returns;
 *      away_wait_begin() at its next wait measures how long it was away.
 *   3. While it is away, CPU 0's 100 Hz thread sampler (process.c
 *      cp_wait_sample) calls away_sample() for it every tick with what the
 *      thread is doing: U user code, K in a syscall (nr) on a CPU with that
 *      CPU's kernel PC, T a fault or interrupt, R runnable, F futex park,
 *      B parked in a syscall (nr, futex word), O other.
 *   4. An absence of >= AWAY_MIN_MS prints one row (budget AWAY_ROWS):
 *        [AWAY] tid= tgid= ms= at= clock=0x.. n= U= R= T= F=(0x..) K=nr:n,..
 *               B=nr:n,.. pc=0x..,0x..
 *      `at` is the uptime (ms) the absence began, to line it up with
 *      [NFSPIPE] / [SND] zeros / [WALSA] rows; pc values are kernel PCs
 *      (symbolise with nm hobbyos.elf).
 *
 * Printing only; no behaviour changes.  x86_64 only (the Linux ABI).
 */
#include <stdint.h>

#include "kline.h"
#include "process.h"

#if defined(__x86_64__)

#define AWAY_MIN_MS 30
#define AWAY_ROWS 400
#define AWAY_CLOCKS 16
#define AWAY_NK 4
#define AWAY_NPC 3

extern uint64_t timer_get_ms(void);

struct away_clock {
  const void *obj;
  uint64_t win_ms;
  uint32_t sets;
  uint8_t is_clock;
};
static struct away_clock away_clk[AWAY_CLOCKS];

struct away_kv {
  int16_t nr;
  uint16_t n;
};
struct away_thr {
  uint8_t flagged;  /* waits on a clock */
  uint8_t away;     /* between a wait's return and the next wait */
  const void *clock;
  uint64_t ret_ms;  /* when its last clock wait returned */
  uint32_t gen;
  uint16_t u, r, t, f, o, ns;
  uint64_t fword;
  struct away_kv k[AWAY_NK], b[AWAY_NK];
  uint64_t pc[AWAY_NPC];
  int waker;        /* pid + 1 of the thread whose FUTEX_WAKE ended its park */
};
static struct away_thr away_t[MAX_PROCESSES];
static int away_rows;

/* Flight recorder: the last AWAY_FR absences, kept whether or not the row
   budget printed them.  linux_snd's [SND] zeros row calls away_dump_since()
   so the absences behind every audible dropout reach the console, however
   late in the boot it comes (tagged [AWAY-Z]). */
#define AWAY_FR 48
struct away_fr {
  uint64_t at, end;
  uint8_t shown;
  struct kline kl;
};
static struct away_fr away_fr[AWAY_FR];
static uint32_t away_fr_n;

/* Every Linux-ABI thread's last AWAY_RING samples (kind << 9 | nr), so a row
   can say what the WAKER was doing while the away thread waited for it. */
#define AWAY_RING 32
static uint16_t away_ring[MAX_PROCESSES][AWAY_RING];
static uint8_t away_ring_pos[MAX_PROCESSES];

void away_ring_note(int slot, int kind, int nr) {
  if (slot <= 0 || slot >= MAX_PROCESSES) return;
  uint8_t p = away_ring_pos[slot];
  away_ring[slot][p % AWAY_RING] = (uint16_t)(((kind & 0x7F) << 9) | (nr & 0x1FF));
  away_ring_pos[slot] = (uint8_t)((p + 1) % AWAY_RING);
}

/* A FUTEX_WAKE by `waker` made `woken` READY: remembered if `woken` is away. */
void away_futex_woken(int woken, int waker) {
  if (woken > 0 && woken < MAX_PROCESSES && away_t[woken].flagged && away_t[woken].away)
    away_t[woken].waker = waker + 1;
}

static int away_is_clock(const void *obj) {
  for (int i = 0; i < AWAY_CLOCKS; i++)
    if (away_clk[i].obj == obj && away_clk[i].is_clock)
      return 1;
  return 0;
}

/* Called for every NTSYNC EVENT_SET / PULSE (racy counters: a diagnostic). */
void away_clock_set(const void *obj) {
  uint64_t now = timer_get_ms();
  int slot = -1, victim = 0;
  for (int i = 0; i < AWAY_CLOCKS; i++) {
    if (away_clk[i].obj == obj) {
      slot = i;
      break;
    }
    if (!away_clk[i].obj) {
      if (slot < 0) slot = i;
    } else if (!away_clk[i].is_clock && away_clk[i].sets < away_clk[victim].sets) {
      victim = i;
    }
  }
  if (slot < 0 || (away_clk[slot].obj && away_clk[slot].obj != obj)) {
    slot = victim;
    if (away_clk[slot].is_clock)
      return; /* the table holds 16 clocks already */
  }
  struct away_clock *c = &away_clk[slot];
  if (c->obj != obj) {
    c->obj = obj;
    c->win_ms = now;
    c->sets = 0;
    c->is_clock = 0;
  }
  c->sets++;
  if (now - c->win_ms >= 1000) {
    if (c->sets >= 50 && c->sets <= 200)
      c->is_clock = 1;
    c->win_ms = now;
    c->sets = 0;
  }
}

static void away_kv_add(struct away_kv *kv, int nr) {
  for (int i = 0; i < AWAY_NK; i++) {
    if (kv[i].n && kv[i].nr == nr) {
      kv[i].n++;
      return;
    }
    if (!kv[i].n) {
      kv[i].nr = (int16_t)nr;
      kv[i].n = 1;
      return;
    }
  }
}

static void away_reset(struct away_thr *a) {
  a->u = a->r = a->t = a->f = a->o = a->ns = 0;
  a->fword = 0;
  for (int i = 0; i < AWAY_NK; i++) {
    a->k[i].n = 0;
    a->b[i].n = 0;
  }
  for (int i = 0; i < AWAY_NPC; i++)
    a->pc[i] = 0;
  a->waker = 0;
}

/* The waker's last `ticks` samples, newest last: "K0:7,U:2,B230:1" style. */
static void kl_waker(struct kline *kl, int w, int ticks) {
  if (w <= 0 || w >= MAX_PROCESSES) return;
  if (ticks > AWAY_RING) ticks = AWAY_RING;
  struct away_kv kv[AWAY_NK + 2];
  int kinds[AWAY_NK + 2];
  int nk = 0;
  uint8_t p = away_ring_pos[w];
  for (int i = 0; i < ticks; i++) {
    uint16_t e = away_ring[w][(p + AWAY_RING - ticks + i) % AWAY_RING];
    if (!e) continue;
    int kind = e >> 9, nr = (kind == 'K' || kind == 'B') ? (e & 0x1FF) : -1;
    int j = 0;
    for (; j < nk; j++)
      if (kinds[j] == kind && kv[j].nr == nr) break;
    if (j == nk) {
      if (nk == AWAY_NK + 2) continue;
      kinds[nk] = kind;
      kv[nk].nr = (int16_t)nr;
      kv[nk].n = 0;
      nk++;
    }
    kv[j].n++;
  }
  kl_s(kl, " wk=");
  for (int j = 0; j < nk; j++) {
    if (j) kl_c(kl, ',');
    kl_c(kl, (char)kinds[j]);
    if (kv[j].nr >= 0) kl_i(kl, kv[j].nr);
    kl_c(kl, ':');
    kl_i(kl, kv[j].n);
  }
}

/* A clock wait of thread `t` returned: it is now away until its next wait. */
void away_wait_return(struct process *t) {
  if (!t || t->pid <= 0 || t->pid >= MAX_PROCESSES)
    return;
  struct away_thr *a = &away_t[t->pid];
  if (!a->flagged)
    return;
  a->ret_ms = timer_get_ms();
  a->gen = process_slot_seq(t->pid);
  a->away = 1;
  away_reset(a);
}

static void kl_kv(struct kline *kl, const char *tag, const struct away_kv *kv) {
  int any = 0;
  for (int i = 0; i < AWAY_NK; i++) {
    if (!kv[i].n)
      continue;
    kl_s(kl, any ? "," : tag);
    kl_i(kl, kv[i].nr);
    kl_c(kl, ':');
    kl_i(kl, kv[i].n);
    any = 1;
  }
}

/* Thread `t` starts an ntsync wait on objs[0..n): if one is a clock, it is a
   periodic waiter, and the time since its last clock wait returned is an
   absence worth a row when it is long. */
void away_wait_begin(struct process *t, void *const *objs, int n) {
  if (!t || t->pid <= 0 || t->pid >= MAX_PROCESSES)
    return;
  const void *clock = 0;
  for (int i = 0; i < n && !clock; i++)
    if (objs[i] && away_is_clock(objs[i]))
      clock = objs[i];
  struct away_thr *a = &away_t[t->pid];
  if (!clock) {
    a->away = 0; /* not a clock wait: whatever it was, it is not the loop */
    return;
  }
  uint64_t now = timer_get_ms();
  if (a->flagged && a->away && a->gen == process_slot_seq(t->pid) && now - a->ret_ms >= AWAY_MIN_MS) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[AWAY] tid=");
    kl_i(&kl, t->pid);
    kl_s(&kl, " tgid=");
    kl_i(&kl, t->tgid);
    kl_s(&kl, " ms=");
    kl_i(&kl, (long)(now - a->ret_ms));
    kl_s(&kl, " at=");
    kl_i(&kl, (long)a->ret_ms);
    kl_s(&kl, " clock=0x");
    kl_hex(&kl, (unsigned long)(uintptr_t)clock);
    kl_s(&kl, " n=");
    kl_i(&kl, a->ns);
    kl_s(&kl, " U=");
    kl_i(&kl, a->u);
    kl_s(&kl, " R=");
    kl_i(&kl, a->r);
    kl_s(&kl, " T=");
    kl_i(&kl, a->t);
    kl_s(&kl, " F=");
    kl_i(&kl, a->f);
    if (a->fword) {
      kl_s(&kl, "(0x");
      kl_hex(&kl, (unsigned long)a->fword);
      kl_c(&kl, ')');
    }
    kl_s(&kl, " O=");
    kl_i(&kl, a->o);
    kl_kv(&kl, " K=", a->k);
    kl_kv(&kl, " B=", a->b);
    for (int i = 0; i < AWAY_NPC; i++) {
      if (!a->pc[i])
        continue;
      kl_s(&kl, i ? "," : " pc=0x");
      if (i) kl_s(&kl, "0x");
      kl_hex(&kl, (unsigned long)a->pc[i]);
    }
    if (a->waker) {
      kl_s(&kl, " waker=");
      kl_i(&kl, a->waker - 1);
      kl_waker(&kl, a->waker - 1, (int)((now - a->ret_ms) / 10));
    }
    kl_c(&kl, ' ');
    kl_s(&kl, t->name);
    if (kl.n > (int)sizeof kl.b - 2) kl.n = (int)sizeof kl.b - 2; /* the newline always fits */
    kl_c(&kl, '\n');
    struct away_fr *fr = &away_fr[away_fr_n++ % AWAY_FR];
    fr->at = a->ret_ms;
    fr->end = now;
    fr->kl = kl;
    fr->shown = 0;
    if (away_rows < AWAY_ROWS) {
      away_rows++;
      fr->shown = 1;
      kl_emit(&kl);
    }
  }
  a->flagged = 1;
  a->clock = clock;
  a->away = 0;
}

/* Print the recorded absences that ended at or after from_ms and were not
   printed yet (the [SND] zeros row: what kept the client away). */
void away_dump_since(uint64_t from_ms) {
  for (uint32_t i = 0; i < AWAY_FR; i++) {
    struct away_fr *fr = &away_fr[i];
    if (!fr->end || fr->shown || fr->end < from_ms) continue;
    fr->shown = 1;
    struct kline z;
    kl_init(&z);
    kl_s(&z, "[AWAY-Z] ");
    for (int j = 7; j < fr->kl.n && z.n < (int)sizeof z.b - 2; j++) z.b[z.n++] = fr->kl.b[j];
    if (z.n && z.b[z.n - 1] != '\n') kl_c(&z, '\n');
    kl_emit(&z);
  }
}

int away_flagged(int slot) {
  return slot > 0 && slot < MAX_PROCESSES && away_t[slot].flagged && away_t[slot].away;
}

/* CPU 0's sampler, once per tick per away thread.  kind: 'U' 'K' 'T' 'R'
   'F' 'B' 'O'; nr the syscall; word the futex word; pc the CPU's PC. */
void away_sample(int slot, int kind, int nr, uint64_t word, uint64_t pc) {
  if (!away_flagged(slot))
    return;
  struct away_thr *a = &away_t[slot];
  if (a->ns < 0xFFFF) a->ns++;
  switch (kind) {
  case 'U': a->u++; break;
  case 'R': a->r++; break;
  case 'T': a->t++; break;
  case 'F': a->f++; if (word) a->fword = word; break;
  case 'K':
    away_kv_add(a->k, nr);
    for (int i = 0; i < AWAY_NPC; i++) {
      if (a->pc[i] == pc) break;
      if (!a->pc[i]) { a->pc[i] = pc; break; }
    }
    break;
  case 'B':
    away_kv_add(a->b, nr);
    if (word) a->fword = word;
    break;
  default: a->o++; break;
  }
}

#else /* !__x86_64__: no Linux ABI */
void away_ring_note(int slot, int kind, int nr) { (void)slot; (void)kind; (void)nr; }
void away_futex_woken(int woken, int waker) { (void)woken; (void)waker; }
void away_dump_since(uint64_t from_ms) { (void)from_ms; }
void away_clock_set(const void *obj) { (void)obj; }
void away_wait_return(struct process *t) { (void)t; }
void away_wait_begin(struct process *t, void *const *objs, int n) {
  (void)t; (void)objs; (void)n;
}
int away_flagged(int slot) { (void)slot; return 0; }
void away_sample(int slot, int kind, int nr, uint64_t word, uint64_t pc) {
  (void)slot; (void)kind; (void)nr; (void)word; (void)pc;
}
#endif
