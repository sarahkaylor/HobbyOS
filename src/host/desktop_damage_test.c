/*
 * desktop_damage_test.c - Host unit tests for the desktop's damage diff
 * (desktop_damage() in src/user/desktop.c, declared in
 * src/user_include/graphics/desktop_damage.h).
 *
 * Links obj/host_user_desktop.o (desktop.c compiled with -Dmain=desktop_main)
 * so the pure diff the frame loop uses is exercised directly: snapshots of
 * the desktop "chrome" go in, repaint rectangles come out.  The cases are
 * the ones the compositor relies on to be both cheap and correct:
 *
 *   - nothing changed                -> no damage at all
 *   - pointer moved                  -> old + new sprite rects only
 *   - focus changed                  -> both windows + the taskbar strip
 *   - window created/removed         -> -1 (the whole scene repaints)
 *   - menu opened/closed/moved       -> old + new menu rects
 *   - menu re-selected (same rect)   -> still a repaint of that rect
 *   - title/menu changed on a window -> window + taskbar (+ dropdown)
 *   - taskbar clock ticked           -> the clock cell only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"
#include "../user_include/graphics/window.h"
#include "../user_include/graphics/desktop_damage.h"

extern struct window windows[];
extern int num_windows;
extern int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd);
extern void wm_remove_window(int id);
extern void wm_init(void);

static int fails = 0, checks = 0;

static void check(int cond, const char *msg) {
  checks++;
  if (cond) printf("  PASS: %s\n", msg);
  else { printf("  FAIL: %s\n", msg); fails++; }
}

#define TASKBAR_Y (SCREEN_HEIGHT - TASKBAR_H)

static void chrome_zero(struct desktop_chrome *c) {
  memset(c, 0, sizeof *c);
  strcpy(c->clock, "12:00:00");
}

static int has_rect(const struct desktop_rect *r, int n, int x, int y, int w, int h) {
  for (int i = 0; i < n; i++) {
    if (r[i].x == x && r[i].y == y && r[i].w == w && r[i].h == h) return 1;
  }
  return 0;
}

/* ====================================================================== */

static void test_nothing_moved(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  chrome_zero(&a); chrome_zero(&b);
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 0, "identical chrome: no damage");
}

static void test_cursor_move(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  chrome_zero(&a); chrome_zero(&b);
  a.cursor_x = 100; a.cursor_y = 200;
  b.cursor_x = 300; b.cursor_y = 200;
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 2, "pointer move: two rects (old + new sprite)");
  check(has_rect(r, n, 99, 199, 10, 14), "old sprite rect");
  check(has_rect(r, n, 299, 199, 10, 14), "new sprite rect");

  /* Same position: nothing. */
  a.cursor_x = b.cursor_x; a.cursor_y = b.cursor_y;
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 0, "pointer standing still: no damage");
}

static void test_focus_change(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  wm_init();
  int id = wm_create_window(0, -1, -1, -1);
  struct window *w = &windows[0];

  chrome_zero(&a); chrome_zero(&b);
  a.win_count = 1; b.win_count = 1;
  a.focus = -1; b.focus = id;
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 2, "focus gain: window + taskbar");
  check(has_rect(r, n, w->x, w->y, w->w + 2, w->h + 2),
        "newly focused window repaints whole (shadow included)");
  check(has_rect(r, n, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H),
        "taskbar focus highlight repaints");

  a.focus = id; b.focus = -1;
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(has_rect(r, n, w->x, w->y, w->w + 2, w->h + 2),
        "losing focus repaints the old window too");

  wm_remove_window(id);
}

static void test_window_count_change_is_full(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  chrome_zero(&a); chrome_zero(&b);
  a.win_count = 0; b.win_count = 1;
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == -1, "window opened: whole scene");
  a.win_count = 2; b.win_count = 1;
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == -1, "window closed: whole scene");
}

static void test_menu_open_close(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  chrome_zero(&a); chrome_zero(&b);

  /* Start menu opens (panel + its "more ^/v" labels). */
  b.start_menu.x = 4; b.start_menu.y = TASKBAR_Y - 40 - 12;
  b.start_menu.w = 220; b.start_menu.h = 40 + 24;
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 2, "menu opens: new panel + taskbar (Apps highlight)");
  check(has_rect(r, n, 4, TASKBAR_Y - 52, 220, 64), "panel rect");
  check(has_rect(r, n, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H),
        "Apps button highlight");

  /* Selection moves inside the open menu: the panel is re-selected. */
  a = b;
  b.start_sel = 3;
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 1 && has_rect(r, n, 4, TASKBAR_Y - 52, 220, 64),
        "selection move repaints the panel once");

  /* Menu closes: old panel + new (empty) menu + the button again. */
  a = b;
  chrome_zero(&b);
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(has_rect(r, n, 4, TASKBAR_Y - 52, 220, 64), "closed panel restored");

  /* Right-click menu. */
  chrome_zero(&a); chrome_zero(&b);
  b.rc_menu.x = 500; b.rc_menu.y = 100; b.rc_menu.w = 120; b.rc_menu.h = 8 * 20;
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 1 && has_rect(r, n, 500, 100, 120, 160), "right-click menu rect");

  /* Per-window dropdown. */
  chrome_zero(&a); chrome_zero(&b);
  a.app_menu.x = 40; a.app_menu.y = 42; a.app_menu.w = 100; a.app_menu.h = 100;
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 1 && has_rect(r, n, 40, 42, 100, 100), "dropdown close restores it");
}

