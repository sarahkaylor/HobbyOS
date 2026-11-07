#include "virtio_blk.h"
#include "lock.h"
#include "process.h"
#include "arch/cpu.h"
#include "blk_cache.h"
#include "frame.h"
#include "fat16.h"   /* L78: the stress probe's write-verify free-cluster guard */
#include "kline.h"   /* L78: one console record per BKS row (atomic emit) */


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
/* L78: bounded variant for the block-stress probe.  Returns 1 when the token
   was taken, 0 when the holder was still inside a transfer after `timeout_ms`.
   The probe counts a timeout as `busy` and yields; it never spins on the token
   (the L75 stall). */
static int blk_token_try_acquire(uint32_t timeout_ms);

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
 * layout.  `nvme_reinit()` is the recovery half of a controller reset: it
 * RE-INITIALISES the controller from the registers up (CAP/VS re-read, CC
 * reprogrammed, admin + IO queues recreated, namespace revalidated) instead
 * of re-issuing a command into a device whose queue addresses the reset just
 * destroyed.
 * ------------------------------------------------------------------------- */

/* L75: the live controller identity, re-read from the registers on every
 * re-initialisation.  CAP/VS are read-only and survive a reset, so a reset
 * that left the BAR pointing somewhere else (or a device that came back with
 * a different queue geometry) is visible here. */
static struct {
  uint32_t cap_lo, cap_hi, vs;
  uint32_t mqes;      /* CAP.MQES + 1: max queue entries */
  uint32_t dstrd;     /* CAP.DSTRD: doorbell stride, log2(4 << dstrd) */
  uint32_t mpsmin, mpsmax;
  uint32_t nsid;
  uint64_t nsze;      /* namespace size in LBAs, from Identify */
  uint32_t lbads;     /* log2(LBA bytes) from the active LBA format */
  uint32_t flbas;
  int valid;
} nvme_id;

/* Re-read CAP/VS and derive the queue geometry from them.  Also re-derives
 * the four doorbell pointers: DSTRD is a CAP field, and a re-initialisation
 * that kept the boot-time pointers would be trusting a value the reset may
 * have changed. */
static void nvme_read_cap(void) {
  nvme_id.cap_lo = nvme_regs[0];
  nvme_id.cap_hi = nvme_regs[1];
  nvme_id.vs = nvme_regs[2];
  nvme_id.mqes = (nvme_id.cap_lo & 0xFFFF) + 1;
  nvme_id.dstrd = (nvme_id.cap_hi >> 0) & 0xF;
  /* CAP is 64-bit: MPSMIN is bits 51:48 and MPSMAX bits 55:52, i.e. in the
     high word.  (Reading them from the low word yields CAP.CQR/TO and a
     bogus 8 KiB page requirement, which makes nvme_start_ctrl fail with
     CSTS.CFS after the queues it was handed are "misaligned".) */
  nvme_id.mpsmin = (nvme_id.cap_hi >> 16) & 0xF;
  nvme_id.mpsmax = (nvme_id.cap_hi >> 20) & 0xF;

  uint32_t stride_words = (4u << nvme_id.dstrd) / 4u;
  admin_sq_db = &nvme_regs[1024 + 0 * 2 * stride_words];
  admin_cq_db = &nvme_regs[1024 + (0 * 2 + 1) * stride_words];
  io_sq_db    = &nvme_regs[1024 + 1 * 2 * stride_words];
  io_cq_db    = &nvme_regs[1024 + (1 * 2 + 1) * stride_words];
}

/* Prepare the admin queues and enable the controller: the same sequence the
 * boot path runs, expressed once so a re-initialisation cannot drift from it.
 * Returns 0 when CSTS.RDY set and CC read back exactly as written. */
static int nvme_admin_enable(void) {
  nvme_regs[9] = 0x00010001;            /* AQA: 2 admin SQ + 2 admin CQ entries */
  for (int i = 0; i < 2; i++) {
    admin_cq[i].cid = 0xFFFF;
    admin_cq[i].status = 0;
  }
  admin_sq_tail = 0;
  admin_cq_head = 0;
  uint64_t asq = (uint64_t)&admin_sq;
  nvme_regs[10] = (uint32_t)asq;
  nvme_regs[11] = (uint32_t)(asq >> 32);
  uint64_t acq = (uint64_t)&admin_cq;
  nvme_regs[12] = (uint32_t)acq;
  nvme_regs[13] = (uint32_t)(acq >> 32);
  (void)nvme_regs[11];
  (void)nvme_regs[13];

  /* CC (NVMe 1.4 / QEMU's include/block/nvme.h): EN bit 0, CSS bits 6:4,
     MPS bits 10:7, AMS bits 13:11, SHN bits 15:14, IOSQES bits 19:16,
     IOCQES bits 23:20.  MPS = CAP.MPSMIN (0 here: 4 KiB pages), IOSQES=6
     (64 B commands), IOCQES=4 (16 B completions) => 0x00460001, which is
     also what the boot path uses. */
  uint32_t mps = nvme_id.mpsmin;
  uint32_t cc = 1u | ((mps & 0x7) << 7) | (6u << 16) | (4u << 20);
  nvme_regs[5] = cc;
  blk_cc_en++;
  (void)nvme_regs[5];

  uint64_t t0 = timer_get_ms();
  while (!(nvme_regs[7] & 1)) {
    __asm__ volatile("pause");
    if ((timer_get_ms() - t0) > 2000) {
      uart_puts("[BLKQ] reset: RDY never set, CSTS=");
      uart_print_hex(nvme_regs[7]);
      uart_puts(" CC=");
      uart_print_hex(nvme_regs[5]);
      uart_puts("\n");
      return -1;
    }
  }
  /* The device echoes CC; a mismatch means we are not talking to the
     controller we initialised. */
  if (nvme_regs[5] != cc) {
    uart_puts("[BLKQ] reset: CC readback mismatch want=");
    uart_print_hex(cc);
    uart_puts(" got=");
    uart_print_hex(nvme_regs[5]);
    uart_puts("\n");
    return -1;
  }
  return 0;
}

