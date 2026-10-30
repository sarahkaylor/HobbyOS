/*
 * vdisplay.c - per-process virtual display (private framebuffer + private
 * input queue).  Contract: src/include/vdisplay.h; design: icewm.md §9.
 *
 * Two halves:
 *   - the pure core (slot table, input ring, damage union): no process or
 *     VM dependency, compiled into the host test with -DVDISP_HOST_TEST and
 *     exercised by the kernel unit suite (vdisplay_test.c);
 *   - the kernel glue (attach/inherit/map/flush/read), called from both
 *     arch syscall dispatchers.  User pointers are validated by the
 *     dispatchers (sys_user_range / vm_user_read) before reaching here.
 *
 * Kernel rules: plain integer fields only, no packed structs, no wide
 * copies of byte buffers (everything is naturally aligned u16/u32 records).
 */
#include "vdisplay.h"

#ifndef VDISP_HOST_TEST
#include "process.h"
#include "vm.h"
#include "fs.h"
#include "lock.h"
#include "display_mode.h"
#include "errno.h"
#endif

struct vdisp_slot {
  int used;
  uint32_t gen;
  int owner_pid;
  void *obj;
  struct vdisp_event ring[VDISP_RING];
  int head, tail;          /* ring: [tail, head) */
  int dx0, dy0, dx1, dy1;  /* damage union, half-open; dx1 <= dx0 = none */
  uint32_t flush_seq;
  uint32_t mapped;
  uint32_t reads;
  uint64_t last_read_ms;
  int ever_read;
};

static struct vdisp_slot slots[VDISP_MAX];
static uint32_t next_gen = 1;

void vdisp_core_reset(void) {
  for (int i = 0; i < VDISP_MAX; i++) {
    slots[i].used = 0;
    slots[i].gen = 0;
    slots[i].obj = 0;
  }
}

static struct vdisp_slot *slot_of(int id) {
  if (id < 1 || id > VDISP_MAX) return 0;
  struct vdisp_slot *s = &slots[id - 1];
  return s->used ? s : 0;
}

int vdisp_core_alloc(int owner_pid, void *obj) {
  for (int i = 0; i < VDISP_MAX; i++) {
    struct vdisp_slot *s = &slots[i];
    if (s->used) continue;
    s->used = 1;
    s->gen = next_gen++;
    if (next_gen == 0) next_gen = 1;
    s->owner_pid = owner_pid;
    s->obj = obj;
    s->head = s->tail = 0;
    s->dx0 = s->dy0 = s->dx1 = s->dy1 = 0;
    s->flush_seq = 0;
    s->mapped = 0;
    s->reads = 0;
    s->last_read_ms = 0;
    s->ever_read = 0;
    return i + 1;
  }
  return 0;
}

void *vdisp_core_free(int id) {
  struct vdisp_slot *s = slot_of(id);
  if (!s) return 0;
  void *o = s->obj;
  s->used = 0;
  s->obj = 0;
  s->gen = 0;
  return o;
}

int vdisp_core_valid(int id, uint32_t gen) {
  struct vdisp_slot *s = slot_of(id);
  return s && s->gen == gen;
}

uint32_t vdisp_core_gen(int id) {
  struct vdisp_slot *s = slot_of(id);
  return s ? s->gen : 0;
}

int vdisp_core_owner(int id) {
  struct vdisp_slot *s = slot_of(id);
  return s ? s->owner_pid : -1;
}

void *vdisp_core_obj(int id) {
  struct vdisp_slot *s = slot_of(id);
  return s ? s->obj : 0;
}

int vdisp_core_inject(int id, const struct vdisp_event *ev, int n) {
  struct vdisp_slot *s = slot_of(id);
  if (!s || n <= 0) return 0;
  int k = 0;
  for (; k < n; k++) {
    int nh = (s->head + 1) % VDISP_RING;
    if (nh == s->tail) break; /* full: drop the rest */
    s->ring[s->head].type = ev[k].type;
    s->ring[s->head].code = ev[k].code;
    s->ring[s->head].value = ev[k].value;
    s->head = nh;
  }
  return k;
}

