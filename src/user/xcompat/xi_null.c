/*
 * xi_null.c - libXi.so.6: "this display has no XInput2 devices", the answer
 * Unity's XI path must be able to consume without jumping to address 0.
 *
 * WHY THIS FILE EXISTS (measured, docs/rimworld-launch-log.md §19)
 *
 *   Right after the window handshake Unity calls XIGrabTouchBegin on the
 *   window it just mapped:
 *
 *     mov  0x60c8d5(%rip),%r10        # UnityPlayer+0x1f5a940 -> &slot
 *     mov  0x8(%rax),%rdx             # the Window id
 *     mov  %rsi,(%rsp)
 *     xor  %esi,%esi                  # deviceid = 0
 *     mov  $0x1,%ecx                  # owner_events = 1
 *     mov  $0x1,%r9d                  # num_modifiers = 1
 *     call *(%r10)                    # -> PC = 0 when the slot is NULL
 *
 *   The slot (UnityPlayer+0x2026358) is filled by the resolver at
 *   UnityPlayer+0x194162a from the name at UnityPlayer+0x294a76, i.e.
 *   "XIGrabTouchBegin", out of the libXi.so.6 handle.  That dlopen failed
 *   ("libXi.so.6: cannot open shared object file"), so the resolver stored
 *   NULL and the call site does not test it: the process died with
 *   PC=0 and the Xlib Display* in rdi (ucontext dump, launch-log §19.2).
 *
 *   A missing DSO is not survivable here the way a missing *extension* is:
 *   XQueryExtension("XInputExtension") already answers False and Unity still
 *   calls the entry point.  So the soname must exist and every name in
 *   Unity's XI list must resolve to a real function that answers "no such
 *   device, no such grab".
 *
 * THE CONTRACT (same discipline as udev_null.c / alsa_null.c)
 *
 *   - XIQueryVersion -> BadRequest (1), *major=2 *minor=0: the server has no
 *     XInput extension, so no version can be negotiated.
 *   - XIQueryDevice  -> NULL with *ndevices_return = 0 (zero devices).
 *   - XIGetSelectedEvents -> NULL with *num_masks_return = 0.
 *   - XISelectEvents  -> 0 (Success) and does nothing: there is no XI event
 *     to select, and the shim's event pump speaks no XI wire protocol.
 *   - XIGrabTouchBegin -> Failure (1): no touch device exists, so a touch
 *     grab cannot be granted.  Unity takes its no-touch path (which is what
 *     it would do on any desktop without a touchscreen).
 *   - XIUngrabTouchBegin -> Success (0): nothing was grabbed, nothing to undo.
 *   - XIFreeDeviceInfo / XIBarrierReleasePointer -> no-ops.
 *
 *   Nothing blocks, nothing allocates, no libc, no DT_NEEDED: the compat
 *   loader maps this DSO before ld.so has finished (same rule as libX11.so.6).
 */
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long PointerBarrier;
typedef int Bool;
typedef int Status;

#define XAPI __attribute__((visibility("default")))

#define XI_SUCCESS 0
#define XI_FAILURE 1
#define XI_BADREQUEST 1

/* fd 2, raw: the DSO has no libc and the trace must not allocate.  Unity
 * dup2()s its Player.log over fd 1/2, so these rows land in the engine's log
 * and on the serial console, in order, with no stdio lock behind them. */
static void xi_row(const char *s) {
  unsigned long n = 0;
  while (s[n]) n++;
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(1L), "D"(2), "S"(s), "d"(n)
                   : "rcx", "r11", "memory");
  (void)r;
}

XAPI Status XIQueryVersion(Display *dpy, int *major, int *minor) {
  (void)dpy;
  xi_row("[XI] XIQueryVersion -> BadRequest (no XInput extension)\n");
  if (major) *major = 2;
  if (minor) *minor = 0;
  return XI_BADREQUEST;
}

XAPI void *XIQueryDevice(Display *dpy, int deviceid, int *ndevices_return) {
  (void)dpy; (void)deviceid;
  if (ndevices_return) *ndevices_return = 0;
  return 0;
}

XAPI void XIFreeDeviceInfo(void *info) { (void)info; }

/* XIEventMask { int deviceid; int mask_len; unsigned char *mask; } */
XAPI int XISelectEvents(Display *dpy, Window win, void *masks, int num_masks) {
  (void)dpy; (void)win; (void)masks; (void)num_masks;
  xi_row("[XI] XISelectEvents -> Success (no XI events on this display)\n");
  return XI_SUCCESS;
}

XAPI void *XIGetSelectedEvents(Display *dpy, Window win, int *num_masks) {
  (void)dpy; (void)win;
  if (num_masks) *num_masks = 0;
  return 0;
}

/* int XIGrabTouchBegin(Display*, int deviceid, Window, int owner_events,
 *                      XIEventMask *mask, int num_modifiers,
 *                      XIGrabModifiers *modifiers) */
XAPI int XIGrabTouchBegin(Display *dpy, int deviceid, Window grab_window,
                          int owner_events, void *mask, int num_modifiers,
                          void *modifiers) {
  (void)dpy; (void)num_modifiers; (void)modifiers;
  /* The measured call is (dpy, 0, <window>, 1, mask, 1, mods); name the two
   * values that identify it so a later ladder row is self-explaining. */
  char b[96];
  const char *p = "[XI] XIGrabTouchBegin dev=";
  int i = 0;
  while (*p && i < 90) b[i++] = *p++;
  b[i++] = (char)('0' + (deviceid % 10));
  p = " win=0x";
  while (*p && i < 90) b[i++] = *p++;
  unsigned long v = (unsigned long)grab_window;
  char t[16];
  int n = 0;
  if (!v) t[n++] = '0';
  while (v) { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; }
  while (n && i < 90) b[i++] = t[--n];
  p = " owner=";
  while (*p && i < 90) b[i++] = *p++;
  b[i++] = (char)('0' + (owner_events ? 1 : 0));
  p = " -> Failure (no touch device)\n";
  while (*p && i < 90) b[i++] = *p++;
  b[i] = 0;
  xi_row(b);
  return XI_FAILURE;
}

/* Status XIUngrabTouchBegin(Display*, int deviceid, Window, int num_modifiers,
 *                           XIGrabModifiers *modifiers) */
XAPI Status XIUngrabTouchBegin(Display *dpy, int deviceid, Window grab_window,
                               int num_modifiers, void *modifiers) {
  (void)dpy; (void)deviceid; (void)grab_window;
  (void)num_modifiers; (void)modifiers;
  xi_row("[XI] XIUngrabTouchBegin -> Success (nothing was grabbed)\n");
  return XI_SUCCESS;
}

/* Lives in libXi on a real system; libX11.so.6 carries a copy too because
 * Unity's table resolved it before this DSO existed.  Both are no-ops. */
XAPI void XIBarrierReleasePointer(Display *dpy, int deviceid,
                                  PointerBarrier barrier, unsigned int eventid) {
  (void)dpy; (void)deviceid; (void)barrier; (void)eventid;
}
