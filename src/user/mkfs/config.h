/*
 * config.h — HobbyOS port configuration for dosfstools 4.2 mkfs.fat.
 *
 * Upstream, this file does not exist: configure emits every HAVE_* as a
 * -D flag and src/version.h is generated from version.h.in.  The port keeps
 * the upstream sources byte-identical (see the marked patches listed below),
 * pins the feature set here, and force-includes it with `-include` on every
 * port object (the Makefile rule), the same role configure's checks play.
 *
 * Convention: this mirrors the autotools config.h.in layout — a feature is
 * selected by the macro being PRESENT, never by a zero-valued #define
 * (upstream tests are #ifdef HAVE_X / #if defined(HAVE_X), which a zero
 * define would wrongly satisfy).  Every "#undef HAVE_X" line below is a
 * deliberately-unsatisfied sysroot-gap probe, documented in the comment.
 *
 * Port decisions (each is a sysroot-gap resolution from L4's dosfstools
 * audit, docs/selfhost/buildtools-audit.md §5):
 *
 *   HAVE_ENDIAN_H / HAVE_SYS_ENDIAN_H / HAVE_LIBKERN_OSBYTEORDER_H — undef:
 *                        the sysroot has no <endian.h>.  endian_compat.h
 *                        gains a HobbyOS fallback arm (marked patch) armed
 *                        by HAVE_HOBBYOS_ENDIAN that pins the two
 *                        little-endian HobbyOS arches.
 *   HAVE_ICONV           — undef: no iconv in the sysroot; charconv.c's
 *                          #else arm (internal CP850 tables) is exactly what
 *                          volume labels need and needs no iconv at all.
 *   HAVE_LINUX_LOOP_H    — undef: no /dev/loop; the LOOP_GET_STATUS64 probe
 *                          in device_info.c compiles out (Linux-ism).
 *   HAVE_DECL_GETMNTENT / HAVE_DECL_GETMNTINFO
 *                        — undef: no mount table on the OS;
 *                          is_device_mounted() becomes the upstream no-op
 *                          return (0), the audit's "check_mount → no-op".
 *   HAVE_SYS_IOCCOM_H / HAVE_SYS_MKDEV_H
 *                        — undef: Solaris shims, never applicable.
 *   HAVE_TERMIOS_H       — undef: no sysroot <termios.h>; common.c's
 *                          terminal helpers use the port's hb_termios.h
 *                          stub pair (pattern from L4 §4.3):
 *                          tcgetattr/tcsetattr return -1/ENOTTY, get_line()
 *                          falls back to printf + fgets (never reached by
 *                          mkfs.fat).
 *   HAVE_VASPRINTF       — present: the sysroot libc implements vasprintf()
 *                          (src/libc/src/stdio.c), so common.c uses it and
 *                          skips its own static copy.
 *   HAVE_ALARM           — undef: no sysroot alarm(); the SIGALRM progress
 *                          tick in mkfs.fat.c's bad-block check (-c,
 *                          optional) is compiled out (marked patch).  The
 *                          bad-block scan itself still works, it just never
 *                          prints the periodic progress counter.
 *
 * Marked patches in the transcribed sources (each tagged "HobbyOS port
 * (W2-MKFS)"):
 *   mkfs.fat.c     — alarm()/alarm_intr/SIGALRM guarded by #if HAVE_ALARM.
 *   common.c       — <termios.h> include switches to hb_termios.h when
 *                    !HAVE_TERMIOS_H.
 *   device_info.c  — <sys/ioctl.h> and <sys/sysmacros.h> includes move
 *                    behind #ifdef __linux__ (they are only used by the
 *                    Linux sysfs block-device probe, which is compiled out
 *                    for the ARM/x64 userland).
 *   endian_compat.h— #else/#error tail becomes the HAVE_HOBBYOS_ENDIAN
 *                    fallback (explicit little-endian swaps).
 *
 * First-party glue (this port's own files, cstyle-clean):
 *   hb_termios.h   — struct termios + tcgetattr/tcsetattr stubs.
 *   hb_blkdev.c    — blkdev_get_*() geometry stubs returning -1/ENOTTY
 *                    (the regular-file path in device_info.c returns before
 *                    any of them is reached, so they are dead but linked).
 *   version.h      — the generated version.h, pinned to the 4.2 release.
 */

/* Present features */
#define HAVE_HOBBYOS_ENDIAN 1
#define HAVE_VASPRINTF 1        /* sysroot stdio implements vasprintf() (stdio.c) */

/* Absent sysroot surface (probe results, kept as upstream tests them) */
/* #undef HAVE_ALARM */
/* #undef HAVE_DECL_GETMNTENT */
/* #undef HAVE_DECL_GETMNTINFO */
/* #undef HAVE_ENDIAN_H */
/* #undef HAVE_ICONV */
/* #undef HAVE_LIBKERN_OSBYTEORDER_H */
/* #undef HAVE_LINUX_LOOP_H */
/* #undef HAVE_SYS_IOCCOM_H */
/* #undef HAVE_SYS_ENDIAN_H */
/* #undef HAVE_SYS_MKDEV_H */
/* #undef HAVE_TERMIOS_H */

/* PACKAGE_* mirror the autotools @PACKAGE_*@ substitutions made at build
 * time upstream (banner + getopt internals). */
#define PACKAGE "dosfstools"
#define PACKAGE_NAME "dosfstools"
#define PACKAGE_VERSION "4.2"
#define PACKAGE_STRING "dosfstools 4.2"
#define PACKAGE_BUGREPORT "https://github.com/dosfstools/dosfstools/issues"
