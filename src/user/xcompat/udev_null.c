/*
 * udev_null.c - libudev.so.1: "this machine has no enumerable devices", the
 * answer SDL's joystick backend must be able to consume without failing.
 *
 * WHY THIS FILE EXISTS (measured, docs/rimworld-launch-log.md §11)
 *
 *   Unity's embedded SDL2 initialises its subsystems in order: timer, audio,
 *   video, joystick, ...  The joystick stage calls SDL_UDEV_Init(), which
 *   dlopen()s libudev.so.1 and, if it cannot get a udev handle + monitor,
 *   returns -1.  SDL_Init() then *rolls back* every subsystem it already
 *   started -- including the X11 video driver that had just finished
 *   X11_InitModes + X11_InitKeyboard successfully.  The player's next call
 *   is SDL_GetNumVideoDisplays(), which answers with
 *
 *     Error getting num native displays: Video subsystem has not been
 *     initialized
 *
 *   and the engine parks; there is never a second XOpenDisplay, never a
 *   window, never a frame.  So a *missing* libudev is a hard failure for a
 *   game that will never see a joystick on this OS.
 *
 *   Host A/B is the discriminator: with libudev hidden from dlopen the same
 *   line appears verbatim (tools/rw/udev-absent-probe.c), and the host engine
 *   recovers past it.  With libudev present the host never prints it at all.
 *
 * THE CONTRACT
 *
 *   Zero devices, forever, and every call cheap and non-blocking:
 *
 *     - udev_new()                    -> a non-NULL sentinel handle
 *     - udev_enumerate_*              -> success, then an EMPTY list
 *                                        (get_list_entry() == NULL)
 *     - udev_device_new_*             -> NULL (there is no device to open)
 *     - udev_monitor_new_from_netlink -> a sentinel monitor whose fd is the
 *                                        read end of a pipe that is never
 *                                        written to; poll() never fires
 *     - receive_device()              -> NULL (the documented "nothing
 *                                        happened" return, not an error)
 *
 *   This is the same shape as alsa_null.c's "enumerate zero devices, refuse
 *   the open": the failure the caller already handles, chosen deliberately
 *   instead of a half-working device list.
 *
 * WHY THE MONITOR FD IS A PIPE AND NOT -1
 *
 *   SDL's SDL_udev.c can run in two modes.  If udev_monitor_new_from_netlink
 *   returns NULL it sets udev_hotplug_ok = SDL_FALSE and continues happily --
 *   that is the configuration this shim could have used.  But the *enumerate*
 *   path (SDL_UDEV_Scan, used by the joystick/HIDAPI device discovery) needs
 *   a real udev handle and returns -1 when udev_ctx is NULL.  So a monitor
 *   must exist.  Given a monitor, SDL stores udev_watch.fd =
 *   udev_monitor_get_fd(...) and adds that fd to its poll set; a negative fd
 *   is ignored by poll() but is also indistinguishable from an error to any
 *   caller that checks, and several SDL versions treat fd < 0 as "hotplug
 *   broken" and disable the watch.  A pipe read end with its write end held
 *   open is the exact semantics of "a monitor that never signals": readable
 *   never, EOF never, poll() blocks (or times out) — measured on the host in
 *   both variants, see EXPORTS.md §7.3.
 *
 * WHY NOTHING IS ALLOCATED
 *
 *   Every object is a static sentinel byte used only as an address.  There is
 *   no malloc (this DSO has no libc and no DT_NEEDED), so there is nothing to
 *   leak, and udev_new() returning the same pointer every time is a property
 *   the host test asserts.  unref() never frees, which makes double-unref and
 *   unref-after-failed-construction safe.  The pipe is created once, lazily,
 *   and deliberately never closed: a poll() in another thread holding that fd
 *   must not be able to see it recycled.
 *
 * NULL-SAFETY
 *
 *   SDL's error paths call unref() on things that may be NULL, and its
 *   device walk calls get_action()/get_sysname() on whatever
 *   receive_device() returned (NULL when nothing arrived).  Every entry point
 *   here accepts NULL for every pointer argument and returns the harmless
 *   answer rather than faulting.
 *
 * Built by src/user/xcompat/Makefile.inc: -shared -fPIC -nostdlib, no
 * DT_NEEDED, SONAME libudev.so.1 (real soname, VFAT long names work).
 */

