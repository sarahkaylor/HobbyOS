#ifndef HB_CURSES_H
#define HB_CURSES_H

/* Define the classic ncurses header guard too: real nano sources test
 * #ifdef _CURSES_H_ (e.g. place_the_cursor()'s wnoutrefresh(midwin)) to
 * decide whether certain refreshes are compiled in.  With it undefined
 * those paths vanish and per-keystroke screen updates never happen. */
#ifndef _CURSES_H_
#define _CURSES_H_
#endif

/* curses.h — the HobbyOS curses shim for ported ncurses programs.
 *
 * The sysroot has no ncurses (and no terminfo); this header + hb_curses.c
 * give ported full-screen programs a working subset of the ncurses API on
 * top of the HobbyOS desktop's terminal surface.
 *
 * How it works
 * ------------
 * A desktop window is line-oriented by default.  At startup the shim asks
 * the desktop to switch the window into TERMINAL MODE by printing the OSC
 * opt-in `ESC ] V 1 ~` and then reading until the desktop's size reply
 * `ESC ] S <rows>;<cols> ~` arrives (any keys typed before the reply are
 * stashed, not lost).  In terminal mode the desktop interprets the window's
 * output as ANSI (cursor addressing, SGR attributes, erases) and paints a
 * character grid, so the shim can render a full-screen UI:
 *
 *   - drawing calls (waddstr/wmove/...) update an in-memory cell grid per
 *     window; wnoutrefresh() copies a window's cells into a virtual screen;
 *   - doupdate() diffs the virtual screen against a physical-screen model
 *     (what the desktop is believed to show) and emits only the changed
 *     runs as `ESC [ <row> ; <col> H` + SGR + text;
 *   - wgetch() reads the window's stdin (a pipe from the desktop), parses
 *     ANSI key sequences back into KEY_* codes, and surfaces a live window
 *     resize (a fresh `ESC ] S` message) as KEY_RESIZE.
 *
 * The desktop reflows windows when other apps open/close; the shim then
 * gets a new size message, updates LINES/COLS, and nano rebuilds its
 * windows (its own KEY_RESIZE handling does the rest).
 *
 * Host tests replace the read/write/sleep transport (hb_set_io) so the
 * shim can be driven without a desktop.
 */

#include <stddef.h>
#include <stdbool.h>   /* real ncurses.h includes these; nano relies on them */
#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hb_win WINDOW;

#define ERR (-1)
#define OK 0
#define TRUE 1
#define FALSE 0

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- attributes ------------------------------------------------------- */
/* Only the bits matter; values are private to the shim. */
#define A_NORMAL      0x00000000u
#define A_ATTRIBUTES  0xffffffffu
#define A_STANDOUT    0x00010000u
#define A_UNDERLINE   0x00020000u
#define A_REVERSE     0x00040000u
#define A_BLINK       0x00080000u
#define A_DIM         0x00100000u
#define A_BOLD        0x00200000u
#define A_ALTCHARSET  0x00400000u
#define A_ITALIC      0x00800000u
#define A_PROTECT     0x01000000u
#define WA_ATTRIBUTES 0xffffffffu
#define COLOR_PAIR(n) (0x40000000u | ((unsigned long)(n) & 0xffu))
#define PAIR_NUMBER(a) 0

typedef unsigned long chtype;

/* ---- colors (present but monochrome: has_colors() is FALSE) ----------- */
#define COLORS 8
#define COLOR_PAIRS 64
#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_MAGENTA 5
#define COLOR_CYAN 6
#define COLOR_WHITE 7

