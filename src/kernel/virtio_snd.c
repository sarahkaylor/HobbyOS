/* virtio_snd.c — virtio-sound PCI driver (virtio 1.2 §5.14), x86_64.
 * Lane C4-audio (docs/cp-audio.md, AD-9).
 *
 * Transport: the modern virtio-pci transport, exactly as virtio_gpu_x64.c
 * does it (capability walk, ioremap'd BAR, VERSION_1 only, split queues, no
 * MSI-X vector).  QEMU's virtio-sound-pci is 1af4:1059 (device id 25).
 *
 * Queues: 0 control (polled: the avail ring says NO_INTERRUPT and the driver
 * spins on the used index; process context only), 2 TX (completions by
 * INTx).  The event queue (1) and RX queue (3) are left disabled: QEMU sends
 * no PCM_PERIOD_ELAPSED events (a TX buffer's return IS the period event) and
 * there is no capture stream.
 *
 * TX message = 3 descriptors: virtio_snd_pcm_xfer (stream id, readable), the
 * PCM bytes (readable, the ALSA layer's ring buffer), virtio_snd_pcm_status
 * (writable).  QEMU copies the PCM bytes into its own buffer when it pops
 * the message, and returns the message once the audio backend has consumed
 * them; the ALSA layer counts the returned frames as hw_ptr progress.
 *
 * The device's INTx: line from PCI config 0x3C, vector 32+line on the legacy
 * PIC (the same model as the Venus GPU's fence IRQ).  The line is put in
 * level mode in the ELCR (a PCI INTx is level; q35 shares lines). */
#include "virtio_snd.h"
#include "errno.h"

#ifdef __x86_64__

#include "lock.h"
#include "arch/cpu.h"
#include "kmem.h"

extern void uart_puts(const char *s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t val);
extern void gic_enable_interrupt(uint32_t intid);

static inline void vs_outl(uint16_t port, uint32_t val) {
  __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t vs_inl(uint16_t port) {
  uint32_t r;
  __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port));
  return r;
}
static inline void vs_outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t vs_inb(uint16_t port) {
  uint8_t r;
  __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
  return r;
}

static uint32_t vs_pci_r(uint8_t bus, uint8_t slot, uint8_t off) {
  vs_outl(0xCF8, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | (off & 0xFC));
  return vs_inl(0xCFC);
}
static void vs_pci_w(uint8_t bus, uint8_t slot, uint8_t off, uint32_t v) {
  vs_outl(0xCF8, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | (off & 0xFC));
  vs_outl(0xCFC, v);
}

#define VSND_PCI_VENDOR 0x1AF4
#define VSND_PCI_DEVICE 0x1059 /* 0x1040 + 25 */

#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4

#define CC_DEV_FEAT_SEL 0x00u
#define CC_DEV_FEAT     0x04u
#define CC_DRV_FEAT_SEL 0x08u
#define CC_DRV_FEAT     0x0Cu
#define CC_STATUS       0x14u
#define CC_Q_SEL        0x16u
#define CC_Q_SIZE       0x18u
#define CC_Q_MSIX       0x1Au
#define CC_Q_ENABLE     0x1Cu
#define CC_Q_NOFF       0x1Eu
#define CC_Q_DESC       0x20u
#define CC_Q_DRIVER     0x28u
#define CC_Q_DEVICE     0x30u

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8

#define D_NEXT 1
#define D_WRITE 2

#define QMAX 192         /* descriptors per queue we use at most */
#define TX_SLOTS_MAX 64  /* 3 descriptors each */
#define CTL_SPIN_LIMIT 200000000ull

struct vs_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed));
struct vs_used_elem {
  uint32_t id;
  uint32_t len;
} __attribute__((packed));
struct vs_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[QMAX];
  uint16_t used_event;
} __attribute__((packed));
struct vs_used {
  uint16_t flags;
  uint16_t idx;
  struct vs_used_elem ring[QMAX];
  uint16_t avail_event;
} __attribute__((packed));

struct vs_queue {
  struct vs_desc desc[QMAX] __attribute__((aligned(16)));
  struct vs_avail avail __attribute__((aligned(16)));
  struct vs_used used __attribute__((aligned(16)));
  uint16_t size, notify_off, avail_next, used_ack;
};

