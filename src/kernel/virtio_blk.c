#include "virtio_blk.h"
#include "lock.h"
#include "process.h"
#include "arch/cpu.h"
#include "blk_cache.h"


#ifdef __x86_64__

#include "virtio_blk.h"
#include "lock.h"
#include "process.h"
#include "arch/cpu.h"

extern void uart_puts(const char* s);
extern uint64_t timer_get_ms(void);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);
extern void *memcpy(void *dest, const void *src, size_t n);

static spinlock_t blk_lock;
static spinlock_t blk_request_lock;
int virtio_blk_irq = -1;

struct nvme_cmd {
  uint8_t opcode;
  uint8_t flags;
  uint16_t cid;
  uint32_t nsid;
  uint64_t rsvd2;
  uint64_t mptr;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10;
  uint32_t cdw11;
  uint32_t cdw12;
  uint32_t cdw13;
  uint32_t cdw14;
  uint32_t cdw15;
} __attribute__((packed));

struct nvme_cqe {
  uint32_t result;
  uint32_t rsvd;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t cid;
  uint16_t status;
} __attribute__((packed));

static volatile uint32_t* nvme_regs;
static volatile uint32_t* admin_sq_db;
static volatile uint32_t* admin_cq_db;
static volatile uint32_t* io_sq_db;
static volatile uint32_t* io_cq_db;

static volatile struct nvme_cmd admin_sq[2] __attribute__((aligned(4096)));
static volatile struct nvme_cqe admin_cq[2] __attribute__((aligned(4096)));
/* L55: the x86_64 "virtio_blk" path is really the **NVMe** driver (PCI class
 * 01/08, admin+IO queue creation, `submit_io_cmd`).  The completion wedge it
 * used to hit was not a lost interrupt: with a 2-entry ring, a cid-keyed poll,
 * and the whole request (submit + wait) inside an IRQ-off spinlock, a single
 * slow completion aged the holder's heartbeat past LOSTWAKE_DEAD_OWNER_MS, the
 * spinlock's stall breaker freed `blk_request_lock`, a second CPU entered the
 * driver, and the two consumers desynchronized the ring: one consumed the
 * other's CQE (slot -> 0xFFFF, head advanced), so the victim waited for a cid
 * that could never appear again.  Evidence in docs/rimworld-launch-log.md §42.
 *
 * The mechanism now, in order of importance:
 *   1. a single-in-flight TOKEN (`blk_in_use`) outside any lock, so no lock is
 *      held across the device wait and no stall breaker can admit a second
 *      driver entrant (`blk_inside` asserts that), and the poll refreshes this
 *      CPU's heartbeat because it provably is alive;
 *   2. a PHASE-TAGGED completion ring (`io_cq_phase`), which is the only
 *      reliable "this CQE is new" signal in NVMe; a stale/abandoned CQE is
 *      drained instead of mistaken for the current request;
 *   3. a bounded wait that RESETS the controller, recreates the queues and
 *      retries the request once — a lost completion becomes a completed
 *      transfer and the ring is left consistent either way;
 *   4. a 16-entry queue instead of 2, so a completion can never find the CQ
 *      full behind one in-flight command.
 */
#define NVME_IOQ_DEPTH 16

static volatile struct nvme_cmd io_sq[NVME_IOQ_DEPTH] __attribute__((aligned(4096)));
static volatile struct nvme_cqe io_cq[NVME_IOQ_DEPTH] __attribute__((aligned(4096)));
static uint8_t bounce_buf[4096] __attribute__((aligned(4096)));

static uint16_t admin_sq_tail = 0;
static uint16_t admin_cq_head = 0;
static uint16_t io_sq_tail = 0;
static uint16_t io_cq_head = 0;
/* L55: expected CQE phase tag.  The device writes phase 1 into a fresh CQ and
   flips it on every wraparound; host-side `cid = 0xFFFF` markers cannot
   distinguish "not written yet" from "written with cid 0xFFFF" and, worse,
   cannot tell an abandoned completion from the current one. */
static uint16_t io_cq_phase = 1;
/* Completion-queue slots use 0xFFFF as their free marker; a live command id
   must never collide with it, or the poll in submit_io_cmd() exits early on
   a stale slot and desynchronizes the completion queues.  next_io_cid()
   maintains that invariant. */
static uint16_t io_cid = 100;

/* L55 diagnostics: the shape of the wedge, countable in one run. */
static volatile int blk_in_use;           /* single-in-flight token */
static volatile uint32_t blk_inside;      /* concurrent driver entrants (must stay <= 1) */
static volatile uint32_t blk_requests;    /* completed device commands */
static volatile uint32_t blk_stalls;      /* bounded waits that expired */
static volatile uint32_t blk_slow;        /* commands that took > 250 ms */
static volatile uint32_t blk_stale;       /* abandoned CQEs drained */
static volatile uint32_t blk_resets;      /* controller resets performed */

/* L67: the stalled-request tuple.  The L55 row names the ring indices but
 * not WHAT was in flight, so "the device is not completing" and "we lost the
 * completion" could not be told apart.  These fields make one row answer it:
 * the command (opcode/LBA/NLB/cid/slot), the ring (tail/head/phase), the
 * device's own view (CAP/VS are read-only and survive a reset; the last
 * completion's CQE.sq_head is where the device says our SQ head is), and the
 * MMIO mapping that carries the doorbell (pd480 + the physical BAR). */
static volatile struct {
  uint16_t cid;
  uint16_t slot;
  uint8_t  opcode;
  uint32_t nsid;
  uint64_t lba;
  uint32_t nlb;
  uint64_t prp;
} blk_cur;
static volatile uint16_t blk_last_cid;
static volatile uint16_t blk_last_sqhead;
static volatile uint64_t blk_last_done_ms;
static volatile uint32_t blk_dev_off;     /* waits that saw CC.EN/CSTS.RDY clear */
static volatile uint32_t blk_cc_zero;     /* CC=0 writes this kernel issued */
static volatile uint32_t blk_cc_en;       /* CC=enable writes this kernel issued */
static volatile uint64_t blk_cc_zero_ms;  /* when the last CC=0 write happened */
static volatile uint32_t blk_recover_us;  /* last recovery duration */
static volatile uint32_t blk_prerepair;   /* pre-submit repairs (nothing in flight) */

/* M6/L69: the device generation (see virtio_blk.h).  Read by the block cache
 * to refuse publishing a frame filled across a reset, and by the raw read path
 * to re-issue a transfer whose completion overlapped one. */
volatile uint64_t blk_dev_epoch = 1;
volatile uint32_t blk_untrusted;         /* transfers discarded: epoch moved */
static volatile uint32_t blk_untrusted_rows;

#ifdef KERNEL_MODE_UNIT_TEST
/* Aborted-transfer injection (see virtio_blk_test_abort in the header).
 * `armed` snapshots the bounce buffer; `apply` restores it (no DMA happened)
 * and moves the device generation — the exact aborted-AIO signature. */
static int blk_test_abort_mode;
static uint64_t blk_test_abort_sector;
static int blk_test_abort_left;

void virtio_blk_test_abort(int mode, uint64_t sector, int times) {
  blk_test_abort_mode = mode;
  blk_test_abort_sector = sector;
  blk_test_abort_left = times;
}

static int blk_test_abort_armed(uint64_t sector, uint8_t *pre) {
  if (blk_test_abort_mode && sector == blk_test_abort_sector &&
      (blk_test_abort_mode == 2 || blk_test_abort_left > 0)) {
    for (int k = 0; k < 512; k++)
      pre[k] = bounce_buf[k];
    return 1;
  }
  return 0;
}

static void blk_test_abort_apply(int armed, const uint8_t *pre) {
  if (!armed)
    return;
  for (int k = 0; k < 512; k++)
    bounce_buf[k] = pre[k];
  __atomic_add_fetch(&blk_dev_epoch, 1, __ATOMIC_SEQ_CST);
  if (blk_test_abort_mode == 1)
    blk_test_abort_left--;
}
#endif

/* L67: defined below; the health hook above the wait path uses them. */
static void blk_token_acquire(void);
static void blk_token_release(void);

static inline void outl(uint16_t port, uint32_t val) {
  __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
  uint32_t ret;
  __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
  uint32_t address = ((uint32_t)1 << 31) |
                     ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) |
                     ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(0x0CF8, address);
  return inl(0x0CFC);
}

static void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
  uint32_t address = ((uint32_t)1 << 31) |
                     ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) |
                     ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(0x0CF8, address);
  outl(0x0CFC, val);
}

