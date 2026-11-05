#include "lock.h"
#include "kline.h"
#include <stdint.h>

/** The single console lock, shared with the arch-independent print
    helpers in main.c (they serialize against uart_puts/uart_putc so the
    whole UART has exactly one lock — previously uart_puts used this lock
    while print_int used a second one, which let bytes interleave). */
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

  /* Print the lock's address once (raw MMIO — uart_puts itself needs the
     lock and isn't available this early): [LOCKFOREVER] screams report raw
     lock addresses, so matching them is trivial when debugging a pileup. */
  {
    static const char hx[] = "0123456789abcdef";
    static const char pre[] = "[UART] lock @ 0x";
    uint64_t v = (uint64_t)&uart_lock;
    for (const char *p = pre; *p; p++) outb(COM1_PORT, (uint8_t)*p);
    for (int i = 15; i >= 0; i--) outb(COM1_PORT, (uint8_t)hx[(v >> (i * 4)) & 0xF]);
    outb(COM1_PORT, '\r');
    outb(COM1_PORT, '\n');
  }
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

/**
 * Lock-free UART write body.  This is the ONLY function that touches the
 * COM1 registers.  It deliberately takes no lock: deadlock diagnostics
 * (spinlock screams, stall watchdog) run while the console lock itself may
 * be wedged and MUST still reach the wire.
 *
 * uart_putc / uart_puts serialize with uart_lock around this body, which
 * gives the whole console ONE lock.  (Previously uart_puts took uart_lock
 * while print_int/uart_print_hex took a separate print_lock, so two cores
 * could interleave bytes inside one logical write — the byte-soup
 * "[CONSOLE] " / "I0nsi0de" interleaves seen under SMP.)
 */
static void uart_putc_body(char c) {
  uart_last_activity_ms = timer_get_ms();
  if (c == '\n') {
    while (!is_transmit_empty()) {
      // Spin
    }
    outb(COM1_PORT, '\r');
  }
  while (!is_transmit_empty()) {
    // Spin
  }
  outb(COM1_PORT, c);
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
/* GB-204: one claim word.  The first CPU to exchange 0->1 owns the fatal
   record; every other CPU sees 1, counts the drop and halts without writing. */
static volatile int panic_claim_word = 0;

void panic_puts2(const char *s); /* COM2 sink, defined below */
void panic_putc2(char c);
void print_int_raw2(int val);

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
  static volatile int reporting = 0;
  /* Count first: a suppressed record is still a measured record (GB-204). */
  __atomic_fetch_add(&panic_sink_dropped, 1, __ATOMIC_RELAXED);
  /* COM2 carries every violation as one clean counter line, cap or no cap:
     nothing else writes COM2, so this is the uncapped view of the count. */
  panic_puts2("[PANIC-SINK] suppressed=");
  print_int_raw2(__atomic_load_n(&panic_sink_dropped, __ATOMIC_RELAXED));
  panic_putc2('\n');
  if (panic_sink_violations >= 3 || reporting) return;
  reporting = 1;
  panic_sink_violations++;
  /* COM1, not COM2: the T3 harness captures only stdio (`-serial stdio`), so a
     report on COM2 is a report nobody reads -- measured, 0 violations visible
     with the COM2 reporter while 70 records shredded.  Writing byte-wise here
     can itself shred one line, which is accepted: it is capped at 3 per boot
     and the alternative is an instrument that cannot be observed. */
  const char *tag = "[PANIC-SINK-VIOLATION] ";
  for (const char *p = tag; *p; p++) uart_putc_body(*p);
  for (const char *p = what; *p; p++) uart_putc_body(*p);
  const char *mid = " from a returning path";
  for (const char *p = mid; *p; p++) uart_putc_body(*p);
  if (s) {
    for (const char *p = ": "; *p; p++) uart_putc_body(*p);
    for (int i = 0; s[i] && i < 48; i++) uart_putc_body(s[i]);
  }
  uart_putc_body('\n');
  reporting = 0;
}

/** Raw single character — lock-free, for NON-RETURNING paths only (GB-097). */
void panic_putc(char c) {
  if (!panic_depth) panic_sink_violation("panic_putc", 0);
  uart_putc_body(c);
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
  while (*s != '\0') {
    uart_putc_body(*s);
    s++;
  }
}

/**
 * Outputs a single character to the COM1 serial port, serialized against
 * all other console output.
 */
void uart_putc(char c) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  uart_putc_body(c);
  spinlock_release_irqrestore(&uart_lock, flags);
}

/**
 * Outputs a null-terminated string to the serial port.
 * Uses a spinlock to ensure atomic serial printing from multiple cores.
 */
void uart_puts(const char *s) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  while (*s != '\0') {
    uart_putc_body(*s);
    s++;
  }
  spinlock_release_irqrestore(&uart_lock, flags);
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
static volatile int gb207_ipi_seen = 0;
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
   what makes this vehicle non-perturbing to the wave that follows. */
void gb207_ipi_handler(void) {
  uint32_t me = get_cpuid();
  gb207_ipi_seen = 1;
#ifdef GB207_PREFIX_NO_CLAIM
  /* Pre-fix tree: no claim exists, so this CPU writes its own fatal record —
     exactly the two-record failure GB-204 fixes. */
  gb207_record("cpu1", me);
#else
  if (!panic_claim())
    return;
  gb207_record("cpu1", me);
  panic_leave();
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
  for (int attempt = 0; attempt < 2 && !gb207_ipi_seen; attempt++) {
    x64_send_test_ipi(1, 0x83);
    for (volatile uint64_t spin = 0; spin < 20000000ULL && !gb207_ipi_seen; spin++)
      __asm__ volatile("pause");
  }
  gb207_record("cpu0", get_cpuid());
  for (volatile uint64_t spin = 0; spin < 20000000ULL && !gb207_ipi_seen; spin++)
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
    kl_s(&kl, gb207_ipi_seen ? " ipi=seen\n" : " ipi=MISSED\n");
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