static void test_chrome_dirty(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  wm_init();
  int id = wm_create_window(0, -1, -1, -1);
  struct window *w = &windows[0];

  chrome_zero(&a); chrome_zero(&b);
  a.win_count = 1; b.win_count = 1;
  b.chrome_dirty[0] = 1;
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 2, "title change: window + taskbar");
  check(has_rect(r, n, w->x, w->y, w->w + 2, w->h + 2), "window chrome rect");
  check(has_rect(r, n, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H), "taskbar label");

  /* With the dropdown open it repaints too. */
  a = b;
  b.app_menu.x = 10; b.app_menu.y = 50; b.app_menu.w = 100; b.app_menu.h = 40;
  n = desktop_damage(&a, &b, r, DMG_MAX);
  check(has_rect(r, n, 10, 50, 100, 40), "open dropdown included");

  wm_remove_window(id);
}

static void test_clock_tick(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  chrome_zero(&a); chrome_zero(&b);
  strcpy(b.clock, "12:00:01");
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n == 1, "clock tick: one rect");
  /* The clock is right-aligned in a 96px cell (CLOCK_W). */
  check(has_rect(r, n, SCREEN_WIDTH - 96, TASKBAR_Y, 96, TASKBAR_H),
        "clock cell rect");
}

/* Everything on at once stays within DMG_MAX (the frame loop falls back to
 * a whole-scene repaint when the diff overflows). */
static void test_budget(void) {
  struct desktop_chrome a, b;
  struct desktop_rect r[DMG_MAX];
  wm_init();
  wm_create_window(0, -1, -1, -1);
  wm_create_window(0, -1, -1, -1);
  chrome_zero(&a); chrome_zero(&b);
  a.win_count = 2; b.win_count = 2;
  a.cursor_x = 10; b.cursor_x = 20;
  b.chrome_dirty[0] = 1;
  b.chrome_dirty[1] = 1;
  b.rc_menu.x = 300; b.rc_menu.y = 300; b.rc_menu.w = 120; b.rc_menu.h = 20;
  strcpy(b.clock, "23:59:59");
  int n = desktop_damage(&a, &b, r, DMG_MAX);
  check(n > 0 && n < DMG_MAX, "busy frame stays inside the rect budget");
  wm_remove_window(1);
  wm_remove_window(0);
}

/* ====================================================================== */
/* Frame-level present plumbing (GX D1, docs/graphics-accel.md)           */
/* ====================================================================== */
/* desktop_main() runs in a forked child with mock input, and the compat
 * flush mocks record every present.  The state machine below drives one
 * desktop frame per callback and asserts what went to the driver: chrome
 * rects for the menu/cursor/clock, full presents where the scene changed,
 * the row band + pointer cell for a text repair, and -- rebuilt from the
 * recorded presents alone -- a presented screen that equals the
 * framebuffer byte for byte (the pixel-equality contract the driver lanes
 * depend on). */

extern void set_flush_callback(void (*cb)(void));
extern void inject_mock_event(uint16_t type, uint16_t code, uint32_t value);
extern int desktop_main(void);
extern void *map_fb(void);
extern void mock_flush_reset(void);
extern int mock_flush_full_count;
extern int mock_flush_rect_calls;
extern int mock_flush_rect_count;
extern int mock_flush_last_full;
extern struct fb_rect mock_flush_rects[];
extern int mock_spawn2_intercept;
extern int mock_sysinfo_uptime_enabled;
extern int mock_sysinfo_uptime_ms;
extern int mock_sysinfo_time_enabled;
extern struct sys_time mock_sysinfo_time;

static uint32_t *pc_fb;
static uint32_t pc_shadow[SCREEN_WIDTH * SCREEN_HEIGHT];

static int pc_checks = 0, pc_fails = 0, pc_frame = 0;

static void pc_check(int cond, const char *msg) {
  pc_checks++;
  if (cond) printf("    PASS: %s\n", msg);
  else { printf("    FAIL: %s\n", msg); pc_fails++; }
}