static uint16_t submit_admin_cmd(struct nvme_cmd* cmd) {
  uint16_t cid = cmd->cid;
  uint64_t t0 = timer_get_ms();

  admin_sq[admin_sq_tail].opcode = cmd->opcode;
  admin_sq[admin_sq_tail].flags = cmd->flags;
  admin_sq[admin_sq_tail].cid = cmd->cid;
  admin_sq[admin_sq_tail].nsid = cmd->nsid;
  admin_sq[admin_sq_tail].rsvd2 = cmd->rsvd2;
  admin_sq[admin_sq_tail].mptr = cmd->mptr;
  admin_sq[admin_sq_tail].prp1 = cmd->prp1;
  admin_sq[admin_sq_tail].prp2 = cmd->prp2;
  admin_sq[admin_sq_tail].cdw10 = cmd->cdw10;
  admin_sq[admin_sq_tail].cdw11 = cmd->cdw11;
  admin_sq[admin_sq_tail].cdw12 = cmd->cdw12;
  admin_sq[admin_sq_tail].cdw13 = cmd->cdw13;
  admin_sq[admin_sq_tail].cdw14 = cmd->cdw14;
  admin_sq[admin_sq_tail].cdw15 = cmd->cdw15;

  admin_sq_tail = (admin_sq_tail + 1) % 2;

  __asm__ volatile("" : : : "memory");

  *admin_sq_db = admin_sq_tail;

  while (admin_cq[admin_cq_head].cid != cid) {
    __asm__ volatile("pause");
    /* L55: an admin wait must be bounded too — during a controller reset a
       command the device never consumed would otherwise wedge the console
       with no way back. */
    if ((timer_get_ms() - t0) > 2000) {
      uart_puts("[BLKQ] admin timeout cid=");
      print_int((int)cid);
      uart_puts(" CSTS=");
      uart_print_hex(nvme_regs[7]);
      uart_puts("\n");
      admin_cq[admin_cq_head].cid = 0xFFFF;
      return 0x7FFF;
    }
  }

  __asm__ volatile("" : : : "memory");
  uint16_t status = admin_cq[admin_cq_head].status;
  admin_cq[admin_cq_head].cid = 0xFFFF;
  admin_cq_head = (admin_cq_head + 1) % 2;
  *admin_cq_db = admin_cq_head;

  return status >> 1;
}

extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

static uint16_t next_io_cid(void) {
  if (io_cid == 0xFFFF) {
    /* Skip the free marker so ids stay distinct from it. */
    io_cid = 0;
  }
  return io_cid++;
}

/* ---------------------------------------------------------------------------
 * L55: IO queue lifecycle.
 *
 * `nvme_io_queues_create()` is the single place that creates CQ/SQ 1, so the
 * boot path and the reset path can never disagree about depth or doorbell
 * layout.  `nvme_reinit()` is the recovery half of the completion timeout: it
 * puts the controller back into a state where the *next* request is valid,
 * which the old BLKSTALL path did not (it returned an error with the ring
 * still desynchronized, so every later request inherited the wedge).
 * ------------------------------------------------------------------------- */
static int nvme_io_queues_create(void) {
  struct nvme_cmd create_cq = {0};
  create_cq.opcode = 0x05; /* Create I/O Completion Queue */
  create_cq.cid = 1;
  create_cq.prp1 = (uint64_t)&io_cq;
  create_cq.cdw10 = ((NVME_IOQ_DEPTH - 1) << 16) | 0x01;
  create_cq.cdw11 = 0x01; /* PC = 1, IEN = 0: polled completions */
  uint16_t status_cq = submit_admin_cmd(&create_cq);
  if (status_cq != 0) {
    uart_puts("[BLKQ] create CQ failed status=");
    uart_print_hex(status_cq);
    uart_puts("\n");
    return -1;
  }

  struct nvme_cmd create_sq = {0};
  create_sq.opcode = 0x01; /* Create I/O Submission Queue */
  create_sq.cid = 2;
  create_sq.prp1 = (uint64_t)&io_sq;
  create_sq.cdw10 = ((NVME_IOQ_DEPTH - 1) << 16) | 0x01;
  create_sq.cdw11 = (1 << 16) | 0x01; /* CQID = 1, PC = 1 */
  uint16_t status_sq = submit_admin_cmd(&create_sq);
  if (status_sq != 0) {
    uart_puts("[BLKQ] create SQ failed status=");
    uart_print_hex(status_sq);
    uart_puts("\n");
    return -1;
  }
  return 0;
}

/* Reset the controller and rebuild both queues with an EMPTY, zeroed IO ring.
   Called only with the request token held (single entrant) and after the
   in-flight command has been abandoned.

   L67: a host-side reset can still be IN PROGRESS when we get here.  QEMU's
   nvme_ctrl_reset() writes CSTS=0 first and only tears the queues down and
   clears CC after it has drained the namespaces, so a guest that "repairs"
   in that window enables the controller and creates queues *inside* the
   reset, and the reset then frees them: the admin Create SQ times out and a
   good read is turned into -EIO.  Two defences:
     1. if the device is in that half-reset state (CC.EN=1, CSTS.RDY=0) give
        it a bounded moment to finish before touching it;
     2. after creating the queues, re-read CC/CSTS a few ms later and redo
        the whole sequence if the device fell out of ready again. */
static int nvme_reinit(void) {
  blk_resets++;
  /* M6/L69: from this instant on, no completion that was outstanding before
     the reset can be trusted — QEMU destroys the requests it had in flight,
     and the aborted AIO may still post a SUCCESS CQE with no DMA.  Bump the
     device generation BEFORE touching the controller so a concurrent fill
     (there is none: the token is held, but the cache reads the epoch without
     it) can never publish a frame that straddled this reset. */
  __atomic_add_fetch(&blk_dev_epoch, 1, __ATOMIC_SEQ_CST);
  uart_puts("[BLKQ] controller reset #");
  print_int((int)blk_resets);
  uart_puts("\n");

  /* (1) let a host reset that is already running finish (bounded). */
  {
    uint64_t t_settle = timer_get_ms();
    while ((nvme_regs[5] & 1) && !(nvme_regs[7] & 1)) {
      __asm__ volatile("pause");
      if ((timer_get_ms() - t_settle) > 500) {
        uart_puts("[BLKQ] reset: waiting out a host reset, CC=");
        uart_print_hex(nvme_regs[5]);
        uart_puts(" CSTS=");
        uart_print_hex(nvme_regs[7]);
        uart_puts("\n");
        break;
      }
    }
  }

  for (int attempt = 0; attempt < 4; attempt++) {
  /* Disable, then wait (bounded) for the controller to acknowledge. */
  blk_cc_zero++;
  blk_cc_zero_ms = timer_get_ms();
  nvme_regs[5] = 0;
  uint64_t t0 = timer_get_ms();
  while (nvme_regs[7] & 1) {
    __asm__ volatile("pause");
    if ((timer_get_ms() - t0) > 2000) {
      uart_puts("[BLKQ] reset: RDY never cleared, CSTS=");
      uart_print_hex(nvme_regs[7]);
      uart_puts("\n");
      break;
    }
  }

  /* AQA, admin queue addresses, CC — the same sequence as boot. */
  nvme_regs[9] = 0x00010001;
  for (int i = 0; i < 2; i++) {
    admin_cq[i].cid = 0xFFFF;
    admin_cq[i].status = 0;
  }
  admin_sq_tail = 0;
  admin_cq_head = 0;
  uint64_t admin_sq_phys = (uint64_t)&admin_sq;
  nvme_regs[10] = (uint32_t)admin_sq_phys;
  nvme_regs[11] = (uint32_t)(admin_sq_phys >> 32);
  (void)nvme_regs[11];
  uint64_t admin_cq_phys = (uint64_t)&admin_cq;
  nvme_regs[12] = (uint32_t)admin_cq_phys;
  nvme_regs[13] = (uint32_t)(admin_cq_phys >> 32);
  (void)nvme_regs[13];
  nvme_regs[5] = 0x00460001;
  blk_cc_en++;
  (void)nvme_regs[5];

  t0 = timer_get_ms();
  while (!(nvme_regs[7] & 1)) {
    __asm__ volatile("pause");
    if ((timer_get_ms() - t0) > 2000) {
      uart_puts("[BLKQ] reset: RDY never set, CSTS=");
      uart_print_hex(nvme_regs[7]);
      uart_puts("\n");
      continue; /* retry the whole sequence */
    }
  }

  /* The device re-initialises the CQ from scratch: zero EVERY entry so no
     pre-reset phase tag can ever validate a stale CQE, then rebuild. */
  for (int i = 0; i < NVME_IOQ_DEPTH; i++) {
    io_cq[i].result = 0;
    io_cq[i].rsvd = 0;
    io_cq[i].sq_head = 0;
    io_cq[i].sq_id = 0;
    io_cq[i].cid = 0xFFFF;
    io_cq[i].status = 0;
    io_sq[i].opcode = 0;
    io_sq[i].cid = 0xFFFF;
  }
  io_sq_tail = 0;
  io_cq_head = 0;
  io_cq_phase = 1;
  *io_cq_db = 0;

  if (nvme_io_queues_create() != 0)
    continue; /* a host reset may have eaten the admin command; redo */

  /* (2) settle check: the host reset can complete just after our enable and
     free the queues we have only now created.  Give it a few ms and confirm
     the controller is still enabled and ready before trusting the result. */
  {
    /* Require the device to STAY enabled for 50 ms: a host reset that is
       still draining will clear CC a little later, and returning now would
       hand the caller a disabled controller. */
    int stable = 0;
    uint64_t t_v = timer_get_ms();
    while ((timer_get_ms() - t_v) < 50) {
      if (!((nvme_regs[5] & 1) && (nvme_regs[7] & 1))) { stable = 0; break; }
      stable = 1;
      __asm__ volatile("pause");
    }
    if (stable) {
      /* M6/L69: NOTHING FILLED AROUND A RESET MAY STAY CACHED.  The aborted
         transfer's frame is only the one we can see; a completion that QEMU
         posted for a recycled request can also carry the OLD request's (empty)
         data with status 0, and it is indistinguishable from a real one.  Drop
         every cached unit after a reset (entries owned by a live fill are
         skipped by the cache), so the next read of any page comes from the
         repaired device.  One flush per reset — resets are 1-2 per run. */
      uint64_t dropped = blk_cache_flush_all();
      {
        static int fl_rows;
        if (fl_rows < 8) {
          fl_rows++;
          uart_puts("[BLKC] post-reset flush dropped=");
          print_int((int)dropped);
          uart_puts(" cached units\n");
        }
      }
      return 0;
    }
    uart_puts("[BLKQ] reset: settled out of ready (attempt ");
    print_int(attempt);
    uart_puts("), retrying\n");
  }
  } /* for (attempt) */
  uart_puts("[BLKQ] reset: gave up after 4 attempts\n");
  return -1;
}

