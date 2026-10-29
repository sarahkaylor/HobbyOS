#include "virtio_input.h"
#include "evdev.h"
#include "lock.h"
#include "arch/cpu.h"


#ifdef __x86_64__

/* x86_64 input: i8042 PS/2 keyboard + mouse, plus the VMware-backdoor
 * absolute pointer ("vmmouse", exposed by QEMU's pc/q35 machines with
 * vmport=on -- the default).  With vmmouse active QEMU treats the guest
 * pointer as absolute: a QEMU GTK/SDL/VNC window no longer has to grab
 * the host pointer, and the guest cursor sits exactly under the host one.
 * Without it (vmport=off, real hardware) the PS/2 relative stream is the
 * fallback.  Both feed the shared evdev decoder (src/kernel/evdev.c) and
 * the same kernel event ring the desktop reads with get_events(). */

#include "display_mode.h"
#include "timer.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);

#define EVENT_RING_SIZE 1024
static struct virtio_input_event event_ring[EVENT_RING_SIZE];
static int ring_head = 0;
static int ring_tail = 0;
static spinlock_t input_lock;

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static int ring_free(void) {
  return (ring_tail - ring_head - 1 + EVENT_RING_SIZE) % EVENT_RING_SIZE;
}

static void ring_put(uint16_t type, uint16_t code, uint32_t value) {
  event_ring[ring_head].type = type;
  event_ring[ring_head].code = code;
  event_ring[ring_head].value = value;
  ring_head = (ring_head + 1) % EVENT_RING_SIZE;
}

/* The PS/2 -> evdev decode (scancodes, mouse packets, the overflow-drop
   rule, resync, vmmouse packets) lives in src/kernel/evdev.c, shared with
   the host golden suite (src/host/evdev_test.c).  Here we bridge its
   events into the kernel's event ring.

   Events are committed one SYN-terminated packet at a time: a full ring
   never splits a packet (which used to drop e.g. ABS_X but keep ABS_Y, or
   lose a button release).  A motion-only packet that does not fit is
   coalesced into a pending position that is re-emitted once there is
   room, so the cursor always ends up where the device says it is. */
#define STAGE_MAX 16
static struct virtio_input_event stage[STAGE_MAX];
static int stage_n = 0;
static int pending_abs = 0;
static uint32_t pending_x = 0, pending_y = 0;
static uint32_t dropped_packets = 0;

static void flush_pending_abs(void) {
  if (pending_abs && ring_free() >= 3) {
    ring_put(EV_ABS, ABS_X, pending_x);
    ring_put(EV_ABS, ABS_Y, pending_y);
    ring_put(EV_SYN, 0, 0);
    pending_abs = 0;
  }
}

static void decoder_emit(void *ctx, uint16_t type, uint16_t code,
                         uint32_t value) {
  (void)ctx;
  if (stage_n < STAGE_MAX) {
    stage[stage_n].type = type;
    stage[stage_n].code = code;
    stage[stage_n].value = value;
    stage_n++;
  }
  if (type != EV_SYN) return;

  flush_pending_abs();
  int motion_only = 1;
  for (int i = 0; i < stage_n; i++) {
    if (stage[i].type == EV_ABS) {
      if (stage[i].code == ABS_X) pending_x = stage[i].value;
      else if (stage[i].code == ABS_Y) pending_y = stage[i].value;
    } else if (stage[i].type != EV_SYN) {
      motion_only = 0;
    }
  }
  if (!pending_abs && ring_free() >= stage_n) {
    for (int i = 0; i < stage_n; i++)
      ring_put(stage[i].type, stage[i].code, stage[i].value);
  } else if (motion_only) {
    pending_abs = 1; /* pending_x/y already hold the newest position */
  } else {
    dropped_packets++;
  }
  stage_n = 0;
}

static struct evdev_decoder input_decoder;

