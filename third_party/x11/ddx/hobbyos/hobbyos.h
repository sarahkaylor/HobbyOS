/* hw/hobbyos/hobbyos.h -- shared declarations of the HobbyOS DDX. */
#ifndef HOBBYOS_DDX_H
#define HOBBYOS_DDX_H

#define HB_INPUT_POLL_MS_DEFAULT 10

/* hbinput.c */
extern int hbInputPollMs;

/* hbscreen.c */
#include <stddef.h>
int hbConsoleWrite(const void *buf, size_t len);   /* os/log.c (patched) */
int hbScreenWidth(void);
int hbScreenHeight(void);

/* xkb/ddxLoad.c (patches/xserver): keymap file loaded instead of running
 * xkbcomp. */
extern const char *XkbHobbyKeymapFile;

#endif