#include "udev_abi.h"

#define UDEVAPI __attribute__((visibility("default")))

_Static_assert(sizeof(void *) == 8, "the compat class is LP64 only");
_Static_assert(sizeof(udev_devnum_t) == 8, "dev_t must be 8 bytes on glibc/x86-64");

/* ---- raw syscalls (x86-64 Linux numbers) ----------------------------- *
 * Same reasoning as libx11.c: the DSO must not reach into the process's libc
 * for anything.  pipe2 is the only syscall needed. */
#define SYS_pipe2 293

static long un_syscall2(long n, long a, long b) {
  long r;
  register long r10 __asm__("r10") = 0;
  register long r8 __asm__("r8") = 0;
  register long r9 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(n), "D"(a), "S"(b), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return r;
}


/* ---- optional trace (measurement builds only: -DUDEV_TRACE) ----------- */
#ifdef UDEV_TRACE
#define SYS_write 1
static long un_syscall3(long n, long a, long b, long c) {
  long r;
  register long r10 __asm__("r10") = 0;
  register long r8 __asm__("r8") = 0;
  register long r9 __asm__("r9") = 0;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return r;
}
static void utr(const char *s) {
  long n = 0;
  while (s[n]) n++;
  (void)un_syscall3(SYS_write, 2, (long)s, n);
}
#else
#define utr(s) ((void)0)
#endif

/* ---- sentinels -------------------------------------------------------- *
 * Never dereferenced, never written.  Distinct static addresses so a caller
 * can hold, compare and pass them back; nothing else is promised. */
static char udev_null_lib_tag;
static char udev_null_enum_tag;
static char udev_null_monitor_tag;

/* The hotplug pipe: [0] = read end handed to SDL, [1] = write end that is
 * never used (held open so a reader never sees EOF).  -1 = creation failed,
 * in which case get_fd() honestly reports -1. */
static int udev_null_pipe[2] = { -1, -1 };

/* Lazily create the pipe.  Deliberately not atomic: SDL calls this once from
 * its own init, and a lost race can only produce a second pipe, never a
 * partially published one (the fds are written before they are read). */
static void udev_null_ensure_pipe(void) {
  if (udev_null_pipe[0] >= 0 || udev_null_pipe[1] >= 0) return;
  int fds[2] = { -1, -1 };
  long r = un_syscall2(SYS_pipe2, (long)fds, 0 /* no flags: never inherited
                                                 * by an exec we do not do */);
  if (r < 0) return;
  udev_null_pipe[0] = fds[0];
  udev_null_pipe[1] = fds[1];
}

/* ====================================================================== *
 * library handle
 * ====================================================================== */

UDEVAPI udev *udev_new(void) {
  utr("[UD] udev_new\n");
  return (udev *)&udev_null_lib_tag;
}

UDEVAPI udev *udev_unref(udev *u) {
  (void)u;                        /* NULL-safe and free-nothing: the handle
                                   * is a sentinel */
  return 0;
}

UDEVAPI udev *udev_ref(udev *u) {
  (void)u;
  return u ? u : (udev *)&udev_null_lib_tag;
}

/* ====================================================================== *
 * enumeration: always successful, always empty
 * ====================================================================== */

UDEVAPI udev_enumerate *udev_enumerate_new(udev *u) {
  utr("[UD] enumerate_new\n");
  (void)u;                        /* a NULL udev still yields a usable
                                   * enumerator: failing here would make a
                                   * caller treat "no udev" as fatal, which
                                   * is the whole bug this shim removes */
  return (udev_enumerate *)&udev_null_enum_tag;
}