int vdisp_core_read(int id, struct vdisp_event *out, int max, uint64_t now_ms) {
  struct vdisp_slot *s = slot_of(id);
  if (!s) return 0;
  s->reads++;
  s->last_read_ms = now_ms;
  s->ever_read = 1;
  int k = 0;
  while (k < max && s->tail != s->head) {
    out[k].type = s->ring[s->tail].type;
    out[k].code = s->ring[s->tail].code;
    out[k].value = s->ring[s->tail].value;
    s->tail = (s->tail + 1) % VDISP_RING;
    k++;
  }
  return k;
}

void vdisp_core_damage(int id, int x, int y, int w, int h) {
  struct vdisp_slot *s = slot_of(id);
  if (!s) return;
  s->flush_seq++;
  if (w <= 0 || h <= 0) return;
  int x1 = x + w, y1 = y + h;
  if (s->dx1 <= s->dx0 || s->dy1 <= s->dy0) {
    s->dx0 = x; s->dy0 = y; s->dx1 = x1; s->dy1 = y1;
  } else {
    if (x < s->dx0) s->dx0 = x;
    if (y < s->dy0) s->dy0 = y;
    if (x1 > s->dx1) s->dx1 = x1;
    if (y1 > s->dy1) s->dy1 = y1;
  }
}

void vdisp_core_mapped(int id) {
  struct vdisp_slot *s = slot_of(id);
  if (s) s->mapped++;
}

void vdisp_core_status(int id, struct vdisp_status *out, uint64_t now_ms) {
  struct vdisp_slot *s = slot_of(id);
  out->flush_seq = 0; out->dx = out->dy = out->dw = out->dh = 0;
  out->mapped = 0; out->reads = 0; out->read_age_ms = 0xFFFFFFFFu; out->queued = 0;
  if (!s) return;
  out->flush_seq = s->flush_seq;
  if (s->dx1 > s->dx0 && s->dy1 > s->dy0) {
    out->dx = s->dx0; out->dy = s->dy0;
    out->dw = s->dx1 - s->dx0; out->dh = s->dy1 - s->dy0;
  }
  s->dx0 = s->dy0 = s->dx1 = s->dy1 = 0;
  out->mapped = s->mapped;
  out->reads = s->reads;
  if (s->ever_read) {
    uint64_t age = now_ms >= s->last_read_ms ? now_ms - s->last_read_ms : 0;
    out->read_age_ms = age > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)age;
  }
  out->queued = (uint32_t)((s->head - s->tail + VDISP_RING) % VDISP_RING);
}

#ifndef VDISP_HOST_TEST
/* ======================= kernel glue ================================= */

#define VDISP_SURFACE_BYTES ((uint64_t)DISPLAY_STRIDE_BYTES * DISPLAY_HEIGHT)

extern uint64_t timer_get_ms(void);
static spinlock_t vdisp_lock = {0};
static int vdisp_lock_ready = 0;

static uint64_t vlock(void) {
  if (!vdisp_lock_ready) {
    spinlock_init(&vdisp_lock);
    vdisp_lock_ready = 1;
  }
  return spinlock_acquire_irqsave(&vdisp_lock);
}
static void vunlock(uint64_t f) { spinlock_release_irqrestore(&vdisp_lock, f); }

void vdisp_pcb_init(struct process *p) {
  p->vdisp_id = 0;
  p->vdisp_gen = 0;
  p->vdisp_own = 0;
  p->vdisp_own_gen = 0;
}

void vdisp_inherit(struct process *child, struct process *parent) {
  if (!child) return;
  child->vdisp_own = 0;
  child->vdisp_own_gen = 0;
  if (!parent) { child->vdisp_id = 0; child->vdisp_gen = 0; return; }
  parent = process_group(parent);
  child->vdisp_id = parent->vdisp_id;
  child->vdisp_gen = parent->vdisp_gen;
}

/* The display id the process's group is attached to (0 = none / dead). */
int vdisp_attached(struct process *p) {
  if (!p) return 0;
  p = process_group(p);
  if (!p || p->vdisp_id == 0) return 0;
  return vdisp_core_valid(p->vdisp_id, p->vdisp_gen) ? p->vdisp_id : 0;
}

/* Is the owner of slot `id` still the live process that created it?  pids
 * are proc_table slots (reused), so the owner PCB must still name this
 * display + generation in its vdisp_own fields. */