/* Consume the CQE at the head (valid entry) and publish the new head. */
static void nvme_cq_advance(void) {
  io_cq[io_cq_head].cid = 0xFFFF;
  io_cq_head = (uint16_t)((io_cq_head + 1) % NVME_IOQ_DEPTH);
  if (io_cq_head == 0) io_cq_phase ^= 1;
  __asm__ volatile("" : : : "memory");
  *io_cq_db = io_cq_head;
}

/* Fill the SQ slot and ring the doorbell.  Returns the slot used. */
#ifndef RW_BLK_DROP_AT
#define RW_BLK_DROP_AT 0
#endif
static uint32_t blk_submits;

static uint16_t nvme_sq_submit(struct nvme_cmd* cmd) {
  uint16_t slot = io_sq_tail;

  blk_cur.cid = cmd->cid;
  blk_cur.slot = slot;
  blk_cur.opcode = cmd->opcode;
  blk_cur.nsid = cmd->nsid;
  blk_cur.lba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
  blk_cur.nlb = cmd->cdw12 & 0xFFFF;
  blk_cur.prp = cmd->prp1;

  io_sq[slot].opcode = cmd->opcode;
  io_sq[slot].flags = cmd->flags;
  io_sq[slot].cid = cmd->cid;
  io_sq[slot].nsid = cmd->nsid;
  io_sq[slot].rsvd2 = cmd->rsvd2;
  io_sq[slot].mptr = cmd->mptr;
  io_sq[slot].prp1 = cmd->prp1;
  io_sq[slot].prp2 = cmd->prp2;
  io_sq[slot].cdw10 = cmd->cdw10;
  io_sq[slot].cdw11 = cmd->cdw11;
  io_sq[slot].cdw12 = cmd->cdw12;
  io_sq[slot].cdw13 = cmd->cdw13;
  io_sq[slot].cdw14 = cmd->cdw14;
  io_sq[slot].cdw15 = cmd->cdw15;

  __asm__ volatile("" : : : "memory");
  io_sq_tail = (uint16_t)((slot + 1) % NVME_IOQ_DEPTH);
  __asm__ volatile("" : : : "memory");
  blk_submits++;
#if RW_BLK_DROP_AT
  /* L55 recovery self-test: silently lose ONE command's doorbell.  The device
     never sees it, the bounded wait expires, the controller is reset and the
     command is resubmitted — i.e. the old permanent `STILL SPINNING` becomes a
     completed transfer.  Compiled out (RW_BLK_DROP_AT == 0) in the shipped
     build; the value names which submission to drop. */
  if (blk_submits == RW_BLK_DROP_AT) {
    uart_puts("[BLKTEST] dropping doorbell for submit #");
    print_int((int)blk_submits);
    uart_puts(" (timeout-recovery self-test)\n");
    return slot;
  }
#endif
  *io_sq_db = io_sq_tail;
  return slot;
}

/* Wait for `cid`'s completion.  Returns the NVMe status (0 = success) or
   0x7FFF when the bounded wait expired twice (with a controller reset in
   between).  The wait is time-bounded with the wall/monotonic millisecond
   clock, refreshes this CPU's liveness heartbeat (it is provably alive: it is
   polling the device), and never runs with a global lock held. */