/* ---- key codes (values identical to ncurses so nano's tables match) --- */
#define KEY_CODE_YES 0400
#define KEY_MIN 0401
#define KEY_DOWN 0402
#define KEY_UP 0403
#define KEY_LEFT 0404
#define KEY_RIGHT 0405
#define KEY_HOME 0406
#define KEY_BACKSPACE 0407
#define KEY_F0 0410
#define KEY_F(n) (KEY_F0 + (n))
#define KEY_DL 0510
#define KEY_IL 0511
#define KEY_DC 0512
#define KEY_IC 0513
#define KEY_EIC 0514
#define KEY_CLEAR 0515
#define KEY_EOS 0516
#define KEY_EOL 0517
#define KEY_SF 0520
#define KEY_SR 0521
#define KEY_NPAGE 0522
#define KEY_PPAGE 0523
#define KEY_STAB 0524
#define KEY_CTAB 0525
#define KEY_CATAB 0526
#define KEY_ENTER 0527
#define KEY_SRESET 0530
#define KEY_RESET 0531
#define KEY_PRINT 0532
#define KEY_LL 0533
#define KEY_A1 0534      /* upper left of keypad */
#define KEY_A3 0535      /* upper right of keypad */
#define KEY_B2 0536      /* center of keypad */
#define KEY_C1 0537      /* lower left of keypad */
#define KEY_C3 0540      /* lower right of keypad */
#define KEY_BTAB 0541
#define KEY_BEG 0542
#define KEY_CANCEL 0543
#define KEY_CLOSE 0544
#define KEY_COMMAND 0545
#define KEY_COPY 0546
#define KEY_CREATE 0547
#define KEY_END 0550
#define KEY_EXIT 0551
#define KEY_FIND 0552
#define KEY_HELP 0553
#define KEY_MARK 0554
#define KEY_MESSAGE 0555
#define KEY_MOVE 0556
#define KEY_NEXT 0557
#define KEY_OPEN 0560
#define KEY_OPTIONS 0561
#define KEY_PREVIOUS 0562
#define KEY_REDO 0563
#define KEY_REFERENCE 0564
#define KEY_REFRESH 0565
#define KEY_REPLACE 0566
#define KEY_RESTART 0567
#define KEY_RESUME 0570
#define KEY_SAVE 0571
#define KEY_SBEG 0572
#define KEY_SCANCEL 0573
#define KEY_SCOMMAND 0574
#define KEY_SCOPY 0575
#define KEY_SCREATE 0576
#define KEY_SDC 0577
#define KEY_SDL 0600
#define KEY_SELECT 0601
#define KEY_SEND 0602
#define KEY_SEOL 0603
#define KEY_SEXIT 0604
#define KEY_SFIND 0605
#define KEY_SHOME 0610
#define KEY_SIC 0611
#define KEY_SLEFT 0612
#define KEY_SMESSAGE 0613
#define KEY_SMOVE 0614
#define KEY_SNEXT 0615
#define KEY_SOPTIONS 0616
#define KEY_SPREVIOUS 0617
#define KEY_SPRINT 0620
#define KEY_SREDO 0621
#define KEY_SREPLACE 0622
#define KEY_SRIGHT 0623
#define KEY_SRSUME 0624
#define KEY_SSAVE 0625
#define KEY_SSUSPEND 0626
#define KEY_SUNDO 0627
#define KEY_SUSPEND 0630
#define KEY_UNDO 0631
#define KEY_MOUSE 0632
#define KEY_RESIZE 0633
#define KEY_MAX 0777

/* ---- the window type -------------------------------------------------- */
/* Not opaque: nano (and the getmaxyx/getyx macros) reach into the size and
 * cursor fields, exactly like with ncurses macros. */
struct hb_win {
  int rows, cols;       /* window dimensions            */
  int y, x;             /* position on the screen       */
  int cur_y, cur_x;     /* cursor, window coordinates   */
  int attr;             /* active attribute set         */
  int scroll;           /* scrollok flag                */
  unsigned char *ch;    /* rows*cols cell characters    */
  unsigned char *at;    /* rows*cols cell attributes    */
};

#define getmaxyx(win, y, x) ((y) = (win)->rows, (x) = (win)->cols)
#define getbegyx(win, y, x) ((y) = (win)->y, (x) = (win)->x)
#define getparyx(win, y, x) ((y) = -1, (x) = -1)
#define getyx(win, y, x) ((y) = (win)->cur_y, (x) = (win)->cur_x)

/* The screen size, maintained by the shim (and updated on resize). */
extern int LINES, COLS;

/* A match for nano's `if (initscr() == NULL) exit(1);` guard. */
extern WINDOW *stdscr;

/* ncurses' physical screen.  nano calls wrefresh(curscr) for a full
 * unconditional repaint; the shim treats it as such. */
extern WINDOW *curscr;

/* ---- lifecycle --------------------------------------------------------- */
WINDOW *initscr(void);
int endwin(void);
int isendwin(void);
int curs_set(int visibility);
int keypad(WINDOW *win, int flag);
int noecho(void);
int nonl(void);
int raw(void);
int noraw(void);
int cbreak(void);
int nocbreak(void);
int echo(void);
int halfdelay(int tenths);
int nodelay(WINDOW *win, int flag);
int notimeout(WINDOW *win, int flag);
int intrflush(WINDOW *win, int flag);
int scrollok(WINDOW *win, int flag);
int clearok(WINDOW *win, int flag);
int idlok(WINDOW *win, int flag);
int leaveok(WINDOW *win, int flag);
int nl(void);