/* Identify (CNS=0) the namespace and record its geometry.  The controller is
 * the only authority on what the namespace is; a re-initialisation that
 * skipped this would keep using a size/lba-width the reset may have changed
 * (and the driver's own read path is LBA-based, so a wrong nsze silently
 * turns into reads past the end). */
static uint8_t id_ns[4096] __attribute__((aligned(4096)));

static uint16_t nvme_identify_ns(uint32_t nsid, uint8_t *buf) {
  for (int i = 0; i < 4096; i++)
    buf[i] = 0;
  struct nvme_cmd id = {0};
  id.opcode = 0x06;      /* Identify */
  id.nsid = nsid;
  id.cid = 3;
  id.prp1 = (uint64_t)buf;
  id.cdw10 = 0x00;       /* CNS = 0: identify namespace */
  return submit_admin_cmd(&id);
}

static int nvme_ns_validate(int print_rows) {
  uint16_t st = nvme_identify_ns(1, id_ns);
  if (st != 0) {
    uart_puts("[BLKQ] identify ns failed status=");
    uart_print_hex(st);
    uart_puts("\n");
    return -1;
  }
  uint64_t nsze = 0;
  for (int i = 7; i >= 0; i--)
    nsze = (nsze << 8) | id_ns[i];
  uint32_t nlbaf = id_ns[25];
  uint32_t flbas = id_ns[26] & 0x0F;
  uint32_t lbads = 0;
  if (flbas <= nlbaf)
    lbads = id_ns[128 + 4 * flbas + 2];
  if (nsze == 0 || lbads == 0 || lbads > 16) {
    uart_puts("[BLKQ] identify ns implausible nsze=");
    uart_print_hex(nsze);
    uart_puts(" lbads=");
    print_int((int)lbads);
    uart_puts("\n");
    return -1;
  }
  /* Identify Controller (CNS=1) as well: it is the read that proves the
   * admin queue we just created really carries admin commands. */
  for (int i = 0; i < 4096; i++)
    id_ns[i] = 0;
  struct nvme_cmd idc = {0};
  idc.opcode = 0x06;
  idc.nsid = 0;
  idc.cid = 4;
  idc.prp1 = (uint64_t)id_ns;
  idc.cdw10 = 0x01;      /* CNS = 1: identify controller */
  uint16_t stc = submit_admin_cmd(&idc);
  if (stc != 0) {
    uart_puts("[BLKQ] identify ctrl failed status=");
    uart_print_hex(stc);
    uart_puts("\n");
    return -1;
  }
  uint32_t vid = id_ns[0] | (id_ns[1] << 8);
  uint32_t ssvid = id_ns[2] | (id_ns[3] << 8);

  nvme_id.nsid = 1;
  nvme_id.nsze = nsze;
  nvme_id.lbads = lbads;
  nvme_id.flbas = flbas;
  nvme_id.valid = 1;
  if (print_rows) {
    uart_puts("[BLKQ] ns nsid=1 nsze=");
    uart_print_hex(nsze);
    uart_puts(" lbads=");
    print_int((int)lbads);
    uart_puts(" flbas=");
    print_int((int)flbas);
    uart_puts(" vid=0x");
    uart_print_hex(vid);
    uart_puts("/0x");
    uart_print_hex(ssvid);
    uart_puts("\n");
  }
  return 0;
}

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

