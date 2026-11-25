#include "lock.h"
#include "kline.h"
#include "console.h"
#include "process.h"
#include <stdint.h>

/** C0-console: no longer taken by anything.  The console's one lock is the
    private wire word in src/kernel/kconsole.c (a bounded wait, never a
    spinlock_t, so the lock screams can print through it).  The symbol stays
    so an out-of-tree caller still links; docs/cp-console.md. */
spinlock_t uart_lock;

#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

void com2_init(void);

/**
 * Initializes the PC COM1 serial port.
 */
void uart_init(void) {
  spinlock_init(&uart_lock);

  outb(COM1_PORT + 1, 0x00);    // Disable all interrupts
  outb(COM1_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
  outb(COM1_PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
  outb(COM1_PORT + 1, 0x00);    //                  (hi byte)
  outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
  outb(COM1_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
  outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
  com2_init();

  /* Print the lock's address once (raw port I/O, one CPU, before the
     console layer is set up): [LOCKFOREVER] screams report raw lock
     addresses.  Since C0-console the console lock is not a spinlock_t at
     all, so no scream can name it; the row stays for the log's shape. */
  {
    static const char hx[] = "0123456789abcdef";
    static const char pre[] = "[UART] lock @ 0x";
    uint64_t v = (uint64_t)&uart_lock;
    for (const char *p = pre; *p; p++) outb(COM1_PORT, (uint8_t)*p);
    for (int i = 15; i >= 0; i--) outb(COM1_PORT, (uint8_t)hx[(v >> (i * 4)) & 0xF]);
    outb(COM1_PORT, '\r');
    outb(COM1_PORT, '\n');
  }
  cons_init();
}

/**
 * Helper to check if the transmit buffer is empty.
 */
static int is_transmit_empty(void) {
  return inb(COM1_PORT + 5) & 0x20;
}

extern uint64_t timer_get_ms(void);

/* Milliseconds at the last console write.  The stall watchdog in trap.c
   reads this to detect silent hangs during the test wave. */
volatile uint64_t uart_last_activity_ms = 0;

/* ---- the wire (C0-console) ----------------------------------------------
   cons_hw_tx() is the ONLY code that touches the COM1 registers after boot.
   It takes no lock: src/kernel/kconsole.c calls it with the console's wire
   lock held (or, past that lock's bound, tagged "[CONS!] ").

   The 16550's transmit FIFO is enabled (FCR 0xC7), and THRE (LSR bit 5) set
   means the FIFO is EMPTY, so one THRE observation admits 16 bytes.  Polling
   LSR before every byte cost one extra port exit per byte under KVM. */
#define COM1_FIFO 16
static int com1_room;

/* H7-fault round 2 (H7-07/H7-08, docs/cp-fault.md §12): a console write runs
   with interrupts off, and each byte is a port exit that QEMU serves in user
   space (10-50 us on a loaded host), so a 2-8 KiB write() keeps this CPU deaf
   to the 0x82 shootdown IPI for tens to hundreds of milliseconds.  Settle an
   owed TLB flush every FIFO refill and while waiting for the wire. */
extern void vm_arch_tlb_poll(void);

static void com1_put(uint8_t c) {
  if (com1_room <= 0) {
    vm_arch_tlb_poll();
    for (uint32_t spin = 0; !is_transmit_empty() && spin < 10000000u; spin++)
      __asm__ volatile("pause");
    com1_room = COM1_FIFO;
  }
  outb(COM1_PORT, c);
  com1_room--;
}

/* H7-fault round 3 (cp-hunt H6-09): this CPU is inside cons_hw_tx.  A port
   exit the host does not return from (a host I/O stall: QEMU's serial write
   blocked; h3-d05 held the wire 2 s) cannot poll, so a shootdown targeting
   this CPU waited out the stall ([TLBTIMEOUT]).  The shooter now leaves the
   flush owed to a CPU that is in here (vm_arch_shootdown): nothing in here
   touches a user mapping, and the flag is dropped BEFORE the last poll, so a
   CPU seen in here still flushes before it leaves. */
/* One cache-line record per CPU (C1-k7 invariant: every console write sets
   and clears its own CPU's mark, so a packed byte array would bounce one
   line between all CPUs). */
struct x64_cons_mark x64_in_console[MAX_CPUS];
#ifdef KERNEL_MODE_TEST
/* Test kernels: stall <ms> as a port exit the host does not return from --
   inside the console window, interrupts off, no poll (kconsole.c calls it for
   a user line "[LPSTALL] <ms>"; src/user/linux_probe_consstall.c). */
uint64_t cons_hw_irqsave(void);
void cons_hw_irqrestore(uint64_t f);
void cons_hw_stall_ms(uint32_t ms) {
  uint32_t me = get_cpuid();
  uint64_t f = cons_hw_irqsave();
  if (me < MAX_CPUS) __atomic_add_fetch(&x64_in_console[me].in, 1, __ATOMIC_SEQ_CST);
  uint64_t t0 = timer_get_ms();
  while (timer_get_ms() - t0 < ms) __asm__ volatile("pause");
  if (me < MAX_CPUS) __atomic_sub_fetch(&x64_in_console[me].in, 1, __ATOMIC_SEQ_CST);
  vm_arch_tlb_poll();
  cons_hw_irqrestore(f);
}
#endif
/* Bytes per CPU pushed into the wire (console.h cons_hw_bytes).  Incremented
   with the in_console mark so the lock audit can attribute an IRQ-off window
   to a console write instead of to a lock. */
static volatile uint32_t x64_cons_bytes[MAX_CPUS];
uint32_t cons_hw_bytes(uint32_t cpu) {
  return cpu < MAX_CPUS ? __atomic_load_n(&x64_cons_bytes[cpu], __ATOMIC_RELAXED) : 0;
}

void cons_hw_tx(const char *s, int n) {
  uint32_t me = get_cpuid();
  if (me < MAX_CPUS) __atomic_add_fetch(&x64_in_console[me].in, 1, __ATOMIC_SEQ_CST);
  if (me < MAX_CPUS && n > 0)
    __atomic_add_fetch(&x64_cons_bytes[me], (uint32_t)n, __ATOMIC_RELAXED);
  uart_last_activity_ms = timer_get_ms();
  for (int i = 0; i < n; i++) {
    if (s[i] == '\n')
      com1_put('\r');
    com1_put((uint8_t)s[i]);
  }
  if (me < MAX_CPUS) __atomic_sub_fetch(&x64_in_console[me].in, 1, __ATOMIC_SEQ_CST);
  vm_arch_tlb_poll(); /* after the flag: a flush owed to us is settled here */
}

uint64_t cons_hw_irqsave(void) {
  uint64_t f;
  __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) : : "memory");
  return f;
}

void cons_hw_irqrestore(uint64_t f) {
  if (f & 0x200)
    __asm__ volatile("sti" : : : "memory");
}

int cons_hw_irqs_on(uint64_t f) { return (f & 0x200) != 0; }

const void *cons_hw_ctx(uint32_t cpu) {
  return cpu < MAX_CPUS ? (const void *)cpu_locals[cpu].current_proc : 0;
}

void cons_hw_relax(void) {
  __asm__ volatile("pause");
  vm_arch_tlb_poll(); /* H7-07: waiting for the wire with interrupts off */
}

/* fw_cfg (QEMU, legacy port interface): opt/hobbyos/diag, the boot-time
   diagnostic knob (-fw_cfg name=opt/hobbyos/diag,string=all). */
static uint16_t fwcfg_be16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static uint32_t fwcfg_be32(const uint8_t *b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static void fwcfg_sel(uint16_t k) { __asm__ volatile("outw %0, %1" : : "a"(k), "Nd"((uint16_t)0x510)); }
static uint8_t fwcfg_rd(void) { return inb(0x511); }

int cons_hw_bootarg(char *buf, int cap) {
  static const char want[] = "opt/hobbyos/diag";
  uint8_t sig[4];
  fwcfg_sel(0x0000);
  for (int i = 0; i < 4; i++) sig[i] = fwcfg_rd();
  if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U')
    return 0;
  fwcfg_sel(0x0019);
  uint8_t h[4];
  for (int i = 0; i < 4; i++) h[i] = fwcfg_rd();
  uint32_t count = fwcfg_be32(h);
  for (uint32_t f = 0; f < count && f < 256; f++) {
    uint8_t e[64];
    for (int i = 0; i < 64; i++) e[i] = fwcfg_rd();
    int match = 1;
    for (int i = 0; i < (int)sizeof want; i++)
      if (e[8 + i] != (uint8_t)want[i]) { match = 0; break; }
    if (!match)
      continue;
    uint32_t size = fwcfg_be32(e);
    fwcfg_sel(fwcfg_be16(e + 4));
    int n = 0;
    for (uint32_t i = 0; i < size; i++) {
      char c = (char)fwcfg_rd();
      if (n < cap && c) buf[n++] = c;
    }
    return n;
  }
  return 0;
}

/* GB-206: writer for debug instruments that may be called from inside a
   console print.  Since C0-console it is line-buffered like every other
   kernel writer (kconsole.c): the bytes reach the wire as whole lines. */
void uart_putc_dbg(char c) {
  cons_kputc(c);
}

/* ---- GB-097: the raw sink is for NON-RETURNING paths only.

   The rename to panic_* makes a returning caller implausible; this makes it
   DETECTED.  panic_enter()/panic_leave() bracket the fatal printer, which is
   the only legitimate user of the raw sink while the console lock is held or
   wedged.  Reaching the sink from anywhere else increments a counter and
   reports ONE record per boot on COM2 -- COM2 is a dedicated port that nothing
   else writes, so the violation report cannot itself shred COM1 (and taking
   uart_lock here would be the deadlock the raw sink exists to avoid). */
volatile int panic_depth = 0;
static volatile int panic_sink_violations = 0;
/* GB-204: every dropped record is counted BEFORE the cap check, so past the
   cap the count still moves.  The same counter counts a second CPU's failed
   claim: a claim loser is a dropped record too. */
static volatile int panic_sink_dropped = 0;
/* GB-209: one summary line per boot on COM1, and the open-record latch that
   makes the counter count records instead of characters. */
static volatile int panic_sink_summarized = 0;
static volatile int panic_sink_record_open = 0;
static volatile int panic_sink_claim = 0;
/* GB-204: one claim word.  The first CPU to exchange 0->1 owns the fatal
   record; every other CPU sees 1, counts the drop and halts without writing. */
static volatile int panic_claim_word = 0;

void panic_puts2(const char *s); /* COM2 sink, defined below */
void panic_putc2(char c);
void print_int_raw2(int val);

int cons_hw_in_panic(void) { return panic_depth > 0; }

int panic_claim(void) {
  if (__atomic_exchange_n(&panic_claim_word, 1, __ATOMIC_ACQ_REL) != 0) {
    int d = __atomic_add_fetch(&panic_sink_dropped, 1, __ATOMIC_RELAXED);
    /* COM2: the loser's own record, so a dropped fatal record is visible
       rather than inferred from a missing line (GB-204). */
    panic_puts2("[PANIC-CLAIM] dropped=");
    print_int_raw2(d);
    panic_putc2('\n');
    return 0;
  }
  panic_depth++;
  return 1;
}

void panic_leave(void) {
  if (panic_depth > 0) panic_depth--;
  __atomic_store_n(&panic_claim_word, 0, __ATOMIC_RELEASE);
}

void panic_enter(void) { panic_depth++; }

static void panic_sink_violation(const char *what, const char *s) {
  /* GB-209: panic_putc() arrives once per CHARACTER and panic_puts() once per
     FRAGMENT, so the old per-call increment counted bytes (78,781 for one
     returning-path diagnostic).  Count a RECORD — the span between newlines —
     and emit one record per line. */
  if (panic_sink_record_open)
    return;                     /* mid-record: already counted and reported */
  /* GB-209: a sink-local claim (GB-097 clause 1.2's claim word is the fatal
     record's; this one is the violation reporter's).  Two returning CPUs can
     enter here at once; only one may write, or the reports shred each other.
     Deliberately NOT uart_lock: a halted CPU can own it (measured:
     [LOCKFOREVER] cpu=0 owner=1 owner_hb_age=dead), and a diagnostic that
     deadlocks the console is worse than one that is imperfect. */
  if (__atomic_exchange_n(&panic_sink_claim, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  panic_sink_record_open = 1;
  int d = __atomic_add_fetch(&panic_sink_dropped, 1, __ATOMIC_RELAXED);

  /* COM2: the full record.  COM2 is dedicated (nothing else writes it), so a
     record here cannot be byte-interleaved by another CPU's console writer. */
  panic_puts2("[PANIC-SINK-VIOLATION] ");
  panic_puts2(what);
  panic_puts2(" from a returning path suppressed=");
  print_int_raw2(d);
  if (s) {
    panic_puts2(": ");
    for (int i = 0; s[i] && i < 48; i++) panic_putc2(s[i]);
  }
  panic_putc2('\n');

  /* COM1: exactly ONE summary line per boot, under the sink claim.  This is a
     returning path (panic_depth == 0); the raw sink's lock-free rule applies
     to the non-returning fatal printer, but uart_lock itself is off-limits
     here (see above).  The old "byte-wise here can itself shred one line,
     accepted" comment is gone: that acceptance was the defect (GB-097-V2). */
  if (__atomic_exchange_n(&panic_sink_summarized, 1, __ATOMIC_ACQ_REL) == 0) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[PANIC-SINK-VIOLATION] ");
    kl_s(&kl, what);
    kl_s(&kl, " from a returning path (suppressed=");
    kl_i(&kl, d);
    kl_s(&kl, ")\n");
    kl_emit(&kl); /* C0-console: one line through the line-atomic console */
  }
  panic_sink_violations++;
  __atomic_store_n(&panic_sink_claim, 0, __ATOMIC_RELEASE);
}

/** Raw single character — lock-free, for NON-RETURNING paths only (GB-097). */
void panic_putc(char c) {
  if (!panic_depth) panic_sink_violation("panic_putc", 0);
  cons_kputc(c);
  if (c == '\n') panic_sink_record_open = 0;   /* GB-209: record boundary */
}

/* ---- COM2: a second, uncontaminated diagnostic stream (x64 only).
   The console (COM1) interleaves every core's writes even though each
   logical write is atomic, so a 20s-stall watchdog dump gets shredded by
   the loader's console noise.  QEMU maps COM2 at 0x2F8; `-serial file:`
   captures it to its own log, byte-exact.  All writers here are lock-free
   by construction (nothing else uses COM2). */

static void com2_outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static uint8_t com2_inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}
static int com2_tx_empty(void) { return com2_inb(COM2_PORT + 5) & 0x20; }

void com2_init(void) {
  com2_outb(COM2_PORT + 1, 0x00);
  com2_outb(COM2_PORT + 3, 0x80);
  com2_outb(COM2_PORT + 0, 0x03);
  com2_outb(COM2_PORT + 1, 0x00);
  com2_outb(COM2_PORT + 3, 0x03);
  com2_outb(COM2_PORT + 2, 0xC7);
  com2_outb(COM2_PORT + 4, 0x0B);
}

void panic_putc2(char c) {
  if (c == '\n') {
    while (!com2_tx_empty()) {}
    com2_outb(COM2_PORT, '\r');
  }
  while (!com2_tx_empty()) {}
  com2_outb(COM2_PORT, c);
}

void panic_puts2(const char *s) {
  while (*s != '\0') {
    panic_putc2(*s);
    s++;
  }
}

void panic_print_hex2(uint64_t val) {
  static const char hex_chars[] = "0123456789ABCDEF";
  panic_putc2('0');
  panic_putc2('x');
  for (int i = 60; i >= 0; i -= 4) {
    panic_putc2(hex_chars[(val >> i) & 0xF]);
  }
}

void print_int_raw2(int val) {
  if (val < 0) {
    panic_putc2('-');
    val = -val;
  }
  if (val == 0) {
    panic_putc2('0');
    return;
  }
  char buf[16];
  int idx = 0;
  while (val > 0) {
    buf[idx++] = (char)('0' + (val % 10));
    val /= 10;
  }
  while (idx > 0)
    panic_putc2(buf[--idx]);
}

/** Raw string — lock-free, for NON-RETURNING paths only (GB-097). */
void panic_puts(const char *s) {
  if (!panic_depth) panic_sink_violation("panic_puts", s);
  const char *p = s;
  while (*p != '\0')
    p++;
  cons_kwrite(s, (int)(p - s));
  if (p > s && p[-1] == '\n') panic_sink_record_open = 0;   /* GB-209: record boundary */
}

/**
 * One character into this CPU's line slot (src/kernel/kconsole.c); it reaches
 * COM1 with the rest of its line.
 */
void uart_putc(char c) {
  cons_kputc(c);
}

/**
 * A string into this CPU's line slot: every complete line in it is written
 * under one console lock hold, a trailing partial waits for its newline.
 */
void uart_puts(const char *s) {
  cons_kwrite(s, -1);
}

#ifdef PANIC_IPI_TEST
/* ---- GB-207: opt-in two-CPU panic-injection vehicle ---------------------
   A test-only IPI vector (0x83) makes CPU1 enter the fatal-record path while
   CPU0 holds GB-204's claim and is writing its own record.  The proof is one
   record and one counted drop (panic_sink_dropped); the same vehicle built on
   the pre-fix tree, where every CPU writes, shows two records.  The driver is
   selected by the build flag PANIC_IPI_TEST and is absent from every default
   kernel (no boot-time cost, no default-wave timing change). */
extern uint32_t get_cpuid(void);
extern void print_int_raw(int val);
extern void x64_send_test_ipi(uint32_t cpu, uint32_t vector);

volatile int gb207_cpu1_ready = 0;
static volatile int gb207_ipi_seen = 0;   /* the IPI arrived; reporting only */
static volatile int gb207_ipi_done = 0;   /* cpu1 finished its claim attempt */
static volatile int gb207_records = 0;

#ifndef GB207_PREFIX_NO_CLAIM
int panic_dropped_count(void) {
  return __atomic_load_n(&panic_sink_dropped, __ATOMIC_RELAXED);
}
#endif

static void gb207_record(const char *who, uint32_t cpu) {
  __atomic_add_fetch(&gb207_records, 1, __ATOMIC_RELAXED);
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[KERNEL] FATAL: GB-207 injected record by ");
  kl_s(&kl, who);
  kl_s(&kl, "  CPU: ");
  kl_i(&kl, (int)cpu);
  kl_c(&kl, '\n');
  kl_emit(&kl); /* one record, one lock (GB-082) */
}

/* Vector 0x83, CPU1's context (trap.c dispatch).  A claim loser must write
   nothing: panic_claim() has already counted the drop and put the COM2 line
   on the wire.  Returning keeps CPU1 alive for the rest of the boot, which is
   what makes this vehicle non-perturbing to the wave that follows.  The
   completion flag is set only AFTER the claim attempt: setting it at entry
   lets CPU0 release while CPU1 sits between the flag and panic_claim(), and a
   host preemption in that window hands CPU1 the claim and a second record. */
void gb207_ipi_handler(void) {
  uint32_t me = get_cpuid();
  gb207_ipi_seen = 1;
#ifdef GB207_PREFIX_NO_CLAIM
  /* Pre-fix tree: no claim exists, so this CPU writes its own fatal record —
     exactly the two-record failure GB-204 fixes. */
  gb207_record("cpu1", me);
  gb207_ipi_done = 1;
#else
  if (!panic_claim()) {
    gb207_ipi_done = 1; /* the attempt has happened; cpu0 may now release */
    return;
  }
  gb207_record("cpu1", me);
  panic_leave();
  gb207_ipi_done = 1;
#endif
}

void gb207_run(void) {
  if (get_cpuid() != 0)
    return;
  for (volatile uint64_t spin = 0; spin < 400000000ULL && !gb207_cpu1_ready; spin++)
    __asm__ volatile("pause");
  if (!gb207_cpu1_ready) {
    uart_puts("[GB207] SKIP: cpu1 never reached interrupts_enable\n");
    return;
  }
  uart_puts("[GB207] two-CPU panic injection: cpu0 claims, fires IPI 0x83 at cpu1 mid-record\n");
#ifndef GB207_PREFIX_NO_CLAIM
  if (!panic_claim()) {
    uart_puts("[GB207] FAIL: cpu0 could not claim the sink\n");
    return;
  }
  int dropped_before = panic_dropped_count();
#endif
  /* Send before writing and leave only after cpu1's attempt: the claim is
     held across the whole record, so cpu1's claim can only lose.  Retry a few
     times: a fixed IPI written while the ICR is busy is silently lost. */
  for (int attempt = 0; attempt < 2 && !gb207_ipi_done; attempt++) {
    x64_send_test_ipi(1, 0x83);
    for (volatile uint64_t spin = 0; spin < 20000000ULL && !gb207_ipi_done; spin++)
      __asm__ volatile("pause");
  }
  gb207_record("cpu0", get_cpuid());
  /* Completion gate: cpu1 sets gb207_ipi_done only after its claim attempt, so
     this wait cannot release the sink into the flag-to-claim window. */
  for (volatile uint64_t spin = 0; spin < 20000000ULL && !gb207_ipi_done; spin++)
    __asm__ volatile("pause");
#ifndef GB207_PREFIX_NO_CLAIM
  panic_leave();
#endif
  {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[GB207] records=");
    kl_i(&kl, __atomic_load_n(&gb207_records, __ATOMIC_RELAXED));
#ifndef GB207_PREFIX_NO_CLAIM
    kl_s(&kl, " dropped_before=");
    kl_i(&kl, dropped_before);
    kl_s(&kl, " dropped_after=");
    kl_i(&kl, panic_dropped_count());
#else
    kl_s(&kl, " mode=pre-fix-no-claim");
#endif
    kl_s(&kl, gb207_ipi_done ? " ipi=seen\n"
                             : (gb207_ipi_seen ? " ipi=delivered_nodone\n"
                                               : " ipi=MISSED\n"));
    kl_emit(&kl);
  }
}

/* Called from the CPU0 timer tick (trap.c, vector 32).  The injection must
   run AFTER the wave starts: a secondary sits in start_scheduler's IF=0 wait
   until there is a process to run, so an early IPI is never delivered
   (measured: ipi=MISSED within the first second of boot). */
void gb207_tick(void) {
  static int fired = 0;
  extern volatile int scheduler_started;
  if (fired || get_cpuid() != 0)
    return;
  if (!scheduler_started || timer_get_ms() < 4000)
    return;
  fired = 1;
  gb207_run();
}
#endif