static uint16_t nvme_wait_cid(struct nvme_cmd *cmd, uint16_t slot) {
  uint16_t cid = cmd->cid;
  uint64_t t0 = timer_get_ms();
  int repaired = 0;   /* recoveries performed for THIS request */
  int reissue = 0;    /* the next completion comes from a repaired device */
  uint64_t spins = 0;
  int printed_slow = 0;

  for (;;) {
    /* Publish liveness: an IRQ-off poll would otherwise age this CPU's
       heartbeat past LOSTWAKE_DEAD_OWNER_MS and let a peer's lock breaker
       free whatever lock the caller holds (fat_lock, blk_request_lock). */
    if ((spins++ & 0x3FF) == 0) {
      extern volatile uint64_t cpu_heartbeat_ms[];
      cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();
    }

    /* Drain every VALID completion at the head.  Validity is the phase tag,
       not a cid marker: a CQE the device wrote for an abandoned request is
       consumed (and logged) instead of blocking this one for ever. */
    for (;;) {
      uint16_t cqstat = io_cq[io_cq_head].status;
      if ((cqstat & 1) != io_cq_phase) break; /* no new entry at the head */
      uint16_t cqcid = io_cq[io_cq_head].cid;
      uint16_t cqsqh = io_cq[io_cq_head].sq_head;
      uint16_t status = (uint16_t)(cqstat >> 1);
      nvme_cq_advance();
      if (cqcid == cid) {
        blk_requests++;
        blk_last_cid = cid;
        blk_last_sqhead = cqsqh;
        blk_last_done_ms = timer_get_ms();
        /* L67: a completion that arrives after a controller reset is NOT
           trustworthy.  QEMU aborts the requests that were in flight when it
           reset the controller, and an aborted AIO can complete with success
           and no DMA at all: the guest then reads whatever bounce_buf held
           (a page of zeros), which is exactly the [TXT] ZERO that killed the
           process after the L65/L67 wedge.  Discard that completion and
           issue the same command once more into the freshly initialised
           device; only the second transfer is used. */
        if (reissue) {
          reissue = 0;
          /* QEMU completes the ABORTED request during the reset's namespace
             drain, and it writes that CQE into the same queue memory we are
             about to reuse: cid == the command's cid, status 0, no DMA.  That
             is the page of zeros that killed the loader in L65, and the
             reason a "re-read" alone did not fix it (L67 run 1: the re-read
             matched the straggler and returned instantly).  Two defences:
               1. give any straggler a moment to land and DRAIN it;
               2. re-issue under a NEW cid, so a late entry can only ever be
                  drained as stale, never mistaken for this transfer. */
          for (int k = 0; k < 500; k++) {
            if ((io_cq[io_cq_head].status & 1) == io_cq_phase) {
              uart_puts("[BLKQ] drained post-reset straggler cid=");
              print_int((int)io_cq[io_cq_head].cid);
              uart_puts(" want=");
              print_int((int)cid);
              uart_puts("\n");
              nvme_cq_advance();
              blk_stale++;
              k = 0;
            }
            __asm__ volatile("pause");
          }
          cmd->cid = next_io_cid();
          cid = cmd->cid;
          slot = nvme_sq_submit(cmd);
          uart_puts("[BLKQ] re-read after recovery cid=");
          print_int((int)cid);
          uart_puts(" lba=");
          uart_print_hex(blk_cur.lba);
          uart_puts("\n");
          t0 = timer_get_ms();
          printed_slow = 0;
          continue;
        }
        uint64_t dt = timer_get_ms() - t0;
        if (dt > 250 && printed_slow < 4) {
          printed_slow++;
          blk_slow++;
          uart_puts("[BLKSLOW] cid=");
          print_int((int)cid);
          uart_puts(" slot=");
          print_int((int)slot);
          uart_puts(" op=");
          uart_print_hex(blk_cur.opcode);
          uart_puts(" lba=");
          uart_print_hex(blk_cur.lba);
          uart_puts(" ms=");
          print_int((int)dt);
          uart_puts(" status=");
          uart_print_hex(status);
          uart_puts(" n=");
          print_int((int)blk_slow);
          uart_puts("\n");
        }
        return status;
      }
      blk_stale++;
      if (blk_stale <= 8 || (blk_stale & 0x3FF) == 0) {
        uart_puts("[BLKQ] drain stale cqe cid=");
        print_int((int)cqcid);
        uart_puts(" want=");
        print_int((int)cid);
        uart_puts(" head=");
        print_int((int)io_cq_head);
        uart_puts(" n=");
        print_int((int)blk_stale);
        uart_puts("\n");
      }
    }

    uint64_t dt = timer_get_ms() - t0;
    if (dt > 250 && !printed_slow) {
      printed_slow = 1;
      uart_puts("[BLKSLOW] pending cid=");
      print_int((int)cid);
      uart_puts(" slot=");
      print_int((int)slot);
      uart_puts(" op=");
      uart_print_hex(blk_cur.opcode);
      uart_puts(" lba=");
      uart_print_hex(blk_cur.lba);
      uart_puts(" ms=");
      print_int((int)dt);
      uart_puts(" sq_tail=");
      print_int((int)io_sq_tail);
      uart_puts(" cq_head=");
      print_int((int)io_cq_head);
      uart_puts(" phase=");
      print_int((int)io_cq_phase);
      uart_puts("\n");
    }

    /* L67: the controller can be taken out from under an outstanding
       command.  CC.EN and CSTS.RDY are the device's own mirror of its
       state; if either clears while THIS kernel has not disabled the
       controller, the controller was reset by someone else and every
       completion in flight was destroyed with its queues.  There is nothing
       to wait for: the cid can never be posted again.  Detecting it here
       turns a 2 s stall plus a blind reset into a sub-millisecond repair,
       and (with the re-read above) a transfer that the reset aborted into a
       correct one. */
    int device_off = 0;
    if (dt > 20 && (spins & 0x3F) == 0) {
      uint32_t cc_now = nvme_regs[5];
      uint32_t csts_now = nvme_regs[7];
      if (!(cc_now & 1) || !(csts_now & 1)) {
        device_off = 1;
        blk_dev_off++;
        /* M6/L69: we have just learned that someone reset the controller
           under this command.  Bump the generation here as well as in
           nvme_reinit(): the repair can be skipped (repaired >= 3) and the
           cache must still know that this request's bytes are untrustworthy. */
        __atomic_add_fetch(&blk_dev_epoch, 1, __ATOMIC_SEQ_CST);
        if (blk_dev_off <= 6 || (blk_dev_off & 0x3F) == 0) {
          uart_puts("[BLKQ] device off cid=");
          print_int((int)cid);
          uart_puts(" op=");
          uart_print_hex(blk_cur.opcode);
          uart_puts(" lba=");
          uart_print_hex(blk_cur.lba);
          uart_puts(" since_ms=");
          print_int((int)(timer_get_ms() - blk_last_done_ms));
          uart_puts(" cc=");
          uart_print_hex(cc_now);
          uart_puts(" csts=");
          uart_print_hex(csts_now);
          uart_puts(" cap=");
          uart_print_hex(nvme_regs[0]);
          uart_puts("/");
          uart_print_hex(nvme_regs[1]);
          uart_puts(" vs=");
          uart_print_hex(nvme_regs[2]);
          uart_puts(" cc0_writes=");
          print_int((int)blk_cc_zero);
          uart_puts(" cpu=");
          print_int((int)get_cpuid());
          uart_puts(" n=");
          print_int((int)blk_dev_off);
          uart_puts("\n");
        }
      }
    }

    if (dt > 2000 || device_off) {
      uart_puts(device_off ? "[BLKQ] wedge-device-off cid=" : "[BLKQ] stall cid=");
      print_int((int)cid);
      uart_puts(" op=");
      uart_print_hex(blk_cur.opcode);
      uart_puts(" lba=");
      uart_print_hex(blk_cur.lba);
      uart_puts(" nlb=");
      print_int((int)blk_cur.nlb);
      uart_puts(" prp=");
      uart_print_hex(blk_cur.prp);
      uart_puts(" slot=");
      print_int((int)slot);
      uart_puts(" sq_tail=");
      print_int((int)io_sq_tail);
      uart_puts(" cq_head=");
      print_int((int)io_cq_head);
      uart_puts(" phase=");
      print_int((int)io_cq_phase);
      uart_puts(" cqe[cq_head].cid=");
      print_int((int)io_cq[io_cq_head].cid);
      uart_puts(" .status=");
      uart_print_hex(io_cq[io_cq_head].status);
      uart_puts(" at_slot.cid=");
      print_int((int)io_cq[slot].cid);
      uart_puts(" .status=");
      uart_print_hex(io_cq[slot].status);
      uart_puts(" last_cid=");
      print_int((int)blk_last_cid);
      uart_puts(" last_sqhead=");
      print_int((int)blk_last_sqhead);
      uart_puts(" ms_since_done=");
      print_int((int)(timer_get_ms() - blk_last_done_ms));
      uart_puts(" inside=");
      print_int((int)blk_inside);
      uart_puts(" stalls=");
      print_int((int)blk_stalls);
      uart_puts(" devoff=");
      print_int((int)blk_dev_off);
      uart_puts(" reqs=");
      print_int((int)blk_requests);
      uart_puts(" cc0=");
      print_int((int)blk_cc_zero);
      uart_puts(" cpu=");
      print_int((int)get_cpuid());
      uart_puts(" CSTS=");
      uart_print_hex(nvme_regs[7]);
      uart_puts(" CC=");
      uart_print_hex(nvme_regs[5]);
      uart_puts(" CAP=");
      uart_print_hex(nvme_regs[0]);
      uart_puts(" VS=");
      uart_print_hex(nvme_regs[2]);
      uart_puts(" INTMS=");
      uart_print_hex(nvme_regs[3]);
      uart_puts(" INTMC=");
      uart_print_hex(nvme_regs[4]);
      uart_puts("\n");

      if (!device_off)
        blk_stalls++;
      if (repaired >= 3) {
        uart_puts("[BLKQ] giving up cid=");
        print_int((int)cid);
        uart_puts(" -> I/O error (controller left freshly initialised)\n");
        return 0x7FFF;
      }
      /* Recovery, not a symptom patch: put the controller and BOTH rings back
         into a known-good state, then resubmit the same command.  A
         completion the device really did lose becomes a completed transfer,
         and a transfer the reset aborted is re-read (see `reissue`). */
      repaired++;
      reissue = 1;
      uint64_t t_rec = timer_get_ms();
      if (nvme_reinit() != 0) {
        uart_puts("[BLKQ] reinit failed cid=");
        print_int((int)cid);
        uart_puts(" -> I/O error\n");
        return 0x7FFF;
      }
      /* A fresh cid as well: the reset destroyed the old command, and any
         completion QEMU still posts for it must never match this one. */
      cmd->cid = next_io_cid();
      cid = cmd->cid;
      slot = nvme_sq_submit(cmd);
      blk_recover_us = (uint32_t)((timer_get_ms() - t_rec) * 1000);
      uart_puts("[BLKQ] recovered cid=");
      print_int((int)cid);
      uart_puts(" us=");
      print_int((int)blk_recover_us);
      uart_puts(" resets=");
      print_int((int)blk_resets);
      uart_puts("\n");
      t0 = timer_get_ms();
      printed_slow = 0;
      continue;
    }
  }
}

static uint16_t submit_io_cmd(struct nvme_cmd* cmd) {
  uint16_t slot = nvme_sq_submit(cmd);
  return nvme_wait_cid(cmd, slot);
}