static void pc_bail(const char *msg) {
  printf("    FAIL: %s\n", msg);
  pc_fails++;
  exit(1);
}

/* The desktop's pointer math, so the expectations cannot drift. */
static int pc_absx(uint32_t raw) { return (int)((raw * SCREEN_WIDTH) / 0x7FFF); }

static int pc_has_rect(int x, int y, int w, int h) {
  for (int i = 0; i < mock_flush_rect_count; i++) {
    if (mock_flush_rects[i].x == x && mock_flush_rects[i].y == y &&
        mock_flush_rects[i].w == w && mock_flush_rects[i].h == h) return 1;
  }
  return 0;
}

/* Apply the just-recorded present to the presented-screen shadow. */
static void pc_apply_present(void) {
  if (mock_flush_last_full) {
    memcpy(pc_shadow, pc_fb, sizeof pc_shadow);
    return;
  }
  for (int i = 0; i < mock_flush_rect_count; i++) {
    int x0 = mock_flush_rects[i].x, y0 = mock_flush_rects[i].y;
    int x1 = x0 + mock_flush_rects[i].w, y1 = y0 + mock_flush_rects[i].h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
    if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
    for (int y = y0; y < y1; y++) {
      memcpy(&pc_shadow[y * SCREEN_WIDTH + x0], &pc_fb[y * SCREEN_WIDTH + x0],
             (size_t)(x1 - x0) * 4);
    }
  }
}

#define PC_MOVE1_RAW 20000u
#define PC_MOVE2_RAW 26400u

enum {
  PC_INITIAL = 0,   /* frame 1: first paint          -> full present      */
  PC_MENU,          /* frame 2: right-click menu     -> menu rect         */
  PC_LAUNCH,        /* frame 3: window launch        -> full present      */
  PC_CURSOR,        /* frame 4: pointer move         -> old + new sprite  */
  PC_CLOCK,         /* frame 5: clock tick           -> clock cell        */
  PC_ROW,           /* frame 6: text repair          -> band + pointer    */
  PC_DRAG,          /* frame 7: drag motion          -> cursor rects      */
  PC_CONVERGE       /* frame 8: quiet frame          -> empty present     */
};

static int pc_state = PC_INITIAL;