static struct vs_queue vs_ctlq __attribute__((aligned(4096)));
static struct vs_queue vs_txq __attribute__((aligned(4096)));

/* Control request/response buffers (DMA-stable, under vs_ctl_lock). */
static uint8_t vs_ctl_req[64] __attribute__((aligned(64)));
static uint8_t vs_ctl_resp[4 + 8 * 32] __attribute__((aligned(64)));

/* Per-TX-slot header and status. */
static struct {
  uint32_t stream_id;
} __attribute__((packed)) vs_tx_hdr[TX_SLOTS_MAX] __attribute__((aligned(64)));
static struct {
  uint32_t status;
  uint32_t latency_bytes;
} __attribute__((packed)) vs_tx_st[TX_SLOTS_MAX] __attribute__((aligned(64)));
static uint8_t vs_tx_busy[TX_SLOTS_MAX];
static int vs_tx_nslots, vs_tx_inflight_n;

static volatile uint8_t *vs_cc, *vs_notify, *vs_isr, *vs_devcfg;
static uint32_t vs_notify_mult = 4;
static int vs_ready;
static spinlock_t vs_ctl_lock;
static struct vsnd_stream_caps vs_caps;
static struct vsnd_stats vs_st;
static uint8_t vs_bus, vs_slot;
int virtio_snd_irq_vector = -1;

static inline uint32_t cc_r32(uint32_t o) { return *(volatile uint32_t *)(vs_cc + o); }
static inline void cc_w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(vs_cc + o) = v; }
static inline uint16_t cc_r16(uint32_t o) { return *(volatile uint16_t *)(vs_cc + o); }
static inline void cc_w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(vs_cc + o) = v; }
static inline uint8_t cc_r8(uint32_t o) { return *(volatile uint8_t *)(vs_cc + o); }
static inline void cc_w8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(vs_cc + o) = v; }
static inline void cc_w64(uint32_t o, uint64_t v) {
  cc_w32(o, (uint32_t)v);
  cc_w32(o + 4, (uint32_t)(v >> 32));
}

static void vs_kick(struct vs_queue *q, uint16_t qid) {
  arch_memory_barrier();
  *(volatile uint16_t *)(vs_notify + (uint64_t)q->notify_off * vs_notify_mult) = qid;
}

static uint64_t vs_bar_phys(uint8_t bar) {
  if (bar > 5) return 0;
  uint32_t lo = vs_pci_r(vs_bus, vs_slot, (uint8_t)(0x10 + bar * 4));
  uint64_t base = lo & 0xFFFFFFF0u;
  if ((lo & 0x6) == 0x4 && bar < 5)
    base |= (uint64_t)vs_pci_r(vs_bus, vs_slot, (uint8_t)(0x10 + (bar + 1) * 4)) << 32;
  return base;
}

static int vs_queue_setup(struct vs_queue *q, uint16_t qid, int no_irq) {
  cc_w16(CC_Q_SEL, qid);
  uint16_t max = cc_r16(CC_Q_SIZE);
  if (max == 0) return -1;
  uint16_t sz = max < QMAX ? max : QMAX;
  cc_w16(CC_Q_SIZE, sz);
  cc_w16(CC_Q_MSIX, 0xFFFF);
  q->size = sz;
  q->avail_next = 0;
  q->used_ack = 0;
  q->avail.flags = no_irq ? 1 : 0;
  q->avail.idx = 0;
  q->used.idx = 0;
  cc_w64(CC_Q_DESC, kimg_pa(q->desc));
  cc_w64(CC_Q_DRIVER, kimg_pa(&q->avail));
  cc_w64(CC_Q_DEVICE, kimg_pa(&q->used));
  cc_w16(CC_Q_ENABLE, 1);
  q->notify_off = cc_r16(CC_Q_NOFF);
  return 0;
}

/* One synchronous control command: req (readable) -> resp (writable).
 * Caller holds vs_ctl_lock. */