/* ---- VMware backdoor (vmmouse) ----------------------------------------- */
#define VMW_MAGIC 0x564D5868u
#define VMW_PORT 0x5658
#define VMW_CMD_GETVERSION 10
#define VMW_CMD_ABSPOINTER_DATA 39
#define VMW_CMD_ABSPOINTER_STATUS 40
#define VMW_CMD_ABSPOINTER_COMMAND 41
#define VMMOUSE_CMD_READ_ID 0x45414552u
#define VMMOUSE_CMD_DISABLE 0x000000F5u
#define VMMOUSE_CMD_REQUEST_ABSOLUTE 0x53424152u
#define VMMOUSE_VERSION_ID 0x3442554Au
#define VMMOUSE_ERROR 0xFFFF0000u

static int vmmouse_present = 0;
static uint32_t vmmouse_packets = 0;

struct vmw_regs { uint32_t eax, ebx, ecx, edx, esi, edi; };

static void vmw_call(uint32_t cmd, uint32_t arg, struct vmw_regs *r) {
  uint32_t a = VMW_MAGIC, b = arg, c = cmd, d = VMW_PORT, si = 0, di = 0;
  __asm__ volatile("inl %%dx, %%eax"
                   : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(si), "+D"(di)
                   :
                   : "memory");
  r->eax = a; r->ebx = b; r->ecx = c; r->edx = d; r->esi = si; r->edi = di;
}

/* Detect the backdoor, read the vmmouse ID and switch it to absolute.
   Returns 1 when the absolute pointer is live. */
static int vmmouse_enable(void) {
  struct vmw_regs r;
  vmw_call(VMW_CMD_GETVERSION, ~VMW_MAGIC, &r);
  if (r.ebx != VMW_MAGIC || r.eax == 0xFFFFFFFFu) return 0;

  vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMMOUSE_CMD_READ_ID, &r);
  vmw_call(VMW_CMD_ABSPOINTER_STATUS, 0, &r);
  if ((r.eax & VMMOUSE_ERROR) == VMMOUSE_ERROR || (r.eax & 0xFFFF) == 0)
    return 0;
  vmw_call(VMW_CMD_ABSPOINTER_DATA, 1, &r);
  if (r.eax != VMMOUSE_VERSION_ID) return 0;

  vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMMOUSE_CMD_REQUEST_ABSOLUTE, &r);
  return 1;
}

