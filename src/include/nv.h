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
  int fenced;        /* window answered 0xBADF5620: real, but not ours until a
                      * driver initialises it (docs/nv-gsp-reference.md 3.1) */
  const char *name;
};

/* MMIO backend.  The real one is BAR0; the unit test supplies a fake. */
struct nv_mmio_ops {
  uint32_t (*rd32)(void *ctx, uint32_t off);
  void (*wr32)(void *ctx, uint32_t off, uint32_t val);
};

struct nv_dev {
  int hardware; /* 1 when attached to a real card (nv_pci.c), 0 in unit tests */
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
  uint32_t timer_hz;   /* measured PTIMER ticks/s (0 = not measured) */
  /* GSP firmware stage observations (nv_gsp.c).  Read-only facts the card
   * reports about its own firmware state; they are in nv_info so a userspace
   * tool can see them without a second ioctl. */
  uint32_t wpr2_lo, wpr2_hi; /* legacy WPR2 pair: measured NOT to move */
  uint32_t wpr_ctl_a, wpr_ctl_b; /* PFB 0x1FA804/08: the pair that moves */
  uint32_t gsp_msg0, gsp_msg1; /* falcon mailbox pair (boot handshake) */
  uint32_t gsp_ucctl;          /* RISC-V core status word */
  int state;
  int failed_stage;
  int gsp_stage;

  /* subengines */
  struct nv_falcon falcon[3]; /* PMU, GSP, SEC2 */
  uint32_t falcon_mask;       /* bit per present falcon */

  struct nv_mmio_ops ops;
  void *ops_ctx;
  /* Boot-path addresses that must stay valid across stages. */
  uint64_t booter_phys;   /* the booter blob's contiguous frames */
  uint32_t booter_size;
  uint64_t gsp_meta_phys; /* the 256-byte metadata block, DMA-able */
  uint64_t radix3_root;   /* the RM image's radix3 tree */
  uint32_t radix3_pages;
  /* FWSEC ucode as located in the card's VBIOS (nv_vbios_probe). */
  uint32_t ucode_desc, ucode_payload, ucode_size;
  uint32_t ucode_imem, ucode_dmem, ucode_engid, ucode_id;
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

/* GSP firmware stage (nv_gsp.c): probe is read-only and safe; boot() is the
 * staged machine and stops at the first unverified contract with -ENOTSUP. */
int nv_gsp_probe(struct nv_dev *d);
/* Read-only VBIOS probe: finds the PCI ROM image and the falcon ucode table in
 * it (docs/nv-pmu-acr-contract.md section 1).  No writes to the card beyond the
 * PROM-shadow toggle, which is restored. */
int nv_vbios_probe(struct nv_dev *d);
/* Copy `len` bytes out of the VBIOS PROM aperture into `dst`.  Byte-wise over
 * the dword path, because that is the only way the aperture is readable here. */
int nv_vbios_read(struct nv_dev *d, uint32_t off, void *dst, uint32_t len);
int nv_gsp_boot(struct nv_dev *d);
/* FWSEC step 1-3: reset the GSP falcon, then route its DMA at system memory.
 * Needs no ucode, so it is verifiable on its own (docs/nv-pmu-acr-contract.md). */
int nv_gsp_fwsec_reset(struct nv_dev *d);
/* Read + parse + hash the staged firmware.  Pure file I/O, zero MMIO writes, so
 * it is safe to run on every boot. */
int nv_gsp_firmware_check(struct nv_dev *d);

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
#define NV_ABI_VERSION 2
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
#define NV_IOC_PEEK NV_IOC(3, 0x04, sizeof(struct nv_peek_req))

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
  uint32_t wpr2_lo, wpr2_hi;  /* legacy WPR2 pair (does not move) */
  uint32_t wpr_ctl_a, wpr_ctl_b; /* PFB 0x1FA804/08, the pair that moves */
  uint32_t gsp_msg0, gsp_msg1; /* GSP falcon mailbox pair */
  uint32_t gsp_ucctl;          /* GSP RISC-V status word */
  /* Boot-path addresses that must stay valid across stages. */
  uint64_t booter_phys;  /* the booter blob's contiguous frames */
  uint32_t booter_size;
  uint64_t gsp_meta_phys; /* the 256-byte metadata block, DMA-able */
  uint64_t radix3_root;   /* the RM image's radix3 tree */
  uint32_t radix3_pages;
};

struct nv_reg_req {
  uint32_t off; /* BAR0 byte offset */
  uint32_t val; /* out: register value */
};

/* Batch register read for bring-up work: one ioctl, up to 256 words, inside
 * the driver's whitelist (see nv_reg_allowed).  A per-word ioctl cannot dump a
 * 4 KiB engine window without 1024 syscalls, and bring-up is exactly the job of
 * comparing windows of registers against a reference. */
#define NV_PEEK_MAX 256
struct nv_peek_req {
  uint32_t first;  /* BAR0 offset of the first word (4-byte aligned) */
  uint32_t count;  /* words to read, 1..NV_PEEK_MAX */
  uint32_t result; /* out: 0 = ok, negative errno */
  uint32_t val[NV_PEEK_MAX];
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