/* M6/L69: is the controller enabled and ready RIGHT NOW?  Two MMIO reads.
 *
 * Why this is needed at both ends of every command: QEMU's controller reset
 * aborts the request it had in flight and still posts a SUCCESS completion for
 * it.  `nvme_wait_cid` only polls CC/CSTS once a command has been outstanding
 * for 20 ms, so a reset that lands during a FAST command is invisible: the
 * command's stale completion (success, no DMA, previous sector's bytes) is
 * consumed as if it were data, and the *next* command then detects the reset
 * and repairs.  The result is exactly one silently wrong sector per reset —
 * measured L69: a wrong FAT entry (`chain=-1` mid-file on a chain the host
 * verifies is complete), a torn page (`[TXT] IMG SUM … raw sum=0x7243B846`,
 * matching no page of the file), and a poisoned zero frame.
 *
 * Checking before the submit means a reset that landed between commands is
 * repaired with NOTHING in flight (so the repair cannot abort our own
 * transfer); checking after the completion means a reset that landed during
 * the command discards that completion instead of using it. */
static inline int nvme_ready_now(void) {
  return (nvme_regs[5] & 1) && (nvme_regs[7] & 1);
}

void virtio_blk_handle_irq(void) {
}

/* L67: repair hook for the page-in path.  A host-side controller reset can
 * leave the device disabled between requests (CC.EN=0, CSTS.RDY=0) with no
 * command outstanding, so nothing in the request path notices.  The exec-page
 * gate calls this before each of its retry reads: if the controller is not
 * enabled and ready, rebuild it.  Returns 1 when a repair was performed, 0
 * when the device was already healthy, -1 when a repair was attempted and
 * failed.  Takes the single-in-flight token, so it can never race a request. */
int virtio_blk_health(void) {
  if (!nvme_regs)
    return 0;
  if ((nvme_regs[5] & 1) && (nvme_regs[7] & 1))
    return 0;
  uart_puts("[BLKQ] health: controller not ready (cc=");
  uart_print_hex(nvme_regs[5]);
  uart_puts(" csts=");
  uart_print_hex(nvme_regs[7]);
  uart_puts("), repairing\n");
  blk_token_acquire();
  int rc = -1;
  if (!((nvme_regs[5] & 1) && (nvme_regs[7] & 1)))
    rc = nvme_reinit();
  blk_token_release();
  return (rc == 0) ? 1 : -1;
}

/* L67: the 5 s controller-state row (see the header).  Reads only. */
void virtio_blk_diag_row(void) {
  if (!nvme_regs)
    return;
  extern uint64_t cpu_pd7[512];
  uart_puts("[BLKD] cc=");
  uart_print_hex(nvme_regs[5]);
  uart_puts(" csts=");
  uart_print_hex(nvme_regs[7]);
  uart_puts(" cap=");
  uart_print_hex(nvme_regs[0]);
  uart_puts("/");
  uart_print_hex(nvme_regs[1]);
  uart_puts(" vs=");
  uart_print_hex(nvme_regs[2]);
  uart_puts(" pd480=");
  uart_print_hex(cpu_pd7[480]);
  uart_puts(" reqs=");
  print_int((int)blk_requests);
  uart_puts(" stalls=");
  print_int((int)blk_stalls);
  uart_puts(" devoff=");
  print_int((int)blk_dev_off);
  uart_puts(" cc0=");
  print_int((int)blk_cc_zero);
  uart_puts(" cc0_ms=");
  print_int((int)blk_cc_zero_ms);
  uart_puts(" resets=");
  print_int((int)blk_resets);
  uart_puts(" pre=");
  print_int((int)blk_prerepair);
  uart_puts(" recover_us=");
  print_int((int)blk_recover_us);
  uart_puts("\n");
}

int virtio_blk_init(void) {
  spinlock_init(&blk_lock);
  spinlock_init(&blk_request_lock);

  // Scan PCI bus for NVMe controller
  int found = 0;
  uint8_t nvme_bus = 0, nvme_slot = 0;
  for (uint32_t bus = 0; bus < 256; bus++) {
    for (uint32_t slot = 0; slot < 32; slot++) {
      uint32_t id = pci_read_config(bus, slot, 0, 0);
      if ((id & 0xFFFF) != 0xFFFF) {
        uint32_t class_rev = pci_read_config(bus, slot, 0, 0x08);
        uint8_t base_class = (class_rev >> 24) & 0xFF;
        uint8_t sub_class = (class_rev >> 16) & 0xFF;
        if (base_class == 0x01 && sub_class == 0x08) {
          nvme_bus = bus;
          nvme_slot = slot;
          found = 1;
          break;
        }
      }
    }
    if (found) break;
  }

  if (!found) {
    uart_puts("NVMe controller not found on PCI bus!\n");
    return -1;
  }

  uart_puts("Found NVMe controller on PCI bus!\n");

  uart_puts("PCI Config Space of NVMe:\n");
  for (int i = 0; i < 16; i++) {
    uart_print_hex(pci_read_config(nvme_bus, nvme_slot, 0, i * 4));
    uart_puts(" ");
    if ((i + 1) % 4 == 0) uart_puts("\n");
  }

  // Do NOT reprogram BAR0 to a safe 32-bit physical address. Keep the original high 64-bit BAR address assigned by UEFI/OVMF.
  // pci_write_config(nvme_bus, nvme_slot, 0, 0x10, 0xE0000000);
  // pci_write_config(nvme_bus, nvme_slot, 0, 0x14, 0x00000000); // Clear high 32-bits

  // Enable Memory Space and Bus Mastering
  uint32_t cmd = pci_read_config(nvme_bus, nvme_slot, 0, 0x04);
  pci_write_config(nvme_bus, nvme_slot, 0, 0x04, cmd | 0x06);

  // Read BAR0 back to verify
  uint32_t bar0_low = pci_read_config(nvme_bus, nvme_slot, 0, 0x10);
  uint32_t bar0_high = pci_read_config(nvme_bus, nvme_slot, 0, 0x14);
  uint64_t nvme_regs_phys = (bar0_low & 0xFFFFFFF0) | ((uint64_t)bar0_high << 32);

  // Setup register pointer (dynamically map BAR using index 480 of cpu_pd7)
  extern uint64_t cpu_pd7[512];
  cpu_pd7[480] = (nvme_regs_phys & 0xFFFFFFFFFFE00000ULL) | 0x9B;

  // Flush TLB
  uint64_t cr3_val;
  __asm__ volatile(
      "mov %%cr3, %0\n"
      "mov %0, %%cr3\n"
      : "=r"(cr3_val)
      :
      : "memory"
  );

  nvme_regs = (volatile uint32_t*)(0x1FC000000ULL + (nvme_regs_phys & 0x1FFFFF));

  uart_puts("NVMe BAR0 Phys: ");
  uart_print_hex(nvme_regs_phys);
  uart_puts(" (Mapped to Virtual: ");
  uart_print_hex((uint64_t)nvme_regs);
  uart_puts(")\n");

  // Reset/Disable Controller
  nvme_regs[5] = 0; // CC = 0 (disables controller)
  uart_puts("Resetting controller, waiting for CSTS.RDY = 0...\n");
  int wait_loops = 0;
  while (nvme_regs[7] & 1) { // Wait for CSTS.RDY = 0
    __asm__ volatile("pause");
    wait_loops++;
    if (wait_loops == 10000000) {
      uart_puts("CSTS=");
      uart_print_hex(nvme_regs[7]);
      uart_puts(" CC=");
      uart_print_hex(nvme_regs[5]);
      uart_puts("\n");
      wait_loops = 0;
    }
  }
  uart_puts("Controller reset complete!\n");

  // Configure Admin Queues
  // AQA: ASQS = 1 (2 entries), ACQS = 1 (2 entries)
  nvme_regs[9] = 0x00010001;

  // Initialize Admin Queue memory
  for (int i = 0; i < 2; i++) {
    admin_cq[i].cid = 0xFFFF;
    io_cq[i].cid = 0xFFFF;
  }

  // ASQ
  uint64_t admin_sq_phys = (uint64_t)&admin_sq;
  nvme_regs[10] = (uint32_t)admin_sq_phys;
  nvme_regs[11] = (uint32_t)(admin_sq_phys >> 32);
  (void)nvme_regs[11]; // Flush write

  // ACQ
  uint64_t admin_cq_phys = (uint64_t)&admin_cq;
  nvme_regs[12] = (uint32_t)admin_cq_phys;
  nvme_regs[13] = (uint32_t)(admin_cq_phys >> 32);
  (void)nvme_regs[13]; // Flush write

  // Configure CC: IOSQES=6 (64 bytes), IOCQES=4 (16 bytes), MPS=0, AMS=0, CSS=0, EN=1
  nvme_regs[5] = 0x00460001;
  (void)nvme_regs[5]; // Flush write

  // Wait for CSTS.RDY = 1
  uart_puts("Enabling controller... CC=");
  uart_print_hex(nvme_regs[5]);
  uart_puts(" CSTS=");
  uart_print_hex(nvme_regs[7]);
  uart_puts("\n");

  wait_loops = 0;
  while (!(nvme_regs[7] & 1)) {
    __asm__ volatile("pause");
    wait_loops++;
    if (wait_loops == 10000000) {
      uart_puts("CSTS=");
      uart_print_hex(nvme_regs[7]);
      uart_puts(" CC=");
      uart_print_hex(nvme_regs[5]);
      uart_puts("\n");
      wait_loops = 0;
    }
  }
  uart_puts("Controller enabled successfully!\n");

  // Determine Doorbell Stride
  uint32_t dstrd = (nvme_regs[1] >> 0) & 0xF;
  uint32_t stride_words = (4 << dstrd) / 4;

  admin_sq_db = &nvme_regs[1024 + 0 * 2 * stride_words];
  admin_cq_db = &nvme_regs[1024 + (0 * 2 + 1) * stride_words];
  io_sq_db    = &nvme_regs[1024 + 1 * 2 * stride_words];
  io_cq_db    = &nvme_regs[1024 + (1 * 2 + 1) * stride_words];

  /* L55: one place creates CQ/SQ 1 (also used by the reset path).  The ring
     is cleared first: a fresh controller's CQ phase starts at 1 and every
     entry must read "not written" until the device posts into it. */
  for (int i = 0; i < NVME_IOQ_DEPTH; i++) {
    io_cq[i].result = 0;
    io_cq[i].rsvd = 0;
    io_cq[i].sq_head = 0;
    io_cq[i].sq_id = 0;
    io_cq[i].cid = 0xFFFF;
    io_cq[i].status = 0;
    io_sq[i].cid = 0xFFFF;
  }
  io_sq_tail = 0;
  io_cq_head = 0;
  io_cq_phase = 1;
  int qcreat = nvme_io_queues_create();
  uart_puts("[BLKI] nvme ioq depth=");
  print_int(NVME_IOQ_DEPTH);
  uart_puts(" sq_tail=");
  print_int((int)io_sq_tail);
  uart_puts(" cq_head=");
  print_int((int)io_cq_head);
  uart_puts(" phase=");
  print_int((int)io_cq_phase);
  uart_puts(" sq_db=");
  uart_print_hex((uint64_t)io_sq_db);
  uart_puts(" cq_db=");
  uart_print_hex((uint64_t)io_cq_db);
  uart_puts(" create=");
  print_int(qcreat);
  uart_puts("\n");
  if (qcreat != 0) return -1;

  uart_puts("NVMe driver initialized successfully!\n");
  return 0;
}