/* Reset the controller and rebuild it FROM THE REGISTERS UP.  Called only
   with the request token held (single entrant) and after the in-flight
   command has been abandoned.

   L75: this is a full re-initialisation, not a re-enable:
     1. wait out a host reset that is already running (CC.EN=1 && CSTS.RDY=0);
     2. re-read CAP/VS and re-derive AQA sizing + the four doorbell
        pointers from CAP.DSTRD;
     3. CC=0, wait RDY=0, program AQA + ASQ/ACQ + CC (EN, IOSQES=6,
        IOCQES=4, AMS=0, MPS=CAP.MPSMIN) and verify CC reads back;
     4. zero both rings and create CQ 1 / SQ 1 again (the reset freed the
        old queue objects, and the old doorbells now point at nothing);
     5. revalidate the namespace (Identify CNS=0 + CNS=1: nsze, LBA format,
        controller id) before any transfer is issued into it;
     6. flush every cached unit (a frame filled across the reset is not
        evidence) and confirm the device stays ready for 50 ms.

   L67's half-reset defences stay: a host reset can still be in progress
   when we get here, and QEMU's nvme_ctrl_reset() writes CSTS=0 first and
   clears CC only after it has drained the namespaces, so enabling inside
   that window creates queues the reset then frees. */
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

  /* (0) A reset invalidates every cached unit, whether or not the rebuild
     below succeeds: the frames were filled by a device that no longer
     exists.  Flush first so no failure path can leave a poisoned frame
     cached (L69's rule, now unconditional). */
  {
    uint64_t dropped = blk_cache_flush_all();
    static int fl_rows;
    if (fl_rows < 8) {
      fl_rows++;
      uart_puts("[BLKC] post-reset flush dropped=");
      print_int((int)dropped);
      uart_puts(" cached units\n");
    }
  }

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

  /* (2) CAP/VS are the device's identity; re-read them before programming
     anything derived from them. */
  nvme_read_cap();
  uart_puts("[BLKQ] reinit cap=");
  uart_print_hex(nvme_id.cap_lo);
  uart_puts("/");
  uart_print_hex(nvme_id.cap_hi);
  uart_puts(" vs=");
  uart_print_hex(nvme_id.vs);
  uart_puts(" mqes=");
  print_int((int)nvme_id.mqes);
  uart_puts(" dstrd=");
  print_int((int)nvme_id.dstrd);
  uart_puts(" mps=");
  print_int((int)nvme_id.mpsmin);
  uart_puts("/");
  print_int((int)nvme_id.mpsmax);
  uart_puts(" sq_db=");
  uart_print_hex((uint64_t)io_sq_db);
  uart_puts(" cq_db=");
  uart_print_hex((uint64_t)io_cq_db);
  uart_puts("\n");

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

  /* (3) admin queues + CC. */
  if (nvme_admin_enable() != 0) {
    uart_puts("[BLKQ] reset: enable failed (attempt ");
    print_int(attempt);
    uart_puts(")\n");
    continue;
  }
  uart_puts("[BLKQ] reinit cc=");
  uart_print_hex(nvme_regs[5]);
  uart_puts(" csts=");
  uart_print_hex(nvme_regs[7]);
  uart_puts(" aqa=");
  uart_print_hex(nvme_regs[9]);
  uart_puts(" asq=");
  uart_print_hex(((uint64_t)nvme_regs[11] << 32) | nvme_regs[10]);
  uart_puts(" acq=");
  uart_print_hex(((uint64_t)nvme_regs[13] << 32) | nvme_regs[12]);
  uart_puts("\n");

  /* (4) The device re-initialises the CQ from scratch: zero EVERY entry so
     no pre-reset phase tag can ever validate a stale CQE, then rebuild. */
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

  /* (4b) The IO queue objects exist again; revalidate the namespace through
     them before the caller's transfer is re-issued. */
  if (nvme_ns_validate(blk_resets <= 8) != 0)
    continue;

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
      uart_puts("[BLKQ] reinit ok resets=");
      print_int((int)blk_resets);
      uart_puts(" nsze=");
      uart_print_hex(nvme_id.nsze);
      uart_puts(" lbads=");
      print_int((int)nvme_id.lbads);
      uart_puts(" qd=");
      print_int(NVME_IOQ_DEPTH);
      uart_puts("\n");
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

/* L75: every controller state change we can observe, logged.
 *
 * The task's instrument: CC, CSTS, INTMS/INTMC and the admin ring indices
 * are sampled at the timer tick (100 Hz, CPU 0) and a row is printed for
 * EVERY transition, so "the controller went away at t=NNN" is a timestamped
 * event with the register values on both sides of it instead of a
 * post-mortem inference.  Cheap: four MMIO reads per tick.
 *
 * A transition this log cannot explain (CC changing with no `[BLKQ]`
 * controller reset and no blk_cc_zero increment) means something other than
 * this driver wrote the register file -- which is exactly how the L65-L73
 * resets happened (a page-cache frame aliasing the BAR). */
static struct {
  uint32_t cc, csts, intms, intmc;
  uint16_t asq_tail, acq_head;
} mmio_snap;
static volatile uint32_t blk_mmio_changes;
static volatile uint32_t blk_mmio_rows;

void virtio_blk_watch_mmio(void) {
  if (!nvme_regs)
    return;
  uint32_t cc = nvme_regs[5];
  uint32_t csts = nvme_regs[7];
  uint32_t intms = nvme_regs[3];
  uint32_t intmc = nvme_regs[4];
  uint16_t asqt = admin_sq_tail;
  uint16_t acqh = admin_cq_head;
  if (cc == mmio_snap.cc && csts == mmio_snap.csts &&
      intms == mmio_snap.intms && intmc == mmio_snap.intmc &&
      asqt == mmio_snap.asq_tail && acqh == mmio_snap.acq_head)
    return;
  blk_mmio_changes++;
  if (blk_mmio_rows < 48 || (blk_mmio_changes & 0x3F) == 0) {
    blk_mmio_rows++;
    uart_puts("[BLKQ] mmio cc=");
    uart_print_hex(cc);
    uart_puts(" csts=");
    uart_print_hex(csts);
    uart_puts(" intms=");
    uart_print_hex(intms);
    uart_puts(" intmc=");
    uart_print_hex(intmc);
    uart_puts(" asqt=");
    print_int((int)asqt);
    uart_puts(" acqh=");
    print_int((int)acqh);
    uart_puts(" was_cc=");
    uart_print_hex(mmio_snap.cc);
    uart_puts(" was_csts=");
    uart_print_hex(mmio_snap.csts);
    uart_puts(" n=");
    print_int((int)blk_mmio_changes);
    uart_puts(" cpu=");
    print_int((int)get_cpuid());
    uart_puts(" ms=");
    print_int((int)timer_get_ms());
    uart_puts("\n");
  }
  mmio_snap.cc = cc;
  mmio_snap.csts = csts;
  mmio_snap.intms = intms;
  mmio_snap.intmc = intmc;
  mmio_snap.asq_tail = asqt;
  mmio_snap.acq_head = acqh;
}

