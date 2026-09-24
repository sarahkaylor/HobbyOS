#ifndef DESKTOP_DAMAGE_H
#define DESKTOP_DAMAGE_H

/* Damage bookkeeping for the desktop compositor (desktop.c).
 *
 * The desktop captures its "chrome" (pointer, menus, taskbar clock, focus,
 * the window set) once per frame and diffs it against the previous frame.
 * Whatever differs - plus the per-window text damage window.c tracks - is
 * the only thing the next frame repaints.  The pure diff below is exposed
 * so the host test (desktop_damage_test.c) can drive it directly. */

#include "window.h"   /* MAX_WINDOWS */

#define DMG_MAX 12

/* One repaint rectangle, in screen pixels. */
struct desktop_rect { int x, y, w, h; };

/* Everything that decides where chrome pixels go, captured once per frame.
 * A zero-size menu rectangle means "closed". */
struct desktop_chrome {
  int win_count;                     /* number of open windows          */
  int focus;                         /* focused window id (-1 = none)   */
  int cursor_x, cursor_y;            /* pointer position                */
  struct desktop_rect start_menu;    /* Apps menu panel (open: w/h > 0) */
  int start_sel, start_scroll;       /* highlight inside the panel      */
  struct desktop_rect rc_menu;       /* right-click menu                */
  struct desktop_rect app_menu;      /* per-window menu dropdown        */
  char clock[12];                    /* taskbar clock text              */
  int chrome_dirty[MAX_WINDOWS];     /* per window: title/menus changed */
};

/* Diff two chrome snapshots into repaint rectangles.
 * Returns -1 when the whole scene must be repainted (the window set or the
 * layout changed), else the number of rectangles written to `out`
 * (0 = nothing on screen moved).  Never writes more than `max` rects. */
int desktop_damage(const struct desktop_chrome *prev,
                   const struct desktop_chrome *cur,
                   struct desktop_rect *out, int max);

#endif
