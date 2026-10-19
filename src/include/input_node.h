#ifndef INPUT_NODE_H
#define INPUT_NODE_H

/* input_node.h — device-free /dev/input/event* evdev node logic (GB-027).
 *
 * The compat node feeds a Linux-ABI `struct input_event` stream (24 B)
 * from the kernel's virtio-input/PS2->evdev decode ring, and answers the
 * EVIOCG* probe ioctls an SDL evdev driver (or any static-musl binary)
 * sends before it starts reading.  Those are two pure, device-free
 * functions here — event translation and ioctl routing/fill — so the
 * same source links into the kernel (both arches) and the host
 * differential suite (the nfs_proto / ipc_proto / evdev / fb_ioctl /
 * linux_input single-TU precedent).
 *
 * All ABI values are locked (not derived from memory) in
 * src/include/linux_input.h / tools/linux-abi/probe_abi.c /
 * docs/linux-abi.md §9: struct input_event 24 B (time@0, type@16,
 * code@18, value@20), EVIOCGVERSION=0x80044501, EVIOCGNAME/EVIOCGBIT
 * /EVIOCGABS from the Linux _IOC('E',...) formula.
 */

#include "linux_input.h"
#include <stddef.h>
#include <stdint.h>

/* EVIOCGVERSION reply: the Linux input ABI version (EV_VERSION 0x010001). */
#define HB_INPUT_EVIOCG_VERSION (0x010001u)

/* Fill one Linux-ABI event (24 B) from an internal type/code/value triple
 * plus a timestamp.  The compat node's read path calls this once per event
 * copied out of the virtio_input ring.  Pure and host-testable. */
void hb_input_fill_event(struct hb_input_event *out, uint16_t type,
                         uint16_t code, uint32_t value,
                         uint64_t sec, uint64_t usec);

/* Out-buffer size (bytes) an EVIOCG* request writes, so the syscall shim
 * can validate the user buffer before routing.  (size_t)-1 = unknown. */
size_t hb_input_ioctl_arg_size(unsigned long req);

/* Route one /dev/input/event* ioctl into `arg` exactly as a real evdev
 * node would (version / name / event-bitmap / absinfo).  0 | -1. */
int hb_input_ioctl_route(unsigned long req, void *arg);

#endif /* INPUT_NODE_H */