/* Drain queued vmmouse packets (4 words each).  Under input_lock. */
static void vmmouse_drain_nolock(void) {
  if (!vmmouse_present) return;
  for (int guard = 0; guard < 64; guard++) {
    struct vmw_regs r;
    vmw_call(VMW_CMD_ABSPOINTER_STATUS, 0, &r);
    if ((r.eax & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
      /* Device reset / queue error: re-arm absolute mode. */
      vmw_call(VMW_CMD_ABSPOINTER_COMMAND, VMMOUSE_CMD_DISABLE, &r);
      vmmouse_present = vmmouse_enable();
      return;
    }
    uint32_t queued = r.eax & 0xFFFF;
    if (queued < 4) return;
    vmw_call(VMW_CMD_ABSPOINTER_DATA, 4, &r);
    vmmouse_packets++;
    /* QEMU (and VMware) report x/y in 0..0xFFFF. */
    evdev_vmmouse_packet(&input_decoder, r.eax, r.ebx, r.ecx, r.edx, 0xFFFF,
                         decoder_emit, 0);
  }
}

/* Drain any bytes the i8042 has for us.  Runs under input_lock.  The
   guest's interrupt-driven drain alone lets QEMU's i8042 output queue fill
   (q35 PS/2 bursts): when the queue is full QEMU defers the packet to the
   next input event and it lands LATE — the integrated absolute position is
   then read before the motion arrives, so clicks land off-target.  The
   desktop polls get_events() on every loop, so draining here too keeps the
   queue empty (packets stay in order, none deferred) without any IRQ work.

   With vmmouse live, its queue is drained before each PS/2 mouse byte is
   decoded: QEMU queues the vmmouse packet BEFORE injecting the fake PS/2
   packet that raises IRQ 12, so the decoder always knows a fake is due by
   the time it sees one (and swallows it). */
static void input_drain_nolock(void) {
  vmmouse_drain_nolock();
  for (int guard = 0; guard < 4096; guard++) {
    uint8_t status = inb(0x64);
    if (!(status & 1)) {
      break;
    }
    uint8_t data = inb(0x60);
    if (status & 0x20) {
      vmmouse_drain_nolock();
      evdev_mouse_byte_at(&input_decoder, data, timer_get_ms(), decoder_emit,
                          0);
    } else {
      evdev_kbd_byte(&input_decoder, data, decoder_emit, 0);
    }
  }
}

void virtio_input_handle_irq(int irq) {
  (void)irq;
  uint64_t flags = spinlock_acquire_irqsave(&input_lock);
  input_drain_nolock();
  spinlock_release_irqrestore(&input_lock, flags);
}

int virtio_input_get_events(struct virtio_input_event *buf, int max_events) {
  uint64_t flags = spinlock_acquire_irqsave(&input_lock);
  input_drain_nolock();
  int count = 0;
  while (ring_tail != ring_head && count < max_events) {
    buf[count] = event_ring[ring_tail];
    ring_tail = (ring_tail + 1) % EVENT_RING_SIZE;
    count++;
  }
  flush_pending_abs();
  spinlock_release_irqrestore(&input_lock, flags);
  return count;
}

/* ---- i8042 setup -------------------------------------------------------- */
#define PS2_SPIN 2000000

static int ps2_wait_write(void) {
  for (int i = 0; i < PS2_SPIN; i++)
    if (!(inb(0x64) & 2)) return 0;
  return -1;
}

static int ps2_wait_read(void) {
  for (int i = 0; i < PS2_SPIN; i++)
    if (inb(0x64) & 1) return 0;
  return -1;
}

static void ps2_write_cmd(uint8_t cmd) {
  ps2_wait_write();
  outb(0x64, cmd);
}

static void ps2_write_data(uint8_t data) {
  ps2_wait_write();
  outb(0x60, data);
}

/* Returns the byte, or -1 on timeout (no device). */
static int ps2_read_data(void) {
  if (ps2_wait_read() != 0) return -1;
  return inb(0x60);
}

static void ps2_flush_output(void) {
  for (int i = 0; i < 64 && (inb(0x64) & 1); i++) (void)inb(0x60);
}

static int ps2_write_mouse(uint8_t data) {
  ps2_write_cmd(0xD4);
  ps2_write_data(data);
  return ps2_read_data(); /* 0xFA = ACK */
}

/* IntelliMouse knock: sample rates 200, 100, 80, then GET ID == 3 means
   the 4-byte (wheel) protocol is on. */
static int ps2_mouse_enable_wheel(void) {
  static const uint8_t knock[3] = {200, 100, 80};
  for (int i = 0; i < 3; i++) {
    ps2_write_mouse(0xF3);
    ps2_write_mouse(knock[i]);
  }
  if (ps2_write_mouse(0xF2) != 0xFA) return 0;
  int id = ps2_read_data();
  return id == 3;
}

int virtio_input_init(void) {
  spinlock_init(&input_lock);
  evdev_decoder_init(&input_decoder);
  evdev_decoder_set_screen(&input_decoder, DISPLAY_WIDTH, DISPLAY_HEIGHT);

  ps2_flush_output();
  // Enable mouse
  ps2_write_cmd(0xA8);
  // Enable keyboard
  ps2_write_cmd(0xAE);

  // Read Controller Command Byte
  ps2_write_cmd(0x20);
  int ccbr = ps2_read_data();
  uint8_t ccb = ccbr < 0 ? 0x47 : (uint8_t)ccbr;
  uart_puts("Original CCB: ");
  uart_print_hex(ccb);
  uart_puts("\n");

  // Enable interrupts for keyboard (bit 0) and mouse (bit 1)
  ccb |= 0x03;
  // Clear disable flags for keyboard (bit 4) and mouse (bit 5)
  ccb &= ~0x30;

  uart_puts("Modified CCB: ");
  uart_print_hex(ccb);
  uart_puts("\n");

  ps2_write_cmd(0x60);
  ps2_write_data(ccb);

  // Configure mouse
  ps2_write_mouse(0xF6); // Set defaults
  int wheel = ps2_mouse_enable_wheel();
  ps2_write_mouse(0xF3); // back to 100 samples/s
  ps2_write_mouse(100);
  evdev_mouse_set_packet_len(&input_decoder, wheel ? 4 : 3);
  ps2_write_mouse(0xF4); // Enable data reporting
  ps2_flush_output();
  uart_puts(wheel ? "[INPUT] PS/2 mouse: IntelliMouse (wheel, 4-byte)\n"
                  : "[INPUT] PS/2 mouse: standard 3-byte\n");

  vmmouse_present = vmmouse_enable();
  uart_puts(vmmouse_present
                ? "[INPUT] vmmouse absolute pointer enabled\n"
                : "[INPUT] vmmouse not present: PS/2 relative pointer\n");

  extern void gic_enable_interrupt(uint32_t intid);
  gic_enable_interrupt(33); // Keyboard IRQ 1
  gic_enable_interrupt(44); // Mouse IRQ 12

  return 0;
}

#else

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

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__((packed));

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[64];
  uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
} __attribute__((packed));

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[64];
  uint16_t avail_event;
} __attribute__((packed));