static void pc_recorder(void) {
  pc_frame++;
  if (pc_frame > 60) pc_bail("present state machine stalled");

  pc_apply_present();

  switch (pc_state) {
  case PC_INITIAL:
    pc_check(mock_flush_full_count == 1 && mock_flush_rect_calls == 0 &&
             mock_flush_last_full,
             "first frame: full present, no rect flush");
    /* Right-click at the desktop center opens the menu there. */
    inject_mock_event(EV_KEY, 0x111, 1);
    inject_mock_event(EV_KEY, 0x111, 0);
    pc_state = PC_MENU;
    break;

  case PC_MENU: {
      int mx = SCREEN_WIDTH / 2, my = SCREEN_HEIGHT / 2;
      pc_check(mock_flush_full_count == 1 && mock_flush_rect_calls == 1 &&
               !mock_flush_last_full && mock_flush_rect_count == 1 &&
               pc_has_rect(mx, my, 120, 7 * 20),
               "menu open: presents exactly the right-click menu rect");
      /* Left click on item 0 (EDITOR.BIN in the mock listing) launches it. */
      inject_mock_event(EV_KEY, 0x110, 1);
      inject_mock_event(EV_KEY, 0x110, 0);
      pc_state = PC_LAUNCH;
      break;
    }

  case PC_LAUNCH:
    pc_check(mock_flush_full_count == 2 && mock_flush_last_full &&
             mock_flush_rect_calls == 1,
             "window launch: whole scene repaints -> full present");
    inject_mock_event(EV_ABS, ABS_X, PC_MOVE1_RAW);
    pc_state = PC_CURSOR;
    break;

  case PC_CURSOR: {
      int mx = pc_absx(PC_MOVE1_RAW), my = SCREEN_HEIGHT / 2;
      pc_check(mock_flush_full_count == 2 && mock_flush_rect_calls == 2 &&
               !mock_flush_last_full && mock_flush_rect_count == 2 &&
               pc_has_rect(SCREEN_WIDTH / 2 - 1, my - 1, 10, 14) &&
               pc_has_rect(mx - 1, my - 1, 10, 14),
               "cursor move: old + new sprite rects only");
      /* Clock tick: one mocked second later + an uptime jump to redraw. */
      mock_sysinfo_time_enabled = 1;
      mock_sysinfo_time.hour = 12;
      mock_sysinfo_time.minute = 0;
      mock_sysinfo_time.second = 1;
      mock_sysinfo_uptime_enabled = 1;
      mock_sysinfo_uptime_ms = 20000;
      pc_state = PC_CLOCK;
      break;
    }

  case PC_CLOCK:
    pc_check(mock_flush_full_count == 2 && mock_flush_rect_calls == 3 &&
             !mock_flush_last_full && mock_flush_rect_count == 1 &&
             pc_has_rect(SCREEN_WIDTH - 96, SCREEN_HEIGHT - 26, 96, 26),
             "clock tick: presents just the clock cell");
    /* Window text change: the repaired row band + the pointer re-stamp. */
    wm_text_putc(&windows[0], 'h');
    wm_text_putc(&windows[0], 'i');
    wm_text_putc(&windows[0], '\n');
    mock_sysinfo_uptime_ms += 3000;    /* redraw without a clock change */
    pc_state = PC_ROW;
    break;

  case PC_ROW: {
      int wx = windows[0].x, wy = windows[0].y, ww = windows[0].w;
      int mx = pc_absx(PC_MOVE1_RAW), my = SCREEN_HEIGHT / 2;
      pc_check(mock_flush_full_count == 2 && mock_flush_rect_calls == 4 &&
               !mock_flush_last_full && mock_flush_rect_count == 2 &&
               pc_has_rect(wx + 2, wy + 44, ww - 4, 10) &&
               pc_has_rect(mx - 1, my - 1, 10, 14),
               "row repair: presents the repaired band + re-stamped pointer");
      /* Drag across the window: press, then move with the button held. */
      inject_mock_event(EV_KEY, 0x110, 1);
      inject_mock_event(EV_ABS, ABS_X, PC_MOVE2_RAW);
      pc_state = PC_DRAG;
      break;
    }

  case PC_DRAG: {
      int m1 = pc_absx(PC_MOVE1_RAW), m2 = pc_absx(PC_MOVE2_RAW);
      int my = SCREEN_HEIGHT / 2;
      pc_check(mock_flush_full_count == 2 && mock_flush_rect_calls == 5 &&
               !mock_flush_last_full && mock_flush_rect_count == 2 &&
               pc_has_rect(m1 - 1, my - 1, 10, 14) &&
               pc_has_rect(m2 - 1, my - 1, 10, 14),
               "drag motion: presents the moved cursor region, no full flush");
      /* Release: the next frame paints nothing; the presented screen built
       * from the recorded presents must now equal the framebuffer. */
      inject_mock_event(EV_KEY, 0x110, 0);
      pc_state = PC_CONVERGE;
      break;
    }

  case PC_CONVERGE:
    pc_check(mock_flush_full_count == 2 && mock_flush_rect_calls == 6 &&
             !mock_flush_last_full && mock_flush_rect_count == 0,
             "quiet frame: empty present (count 0), still no full flush");
    pc_check(memcmp(pc_shadow, pc_fb, sizeof pc_shadow) == 0,
             "presented screen == framebuffer, byte for byte");
    printf("  === present frames: %d checks, %d failed ===\n",
           pc_checks, pc_fails);
    if (pc_fails) exit(1);
    printf("  ALL FRAME PRESENT CHECKS PASSED\n");
    exit(0);
  }
}

static void pc_run_child(void) {
  pc_fb = (uint32_t *)map_fb();
  if (!pc_fb) pc_bail("map_fb failed");
  mock_spawn2_intercept = 1;      /* fake the editor launch (pid + pipes)  */
  mock_flush_reset();
  signal(SIGPIPE, SIG_IGN);       /* no reader on forwarded mouse writes   */
  alarm(20);                      /* hard bound if the loop never frames   */
  set_flush_callback(pc_recorder);
  desktop_main();                 /* runs until the state machine exits    */
  pc_bail("desktop_main returned");
}

static void test_frame_presents(void) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    pc_run_child();
    exit(1);                      /* not reached */
  }
  int stt = 0;
  waitpid(pid, &stt, 0);
  checks++;
  if (WIFEXITED(stt) && WEXITSTATUS(stt) == 0) {
    printf("  PASS: frame presents: rect flush == full flush, end to end\n");
  } else {
    printf("  FAIL: frame presents: child rc=%d (see child output above)\n",
           WIFEXITED(stt) ? WEXITSTATUS(stt) : -1);
    fails++;
  }
}

int main(void) {
  printf("desktop_damage_test\n");
  test_nothing_moved();
  test_cursor_move();
  test_focus_change();
  test_window_count_change_is_full();
  test_menu_open_close();
  test_chrome_dirty();
  test_clock_tick();
  test_budget();
  test_frame_presents();

  printf("=== %d checks, %d failed ===\n", checks, fails);
  if (fails) {
    printf("FAILED\n");
    return 1;
  }
  printf("ALL DESKTOP DAMAGE TESTS PASSED\n");
  return 0;
}