/* L55: single-in-flight token.  It is a plain flag, not a lock: it is never
 * held while a *lock* is held across the device wait, so the spinlock stall
 * breaker (which frees a lock whose owner stopped taking timer interrupts)
 * cannot admit a second driver entrant — the exact mechanism that used to
 * desynchronize the completion ring.  A waiter sleeps with interrupts on. */
static void blk_token_acquire(void) {
  for (;;) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_request_lock);
    if (!blk_in_use) {
      blk_in_use = 1;
      spinlock_release_irqrestore(&blk_request_lock, flags);
      break;
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    safe_wfi();
  }
  if (__atomic_add_fetch(&blk_inside, 1, __ATOMIC_SEQ_CST) > 1) {
    uart_puts("[BLKRACE] two drivers inside the queue, inside=");
    print_int((int)blk_inside);
    uart_puts("\n");
  }
}

static void blk_token_release(void) {
  __atomic_sub_fetch(&blk_inside, 1, __ATOMIC_SEQ_CST);
  uint64_t flags = spinlock_acquire_irqsave(&blk_request_lock);
  blk_in_use = 0;
  spinlock_release_irqrestore(&blk_request_lock, flags);
}

int virtio_blk_read_raw(uint64_t sector, void* buf, uint32_t count) {
  if (count == 0) count = 1;
  blk_token_acquire();
  int res = 0;
  for (uint32_t i = 0; i < count; i++) {
    struct nvme_cmd cmd = {0};
    cmd.opcode = 0x02; // Read
    cmd.nsid = 1;      // Namespace 1
    cmd.cid = next_io_cid();
    cmd.prp1 = (uint64_t)&bounce_buf[0];
    cmd.cdw10 = (uint32_t)(sector + i);
    cmd.cdw11 = (uint32_t)((sector + i) >> 32);
    cmd.cdw12 = 0; // 1 block (0)

#ifdef KERNEL_MODE_UNIT_TEST
    /* M6/L69 test seam: reproduce QEMU's aborted transfer exactly — the
       command completes with SUCCESS and no DMA, so the bounce buffer still
       holds the previous command's sector, and the device generation moves
       (that is how a reset announces itself).  `mode` 1 aborts `times` reads
       of `sector`; mode 2 aborts every read of it (the driver must then fail
       the read instead of returning stale bytes). */
    uint8_t pre_bounce[512];
    int abort_this = blk_test_abort_armed(sector + i, pre_bounce);
#endif

    /* M6/L69: NEVER HAND OUT BYTES FROM A TRANSFER THAT OVERLAPPED A DEVICE
     * EVENT.  An aborted AIO can complete with status 0 and no DMA at all, so
     * `status == 0` is not proof that `bounce_buf` was written: the buffer
     * then holds the previous command's sector (often zeros, because a fresh
     * frame from the allocator is virgin memory).  The epoch is bumped by
     * every detection and every repair; a change across this submit means the
     * completion is not evidence, so re-issue the sector once against the
     * repaired device and only then copy out.  A second change is a hard I/O
     * error — never stale data. */
    uint64_t ep0 = blk_dev_epoch;
    /* Pre-submit: a reset that landed between two commands leaves the
       controller disabled; repair with NOTHING in flight before submitting. */
    if (!nvme_ready_now()) {
      nvme_reinit();
      ep0 = blk_dev_epoch;
      blk_prerepair++;
    }
    uint16_t status = submit_io_cmd(&cmd);
    /* Post-completion: a reset that landed DURING the command makes even a
       SUCCESS status meaningless (aborted AIO, no DMA, previous sector's
       bytes).  Learn about it now rather than at the next command's 20 ms
       poll, and let the re-issue path below discard this completion. */
    if (status == 0 && !nvme_ready_now())
      __atomic_add_fetch(&blk_dev_epoch, 1, __ATOMIC_SEQ_CST);
#ifdef KERNEL_MODE_UNIT_TEST
    blk_test_abort_apply(abort_this, pre_bounce);
#endif
    if (status == 0 && blk_dev_epoch != ep0) {
      ep0 = blk_dev_epoch;
      cmd.cid = next_io_cid();
#ifdef KERNEL_MODE_UNIT_TEST
      abort_this = blk_test_abort_armed(sector + i, pre_bounce);
#endif
      status = submit_io_cmd(&cmd);
#ifdef KERNEL_MODE_UNIT_TEST
      blk_test_abort_apply(abort_this, pre_bounce);
#endif
      blk_untrusted++;
      if (blk_untrusted_rows < 12 || (blk_untrusted & 0x3F) == 0) {
        blk_untrusted_rows++;
        uart_puts("[BLKQ] untrusted completion re-read lba=");
        uart_print_hex(sector + i);
        uart_puts(" n=");
        print_int((int)blk_untrusted);
        uart_puts(" status=");
        uart_print_hex(status);
        uart_puts("\n");
      }
      if (status == 0 && blk_dev_epoch != ep0)
        status = 0x7FFF; /* the device moved again: do not trust the bytes */
    }
    if (status != 0) {
      res = -1;
      break;
    }
    memcpy((uint8_t*)buf + (i * 512), (const void*)&bounce_buf[0], 512);
  }
  /* L9 liveness: bound the IRQ-off refresh gap to ONE device request; this
     core is provably alive right after a completed transfer. */
  {
    extern volatile uint64_t cpu_heartbeat_ms[];
    cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();
  }
  blk_token_release();
  return res;
}

