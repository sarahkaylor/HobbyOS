/*
 * X11-stack overlay for <unistd.h> (cross/include; see limits.h here).
 * The repo libc implements the POSIX.1c reentrant pwd/time/string calls
 * (getpwuid_r/getpwnam_r 5-argument forms, strtok_r, localtime_r...), but
 * does not advertise them; X.Org's Xos_r.h otherwise picks pre-POSIX
 * 4-argument getpw*_r shapes.  Kept out of the repo header so default-build
 * ports keep seeing exactly what they saw before.
 */
#ifndef HB_X11_UNISTD_OVERLAY_H
#define HB_X11_UNISTD_OVERLAY_H
#include_next <unistd.h>
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200809L
#endif
#endif
