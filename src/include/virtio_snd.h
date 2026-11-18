/* virtio_snd.h — virtio-sound (virtio 1.2 §5.14) PCI driver, x86_64
 * (lane C4-audio, docs/cp-audio.md).
 *
 * One playback PCM stream: the first output stream the device reports.  The
 * control queue is polled (synchronous commands, process context only); the
 * TX queue completes by INTx.  The ALSA layer (linux_snd.c) owns the PCM
 * state and the ring buffer; this driver only moves messages.
 *
 * Serialisation: every vsnd_tx_* call must be made under the caller's PCM
 * lock (linux_snd.c's lsnd_lock, irqsave), from process context or from the
 * IRQ handler's callback.  The control-queue calls take the driver's own
 * lock and spin; never call them from the IRQ handler.
 *
 * ARM: no virtio-mmio variant (AD-11); virtio_snd_present() is 0 there. */
#ifndef VIRTIO_SND_H
#define VIRTIO_SND_H

#include <stdint.h>

/* virtio-snd PCM formats / rates (spec §5.14.6.6.3: bit positions). */
#define VSND_PCM_FMT_S16   5
#define VSND_PCM_FMT_S32  17
#define VSND_PCM_FMT_FLOAT 19
#define VSND_PCM_RATE_44100 6
#define VSND_PCM_RATE_48000 7

/* Request codes (spec §5.14.6.1). */
#define VSND_R_PCM_INFO       0x0100u
#define VSND_R_PCM_SET_PARAMS 0x0101u
#define VSND_R_PCM_PREPARE    0x0102u
#define VSND_R_PCM_RELEASE    0x0103u
#define VSND_R_PCM_START      0x0104u
#define VSND_R_PCM_STOP       0x0105u
#define VSND_S_OK       0x8000u
#define VSND_S_BAD_MSG  0x8001u
#define VSND_S_NOT_SUPP 0x8002u
#define VSND_S_IO_ERR   0x8003u

struct vsnd_stream_caps {
  uint32_t stream_id;
  uint32_t features;
  uint64_t formats; /* bit VSND_PCM_FMT_* */
  uint64_t rates;   /* bit VSND_PCM_RATE_* */
  uint8_t ch_min, ch_max;
  uint32_t n_streams; /* config.streams */
};

/* Boot: probe the PCI bus for 1af4:1059 and bring it up.  0 = device ready. */
int virtio_snd_init(void);
int virtio_snd_present(void);
int virtio_snd_caps(struct vsnd_stream_caps *out);

/* Control queue (process context): SET_PARAMS and the four state commands on
 * the selected stream.  Return 0 on VIRTIO_SND_S_OK, -EIO otherwise
 * (*status, when non-NULL, gets the device's status code or 0 on timeout). */
int virtio_snd_set_params(uint32_t buffer_bytes, uint32_t period_bytes,
                          uint8_t channels, uint8_t format, uint8_t rate,
                          uint32_t *status);
int virtio_snd_pcm_cmd(uint32_t code, uint32_t *status);

/* TX queue (caller holds the PCM lock).  post: one message of `len` bytes at
 * physical `data_pa`; returns its slot (>= 0) or -1 when every slot is in
 * flight.  reap: hands each completed slot to `cb` (slot, device status). */
int virtio_snd_tx_post(uint64_t data_pa, uint32_t len);
int virtio_snd_tx_reap(void (*cb)(int slot, uint32_t status, void *ctx), void *ctx);
int virtio_snd_tx_slots(void);
int virtio_snd_tx_inflight(void);

/* INTx: -1 until a usable line was found.  The x64 IRQ dispatch calls the
 * handler for this vector (trap.c), which reads the ISR and calls
 * linux_snd_irq(). */
extern int virtio_snd_irq_vector;
void virtio_snd_handle_irq(void);
void linux_snd_irq(void); /* linux_snd.c */

/* Counters for the probe/diagnostics rows. */
struct vsnd_stats {
  uint64_t irqs, tx_posted, tx_done, tx_bad, ctl_cmds, ctl_timeouts;
};
void virtio_snd_stats(struct vsnd_stats *out);

#endif /* VIRTIO_SND_H */