/* L75: the data-path proof across a full re-initialisation.
 *
 * Reads one fixed, never-written 512 KiB region of the namespace twice
 * through the block cache and twice raw, in between forcing the FULL
 * re-initialisation (CAP/VS re-read, CC reprogram, queues recreated,
 * namespace revalidated) and the post-reset cache flush.  The four sums must
 * be identical: a cache frame filled across the reset would show up as
 * sum_c2 != sum_c1, an aborted DMA as sum_r2 != sum_r1, and a stale queue
 * would make the second raw read fail outright.
 *
 * Runs once per boot, 4 minutes in (well into the game's load), from the 5 s
 * sampler thread -- never from a fault path, and only with the request token
 * held across the re-initialisation itself.
 */
static uint64_t selfcheck_sum_raw(uint64_t lba, int nsec) {
  uint8_t buf[4096];
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < nsec; i += 8) {
    int n = (nsec - i < 8) ? (nsec - i) : 8;
    if (virtio_blk_read_raw(lba + i, buf, (uint32_t)n) != 0)
      return 0;
    for (int k = 0; k < n * 512; k++) {
      h ^= buf[k];
      h *= 0x100000001b3ULL;
    }
  }
  return h;
}

static uint64_t selfcheck_sum_cache(uint64_t lba, int nsec) {
  uint8_t buf[4096];
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < nsec; i += 8) {
    int n = (nsec - i < 8) ? (nsec - i) : 8;
    if (blk_cache_read(lba + i, buf, (uint32_t)n) != 0)
      return 0;
    for (int k = 0; k < n * 512; k++) {
      h ^= buf[k];
      h *= 0x100000001b3ULL;
    }
  }
  return h;
}

void virtio_blk_selfcheck(void) {
  static int done;
  static uint32_t last_reqs;
  static uint64_t last_sample, quiet_since;
  if (done || !nvme_regs || !nvme_id.valid)
    return;
  uint64_t now = timer_get_ms();
  if (now < 240000)
    return;
  /* L78: a forced controller re-initialisation is a deliberate disturbance.
     Running it at a fixed t=240 s put it in the middle of the loader's page-in
     stream (the census held the device token for the whole rebuild, a FAT
     writer waited on it for 15 s and the stall breaker fired: measured), so
     the re-init now waits for a quiet device: no completed command for
     QUIET_MS.  It still runs if the device never goes quiet (forced at 1500 s)
     and is skipped with a row after 1750 s, so the window's end is never spent
     inside a rebuild. */
  if (!last_sample) {
    last_sample = now;
    quiet_since = now;
    last_reqs = blk_requests;
  }
  if (now - last_sample >= 1000) {
    if (blk_requests != last_reqs)
      quiet_since = now;
    last_reqs = blk_requests;
    last_sample = now;
  }
  int quiet = (now - quiet_since) >= 1500;
  if (!quiet && now < 1500000)
    return;
  if (!quiet && now >= 1750000) {
    done = 1;
    struct kline ks;
    kl_init(&ks);
    kl_s(&ks, "[BLKQ] selfcheck skipped: device busy for the whole window t=");
    kl_i(&ks, (long)(now / 1000));
    kl_s(&ks, "s reqs=");
    kl_i(&ks, (long)blk_requests);
    kl_c(&ks, '\n');
    kl_emit(&ks);
    return;
  }
  uint64_t lba = (nvme_id.nsze > 32768) ? nvme_id.nsze - 8192 : 0;
  int nsec = 1024;
  uint64_t c1 = selfcheck_sum_cache(lba, nsec);
  uint64_t r1 = selfcheck_sum_raw(lba, nsec);
  int rr;
  /* The sums above take the token themselves (they are cache/raw reads), so
     the rebuild's token is taken here, with a deadline: a busy device means
     "next census", never a spin inside the 5 s census. */
  if (!blk_token_try_acquire(200))
    return;
  done = 1;
  uint64_t t0 = timer_get_ms();
  rr = nvme_reinit();
  blk_token_release();
  uint64_t ms = timer_get_ms() - t0;
  uint64_t c2 = selfcheck_sum_cache(lba, nsec);
  uint64_t r2 = selfcheck_sum_raw(lba, nsec);
  uart_puts("[BLKQ] selfcheck lba=");
  uart_print_hex(lba);
  uart_puts(" n=");
  print_int(nsec);
  uart_puts(" cache1=");
  uart_print_hex(c1);
  uart_puts(" cache2=");
  uart_print_hex(c2);
  uart_puts(" raw1=");
  uart_print_hex(r1);
  uart_puts(" raw2=");
  uart_print_hex(r2);
  uart_puts(" reinit_rc=");
  print_int(rr);
  uart_puts(" ms=");
  print_int((int)ms);
  uart_puts(" match=");
  print_int((c1 == c2 && r1 == r2 && r1 != 0) ? 1 : 0);
  uart_puts("\n");
}

