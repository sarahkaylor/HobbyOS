#ifndef HB_TERM_H
#define HB_TERM_H

/* term.h — minimal <term.h> for the HobbyOS curses shim.
 *
 * Upstream nano includes <term.h> to reach tgetstr()/tigetstr() (the
 * ncurses low-level terminfo calls).  The shim implements a tiny subset of
 * those in hb_curses.c and declares them in curses.h, so the port's term.h
 * just pulls that in.  There is no terminfo database on HobbyOS.
 */

#include "curses.h"

#endif /* HB_TERM_H */
