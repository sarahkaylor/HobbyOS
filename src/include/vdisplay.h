#ifndef VDISPLAY_H
#define VDISPLAY_H

/*
 * Virtual display (icewm.md section 9, "HBHOST / virtual display").
 *
 * A process can attach a PRIVATE framebuffer + a PRIVATE input queue to
 * itself.  Children it spawns or forks afterwards inherit the attachment.
 * For an attached process:
 *
 *   SYS_MAP_FB          maps the private surface (a memfd the attacher
 *                       created, DISPLAY_WIDTH x DISPLAY_HEIGHT x 4 bytes,
 *                       stride DISPLAY_STRIDE_BYTES -- the exact geometry
 *                       of the real framebuffer, so SYS_FB_IOCTL needs no
 *                       change) at the usual USER_FB_BASE_V2 slot;
 *   SYS_FLUSH_FB(_RECTS) never touch the real display: they record damage
 *                       (a union rect + a sequence number) for the host,
 *                       and are paced to ~60/s (an early flush sleeps until
 *                       its frame slot -- a virtual present is otherwise
 *                       free and loop-paced games would run unbounded);
 *   SYS_FB_IOCTL        FBIOBLANK is a no-op (geometry queries unchanged);
 *   SYS_GET_EVENTS /    read the private queue, which the host fills with
 *   SYS_INPUT_READ      VDISP_OP_INJECT.
 *
 * Unattached processes (the X server, the default desktop) are untouched.
 *
 *   SYS_VDISPLAY (103)  (op, a1, a2) -> >= 0 | -errno
 *     VDISP_OP_ATTACH  a1 = memfd fd     -> 0: create a display backed by the
 *                      memfd (>= VDISP_SURFACE_BYTES) and attach the caller
 *                      (replacing any inherited attachment).  The caller
 *                      owns it: it dies with the owner (or VDISP_OP_DETACH).
 *     VDISP_OP_DETACH                    -> 0: release (owner) / detach.
 *     VDISP_OP_INJECT  a1 = struct vdisp_event *, a2 = count
 *                      -> number queued (the ring drops when full).
 *     VDISP_OP_STATUS  a1 = struct vdisp_status * -> 0; returns and CLEARS
 *                      the accumulated damage.
 */

#include <stdint.h>

#define VDISP_OP_ATTACH 1
#define VDISP_OP_DETACH 2
#define VDISP_OP_INJECT 3
#define VDISP_OP_STATUS 4

#define VDISP_MAX 16       /* displays system-wide */
#define VDISP_RING 256     /* queued input events per display */
#define VDISP_FRAME_MS 16  /* present pacing: ~60 flushes/s per display */

/* Same layout as struct virtio_input_event (the SYS_GET_EVENTS record). */
struct vdisp_event {
  uint16_t type;
  uint16_t code;
  uint32_t value;
};

struct vdisp_status {
  uint32_t flush_seq;   /* bumps on every SYS_FLUSH_FB(_RECTS) by an attachee */
  int32_t dx, dy, dw, dh; /* damage union since the last STATUS (dw==0: none) */
  uint32_t mapped;      /* number of SYS_MAP_FB calls by attachees */
  uint32_t reads;       /* SYS_GET_EVENTS/SYS_INPUT_READ calls on the queue */
  uint32_t read_age_ms; /* ms since the last such read (0xFFFFFFFF: never) */
  uint32_t queued;      /* events still in the ring */
};

/* ---- pure core (device-free; host- and kernel-unit-tested) ---- */
void vdisp_core_reset(void);
int vdisp_core_alloc(int owner_pid, void *obj);   /* -> id 1..VDISP_MAX | 0 */
void *vdisp_core_free(int id);                    /* -> the obj it held */
int vdisp_core_valid(int id, uint32_t gen);
uint32_t vdisp_core_gen(int id);
int vdisp_core_owner(int id);
void *vdisp_core_obj(int id);
int vdisp_core_inject(int id, const struct vdisp_event *ev, int n);
int vdisp_core_read(int id, struct vdisp_event *out, int max, uint64_t now_ms);
void vdisp_core_damage(int id, int x, int y, int w, int h);
void vdisp_core_mapped(int id);
void vdisp_core_status(int id, struct vdisp_status *out, uint64_t now_ms);
int vdisp_core_pace(int id, uint64_t now_ms);    /* ms the flusher waits */


/* ---- kernel glue (src/kernel/vdisplay.c; both arch dispatchers) ---- */
struct process;
void vdisp_pcb_init(struct process *p);
void vdisp_inherit(struct process *child, struct process *parent);
int vdisp_attached(struct process *p);              /* id | 0 */
int64_t vdisp_sys_attach(struct process *caller, int fd);
int64_t vdisp_sys_detach(struct process *caller);
int64_t vdisp_sys_inject(struct process *caller, const struct vdisp_event *kev, int n);
int64_t vdisp_sys_status(struct process *caller, struct vdisp_status *out);
int64_t vdisp_map_fb(struct process *caller, int id);
void vdisp_flush(int id, int x, int y, int w, int h);
int vdisp_flush_pace(int id);                       /* ms to sleep */
int vdisp_get_events(int id, struct vdisp_event *out, int max);

#endif
