#ifndef NV_H
#define NV_H

/* ============================================================================
 *  nv: HobbyOS native NVIDIA GPU driver -- shared kernel/userland interface.
 *
 *  Written for x86_64 (a GPU needs PCIe, MSI-X and a 64-bit BAR window; the
 *  aarch64 port has none of those).  On ARM every function here compiles to
 *  an empty TU, exactly like the virtio-gpu 3D and VT-d drivers.
 *
 *  Layering (see docs/nvidia-driver.md):
 *      pci.c        generic PCIe: ECAM/legacy config, BAR sizing, MSI-X
 *      nv.c         this driver: identify, PMC/PBUS/PTIMER, falcon discovery
 *      nv_falcon.c  falcon engine: memory windows, DMA ucode load, start/halt
 *      nv_gsp.c     GSP-RM firmware boot (the rung that unlocks 3D)
 *
 *  Everything that touches hardware goes through struct nv_mmio_ops, so the
 *  same code runs against a fake register file in the kernel unit tier.  That
 *  is the only way this driver gets coverage before the card is available.
 * ========================================================================== */

#ifdef __x86_64__

#include <stdint.h>

/* Bring-up state, in the order the driver reaches it.  A failed stage names
 * itself: nv.state is left at the last stage that completed. */
enum nv_state {
  NV_STATE_ABSENT = 0, /* no NVIDIA display-class device on the bus */
  NV_STATE_PCI = 1,    /* config space found, BARs sized */
  NV_STATE_BAR = 2,    /* BAR0 (and BAR1 window) mapped */
  NV_STATE_ID = 3,     /* PMC_BOOT_0 read and decoded */
  NV_STATE_PMC = 4,    /* PMC engine enables checked (PTIMER live) */
  NV_STATE_TIMER = 5,  /* PTIMER counts: register space is not a dead window */
  NV_STATE_FALCON = 6, /* at least one falcon decoded (PMU/GSP/SEC2) */
  NV_STATE_FW = 7,     /* firmware blobs located */
  NV_STATE_GSP = 8,    /* GSP boot attempted (status in nv.gsp_stage) */
  NV_STATE_READY = 9,  /* GSP-RM answering: the driver can allocate/submit */
};

/* One falcon engine (PMU, GSP, SEC2). */
struct nv_falcon {
  uint32_t base;     /* BAR0 offset of the 4 KiB IO window */
  uint32_t caps;     /* UC_CAPS raw */
  uint32_t caps2;    /* UC_CAPS2 raw */
  uint32_t ctl;      /* UC_CTRL raw */
  uint32_t busy;     /* STATUS_BUSY raw */
  int version;       /* UC_CAPS2.VERSION, 0 when the window refused to decode */
  int present;
  const char *name;
};

/* MMIO backend.  The real one is BAR0; the unit test supplies a fake. */
struct nv_mmio_ops {
  uint32_t (*rd32)(void *ctx, uint32_t off);
  void (*wr32)(void *ctx, uint32_t off, uint32_t val);
};

struct nv_dev {
  /* identity */
  uint16_t vendor, device;
  uint16_t subsystem_vendor, subsystem_device;
  uint8_t bus, slot, func, revision;
  uint8_t gsp_required; /* chipset >= nv162: no useful GPU work without GSP */
  uint32_t chipset;     /* PMC_BOOT_0 bits 20-28 (9 bits from Turing) */
  const char *chip_name;
  uint32_t boot0, boot1;
  uint32_t pmc_enable;

  /* address space */
  uint64_t bar0_phys, bar0_size;
  uint64_t bar1_phys, bar1_size; /* VRAM aperture (256 MiB or the ReBAR size) */
  uint64_t bar3_phys, bar3_size;
  volatile uint8_t *bar0; /* mapped BAR0 (ioremap) */
  uint64_t bar1_window_phys; /* BAR1 window this driver mapped, 0 = none */
  uint64_t bar1_window_size;
  uint8_t *bar1_window; /* uncached kernel pointer into the VRAM aperture */

  uint32_t irq_vector; /* 0 = no MSI-X vector allocated */
  int state;
  int failed_stage;
  int gsp_stage;

  /* subengines */
  struct nv_falcon falcon[3]; /* PMU, GSP, SEC2 */
  uint32_t falcon_mask;       /* bit per present falcon */

  struct nv_mmio_ops ops;
  void *ops_ctx;
};

#define NV_FALCON_IDX_PMU 0
#define NV_FALCON_IDX_GSP 1
#define NV_FALCON_IDX_SEC2 2

/* Forward declaration: the ABI structs are defined at the bottom of this
 * header (they are the userland-visible half), but the kernel API above that
 * block takes a struct nv_info * so the ioctl snapshot can be produced in
 * one place. */
struct nv_info;

/* ---- kernel API --------------------------------------------------------- */

/* Boot-time entry: find the card, map it, run the staged bring-up, log a row
 * per stage.  Returns 0 when the driver reached NV_STATE_FALCON or better,
 * a negative errno otherwise (a machine with no NVIDIA card returns
 * -ENODEV and logs nothing beyond one [NV] row). */
int nv_init(void);

int nv_present(void);
struct nv_dev *nv_dev(void);
const char *nv_state_name(int state);

uint32_t nv_rd32(struct nv_dev *d, uint32_t off);
void nv_wr32(struct nv_dev *d, uint32_t off, uint32_t val);

/* Staged pieces, each independently testable (they take a fully populated
 * struct nv_dev and only use d->ops). */
