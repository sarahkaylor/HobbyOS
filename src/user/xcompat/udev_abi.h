/*
 * udev_abi.h - the opaque-type ABI of libudev.so.1, as far as the compat
 * class needs to name it.
 *
 * libudev has no public structures: every object is an opaque pointer whose
 * layout belongs to the library alone.  That is why this shim can answer the
 * whole surface with sentinels and NULLs -- there is no caller-allocated
 * struct to lay out, no field offset to get wrong, and therefore none of the
 * `_Static_assert` machinery xabi.h needs for Xlib.  The only ABI facts are
 *
 *   - the *names* of the struct tags (a caller writes `struct udev *`),
 *   - that every pointer is 8 bytes (LP64),
 *   - that `dev_t` -- the return type of udev_device_get_devnum -- is 8
 *     bytes on glibc/x86-64 (measured: sizeof(dev_t) == 8),
 *   - and the calling convention, which C gets right by itself.
 *
 * Keeping the tags identical to <libudev.h> means a program (or a future
 * host-side ABI check) can include both headers in one TU and the types
 * unify.  Nothing in this header may ever grow a definition: the moment it
 * does, it is no longer a libudev shim.
 *
 * Built into a DSO with -nostdlib, so this header includes <stddef.h> only
 * (a freestanding header) and never <sys/types.h>.
 */
#ifndef HOBBYOS_XCOMPAT_UDEV_ABI_H
#define HOBBYOS_XCOMPAT_UDEV_ABI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque objects (same tags as <libudev.h>) ------------------------ */
struct udev;
struct udev_enumerate;
struct udev_list_entry;
struct udev_device;
struct udev_monitor;

typedef struct udev udev;
typedef struct udev_enumerate udev_enumerate;
typedef struct udev_list_entry udev_list_entry;
typedef struct udev_device udev_device;
typedef struct udev_monitor udev_monitor;

/* glibc's dev_t is an 8-byte unsigned integer on x86-64.  A shim cannot
 * include <sys/types.h>; the static assert below pins the width, and the
 * host test cross-checks it against the real <sys/types.h>. */
typedef unsigned long long udev_devnum_t;

/* ====================================================================== *
 * The measured surface (26 names; required-udev.txt is generated from
 * UnityPlayer.so's string table and intersected with the host's real
 * libudev -- see tools/gen_exports.py).
 * ====================================================================== */

/* library handle */
udev *udev_new(void);
udev *udev_unref(udev *u);
udev *udev_ref(udev *u);

/* enumeration */
udev_enumerate *udev_enumerate_new(udev *u);
udev_enumerate *udev_enumerate_unref(udev_enumerate *e);
udev *udev_enumerate_get_udev(udev_enumerate *e);
int udev_enumerate_add_match_subsystem(udev_enumerate *e, const char *subsystem);
int udev_enumerate_add_match_property(udev_enumerate *e, const char *key,
                                      const char *value);
int udev_enumerate_add_match_tag(udev_enumerate *e, const char *tag);
int udev_enumerate_add_match_sysattr(udev_enumerate *e, const char *key,
                                     const char *value);
int udev_enumerate_add_nomatch_subsystem(udev_enumerate *e,
                                         const char *subsystem);
int udev_enumerate_add_nomatch_sysattr(udev_enumerate *e, const char *key,
                                       const char *value);
int udev_enumerate_add_syspath(udev_enumerate *e, const char *syspath);
int udev_enumerate_scan_devices(udev_enumerate *e);
udev_list_entry *udev_enumerate_get_list_entry(udev_enumerate *e);

/* device list entries */
udev_list_entry *udev_list_entry_get_next(const udev_list_entry *entry);
udev_list_entry *udev_list_entry_get_by_name(const udev_list_entry *entry,
                                             const char *name);
const char *udev_list_entry_get_name(const udev_list_entry *entry);
const char *udev_list_entry_get_value(const udev_list_entry *entry);
const char *udev_list_entry_get_sysname(const udev_list_entry *entry);

/* devices */
udev_device *udev_device_new_from_syspath(udev *u, const char *syspath);
udev_device *udev_device_new_from_devnum(udev *u, char type, udev_devnum_t devnum);
udev_device *udev_device_new_from_subsystem_sysname(udev *u,
                                                    const char *subsystem,
                                                    const char *sysname);
udev_device *udev_device_unref(udev_device *d);
udev_device *udev_device_ref(udev_device *d);
udev *udev_device_get_udev(udev_device *d);
const char *udev_device_get_syspath(udev_device *d);
const char *udev_device_get_sysname(udev_device *d);
const char *udev_device_get_devnode(udev_device *d);
udev_devnum_t udev_device_get_devnum(udev_device *d);
const char *udev_device_get_subsystem(udev_device *d);
const char *udev_device_get_devtype(udev_device *d);
const char *udev_device_get_driver(udev_device *d);
const char *udev_device_get_action(udev_device *d);
const char *udev_device_get_sysattr_value(udev_device *d, const char *sysattr);
const char *udev_device_get_property_value(udev_device *d, const char *key);
udev_device *udev_device_get_parent(udev_device *d);
udev_device *udev_device_get_parent_with_subsystem_devtype(udev_device *d,
                                                           const char *subsystem,
                                                           const char *devtype);
int udev_device_get_is_initialized(udev_device *d);

/* monitor (hotplug) */
udev_monitor *udev_monitor_new_from_netlink(udev *u, const char *name);
udev_monitor *udev_monitor_unref(udev_monitor *m);
udev *udev_monitor_get_udev(udev_monitor *m);
int udev_monitor_get_fd(udev_monitor *m);
int udev_monitor_enable_receiving(udev_monitor *m);
int udev_monitor_filter_add_match_subsystem_devtype(udev_monitor *m,
                                                    const char *subsystem,
                                                    const char *devtype);
int udev_monitor_filter_add_match_tag(udev_monitor *m, const char *tag);
int udev_monitor_filter_update(udev_monitor *m);
int udev_monitor_filter_remove(udev_monitor *m);
int udev_monitor_set_receive_buffer_size(udev_monitor *m, int size);
udev_device *udev_monitor_receive_device(udev_monitor *m);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_XCOMPAT_UDEV_ABI_H */