/* ---- windows ----------------------------------------------------------- */
WINDOW *newwin(int rows, int cols, int y, int x);
int delwin(WINDOW *win);
int wmove(WINDOW *win, int y, int x);
int mvcur(int oldrow, int oldcol, int newrow, int newcol);
int wscrl(WINDOW *win, int n);
int wclrtoeol(WINDOW *win);
int wclrtobot(WINDOW *win);
int werase(WINDOW *win);
int wclear(WINDOW *win);
int erase(void);
int clear(void);
int wredrawln(WINDOW *win, int beg_line, int num_lines);
int wrefresh(WINDOW *win);
int wnoutrefresh(WINDOW *win);
int doupdate(void);
int refresh(void);
int redrawwin(WINDOW *win);

/* ---- drawing ----------------------------------------------------------- */
int waddch(WINDOW *win, const chtype ch);
int mvaddch(int y, int x, const chtype ch);
int mvwaddch(WINDOW *win, int y, int x, const chtype ch);
int waddstr(WINDOW *win, const char *str);
int mvaddstr(int y, int x, const char *str);
int mvwaddstr(WINDOW *win, int y, int x, const char *str);
int waddnstr(WINDOW *win, const char *str, int n);
int mvaddnstr(int y, int x, const char *str, int n);
int mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n);
int wprintw(WINDOW *win, const char *fmt, ...);
int mvprintw(int y, int x, const char *fmt, ...);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs,
            chtype tl, chtype tr, chtype bl, chtype br);
int box(WINDOW *win, chtype verch, chtype horch);

/* ---- attributes -------------------------------------------------------- */
int wattron(WINDOW *win, int attrs);
int wattroff(WINDOW *win, int attrs);
int wattrset(WINDOW *win, int attrs);
int wattr_get(WINDOW *win, int *attrs, short *pair, void *opts);
int wbkgdset(WINDOW *win, chtype ch);
int wbkgd(WINDOW *win, chtype ch);
int wstandend(WINDOW *win);
int wstandout(WINDOW *win);

/* ---- input ------------------------------------------------------------- */
int wgetch(WINDOW *win);
int getch(void);
int ungetch(int ch);
int beep(void);
int flash(void);
int napms(int ms);
int getsyx(int *y, int *x);
int setsyx(int y, int x);
int typeahead(int fd);

/* ---- colors (all monochrome no-ops; nano's ENABLE_COLOR is off) -------- */
int has_colors(void);
int start_color(void);
int use_default_colors(void);
int assume_default_colors(int fg, int bg);
int init_pair(short pair, short fg, short bg);
int color_content(short color, short *r, short *g, short *b);
int pair_content(short pair, short *fg, short *bg);
int init_color(short color, short r, short g, short b);
int can_change_color(void);

/* ---- termcap / escapes (mostly stubs; nano only wants "kb") ------------ */
char *tgetstr(const char *id, char **area);
char *tigetstr(const char *capname);
int tputs(const char *str, int affcnt, int (*putc_fn)(int));
int key_defined(const char *definition);
int define_key(const char *definition, int keycode);
int set_escdelay(int ms);

/* ---- mouse (no backend; compile-time stubs for source compatibility) --- */
typedef unsigned long mmask_t;
typedef struct {
  short id;
  int x, y, z;
  mmask_t bstate;
} MEVENT;
#define BUTTON1_RELEASED 0x00000001UL
#define BUTTON1_PRESSED 0x00000002UL
#define BUTTON1_CLICKED 0x00000004UL
#define REPORT_MOUSE_POSITION 0x10000000UL
#define ALL_MOUSE_EVENTS 0x1fffffffUL
mmask_t mousemask(mmask_t newmask, mmask_t *oldmask);
int mouseinterval(int wait);
int getmouse(MEVENT *event);
int ungetmouse(MEVENT *event);
int wmouse_trafo(const WINDOW *win, int *pY, int *pX, int to_screen);
int wenclose(const WINDOW *win, int y, int x);
int mvwgetch(WINDOW *win, int y, int x);

/* ---- HobbyOS extensions ------------------------------------------------ */
/* TRUE when the shim got a working terminal surface (desktop handshake
 * done).  nano's "standard input is not a terminal" check asks this
 * instead of isatty() — stdin here is always a pipe to the desktop. */
int hb_term_is_terminal(void);

/* Transport override (host tests).  Any pointer may be NULL to keep the
 * default.  read_block: reads >=1 byte, returns count or -1 on EOF/error.
 * read_now: returns >0 bytes, 0 when nothing is available.  write: full
 * write.  sleep_ms: sleep. */
struct hb_io {
  int (*read_block)(char *buf, int cap);
  int (*read_now)(char *buf, int cap);
  int (*write)(const char *buf, int len);
  void (*sleep_ms)(int ms);
};
void hb_set_io(const struct hb_io *io);

#ifdef __cplusplus
}
#endif

#endif /* HB_CURSES_H */