int nv_identify(struct nv_dev *d);
int nv_pmc_enable_timer(struct nv_dev *d);
int nv_timer_read(struct nv_dev *d, uint64_t *out);
int nv_timer_probe(struct nv_dev *d, uint32_t *hz_out);
int nv_falcon_probe_all(struct nv_dev *d);
int nv_bringup(struct nv_dev *d); /* identify -> pmc -> timer -> falcons */
void nv_log_state(struct nv_dev *d);
void nv_mark_probed(void); /* nv_pci.c: attach finished */
void nv_fill_info(struct nv_dev *d, struct nv_info *out); /* ABI snapshot */
uint32_t nv_irq_count(void);
/* read(2) on /dev/nvctl: copy a struct nv_info snapshot out (short read when
 * len is smaller).  Never blocks, never partial across reads: one record. */
int nv_read_info(void *buf, int len);

/* Kernel unit tier (nv_test.c). */
void nv_test_suite(void);

/* Falcon engine helpers (nv_falcon.c). */
int nv_falcon_dmem_write(struct nv_dev *d, const struct nv_falcon *f,
                         uint32_t addr, const uint32_t *words, int n);
int nv_falcon_dmem_read(struct nv_dev *d, const struct nv_falcon *f,
                        uint32_t addr, uint32_t *words, int n);
int nv_falcon_imem_write(struct nv_dev *d, const struct nv_falcon *f,
                         uint32_t addr, const uint32_t *words, int n);
int nv_falcon_imem_read(struct nv_dev *d, const struct nv_falcon *f,
                        uint32_t addr, uint32_t *words, int n);
int nv_falcon_wait_idle(struct nv_dev *d, const struct nv_falcon *f, int ms);
int nv_falcon_halt(struct nv_dev *d, const struct nv_falcon *f);
void nv_falcon_start(struct nv_dev *d, const struct nv_falcon *f,
                     uint32_t entry);
int nv_falcon_xfer(struct nv_dev *d, const struct nv_falcon *f, int is_code,
                   uint32_t ext_base_hi, uint32_t ext_offset,
                   uint32_t local_addr, int size_log2, int port);
int nv_falcon_reset_engines(struct nv_dev *d, const struct nv_falcon *f);

/* Round-trip self-test over DMEM + scratch registers.  Destructive to a
 * running ucode, so it is never called during boot: the userland tool asks
 * for it through NV_IOC_SELFTEST on a card the driver owns. */
int nv_selftest(struct nv_dev *d, uint32_t which, uint32_t *detail);

/* ---- /dev/nvctl ioctl ABI ----------------------------------------------
 *
 * Uses the Linux _IOC encoding like /dev/gpu (docs/gpu-abi.md), reached
 * through the compat ioctl route in linux_compat_fd.c.  Numbers below 0x40
 * so the request fits a 32-bit unsigned long on any consumer. */
#define NV_ABI_VERSION 1
#define NV_FALCON_NAME_LEN 8

/* magic 'n': dir(2 bits) | size(14) | type(8) | nr(8), the Linux layout
 * (_IOC_WRITE == 1, _IOC_READ == 2).  Requests are compared by their full
 * 32-bit number, so the size field only matters for the dispatcher's
 * sanity check. */
#define NV_IOC(dir, nr, size)                                                  \
  (((unsigned long)(dir) << 30) | ((unsigned long)(size) << 16) |               \
   ((unsigned long)('n') << 8) | (unsigned long)(nr))

#define NV_IOC_INFO NV_IOC(2, 0x01, sizeof(struct nv_info))
#define NV_IOC_REG_READ NV_IOC(3, 0x02, sizeof(struct nv_reg_req))
#define NV_IOC_SELFTEST NV_IOC(3, 0x03, sizeof(struct nv_selftest_req))

struct nv_info {
  uint32_t abi; /* NV_ABI_VERSION */
  uint32_t state;
  uint32_t failed_stage;
  uint16_t device_id;
  uint16_t subsystem_device;
  uint16_t subsystem_vendor;
  uint8_t bus, slot, func, revision;
  uint8_t header[4]; /* reserved, zero */
  uint32_t chipset;
  char name[NV_FALCON_NAME_LEN];
  uint32_t boot0;
  uint32_t boot1;
  uint32_t pmc_enable;
  uint32_t gsp_required;
  uint32_t irq_vector;
  uint64_t bar0_phys;
  uint64_t bar0_size;
  uint64_t bar1_phys;
  uint64_t bar1_size;
  uint64_t bar3_phys;
  uint64_t bar3_size;
  uint32_t falcon_mask;
  uint32_t falcon_caps[3];
  uint32_t falcon_caps2[3];
  uint32_t falcon_ctl[3];
  uint32_t falcon_busy[3];
  uint32_t timer_hz;   /* measured PTIMER ticks per second (0 = unmeasured) */
  uint32_t gsp_stage;
};

struct nv_reg_req {
  uint32_t off; /* BAR0 byte offset */
  uint32_t val; /* out: register value */
};

struct nv_selftest_req {
  uint32_t which;  /* 0 = auto (first present falcon), 1 = PMU, 2 = GSP, 3 = SEC2 */
  uint32_t result; /* 0 = pass, negative errno otherwise */
  uint32_t detail[6];
};

/* Request sizes for the dispatcher; -1 means "not an nv request". */
int nv_ioc_size(unsigned long req);

/* The ioctl entry the compat fd layer calls (negative errno on failure). */
struct process;
int nv_ioctl(struct process *p, int fd, unsigned long req, uint64_t arg);
int file_is_devnv(struct process *p, int fd);

#endif /* __x86_64__ */

#endif /* NV_H */