UDEVAPI udev_enumerate *udev_enumerate_unref(udev_enumerate *e) {
  (void)e;
  return 0;
}

UDEVAPI udev *udev_enumerate_get_udev(udev_enumerate *e) {
  (void)e;
  return (udev *)&udev_null_lib_tag;
}

UDEVAPI int udev_enumerate_add_match_subsystem(udev_enumerate *e,
                                               const char *subsystem) {
  (void)e; (void)subsystem;
  return 0;
}

UDEVAPI int udev_enumerate_add_match_property(udev_enumerate *e,
                                              const char *key,
                                              const char *value) {
  (void)e; (void)key; (void)value;
  return 0;
}

UDEVAPI int udev_enumerate_add_match_tag(udev_enumerate *e, const char *tag) {
  (void)e; (void)tag;
  return 0;
}

UDEVAPI int udev_enumerate_add_match_sysattr(udev_enumerate *e, const char *key,
                                             const char *value) {
  (void)e; (void)key; (void)value;
  return 0;
}

UDEVAPI int udev_enumerate_add_nomatch_subsystem(udev_enumerate *e,
                                                 const char *subsystem) {
  (void)e; (void)subsystem;
  return 0;
}

UDEVAPI int udev_enumerate_add_nomatch_sysattr(udev_enumerate *e,
                                               const char *key,
                                               const char *value) {
  (void)e; (void)key; (void)value;
  return 0;
}

UDEVAPI int udev_enumerate_add_syspath(udev_enumerate *e, const char *syspath) {
  (void)e; (void)syspath;
  return 0;
}

UDEVAPI int udev_enumerate_scan_devices(udev_enumerate *e) {
  utr("[UD] scan_devices\n");
  (void)e;
  return 0;                       /* success, with nothing found */
}

UDEVAPI udev_list_entry *udev_enumerate_get_list_entry(udev_enumerate *e) {
  utr("[UD] get_list_entry\n");
  (void)e;
  return 0;                       /* the empty list */
}

/* ====================================================================== *
 * list entries: the list is NULL, so every accessor answers the empty case
 * ====================================================================== */

UDEVAPI udev_list_entry *udev_list_entry_get_next(const udev_list_entry *entry) {
  (void)entry;
  return 0;
}

UDEVAPI udev_list_entry *udev_list_entry_get_by_name(const udev_list_entry *entry,
                                                     const char *name) {
  (void)entry; (void)name;
  return 0;
}

UDEVAPI const char *udev_list_entry_get_name(const udev_list_entry *entry) {
  (void)entry;
  return 0;
}

UDEVAPI const char *udev_list_entry_get_value(const udev_list_entry *entry) {
  (void)entry;
  return 0;
}

UDEVAPI const char *udev_list_entry_get_sysname(const udev_list_entry *entry) {
  (void)entry;
  return 0;
}

/* ====================================================================== *
 * devices: none exist, and every getter is safe on the NULL device that
 * callers may still hold from a failed open
 * ====================================================================== */

UDEVAPI udev_device *udev_device_new_from_syspath(udev *u, const char *syspath) {
  utr("[UD] device_new_from_syspath\n");
  (void)u; (void)syspath;
  return 0;                       /* no sysfs device is enumerable here */
}

UDEVAPI udev_device *udev_device_new_from_devnum(udev *u, char type,
                                                 udev_devnum_t devnum) {
  (void)u; (void)type; (void)devnum;
  return 0;
}

UDEVAPI udev_device *udev_device_new_from_subsystem_sysname(udev *u,
                                                            const char *subsystem,
                                                            const char *sysname) {
  (void)u; (void)subsystem; (void)sysname;
  return 0;
}