struct virtq {
  struct virtq_desc desc[64];
  struct virtq_avail avail;
  uint8_t padding[4096 - (1024 + sizeof(struct virtq_avail))];
  struct virtq_used used;
} __attribute__((aligned(4096)));

#define MAX_INPUT_DEVS 4

struct virtio_input_dev {
  uint8_t* mmio;
  int irq;
  struct virtq vq __attribute__((aligned(4096)));
  struct virtio_input_event events[64];
  uint16_t ack_used_idx;
};

static struct virtio_input_dev input_devs[MAX_INPUT_DEVS];
static int num_input_devs = 0;

// Kernel event ring buffer
#define EVENT_RING_SIZE 256
static struct virtio_input_event event_ring[EVENT_RING_SIZE];
static int ring_head = 0;
static int ring_tail = 0;
static spinlock_t input_lock;

static inline void reg_write32(uint8_t* mmio, uint32_t offset, uint32_t val) {
  *(volatile uint32_t*)(mmio + offset) = val;
}

static inline uint32_t reg_read32(uint8_t* mmio, uint32_t offset) {
  return *(volatile uint32_t*)(mmio + offset);
}

void virtio_input_handle_irq(int irq) {
  uint64_t flags = spinlock_acquire_irqsave(&input_lock);

  for (int d = 0; d < num_input_devs; d++) {
    struct virtio_input_dev* dev = &input_devs[d];
    if (dev->irq == irq && dev->mmio) {
      uint32_t status = reg_read32(dev->mmio, VIRTIO_INTERRUPT_STATUS);
      if (status) {
        reg_write32(dev->mmio, VIRTIO_INTERRUPT_ACK, status);

        // Process the used ring
        while (dev->ack_used_idx != *(volatile uint16_t*)&dev->vq.used.idx) {
          arch_memory_barrier();

          uint16_t idx = dev->ack_used_idx % 64;
          uint32_t id = dev->vq.used.ring[idx].id;

          // Copy event to the global ring
          int next_head = (ring_head + 1) % EVENT_RING_SIZE;
          if (next_head != ring_tail) {
            event_ring[ring_head] = dev->events[id];
            ring_head = next_head;
          }

          // Re-enqueue the descriptor to receive more events
          dev->vq.desc[id].addr = (uint64_t)&dev->events[id];
          dev->vq.desc[id].len = sizeof(struct virtio_input_event);
          dev->vq.desc[id].flags = 2; // VIRTQ_DESC_F_WRITE

          uint16_t avail_idx = dev->vq.avail.idx;
          dev->vq.avail.ring[avail_idx % 64] = id;

          arch_memory_barrier();
          dev->vq.avail.idx++;
          arch_memory_barrier();

          dev->ack_used_idx++;
        }

        reg_write32(dev->mmio, VIRTIO_QUEUE_NOTIFY, 0); // queue 0 is event queue
      }
    }
  }

  spinlock_release_irqrestore(&input_lock, flags);
}