int virtio_blk_write_raw(uint64_t sector, const void* buf, uint32_t count) {
  /* Hard invariant: LBA 0 is the FAT boot sector (eb 3c 90 ...); no kernel
     path may ever write it.  The observed post-wave corruption -- a
     32-byte zeroed-name directory entry spliced over the boot sector -- is
     exactly a read-modify-write whose sector base resolved to 0.  Refuse
     (and attribute, bounded) instead of destroying the volume. */
  if (sector == 0 && count > 0) {
    static int guard_once;
    if (guard_once++ < 4) {
      uart_puts("[BLK-GUARD] refused write to sector 0, caller LR=");
      uart_print_hex((uint64_t)__builtin_return_address(0));
      uart_puts("\n");
    }
    return -1;
  }
  if (count == 0) count = 1;
  blk_token_acquire();
  int res = 0;
  for (uint32_t i = 0; i < count; i++) {
    memcpy((void*)&bounce_buf[0], (const uint8_t*)buf + (i * 512), 512);
    struct nvme_cmd cmd = {0};
    cmd.opcode = 0x01; // Write
    cmd.nsid = 1;      // Namespace 1
    cmd.cid = next_io_cid();
    cmd.prp1 = (uint64_t)&bounce_buf[0];
    cmd.cdw10 = (uint32_t)(sector + i);
    cmd.cdw11 = (uint32_t)((sector + i) >> 32);
    cmd.cdw12 = 0; // 1 block (0)

    /* M6/L69: same rule as reads (see nvme_ready_now).  A write that was
       aborted by a reset is counted as landed, which is how Player.log grew
       NUL holes whose sector read back all zeros (`[FATW] ZERORD`). */
    uint64_t ep0 = blk_dev_epoch;
    if (!nvme_ready_now()) {
      nvme_reinit();
      ep0 = blk_dev_epoch;
      blk_prerepair++;
    }
    uint16_t status = submit_io_cmd(&cmd);
    if (status == 0 && !nvme_ready_now())
      __atomic_add_fetch(&blk_dev_epoch, 1, __ATOMIC_SEQ_CST);
    if (status == 0 && blk_dev_epoch != ep0) {
      /* The write may not have landed (aborted AIO).  Re-issue it once; a
         second event is a hard error, never a silent hole. */
      ep0 = blk_dev_epoch;
      cmd.cid = next_io_cid();
      status = submit_io_cmd(&cmd);
      blk_untrusted++;
      if (status == 0 && blk_dev_epoch != ep0)
        status = 0x7FFF;
    }
    if (status != 0) {
      res = -1;
      break;
    }
  }
  blk_token_release();
  return res;
}

#else

/* M6/L69: the MMIO transport has no controller reset to announce, so the
 * generation never moves.  Defined here so the interface (and the block
 * cache's publish gate) links on every architecture. */
volatile uint64_t blk_dev_epoch = 1;
volatile uint32_t blk_untrusted;

#ifdef KERNEL_MODE_UNIT_TEST
void virtio_blk_test_abort(int mode, uint64_t sector, int times) {
  (void)mode;
  (void)sector;
  (void)times;
}
#endif

// VirtIO MMIO offsets
#define VIRTIO_MAGIC        0x000
#define VIRTIO_VERSION      0x004
#define VIRTIO_DEVICE_ID    0x008
#define VIRTIO_VENDOR_ID    0x00C
#define VIRTIO_DEVICE_FEAT  0x010
#define VIRTIO_DEVICE_FEAT_SEL 0x014
#define VIRTIO_DRIVER_FEAT  0x020
#define VIRTIO_DRIVER_FEAT_SEL 0x024
#define VIRTIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_QUEUE_SEL    0x030
#define VIRTIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_QUEUE_NUM    0x038
#define VIRTIO_QUEUE_ALIGN  0x03C
#define VIRTIO_QUEUE_PFN    0x040
#define VIRTIO_QUEUE_NOTIFY 0x050
#define VIRTIO_INTERRUPT_STATUS 0x060
#define VIRTIO_INTERRUPT_ACK 0x064
#define VIRTIO_STATUS       0x070

#define MMIO_BASE(slot) ((uint8_t*)0x0A000000 + (slot) * 0x200)

// VirtIO Block types
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed));

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[8];
  uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
} __attribute__((packed));

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[8];
  uint16_t avail_event;
} __attribute__((packed));

// VirtIO MMIO V1 (Legacy) strictly dictates that the descriptor, available ring,
// and used ring arrays MUST be allocated contiguously in physical memory.
// The `used` ring must further strictly start on a Page Aligned boundary matching
// the `VIRTIO_QUEUE_ALIGN` configuration (typically 4096).
// We simulate this by defining a solitary struct, padding out the internal gap manually.
struct virtq {
  struct virtq_desc desc[8];
  struct virtq_avail avail;
  uint8_t padding[4096 - (128 + sizeof(struct virtq_avail))];
  struct virtq_used used;
} __attribute__((aligned(4096)));

struct virtio_blk_req {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
} __attribute__((packed));

static struct virtq vq __attribute__((aligned(4096)));

static struct virtio_blk_req blk_req;
static uint8_t blk_status;

static uint8_t* blk_mmio = 0;
static uint16_t ack_used_idx = 0;
int virtio_blk_irq = -1;
static spinlock_t blk_lock;
static spinlock_t blk_request_lock;

static inline void reg_write32(uint32_t offset, uint32_t val) {
  *(volatile uint32_t*)(blk_mmio + offset) = val;
}

static inline uint32_t reg_read32(uint32_t offset) {
  return *(volatile uint32_t*)(blk_mmio + offset);
}

/**
 * Handles interrupts from the VirtIO block device.
 * Acknowledges the interrupt in the device's status register.
 */
void virtio_blk_handle_irq(void) {
  uint64_t flags = spinlock_acquire_irqsave(&blk_lock);
  if (!blk_mmio) {
    spinlock_release_irqrestore(&blk_lock, flags);
    return;
  }
  uint32_t status = reg_read32(VIRTIO_INTERRUPT_STATUS);
  if (status) {
    reg_write32(VIRTIO_INTERRUPT_ACK, status);
  }
  spinlock_release_irqrestore(&blk_lock, flags);
}

/* L67: no NVMe controller state on the MMIO transport. */
void virtio_blk_diag_row(void) {
}

int virtio_blk_health(void) {
  return 0;
}

/**
 * Scans for and initializes the VirtIO block device.
 * Performs the VirtIO legacy MMIO handshake, negotiates features, and sets up the virtqueue.
 *
 * Returns:
 *   0 on success, -1 if the device is not found or fails to initialize.
 */
int virtio_blk_init(void) {
  spinlock_init(&blk_lock);
  spinlock_init(&blk_request_lock);
  // Scan for virtio block device (ID 2)
  for (int i = 0; i < 32; i++) {
    uint8_t* mmio = MMIO_BASE(i);
    uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
    uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);
    if (magic == 0x74726976 && devid == 2) {
      blk_mmio = mmio;
      virtio_blk_irq = 48 + i;
      break;
    }
  }

  if (!blk_mmio) return -1; // Not found

  uint32_t status = 0;

  // Reset device
  reg_write32(VIRTIO_STATUS, status);

  // Acknowledge
  status |= 1; reg_write32(VIRTIO_STATUS, status);

  // Driver
  status |= 2; reg_write32(VIRTIO_STATUS, status);

  // Read features, accept them (by writing 0 since we don't need advanced features)
  // BUT we MUST negotiate VIRTIO_F_VERSION_1 (bit 32) since QEMU virt is MMIO Version 2
  reg_write32(VIRTIO_DRIVER_FEAT_SEL, 1);
  reg_write32(VIRTIO_DRIVER_FEAT, 1); // Set bit 32 (bit 0 of high word)

  // Write 0 to standard features
  reg_write32(VIRTIO_DRIVER_FEAT_SEL, 0);
  reg_write32(VIRTIO_DRIVER_FEAT, 0);

  // Features OK
  status |= 8; reg_write32(VIRTIO_STATUS, status);
  if (!(reg_read32(VIRTIO_STATUS) & 8)) return -1;

  // Setup queue 0
  reg_write32(VIRTIO_GUEST_PAGE_SIZE, 4096);
  reg_write32(VIRTIO_QUEUE_SEL, 0);
  uint32_t max_size = reg_read32(VIRTIO_QUEUE_NUM_MAX);
  if (max_size == 0) return -1;

  reg_write32(VIRTIO_QUEUE_NUM, 8);
  reg_write32(VIRTIO_QUEUE_ALIGN, 4096);

  uint32_t pfn = (uint32_t)((uint64_t)&vq / 4096);
  reg_write32(VIRTIO_QUEUE_PFN, pfn);

  // Driver OK
  status |= 4; reg_write32(VIRTIO_STATUS, status);

  ack_used_idx = 0;
  return 0;
}

extern void uart_puts(const char* s);
extern void uart_print_hex(uint64_t val);
extern void safe_wfi(void);
extern int print_int(int val);

