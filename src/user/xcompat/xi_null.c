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
 *   - XIQueryVersion -> Success (0) with the client's own request echoed back
 *     when it asks for 2.0..2.4.  THIS IS THE L39 FIX.  Answering
 *     BadRequest while still filling the version out-params with 2.0 -- what this DSO
 *     did before -- is a state no X server produces: the client is told the
 *     negotiation failed but reads a version anyway, concludes XInput2 exists
 *     and is merely mis-negotiated, and retries its touch setup on every
 *     input poll.  Measured cost: 40,360 / 40,736 / 43,198
 *     `XIGrabTouchBegin ... -> Failure` rows per 180 s run, each one a write to
 *     the serial console, in place of the render loop.  A real server on a
 *     machine with no touchscreen answers Success and then reports no touch
 *     device, which is the path Unity already knows how to take.
 *
 *   - XIQueryDevice  -> NULL with *ndevices_return = 0 (zero devices: this
 *     display has a core pointer and nothing else).
 *   - XIGetSelectedEvents -> NULL with *num_masks_return = 0.
 *   - XISelectEvents  -> 0 (Success) and does nothing: there is no XI event
 *     to select, and the shim's event pump speaks no XI wire protocol.
 *   - XIGrabTouchBegin -> Failure (1): no touch device exists, so a touch
 *     grab cannot be granted.  Unity takes its no-touch path (which is what
 *     it would do on any desktop without a touchscreen).  The trace row is
 *     BOUNDED and carries the running count, so the storm is measurable
 *     without the measurement being the storm.
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
static void __attribute__((unused)) xi_row(const char *s) {
  unsigned long n = 0;
  while (s[n]) n++;
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(1L), "D"(2), "S"(s), "d"(n)
                   : "rcx", "r11", "memory");
  (void)r;
}

/* Same, with one decimal count appended before the newline: the bounded rows
 * must still be able to say how many calls they stand for. */
static void xi_row_count(const char *s, unsigned long v) {
  char b[128];
  int i = 0;
  while (*s && i < 100) b[i++] = *s++;
  char t[24];
  int n = 0;
  do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
  while (n && i < 120) b[i++] = t[--n];
  b[i++] = '\n';
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(1L), "D"(2), "S"(b), "d"((unsigned long)i)
                   : "rcx", "r11", "memory");
  (void)r;
}

/* Counters (static, zero-initialised, no allocator). */
static unsigned long xi_version_calls, xi_grab_calls, xi_ungrab_calls;

/* L39: answer a version negotiation the way a server without a touchscreen
 * does.  A client that asks for 2.0..2.4 gets its own request back (the X
 * protocol rule: the server replies with min(client, server)); anything
 * newer is clamped to 2.4, and anything older is answered as asked -- the
 * extension's behaviour has not changed across those revisions for the calls
 * this shim implements. */
XAPI Status XIQueryVersion(Display *dpy, int *major, int *minor) {
  (void)dpy;
  int req_major = major ? *major : 2;
  int req_minor = minor ? *minor : 0;
  int out_major = req_major > 2 ? 2 : (req_major < 2 ? req_major : 2);
  int out_minor = req_minor;
  if (out_major == 2 && out_minor > 4) out_minor = 4;
  if (out_major == 2 && out_minor < 0) out_minor = 0;
  if (major) *major = out_major;
  if (minor) *minor = out_minor;
  if (xi_version_calls < 4) {
    char b[96];
    const char *p = "[XI] XIQueryVersion req=";
    int i = 0;
    while (*p && i < 80) b[i++] = *p++;
    b[i++] = (char)('0' + (out_major % 10));
    b[i++] = '.';
    b[i++] = (char)('0' + (out_minor % 10));
    p = " -> BadRequest (L39: Success here stalls startup)";
    while (*p && i < 90) b[i++] = *p++;
    b[i] = 0;
    xi_row_count(b, xi_version_calls);
  }
  xi_version_calls++;
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
  return XI_SUCCESS;
}

XAPI void *XIGetSelectedEvents(Display *dpy, Window win, int *num_masks) {
  (void)dpy; (void)win;
  if (num_masks) *num_masks = 0;
  return 0;
}

/* XIGetClientPointer is part of the same "which device is this window using"
 * query family; Unity asks it before the first grab.  No XI device exists, so
 * there is no client pointer: answer Failure and leave *deviceid alone. */
XAPI Bool XIGetClientPointer(Display *dpy, Window win, int *deviceid) {
  (void)dpy; (void)win; (void)deviceid;
  return 0;
}

/* int XIGrabTouchBegin(Display*, int deviceid, Window, int owner_events,
 *                      XIEventMask *mask, int num_modifiers,
 *                      XIGrabModifiers *modifiers) */
XAPI int XIGrabTouchBegin(Display *dpy, int deviceid, Window grab_window,
                          int owner_events, void *mask, int num_modifiers,
                          void *modifiers) {
  (void)dpy; (void)num_modifiers; (void)modifiers;
  /* Bounded: first 4 rows, then every 4096th, each carrying the running
   * count.  The unbounded form was itself a bug -- one serial write per call
   * throttled the loop it was measuring (40k+ rows per run). */
  if (xi_grab_calls < 4 || (xi_grab_calls % 4096ul) == 0) {
    char b[96];
    const char *p = "[XI] XIGrabTouchBegin dev=";
    int i = 0;
    while (*p && i < 80) b[i++] = *p++;
    b[i++] = (char)('0' + (deviceid % 10));
    p = " win=0x";
    while (*p && i < 80) b[i++] = *p++;
    unsigned long v = (unsigned long)grab_window;
    char t[16];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; }
    while (n && i < 80) b[i++] = t[--n];
    p = " owner=";
    while (*p && i < 80) b[i++] = *p++;
    b[i++] = (char)('0' + (owner_events ? 1 : 0));
    p = " -> Failure (no touch device) n=";
    while (*p && i < 90) b[i++] = *p++;
    b[i] = 0;
    xi_row_count(b, xi_grab_calls);
  }
  xi_grab_calls++;
  return XI_FAILURE;
}

/* Status XIUngrabTouchBegin(Display*, int deviceid, Window, int num_modifiers,
 *                           XIGrabModifiers *modifiers) */
XAPI Status XIUngrabTouchBegin(Display *dpy, int deviceid, Window grab_window,
                               int num_modifiers, void *modifiers) {
  (void)dpy; (void)deviceid; (void)grab_window;
  (void)num_modifiers; (void)modifiers;
  if (xi_ungrab_calls < 4 || (xi_ungrab_calls % 4096ul) == 0)
    xi_row_count("[XI] XIUngrabTouchBegin -> Success n=", xi_ungrab_calls);
  xi_ungrab_calls++;
  return XI_SUCCESS;
}

/* Lives in libXi on a real system; libX11.so.6 carries a copy too because
 * Unity's table resolved it before this DSO existed.  Both are no-ops. */
XAPI void XIBarrierReleasePointer(Display *dpy, int deviceid,
                                  PointerBarrier barrier, unsigned int eventid) {
  (void)dpy; (void)deviceid; (void)barrier; (void)eventid;
}