UDEVAPI udev_device *udev_device_unref(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI udev_device *udev_device_ref(udev_device *d) {
  (void)d;
  return d;
}

UDEVAPI udev *udev_device_get_udev(udev_device *d) {
  (void)d;
  return (udev *)&udev_null_lib_tag;
}

UDEVAPI const char *udev_device_get_syspath(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI const char *udev_device_get_sysname(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI const char *udev_device_get_devnode(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI udev_devnum_t udev_device_get_devnum(udev_device *d) {
  (void)d;
  return 0;                       /* makedev(0,0); never a real device */
}

UDEVAPI const char *udev_device_get_subsystem(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI const char *udev_device_get_devtype(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI const char *udev_device_get_driver(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI const char *udev_device_get_action(udev_device *d) {
  (void)d;
  return 0;                       /* "no event" (NULL is the documented
                                   * answer for a device not from a monitor) */
}

UDEVAPI const char *udev_device_get_sysattr_value(udev_device *d,
                                                  const char *sysattr) {
  (void)d; (void)sysattr;
  return 0;
}

UDEVAPI const char *udev_device_get_property_value(udev_device *d,
                                                   const char *key) {
  (void)d; (void)key;
  return 0;
}

UDEVAPI udev_device *udev_device_get_parent(udev_device *d) {
  (void)d;
  return 0;
}

UDEVAPI udev_device *udev_device_get_parent_with_subsystem_devtype(
    udev_device *d, const char *subsystem, const char *devtype) {
  (void)d; (void)subsystem; (void)devtype;
  return 0;
}

UDEVAPI int udev_device_get_is_initialized(udev_device *d) {
  (void)d;
  return 1;                       /* vacuously: nothing is still initialising */
}

/* ====================================================================== *
 * monitor: valid, enabled, and silent forever
 * ====================================================================== */

UDEVAPI udev_monitor *udev_monitor_new_from_netlink(udev *u, const char *name) {
  utr("[UD] monitor_new\n");
  (void)u; (void)name;            /* "udev" or "kernel": neither exists here */
  udev_null_ensure_pipe();
  return (udev_monitor *)&udev_null_monitor_tag;
}

UDEVAPI udev_monitor *udev_monitor_unref(udev_monitor *m) {
  (void)m;                        /* the pipe fds stay open on purpose: a
                                   * poller in another thread must never see
                                   * them closed or recycled */
  return 0;
}

UDEVAPI udev *udev_monitor_get_udev(udev_monitor *m) {
  (void)m;
  return (udev *)&udev_null_lib_tag;
}

UDEVAPI int udev_monitor_get_fd(udev_monitor *m) {
  utr("[UD] monitor_get_fd\n");
  (void)m;
  return udev_null_pipe[0];       /* read end; -1 if pipe2 failed */
}

UDEVAPI int udev_monitor_enable_receiving(udev_monitor *m) {
  utr("[UD] monitor_enable\n");
  (void)m;
  return 0;                       /* "receiving enabled" -- nothing arrives */
}

UDEVAPI int udev_monitor_filter_add_match_subsystem_devtype(
    udev_monitor *m, const char *subsystem, const char *devtype) {
  utr("[UD] monitor_filter\n");
  (void)m; (void)subsystem; (void)devtype;
  return 0;
}

UDEVAPI int udev_monitor_filter_add_match_tag(udev_monitor *m, const char *tag) {
  (void)m; (void)tag;
  return 0;
}

UDEVAPI int udev_monitor_filter_update(udev_monitor *m) {
  (void)m;
  return 0;
}

UDEVAPI int udev_monitor_filter_remove(udev_monitor *m) {
  (void)m;
  return 0;
}

UDEVAPI int udev_monitor_set_receive_buffer_size(udev_monitor *m, int size) {
  (void)m; (void)size;
  return 0;
}

UDEVAPI udev_device *udev_monitor_receive_device(udev_monitor *m) {
  utr("[UD] monitor_receive\n");
  (void)m;
  return 0;                       /* the documented "nothing pending" */
}