static int vs_ctl_xfer(uint32_t req_len, uint32_t resp_len, uint32_t *status) {
  struct vs_queue *q = &vs_ctlq;
  q->desc[0].addr = kimg_pa(vs_ctl_req);
  q->desc[0].len = req_len;
  q->desc[0].flags = D_NEXT;
  q->desc[0].next = 1;
  q->desc[1].addr = kimg_pa(vs_ctl_resp);
  q->desc[1].len = resp_len;
  q->desc[1].flags = D_WRITE;
  q->desc[1].next = 0;
  *(volatile uint32_t *)vs_ctl_resp = 0;
  q->avail.ring[q->avail_next % q->size] = 0;
  arch_memory_barrier();
  q->avail.idx = (uint16_t)(q->avail_next + 1);
  q->avail_next++;
  vs_kick(q, 0);
  vs_st.ctl_cmds++;
  uint64_t spins = 0;
  while (*(volatile uint16_t *)&q->used.idx == q->used_ack) {
    __asm__ volatile("pause");
    if (++spins >= CTL_SPIN_LIMIT) {
      vs_st.ctl_timeouts++;
      if (status) *status = 0;
      return -EIO;
    }
  }
  arch_memory_barrier();
  q->used_ack = *(volatile uint16_t *)&q->used.idx;
  uint32_t st = *(volatile uint32_t *)vs_ctl_resp;
  if (status) *status = st;
  return st == VSND_S_OK ? 0 : -EIO;
}

static void vs_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static uint32_t vs_get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t vs_get64(const uint8_t *p) {
  return (uint64_t)vs_get32(p) | ((uint64_t)vs_get32(p + 4) << 32);
}

int virtio_snd_set_params(uint32_t buffer_bytes, uint32_t period_bytes, uint8_t channels,
                          uint8_t format, uint8_t rate, uint32_t *status) {
  if (!vs_ready) return -ENODEV;
  uint64_t fl = spinlock_acquire_irqsave(&vs_ctl_lock);
  for (int i = 0; i < 24; i++) vs_ctl_req[i] = 0;
  vs_put32(vs_ctl_req + 0, VSND_R_PCM_SET_PARAMS);
  vs_put32(vs_ctl_req + 4, vs_caps.stream_id);
  vs_put32(vs_ctl_req + 8, buffer_bytes);
  vs_put32(vs_ctl_req + 12, period_bytes);
  vs_put32(vs_ctl_req + 16, 0); /* features */
  vs_ctl_req[20] = channels;
  vs_ctl_req[21] = format;
  vs_ctl_req[22] = rate;
  int r = vs_ctl_xfer(24, 4, status);
  spinlock_release_irqrestore(&vs_ctl_lock, fl);
  return r;
}

int virtio_snd_pcm_cmd(uint32_t code, uint32_t *status) {
  if (!vs_ready) return -ENODEV;
  uint64_t fl = spinlock_acquire_irqsave(&vs_ctl_lock);
  vs_put32(vs_ctl_req + 0, code);
  vs_put32(vs_ctl_req + 4, vs_caps.stream_id);
  int r = vs_ctl_xfer(8, 4, status);
  spinlock_release_irqrestore(&vs_ctl_lock, fl);
  return r;
}

/* --- TX queue -------------------------------------------------------- */

int virtio_snd_tx_slots(void) { return vs_tx_nslots; }
int virtio_snd_tx_inflight(void) { return vs_tx_inflight_n; }

int virtio_snd_tx_post(uint64_t data_pa, uint32_t len) {
  if (!vs_ready || len == 0) return -1;
  struct vs_queue *q = &vs_txq;
  int s;
  for (s = 0; s < vs_tx_nslots; s++)
    if (!vs_tx_busy[s]) break;
  if (s >= vs_tx_nslots) return -1;
  uint16_t h = (uint16_t)(3 * s);
  vs_tx_hdr[s].stream_id = vs_caps.stream_id;
  vs_tx_st[s].status = 0;
  vs_tx_st[s].latency_bytes = 0;
  q->desc[h].addr = kimg_pa(&vs_tx_hdr[s]);
  q->desc[h].len = 4;
  q->desc[h].flags = D_NEXT;
  q->desc[h].next = (uint16_t)(h + 1);
  q->desc[h + 1].addr = data_pa;
  q->desc[h + 1].len = len;
  q->desc[h + 1].flags = D_NEXT;
  q->desc[h + 1].next = (uint16_t)(h + 2);
  q->desc[h + 2].addr = kimg_pa(&vs_tx_st[s]);
  q->desc[h + 2].len = 8;
  q->desc[h + 2].flags = D_WRITE;
  q->desc[h + 2].next = 0;
  vs_tx_busy[s] = 1;
  vs_tx_inflight_n++;
  q->avail.ring[q->avail_next % q->size] = h;
  arch_memory_barrier();
  q->avail.idx = (uint16_t)(q->avail_next + 1);
  q->avail_next++;
  vs_kick(q, 2);
  vs_st.tx_posted++;
  return s;
}