static int owner_alive(int id) {
  int pid = vdisp_core_owner(id);
  struct process *o = process_get_pcb(pid);
  if (!o) return 0;
  if (o->state == PROC_STATE_FREE || o->state == PROC_STATE_EXITED ||
      o->state == PROC_STATE_THREAD_DONE)
    return 0;
  return o->vdisp_own == id && o->vdisp_own_gen == vdisp_core_gen(id);
}

int64_t vdisp_sys_attach(struct process *caller, int fd) {
  struct process *grp = process_group(caller);
  if (!grp) return -EINVAL;
  struct vm_object *obj = file_memfd_obj(grp, fd);
  if (!obj) return -EBADF;
  if (vm_object_size(obj) < VDISP_SURFACE_BYTES) return -EINVAL;
  vm_object_ref(obj); /* the slot's reference */

  void *drop[VDISP_MAX + 1];
  int nd = 0;
  uint64_t f = vlock();
  /* Reclaim displays whose owner is gone, and the caller's previous one. */
  for (int id = 1; id <= VDISP_MAX; id++) {
    if (!vdisp_core_obj(id)) continue;
    if (!owner_alive(id) || id == grp->vdisp_own) {
      void *o = vdisp_core_free(id);
      if (o) drop[nd++] = o;
    }
  }
  int id = vdisp_core_alloc(grp->pid, obj);
  if (id) {
    grp->vdisp_id = id;
    grp->vdisp_gen = vdisp_core_gen(id);
    grp->vdisp_own = id;
    grp->vdisp_own_gen = grp->vdisp_gen;
  }
  vunlock(f);
  for (int i = 0; i < nd; i++) vm_object_unref((struct vm_object *)drop[i]);
  if (!id) {
    vm_object_unref(obj);
    return -ENOSPC;
  }
  return 0;
}

int64_t vdisp_sys_detach(struct process *caller) {
  struct process *grp = process_group(caller);
  if (!grp) return -EINVAL;
  void *o = 0;
  uint64_t f = vlock();
  if (grp->vdisp_own && grp->vdisp_own == grp->vdisp_id &&
      vdisp_core_valid(grp->vdisp_own, grp->vdisp_own_gen))
    o = vdisp_core_free(grp->vdisp_own);
  grp->vdisp_own = 0;
  grp->vdisp_own_gen = 0;
  grp->vdisp_id = 0;
  grp->vdisp_gen = 0;
  vunlock(f);
  if (o) vm_object_unref((struct vm_object *)o);
  return 0;
}

int64_t vdisp_sys_inject(struct process *caller, const struct vdisp_event *kev, int n) {
  int id = vdisp_attached(caller);
  if (!id) return -ENODEV;
  uint64_t f = vlock();
  int k = vdisp_core_inject(id, kev, n);
  vunlock(f);
  return k;
}

int64_t vdisp_sys_status(struct process *caller, struct vdisp_status *out) {
  int id = vdisp_attached(caller);
  if (!id) return -ENODEV;
  uint64_t f = vlock();
  vdisp_core_status(id, out, timer_get_ms());
  vunlock(f);
  return 0;
}

/* SYS_MAP_FB for an attached process: map the private surface. */
int64_t vdisp_map_fb(struct process *caller, int id) {
  struct process *grp = process_group(caller);
  uint64_t f = vlock();
  struct vm_object *obj = (struct vm_object *)vdisp_core_obj(id);
  if (obj) {
    vm_object_ref(obj); /* hold it across the unlocked map below */
    vdisp_core_mapped(id);
  }
  vunlock(f);
  if (!obj || !grp || !grp->as) {
    if (obj) vm_object_unref(obj);
    return -1;
  }
  int64_t va = vm_map_fb_obj(grp, obj, VDISP_SURFACE_BYTES);
  vm_object_unref(obj);
  return va;
}

void vdisp_flush(int id, int x, int y, int w, int h) {
  uint64_t f = vlock();
  vdisp_core_damage(id, x, y, w, h);
  vunlock(f);
}

int vdisp_get_events(int id, struct vdisp_event *out, int max) {
  if (max > 64) max = 64;
  struct vdisp_event tmp[64];
  uint64_t f = vlock();
  int n = vdisp_core_read(id, tmp, max, timer_get_ms());
  vunlock(f);
  for (int i = 0; i < n; i++) {
    out[i].type = tmp[i].type;
    out[i].code = tmp[i].code;
    out[i].value = tmp[i].value;
  }
  return n;
}
#endif /* !VDISP_HOST_TEST */
