/*
 * Server-stage overlay for <signal.h>.  Linux numbers for the signals the
 * X server installs handlers for / blocks; the HobbyOS kernel never raises
 * them (sigaction on an unsupported number just fails, which os/osinit.c
 * only logs).  SI_USER: the Linux si_code value.
 */
#ifndef HB_XSERVER_SIGNAL_OVERLAY_H
#define HB_XSERVER_SIGNAL_OVERLAY_H
#include_next <signal.h>
#ifndef SIGXCPU
#define SIGXCPU 24
#endif
#ifndef SIGXFSZ
#define SIGXFSZ 25
#endif
#ifndef SIGVTALRM
#define SIGVTALRM 26
#endif
#ifndef SIGSYS
#define SIGSYS 31
#endif
#ifndef SI_USER
#define SI_USER 0
#endif
#endif