int virtio_input_get_events(struct virtio_input_event *buf, int max_events) {
  uint64_t flags = spinlock_acquire_irqsave(&input_lock);
  int count = 0;
  while (ring_tail != ring_head && count < max_events) {
    buf[count] = event_ring[ring_tail];
    ring_tail = (ring_tail + 1) % EVENT_RING_SIZE;
    count++;
  }
  spinlock_release_irqrestore(&input_lock, flags);
  return count;
}

int virtio_input_init(void) {
  spinlock_init(&input_lock);
  num_input_devs = 0;

  // Device ID 18 is VirtIO Input
  for (int i = 0; i < 32 && num_input_devs < MAX_INPUT_DEVS; i++) {
    uint8_t* mmio = MMIO_BASE(i);
    uint32_t magic = *(volatile uint32_t*)(mmio + VIRTIO_MAGIC);
    uint32_t devid = *(volatile uint32_t*)(mmio + VIRTIO_DEVICE_ID);

    if (magic == 0x74726976 && devid == 18) {
      struct virtio_input_dev* dev = &input_devs[num_input_devs];
      dev->mmio = mmio;
      dev->irq = 48 + i;
      dev->ack_used_idx = 0;

      uint32_t status = 0;
      reg_write32(mmio, VIRTIO_STATUS, status);
      status |= 1; reg_write32(mmio, VIRTIO_STATUS, status); // ACKNOWLEDGE
      status |= 2; reg_write32(mmio, VIRTIO_STATUS, status); // DRIVER

      reg_write32(mmio, VIRTIO_DRIVER_FEAT_SEL, 1);
      reg_write32(mmio, VIRTIO_DRIVER_FEAT, 1);
      reg_write32(mmio, VIRTIO_DRIVER_FEAT_SEL, 0);
      reg_write32(mmio, VIRTIO_DRIVER_FEAT, 0);

      status |= 8; reg_write32(mmio, VIRTIO_STATUS, status); // FEATURES_OK
      if (!(reg_read32(mmio, VIRTIO_STATUS) & 8)) continue;

      // Queue 0: Event queue
      reg_write32(mmio, VIRTIO_GUEST_PAGE_SIZE, 4096);
      reg_write32(mmio, VIRTIO_QUEUE_SEL, 0);
      uint32_t max_size = reg_read32(mmio, VIRTIO_QUEUE_NUM_MAX);
      if (max_size == 0) continue;

      reg_write32(mmio, VIRTIO_QUEUE_NUM, 64);
      reg_write32(mmio, VIRTIO_QUEUE_ALIGN, 4096);
      reg_write32(mmio, VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)&dev->vq / 4096));

      // Populate the event queue
      for (int j = 0; j < 64; j++) {
        dev->vq.desc[j].addr = (uint64_t)&dev->events[j];
        dev->vq.desc[j].len = sizeof(struct virtio_input_event);
        dev->vq.desc[j].flags = 2; // VIRTQ_DESC_F_WRITE
        dev->vq.desc[j].next = 0;

        dev->vq.avail.ring[j] = j;
      }
      arch_memory_barrier();
      dev->vq.avail.idx = 64;
      arch_memory_barrier();

      status |= 4; reg_write32(mmio, VIRTIO_STATUS, status); // DRIVER_OK

      // Notify to start receiving events
      reg_write32(mmio, VIRTIO_QUEUE_NOTIFY, 0);

      extern void gic_enable_interrupt(uint32_t intid);
      gic_enable_interrupt(dev->irq);

      num_input_devs++;
    }
  }

  return num_input_devs > 0 ? 0 : -1;
}

#endif