/* L78: the block-stress probe (RW_BLK_STRESS=1), survivable shape.
 *
 * L75 shipped this as a thread that issued 24 reads + 3 write-verifies in a
 * tight round, took the L55 single-in-flight token with the unbounded
 * `blk_token_acquire()`, shared one 4 KiB stack buffer across the round, and
 * then `safe_wfi()`-spun for a second.  It made ~70 s into the game's load and
 * then stopped.  The L78 shape is deliberately small and bounded:
 *
 *   - ONE device operation in flight.  `stress_inflight` is asserted before
 *     every op and cleared after it; the driver is single-inflight and the
 *     probe respects that instead of pretending to be a deep queue.
 *   - ONE operation per `BKS_OP_MS` (200 ms -> 5 ops/s).  The game's plateau
 *     is ~150 device ops/s, so the probe is ~3 % of the device's work and
 *     cannot starve the loader's page-ins.
 *   - the token is taken with a DEADLINE (`blk_token_try_acquire`): if the
 *     game is mid-transfer for longer than `BKS_TOKEN_WAIT_MS`, the probe
 *     counts `busy` and yields.  It never spins on the token -- that is the
 *     L75 stall, and an unbounded wait there also parks a CPU in `safe_wfi()`.
 *   - between ops the thread halts (`safe_wfi`) for 250 ms: four wake-ups a
 *     second, no runnable loop, no scheduler state of its own.  A scheduler
 *     sleep (process_sleep_ms/arch_yield) looked cleaner but the windows that
 *     used it died at t=244 s with a SEC-4 canary panic; a kernel thread that
 *     only halts is the shape every other driver thread here already has.
 *   - every failure is a counted, bounded row and the probe CONTINUES:
 *     `busy` (token timeout), `skip` (device being repaired / bad geometry),
 *     `bad` (submit error), `mismatch` (write-verify did not read back), and
 *     `slow` (one op over `BKS_SLOW_MS`, followed by a back-off).  Nothing
 *     retries in a loop and nothing holds a lock across more than one command.
 *   - write-verify only touches a cluster whose FAT entry is 0
 *     (`fat16_lba_scratch_geom` + the entry read below), drops the block cache
 *     for the sector after the raw write, and writes back exactly the bytes it
 *     read.  It can therefore never stamp a live FAT/dir/data sector, and it
 *     cannot leave a stale cached page behind for a later allocation.
 */
#ifdef RW_BLK_STRESS
#define BKS_OP_MS 250         /* one op per 250 ms: <= 4 device ops/s */
#define BKS_TOKEN_WAIT_MS 60  /* skip, never spin, if the game owns the token */
#define BKS_SLOW_MS 2000      /* one op this slow is a row + a back-off */
#define BKS_BACKOFF_MS 5000
#define BKS_ROW_MS 60000
#define BKS_WV_EVERY 8        /* every 8th op is a write-verify */
#define BKS_FAIL_ROWS 32      /* bounded detail rows per counter */

static uint32_t stress_seed = 0x9E3779B9u;
static volatile uint32_t stress_reads, stress_writes, stress_bad,
    stress_mismatch, stress_busy, stress_skipped, stress_slow, stress_ops,
    stress_inflight, stress_rows, stress_bad_rows, stress_mismatch_rows;

static uint32_t stress_rnd(void) {
  stress_seed = stress_seed * 1664525u + 1013904223u;
  return stress_seed >> 8;
}

/* Wait `ms` without touching the scheduler.  Deliberately NOT
 * process_sleep_ms()/arch_yield(): the probe is a kernel thread and the L78
 * windows that used a scheduler sleep here died at t=244 s with
 * `KERNEL PANIC: stack smashing detected (SEC-4 canary)` -- the only new
 * thing in those kernels was a kernel thread calling arch_yield() with a
 * BLOCKED state.  A halt-until-interrupt loop is what every other driver
 * thread in this kernel does; the probe wakes ~4 times a second, so the cost
 * is one idle wake-up per rate-limit period, not a spin. */
static void stress_delay_ms(uint32_t ms) {
  uint64_t t0 = timer_get_ms();
  while (timer_get_ms() - t0 < ms)
    safe_wfi();
}

/* The probe's own submit path, called with the token HELD (never through
 * virtio_blk_read_raw/write_raw, which would take it again).  One command per
 * sector; a nonzero status is returned to the caller, never retried here. */
static int stress_read_locked(uint64_t lba, uint8_t *buf, uint32_t nsec) {
  for (uint32_t i = 0; i < nsec; i++) {
    struct nvme_cmd cmd = {0};
    cmd.opcode = 0x02; /* Read */
    cmd.nsid = 1;
    cmd.cid = next_io_cid();
    cmd.prp1 = (uint64_t)&bounce_buf[0];
    cmd.cdw10 = (uint32_t)(lba + i);
    cmd.cdw11 = (uint32_t)((lba + i) >> 32);
    cmd.cdw12 = 0; /* 1 block */
    uint16_t status = submit_io_cmd(&cmd);
    if (status != 0)
      return -1;
    memcpy(buf + (i * 512), (const void *)&bounce_buf[0], 512);
  }
  return 0;
}