int virtio_snd_tx_reap(void (*cb)(int slot, uint32_t status, void *ctx), void *ctx) {
  if (!vs_ready) return 0;
  struct vs_queue *q = &vs_txq;
  int n = 0;
  uint16_t idx = *(volatile uint16_t *)&q->used.idx;
  while (q->used_ack != idx) {
    arch_memory_barrier();
    uint32_t id = q->used.ring[q->used_ack % q->size].id;
    q->used_ack++;
    int s = (int)(id / 3);
    if (id % 3 != 0 || s >= vs_tx_nslots || !vs_tx_busy[s]) {
      vs_st.tx_bad++;
    } else {
      uint32_t st = *(volatile uint32_t *)&vs_tx_st[s].status;
      vs_tx_busy[s] = 0;
      vs_tx_inflight_n--;
      vs_st.tx_done++;
      n++;
      if (cb) cb(s, st, ctx);
    }
    idx = *(volatile uint16_t *)&q->used.idx;
  }
  return n;
}

/* --- IRQ --------------------------------------------------------------- */

void virtio_snd_handle_irq(void) {
  if (vs_isr) (void)*(volatile uint8_t *)vs_isr; /* deasserts INTx */
  vs_st.irqs++;
  linux_snd_irq();
}

void virtio_snd_stats(struct vsnd_stats *out) {
  if (out) *out = vs_st;
}

int virtio_snd_present(void) { return vs_ready; }
int virtio_snd_caps(struct vsnd_stream_caps *out) {
  if (!vs_ready) return -ENODEV;
  if (out) *out = vs_caps;
  return 0;
}

/* --- bring-up ---------------------------------------------------------- */

struct vs_cap {
  uint8_t kind, bar;
  uint32_t off, len, mult;
};