/**
 * Internal helper to perform a block operation (Read or Write) over
 * `count` consecutive sectors in ONE device request.  Sets up the
 * 3-descriptor chain (Header, Data, Status) and notifies the device.
 * The virtio-blk data descriptor length covers count * 512 bytes; the
 * device reads/writes `count` sectors starting at `sector`.  buf must be
 * physically contiguous (all callers use identity-mapped kernel or
 * physical-block memory).
 *
 * Parameters:
 *   sector - First target sector index on disk.
 *   buf    - Data buffer in memory.
 *   type   - VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT.
 *   count  - Number of consecutive sectors (>= 1).
 *
 * Returns:
 *   0 on success, -1 on failure.
 */
static int virtio_blk_do_op(uint64_t sector, void* buf, uint32_t type, uint32_t count) {
  uint64_t flags = spinlock_acquire_irqsave(&blk_lock);
  if (!blk_mmio) {
    spinlock_release_irqrestore(&blk_lock, flags);
    return -1;
  }
  if (count == 0) count = 1;
  if (count > 1024) count = 1024; /* 512 KiB cap per request */

  uint16_t desc_idx = 0;

  blk_req.type = type;
  blk_req.reserved = 0;
  blk_req.sector = sector;

  // Descriptor 0: The request header
  vq.desc[0].addr = (uint64_t)&blk_req;
  vq.desc[0].len = sizeof(struct virtio_blk_req);
  vq.desc[0].flags = 1; // VIRTQ_DESC_F_NEXT
  vq.desc[0].next = 1;

  // Descriptor 1: The data buffer (count sectors)
  vq.desc[1].addr = (uint64_t)buf;
  vq.desc[1].len = 512 * count;
  vq.desc[1].flags = 1 | (type == VIRTIO_BLK_T_IN ? 2 : 0); // VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE
  vq.desc[1].next = 2;

  // Descriptor 2: The status byte
  vq.desc[2].addr = (uint64_t)&blk_status;
  vq.desc[2].len = 1;
  vq.desc[2].flags = 2; // VIRTQ_DESC_F_WRITE
  vq.desc[2].next = 0;

  // Publish to available ring
  uint16_t head_idx = vq.avail.idx;
  vq.avail.ring[head_idx % 8] = 0; // head descriptor is 0

  // Memory barrier
  arch_memory_barrier();

  vq.avail.idx++;

  arch_memory_barrier();

  // Notify device
  reg_write32(VIRTIO_QUEUE_NOTIFY, 0);

  /* Wait for completion.  blk_lock is NOT held here: the caller already
     holds the blk_in_use token (single in-flight request), so releasing
     blk_lock across the wait cannot admit a second device op, and it
     stops a system-wide freeze when the device is slow - a bare
     IRQ-masked spin here let waiters pile up on blk_lock while IRQ
     handlers blocked on blk_request_lock.  A tight poll is used (wfi in
     the loop measurably throttled the loader's disk I/O); each iteration
     re-checks the ring through a memory barrier so MTTCG sees the
     device's update. */
  spinlock_release_irqrestore(&blk_lock, flags);

  uint64_t wait_guard = 0;
  while (*(volatile uint16_t*)&vq.used.idx == ack_used_idx) {
    /* L9 liveness: this poll is the IRQ-off disk wait the loader core
       stands in for the whole 86 MB image read, so a peer reaper must not
       age the claim stale and DISPOSE the mid-load process.  Refresh the
       ownership heartbeat on a throttled cadence — the core is provably
       alive (it is servicing the device).  Writes only; process.c owns
       the array. */
    if ((wait_guard & 0xFFF) == 0) {
      extern volatile uint64_t cpu_heartbeat_ms[];
      extern uint64_t timer_get_ms(void);
      cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();
    }
    arch_memory_barrier();
    if ((++wait_guard & 0xFFFFFFF) == 0) {
      uart_puts("[BLKDIAG] waiting used.idx=");
      uart_print_hex(vq.used.idx);
      uart_puts(" ack=");
      uart_print_hex(ack_used_idx);
      uart_puts("\n");
    }
  }

  uint64_t relock_flags = spinlock_acquire_irqsave(&blk_lock);

  ack_used_idx = vq.used.idx;

  // Memory barrier before reading status
  arch_memory_barrier();

  // Ack interrupt manually since we disabled IRQs
  uint32_t int_status = reg_read32(VIRTIO_INTERRUPT_STATUS);
  if (int_status) {
    reg_write32(VIRTIO_INTERRUPT_ACK, int_status);
  }

  int res = (blk_status == 0 ? 0 : -1);
  spinlock_release_irqrestore(&blk_lock, relock_flags);
  return res;
}

/**
 * Reads one or more sectors from the block device.
 */
volatile int blk_in_use = 0;

int virtio_blk_read_raw(uint64_t sector, void* buf, uint32_t count) {
  uint64_t flags;
  while (1) {
    flags = spinlock_acquire_irqsave(&blk_request_lock);
    if (!blk_in_use) {
      blk_in_use = 1;
      spinlock_release_irqrestore(&blk_request_lock, flags);
      break;
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    safe_wfi();
  }

  int res = 0;
  if (count == 0) count = 1;
  /* One multi-sector request (the device walks count sectors from
     `sector`); this is what collapses the loader's hundreds of
     single-sector round trips per program into a handful. */
  if (virtio_blk_do_op(sector, buf, VIRTIO_BLK_T_IN, count) != 0) {
    res = -1;
  }
  /* L9 liveness: bound the IRQ-off refresh gap to ONE device op.  The
     loader's fat16_read path issues one op per 512 B sector here
     (count==1), and under TCG a cross-core sampling race once aged a
     genuinely-loading owner past LOSTWAKE_DEAD_OWNER_MS between the
     per-page refreshes in v2_map_image (a false LOSTWAKE dispose).  This
     core is provably alive right after a completed op.  Writes only;
     process.c owns the array. */
  {
    extern volatile uint64_t cpu_heartbeat_ms[];
    extern uint64_t timer_get_ms(void);
    cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();
  }

  flags = spinlock_acquire_irqsave(&blk_request_lock);
  blk_in_use = 0;
  spinlock_release_irqrestore(&blk_request_lock, flags);
  /* SEC-OBS-B: deferred — this completion runs inside fat_lock (FAT
   * metadata I/O); proc_lock here would invert proc -> ... -> fat. */
  process_wake_all_deferred();
  return res;
}

int virtio_blk_write_raw(uint64_t sector, const void* buf, uint32_t count) {
  /* Hard invariant: LBA 0 is the FAT boot sector (eb 3c 90 ...); no kernel
     path may ever write it.  The observed post-wave corruption -- a
     32-byte zeroed-name directory entry spliced over the boot sector -- is
     exactly a read-modify-write whose sector base resolved to 0.  Refuse
     (and attribute, bounded) instead of destroying the volume.  (count == 0
     means one sector here, so the guard ignores count on this arch.) */
  if (sector == 0) {
    static int guard_once;
    if (guard_once++ < 4) {
      uart_puts("[BLK-GUARD] refused write to sector 0, caller LR=");
      uart_print_hex((uint64_t)__builtin_return_address(0));
      uart_puts("\n");
    }
    return -1;
  }
  uint64_t flags;
  while (1) {
    flags = spinlock_acquire_irqsave(&blk_request_lock);
    if (!blk_in_use) {
      blk_in_use = 1;
      spinlock_release_irqrestore(&blk_request_lock, flags);
      break;
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    safe_wfi();
  }

  int res = 0;
  if (count == 0) count = 1;
  if (virtio_blk_do_op(sector, (void*)buf, VIRTIO_BLK_T_OUT, count) != 0) {
    res = -1;
  }

  flags = spinlock_acquire_irqsave(&blk_request_lock);
  blk_in_use = 0;
  spinlock_release_irqrestore(&blk_request_lock, flags);
  /* SEC-OBS-B: deferred — this completion runs inside fat_lock (FAT
   * metadata I/O); proc_lock here would invert proc -> ... -> fat. */
  process_wake_all_deferred();
  return res;
}

#endif

/* ---------------------------------------------------------------------
 * Public block API (shared, arch-neutral): routes reads through the
 * block-level read cache (docs/blk-cache.md) when it is ready, and keeps
 * cached copies coherent on writes (raw write COMMITS first, then the
 * written range is invalidated — never the reverse, otherwise a
 * concurrent reader could refill stale data between the two).
 * ------------------------------------------------------------------- */

int virtio_blk_read_sector(uint64_t sector, void* buf, uint32_t count) {
  if (blk_cache_is_ready())
    return blk_cache_read(sector, buf, count);
  return virtio_blk_read_raw(sector, buf, count);
}

int virtio_blk_write_sector(uint64_t sector, const void* buf, uint32_t count) {
  int rc = virtio_blk_write_raw(sector, buf, count);
  blk_cache_invalidate(sector, count);
  return rc;
}