static int stress_write_locked(uint64_t lba, const uint8_t *buf) {
  struct nvme_cmd cmd = {0};
  memcpy((void *)&bounce_buf[0], buf, 512);
  cmd.opcode = 0x01; /* Write */
  cmd.nsid = 1;
  cmd.cid = next_io_cid();
  cmd.prp1 = (uint64_t)&bounce_buf[0];
  cmd.cdw10 = (uint32_t)lba;
  cmd.cdw11 = (uint32_t)(lba >> 32);
  cmd.cdw12 = 0;
  return submit_io_cmd(&cmd) == 0 ? 0 : -1;
}

static void stress_row(const char *tag, uint64_t now) {
  /* kline + one uart_puts: the console is shared with the game's own rows and
     a record built from many puts is shredded at every call boundary (the L75
     run lost its `[BKS] start` row that way). */
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[BKS] ");
  kl_s(&kl, tag);
  kl_s(&kl, " t=");
  kl_i(&kl, (long)(now / 1000));
  kl_s(&kl, "s ops=");
  kl_i(&kl, (long)stress_ops);
  kl_s(&kl, " reads=");
  kl_i(&kl, (long)stress_reads);
  kl_s(&kl, " write_verify=");
  kl_i(&kl, (long)stress_writes);
  kl_s(&kl, " bad=");
  kl_i(&kl, (long)stress_bad);
  kl_s(&kl, " mismatch=");
  kl_i(&kl, (long)stress_mismatch);
  kl_s(&kl, " busy=");
  kl_i(&kl, (long)stress_busy);
  kl_s(&kl, " skip=");
  kl_i(&kl, (long)stress_skipped);
  kl_s(&kl, " slow=");
  kl_i(&kl, (long)stress_slow);
  kl_s(&kl, " inflight=");
  kl_i(&kl, (long)stress_inflight);
  kl_c(&kl, '\n');
  kl_emit(&kl);
  stress_rows++;
}

/* One op.  Returns when the op is done or refused; never loops on failure. */
/* Static, not stack: the probe is single-inflight (asserted below), so these
 * are private to the one op in flight; a 4 KiB local on a kernel thread's
 * stack is a canary waiting to be smashed, and the L78 P2 window died with
 * `KERNEL PANIC: stack smashing detected` at t=244 s. */
static uint8_t stress_buf[4096];
static uint8_t stress_back[512];
static uint8_t stress_fat[512];