static int vs_bringup(void) {
  uint32_t cmd = vs_pci_r(vs_bus, vs_slot, 0x04);
  /* Memory space + bus master; INTx stays disabled until the line is known. */
  vs_pci_w(vs_bus, vs_slot, 0x04, cmd | 0x2u | 0x4u | 0x400u);
  if (!((vs_pci_r(vs_bus, vs_slot, 0x04) >> 16) & (1u << 4))) return -1;
  struct vs_cap caps[12];
  int n = 0, guard = 0;
  uint8_t ptr = (uint8_t)(vs_pci_r(vs_bus, vs_slot, 0x34) & 0xFC);
  while (ptr && guard++ < 64 && n < 12) {
    uint32_t d0 = vs_pci_r(vs_bus, vs_slot, ptr);
    if ((d0 & 0xFF) == 0x09) {
      uint32_t d1 = vs_pci_r(vs_bus, vs_slot, (uint8_t)(ptr + 4));
      caps[n].kind = (uint8_t)(d0 >> 24);
      caps[n].bar = (uint8_t)d1;
      caps[n].off = vs_pci_r(vs_bus, vs_slot, (uint8_t)(ptr + 8));
      caps[n].len = vs_pci_r(vs_bus, vs_slot, (uint8_t)(ptr + 12));
      caps[n].mult = caps[n].kind == CAP_NOTIFY ? vs_pci_r(vs_bus, vs_slot, (uint8_t)(ptr + 16)) : 0;
      n++;
    }
    ptr = (uint8_t)((d0 >> 8) & 0xFC);
  }
  struct vs_cap *cc = 0, *nc = 0, *ic = 0, *dc = 0;
  for (int i = 0; i < n; i++) {
    if (caps[i].kind == CAP_COMMON && !cc) cc = &caps[i];
    else if (caps[i].kind == CAP_NOTIFY && !nc) nc = &caps[i];
    else if (caps[i].kind == CAP_ISR && !ic) ic = &caps[i];
    else if (caps[i].kind == CAP_DEVICE && !dc) dc = &caps[i];
  }
  if (!cc || !nc || !ic || !dc) return -1;
  if (cc->bar != nc->bar || cc->bar != ic->bar || cc->bar != dc->bar) return -1;
  uint64_t bar = vs_bar_phys(cc->bar);
  if (bar < 0x100000) return -1;
  uint64_t span = 0;
  struct vs_cap *all[4] = {cc, nc, ic, dc};
  for (int i = 0; i < 4; i++)
    if ((uint64_t)all[i]->off + all[i]->len > span) span = (uint64_t)all[i]->off + all[i]->len;
  volatile uint8_t *base = (volatile uint8_t *)ioremap(bar, span);
  if (!base) return -1;
  vs_cc = base + cc->off;
  vs_notify = base + nc->off;
  vs_isr = base + ic->off;
  vs_devcfg = base + dc->off;
  vs_notify_mult = nc->mult ? nc->mult : 4;

  cc_w8(CC_STATUS, 0);
  for (int g = 0; cc_r8(CC_STATUS) != 0; g++) {
    arch_memory_barrier();
    if (g > 1000000) return -1;
  }
  cc_w8(CC_STATUS, ST_ACK);
  cc_w8(CC_STATUS, ST_ACK | ST_DRIVER);
  cc_w32(CC_DEV_FEAT_SEL, 1);
  uint32_t fhi = cc_r32(CC_DEV_FEAT);
  if (!(fhi & 1u)) return -1; /* VIRTIO_F_VERSION_1 */
  cc_w32(CC_DRV_FEAT_SEL, 0);
  cc_w32(CC_DRV_FEAT, 0);
  cc_w32(CC_DRV_FEAT_SEL, 1);
  cc_w32(CC_DRV_FEAT, 1u);
  cc_w8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
  arch_memory_barrier();
  if (!(cc_r8(CC_STATUS) & ST_FEATURES_OK)) return -1;

  uint32_t jacks = *(volatile uint32_t *)(vs_devcfg + 0);
  uint32_t streams = *(volatile uint32_t *)(vs_devcfg + 4);
  uint32_t chmaps = *(volatile uint32_t *)(vs_devcfg + 8);

  if (vs_queue_setup(&vs_ctlq, 0, 1) != 0) return -1;
  if (vs_queue_setup(&vs_txq, 2, 0) != 0) return -1;
  vs_tx_nslots = vs_txq.size / 3;
  if (vs_tx_nslots > TX_SLOTS_MAX) vs_tx_nslots = TX_SLOTS_MAX;
  cc_w8(CC_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
  vs_ready = 1; /* the control helpers below need it */

  uart_puts("[SND] virtio-sound 1af4:1059 bar=");
  uart_print_hex(bar);
  uart_puts(" jacks=");
  print_int((int)jacks);
  uart_puts(" streams=");
  print_int((int)streams);
  uart_puts(" chmaps=");
  print_int((int)chmaps);
  uart_puts(" ctlq=");
  print_int(vs_ctlq.size);
  uart_puts(" txq=");
  print_int(vs_txq.size);
  uart_puts("\n");

  /* PCM_INFO for every stream (<= 8): pick the first OUTPUT stream. */
  uint32_t ns = streams > 8 ? 8 : streams;
  if (ns == 0) {
    vs_ready = 0;
    return -1;
  }
  uint32_t st = 0;
  uint64_t fl = spinlock_acquire_irqsave(&vs_ctl_lock);
  vs_put32(vs_ctl_req + 0, VSND_R_PCM_INFO);
  vs_put32(vs_ctl_req + 4, 0);
  vs_put32(vs_ctl_req + 8, ns);
  vs_put32(vs_ctl_req + 12, 32);
  int r = vs_ctl_xfer(16, 4 + ns * 32, &st);
  spinlock_release_irqrestore(&vs_ctl_lock, fl);
  if (r != 0) {
    uart_puts("[SND] PCM_INFO failed status=");
    uart_print_hex(st);
    uart_puts("\n");
    vs_ready = 0;
    return -1;
  }
  int found = 0;
  for (uint32_t i = 0; i < ns; i++) {
    const uint8_t *e = vs_ctl_resp + 4 + i * 32;
    uint32_t feat = vs_get32(e + 4);
    uint64_t fmts = vs_get64(e + 8), rates = vs_get64(e + 16);
    uint8_t dir = e[24], cmin = e[25], cmax = e[26];
    uart_puts("[SND] stream ");
    print_int((int)i);
    uart_puts(dir == 0 ? " out" : " in");
    uart_puts(" formats=");
    uart_print_hex(fmts);
    uart_puts(" rates=");
    uart_print_hex(rates);
    uart_puts(" ch=");
    print_int(cmin);
    uart_puts("..");
    print_int(cmax);
    uart_puts("\n");
    if (!found && dir == 0) {
      found = 1;
      vs_caps.stream_id = i;
      vs_caps.features = feat;
      vs_caps.formats = fmts;
      vs_caps.rates = rates;
      vs_caps.ch_min = cmin;
      vs_caps.ch_max = cmax;
    }
  }
  vs_caps.n_streams = streams;
  if (!found) {
    vs_ready = 0;
    return -1;
  }

  /* INTx: legacy PIC vector 32+line, level mode in the ELCR. */
  uint32_t il = vs_pci_r(vs_bus, vs_slot, 0x3C) & 0xFF;
  if (il >= 1 && il <= 15 && il != 2) {
    uint16_t port = il < 8 ? 0x4D0 : 0x4D1;
    uint8_t bit = (uint8_t)(1u << (il & 7));
    uint8_t e = vs_inb(port);
    if (!(e & bit)) vs_outb(port, (uint8_t)(e | bit));
    virtio_snd_irq_vector = 32 + (int)il;
    uint32_t c2 = vs_pci_r(vs_bus, vs_slot, 0x04);
    vs_pci_w(vs_bus, vs_slot, 0x04, c2 & ~0x400u);
    (void)*(volatile uint8_t *)vs_isr;
    gic_enable_interrupt((uint32_t)virtio_snd_irq_vector);
  }
  uart_puts("[SND] irq line=");
  print_int((int)il);
  uart_puts(" vector=");
  print_int(virtio_snd_irq_vector);
  uart_puts(virtio_snd_irq_vector >= 0 ? " (TX completions by INTx)\n" : " (polled)\n");
  return 0;
}

int virtio_snd_init(void) {
  spinlock_init(&vs_ctl_lock);
  for (uint32_t bus = 0; bus < 256; bus++) {
    for (uint32_t slot = 0; slot < 32; slot++) {
      uint32_t id = vs_pci_r((uint8_t)bus, (uint8_t)slot, 0);
      if (id == 0xFFFFFFFFu) continue;
      if ((id & 0xFFFF) != VSND_PCI_VENDOR || (id >> 16) != VSND_PCI_DEVICE) continue;
      vs_bus = (uint8_t)bus;
      vs_slot = (uint8_t)slot;
      if (vs_bringup() == 0) return 0;
      uart_puts("[SND] virtio-sound bring-up failed\n");
      return -1;
    }
  }
  return -1; /* no device: silent (the default, every gate) */
}

#else /* !__x86_64__: no virtio-mmio sound variant (AD-11) */

int virtio_snd_irq_vector = -1;
int virtio_snd_init(void) { return -1; }
int virtio_snd_present(void) { return 0; }
int virtio_snd_caps(struct vsnd_stream_caps *out) { (void)out; return -ENODEV; }
int virtio_snd_set_params(uint32_t b, uint32_t p, uint8_t c, uint8_t f, uint8_t r, uint32_t *s) {
  (void)b; (void)p; (void)c; (void)f; (void)r;
  if (s) *s = 0;
  return -ENODEV;
}
int virtio_snd_pcm_cmd(uint32_t code, uint32_t *s) {
  (void)code;
  if (s) *s = 0;
  return -ENODEV;
}
int virtio_snd_tx_post(uint64_t pa, uint32_t len) { (void)pa; (void)len; return -1; }
int virtio_snd_tx_reap(void (*cb)(int, uint32_t, void *), void *ctx) { (void)cb; (void)ctx; return 0; }
int virtio_snd_tx_slots(void) { return 0; }
int virtio_snd_tx_inflight(void) { return 0; }
void virtio_snd_handle_irq(void) {}
void virtio_snd_stats(struct vsnd_stats *out) {
  if (out) {
    out->irqs = out->tx_posted = out->tx_done = out->tx_bad = 0;
    out->ctl_cmds = out->ctl_timeouts = 0;
  }
}

#endif