static void stress_one_op(void) {
  uint8_t *buf = stress_buf, *back = stress_back;
  uint64_t wrote_lba = 0;
  int wrote_ok = 0;
  uint64_t tail = (nvme_id.nsze > 65536) ? nvme_id.nsze - 16384 : 0;
  if (!nvme_regs || !nvme_id.valid || !tail) {
    stress_skipped++;
    return;
  }
  /* Bound 1 of 2: the probe never has two operations outstanding. */
  if (stress_inflight) {
    stress_skipped++;
    return;
  }
  /* Bound 2: take the token with a deadline; a busy device is a skip. */
  if (!blk_token_try_acquire(BKS_TOKEN_WAIT_MS)) {
    stress_busy++;
    return;
  }
  stress_inflight = 1;
  stress_ops++;
  uint64_t t0 = timer_get_ms();
  if (!nvme_ready_now()) {
    /* The game's own page-in path is repairing the controller; leave the
       token immediately rather than forcing a second re-initialisation. */
    stress_skipped++;
  } else if ((stress_ops % BKS_WV_EVERY) == 0) {
    /* Write-verify: one sector, only in an unallocated cluster. */
    uint64_t lba = tail + (stress_rnd() % 8192);
    uint64_t fat_sec = 0, cluster = 0;
    uint32_t off = 0;
    uint8_t *fs = stress_fat;
    int free_ok = 0;
    if (fat16_lba_scratch_geom(lba, &fat_sec, &off, &cluster) &&
        stress_read_locked(fat_sec, fs, 1) == 0) {
      uint16_t entry = (uint16_t)(fs[off] | ((uint16_t)fs[off + 1] << 8));
      free_ok = (entry == 0x0000);
    }
    if (!free_ok) {
      stress_skipped++; /* allocated (or unreadable): never write there */
    } else if (stress_read_locked(lba, buf, 1) != 0) {
      stress_bad++;
      if (stress_bad_rows++ < BKS_FAIL_ROWS) {
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[BKS] read failed lba=");
        kl_hex(&kl, (unsigned long)lba);
        kl_s(&kl, " bad=");
        kl_i(&kl, (long)stress_bad);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    } else if (stress_write_locked(lba, buf) != 0) {
      stress_bad++;
      if (stress_bad_rows++ < BKS_FAIL_ROWS) {
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[BKS] write failed lba=");
        kl_hex(&kl, (unsigned long)lba);
        kl_s(&kl, " bad=");
        kl_i(&kl, (long)stress_bad);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    } else {
      /* The verify read stays raw (no cache), so it can run under the same
         token; the cache invalidation must NOT: blk_cache_invalidate takes
         fill_lock and bucket locks, and a cache fill owns those before it
         ever asks for the device token -- taking them while holding the token
         is the lock order that wedged the census (measured: [STALLBREAK]
         on a wr_lock after a 15 s token hold).  Do it after the release. */
      wrote_lba = lba;
      wrote_ok = 1;
      if (stress_read_locked(lba, back, 1) != 0) {
        stress_bad++;
      } else {
        int same = 1;
        for (int k = 0; k < 512; k++) {
          if (buf[k] != back[k]) {
            same = 0;
            break;
          }
        }
        if (same) {
          stress_writes++;
        } else {
          stress_mismatch++;
          if (stress_mismatch_rows++ < BKS_FAIL_ROWS) {
            /* One row, then continue: a mismatch is data for the report, not
               a reason to retry or to stop the probe. */
            struct kline kl;
            kl_init(&kl);
            kl_s(&kl, "[BKS] verify MISMATCH lba=");
            kl_hex(&kl, (unsigned long)lba);
            kl_s(&kl, " n=");
            kl_i(&kl, (long)stress_mismatch);
            kl_s(&kl, " (logged, continuing)\n");
            kl_emit(&kl);
          }
        }
      }
    }
  } else {
    /* Random 512..4096-byte read over the same free-tail window. */
    uint64_t lba = tail + (stress_rnd() % 8192);
    uint32_t nsec = 1u << (stress_rnd() % 4);
    if (lba + nsec > nvme_id.nsze) {
      stress_skipped++;
    } else if (stress_read_locked(lba, buf, nsec) != 0) {
      stress_bad++;
      if (stress_bad_rows++ < BKS_FAIL_ROWS) {
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[BKS] read failed lba=");
        kl_hex(&kl, (unsigned long)lba);
        kl_s(&kl, " nsec=");
        kl_i(&kl, (long)nsec);
        kl_s(&kl, " bad=");
        kl_i(&kl, (long)stress_bad);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    } else {
      stress_reads++;
    }
  }
  uint64_t dt = timer_get_ms() - t0;
  stress_inflight = 0;
  blk_token_release();
  if (wrote_ok)
    blk_cache_invalidate(wrote_lba, 1);  /* token not held: see above */
  if (dt > BKS_SLOW_MS) {
    /* A single slow op is a row; the caller backs off for a few rounds so a
       recovering device is not hammered while it re-initialises. */
    stress_slow++;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[BKS] slow op_ms=");
    kl_i(&kl, (long)dt);
    kl_s(&kl, " slow=");
    kl_i(&kl, (long)stress_slow);
    kl_s(&kl, " (backing off)\n");
    kl_emit(&kl);
    stress_delay_ms(BKS_BACKOFF_MS);
  }
}

/* Its own kernel thread, started by the driver, mostly asleep. */
static void blk_stress_thread(void *arg) {
  (void)arg;
  /* Let boot settle, then announce the shape so a run proves the probe that
     ran is this one (the row names every bound). */
  stress_delay_ms(BKS_ROW_MS);
  {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[BKS] start ops_per_s=");
    kl_i(&kl, 1000 / BKS_OP_MS);
    kl_s(&kl, " token_wait_ms=");
    kl_i(&kl, BKS_TOKEN_WAIT_MS);
    kl_s(&kl, " max_inflight=1 wv_every=");
    kl_i(&kl, BKS_WV_EVERY);
    kl_s(&kl, " window=");
    kl_i(&kl, 8192);
    kl_s(&kl, " tail_off=");
    kl_i(&kl, 16384);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  uint64_t last_row = timer_get_ms();
  for (;;) {
    stress_one_op();
    uint64_t now = timer_get_ms();
    if (now - last_row >= BKS_ROW_MS) {
      last_row = now;
      stress_row("alive", now);
    }
    /* The rate limit: asleep in halt for 250 ms, four ops a second. */
    stress_delay_ms(BKS_OP_MS);
  }
}

void virtio_blk_stress_start(void) {
  extern int process_create_kernel(void (*entry)(void *), void *arg);
  process_create_kernel(blk_stress_thread, 0);
}
#else
void virtio_blk_stress_tick(void) {
}

void virtio_blk_stress_start(void) {
}
#endif

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
  /* L75: the frame pool's own state, on the same 5 s timeline as the device.
     used= crossing 327680 frames was when the old pool reached the PCI hole
     (0x80000000); hole= must stay 0 and mmio= counts state changes the
     driver did not make. */
  uart_puts(" fr_used=");
  print_int(frame_used_count());
  uart_puts(" fr_high=");
  print_int(frame_high_water());
  uart_puts(" hole=");
  print_int(frame_hole_allocs());
  uart_puts(" mmio=");
  print_int((int)blk_mmio_changes);
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

  /* L75: the 2 MiB window at VA 0x1FC000000 shadows the identity mapping of
     the same physical 2 MiB, so a frame allocated there would BE this
     device.  Reserve the shadow range in the frame pool, and say out loud
     whether the pool's own extent still covers the BAR (it must not: that is
     the L65-L73 bug, where a page-cache frame was the controller's register
     file). */
  {
    extern void frame_reserve_range(uint64_t lo, uint64_t hi);
    extern int frame_hole_allocs(void);
    uint64_t win = nvme_regs_phys & 0xFFFFFFFFFFE00000ULL;
    frame_reserve_range(0x1FC000000ULL, 0x1FE000000ULL);
    uart_puts("[BLKQ] mmio win=");
    uart_print_hex(win);
    uart_puts("-");
    uart_print_hex(win + 0x200000);
    uart_puts(" bar_in_pool=");
    /* A BAR inside the frame pool is the whole bug; the extents put the pool
       at 0x20000000-0x70000000 and 0x100000000-0x200000000 with the MMIO
       shadow reserved, so this must read 0. */
    print_int((win >= FRAME_EXT0_BASE && win < FRAME_EXT0_TOP) ||
              (win >= FRAME_EXT1_BASE && win < FRAME_EXT1_TOP) ? 1 : 0);
    /* And the physical page really is the controller: read CAP through the
       identity map of the window's own physical address and compare with the
       register file.  Equal means "this RAM frame would have been the NVMe
       BAR", which is what the old pool (extent 1 = 0x80000000..) handed out. */
    uart_puts(" alias_read=");
    uart_print_hex(*(volatile uint32_t *)win);
    uart_puts(" cap=");
    uart_print_hex(nvme_regs[0]);
    uart_puts(" hole_allocs=");
    print_int(frame_hole_allocs());
    uart_puts("\n");
  }

#ifdef RW_ALIAS_PROBE
  /* L75 diagnostic (never in a shipped build): if the frame pool still
     covers the BAR's physical page, a plain 4 KiB memory write into the
     frame the allocator would hand out IS a write to AQA/ASQ/ACQ/CC.  This
     is the direct reproducer for the L65-L73 resets: no MMIO instruction,
     just a page write. */
  {
    uint64_t awin = nvme_regs_phys & 0xFFFFFFFFFFE00000ULL;
    uart_puts("[BLKQ] alias-probe pa=");
    uart_print_hex(awin);
    uart_puts(" cc_before=");
    uart_print_hex(nvme_regs[5]);
    uart_puts(" csts_before=");
    uart_print_hex(nvme_regs[7]);
    uart_puts("\n");
    /* 32-bit stores, exactly what a page-cache memcpy ends up doing. */
    for (int i = 0; i < 1024; i++)
      ((volatile uint32_t *)awin)[i] = 0;
    uart_puts("[BLKQ] alias-probe wrote 4 KiB of zeros (32-bit stores) through the pool frame; cc_after=");
    uart_print_hex(nvme_regs[5]);
    uart_puts(" csts_after=");
    uart_print_hex(nvme_regs[7]);
    uart_puts(" pa_cc_read=");
    uart_print_hex(*(volatile uint32_t *)(awin + 0x14));
    uart_puts(" pa_cap_read=");
    uart_print_hex(*(volatile uint32_t *)awin);
    uart_puts(" bar_cc_read=");
    uart_print_hex(nvme_regs[0]);
    uart_puts("\n");
  }
#endif

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

  // Configure CC: EN=1, CSS=0 (NVM), MPS=CAP.MPSMIN, AMS=0,
  // IOSQES=6 (64-byte commands, bits 19:16), IOCQES=4 (16-byte
  // completions, bits 23:20).  L75: the fields are derived from CAP here and
  // re-derived by nvme_admin_enable() on every re-initialisation, so the boot
  // path and the recovery path cannot encode them differently.
  nvme_read_cap();
  {
    uint32_t cc_boot = 1u | ((nvme_id.mpsmin & 0x7) << 7) |
                       (6u << 16) | (4u << 20);
    nvme_regs[5] = cc_boot;
    (void)nvme_regs[5]; // Flush write
  }

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

  /* L75: the namespace geometry is the controller's answer, not a constant.
     Record it once at boot (and again on every re-initialisation) so a read
     past the end of the namespace is a driver bug, not a device surprise. */
  if (nvme_ns_validate(1) != 0) {
    uart_puts("NVMe namespace validation failed!\n");
    return -1;
  }

  uart_puts("NVMe driver initialized successfully!\n");
  virtio_blk_stress_start();
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

/* L78: bounded token wait (see the prototype).  The first ~2 ms poll with a
 * pause; after that the caller is a kernel thread and sleeps in 1 ms slices,
 * so a probe waiting behind the game's loader costs no CPU and is always
 * preemptible.  Never call this with the token already held. */
static int blk_token_try_acquire(uint32_t timeout_ms) {
  uint64_t t0 = timer_get_ms();
  uint32_t spins = 0;
  for (;;) {
    uint64_t flags = spinlock_acquire_irqsave(&blk_request_lock);
    if (!blk_in_use) {
      blk_in_use = 1;
      spinlock_release_irqrestore(&blk_request_lock, flags);
      break;
    }
    spinlock_release_irqrestore(&blk_request_lock, flags);
    if (timer_get_ms() - t0 >= timeout_ms)
      return 0;
    if (spins++ < 200)
      __asm__ volatile("pause");
    else
      safe_wfi(); /* bounded: the deadline check above ends the wait */
  }
  if (__atomic_add_fetch(&blk_inside, 1, __ATOMIC_SEQ_CST) > 1) {
    uart_puts("[BLKRACE] two drivers inside the queue, inside=");
    print_int((int)blk_inside);
    uart_puts("\n");
  }
  return 1;
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

/* L75: the MMIO state-change watcher and the re-init self-check are x86-only
 * (there is no NVMe controller here). */
void virtio_blk_watch_mmio(void) {
}

void virtio_blk_selfcheck(void) {
}

void virtio_blk_stress_tick(void) {
}

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
