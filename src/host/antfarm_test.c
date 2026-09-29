/*
 * antfarm_test.c - host tests for ANTFARM.BIN, the xantfarm port on the
 * HobbyOS X11 support library (src/user/x11/apps/antfarm/main.c).
 *
 * Four layers, cheapest first:
 *
 *   1. world setup: the grid split (air above, dirt below), the ant
 *      crew's initial placement, and the reflow rebuild that keeps the
 *      overlapping farm;
 *   2. the simulation: move_ants()/sand_fall() run for real, with
 *      invariants (bounds, element values) checked every step, plus the
 *      poke() scatter/drop behaviour -- deterministic under a seed;
 *   3. the checkpoint file: write_world() then read_world() round trips
 *      the exact grid in the original's byte format;
 *   4. the real entry point: antfarm_main() in a forked child with pipes
 *      on stdin/stdout -- startup marker, the ESC ] X / ] P / ] T
 *      handshake, animation flushes (ESC ] F ~), a pointer tracking
 *      report being consumed, and a clean kill.
 */
#define main antfarm_main
#include "../user/x11/apps/antfarm/main.c"
#undef main

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include "graphics/graphics.h"
#include "xlib_internal.h"

static int checks_pass = 0;
static int checks_fail = 0;

static void check(int cond, const char *label) {
  if (cond) {
    checks_pass++;
  } else {
    checks_fail++;
    printf("FAIL: %s\n", label);
  }
}

/* ---- helpers ---------------------------------------------------------- */

/* Build a fresh world of the given size with the given seed, without a
 * checkpoint file in sight. */
static void fresh_world(int w, int h, unsigned long seed) {
  checkpoint_file = 0;
  rng_seed(seed);
  ant_init(w, h);
}

static int all_ants_in_bounds(void) {
  for (int a = 0; a < num_ants; a++) {
    if (ants[a].x < 0 || ants[a].x >= world_w ||
        ants[a].y < 0 || ants[a].y >= world_h)
      return 0;
    if (ants[a].dir < 0 || ants[a].dir >= N_DIRS) return 0;
    if (ants[a].behavior < B_WANDERING || ants[a].behavior > B_PANIC) return 0;
  }
  return 1;
}

static int all_elements_valid(void) {
  for (int y = 0; y < world_h; y++)
    for (int x = 0; x < world_w; x++)
      if (world[y][x] != E_AIR && world[y][x] != E_DIRT &&
          world[y][x] != E_SAND)
        return 0;
  return 1;
}

/* ---- 1. world setup --------------------------------------------------- */

static void test_world_setup(void) {
  fresh_world(80, 30, 1234);

  check(world_w == 20 && world_h == 7, "grid sized from the pixel size");
  check(surface == world_h / 3, "surface is the one-third cut (no floats)");

  int air_ok = 1, dirt_ok = 1;
  for (int y = 0; y < world_h; y++) {
    for (int x = 0; x < world_w; x++) {
      if (y < surface && world[y][x] != E_AIR) air_ok = 0;
      if (y >= surface && world[y][x] != E_DIRT) dirt_ok = 0;
    }
  }
  check(air_ok, "a new world is air above the surface");
  check(dirt_ok, "and dirt below it");

  check(num_ants == 10, "default ant crew is ten");
  check(all_ants_in_bounds(), "ants start in bounds");
  int at_top = 1;
  for (int a = 0; a < num_ants; a++)
    if (ants[a].y != 1) at_top = 0;
  check(at_top, "ants start on the first dirt row (y = 1)");

  /* Deterministic placement: the same seed lays out the same crew. */
  int x0[10], d0[10];
  for (int a = 0; a < num_ants; a++) {
    x0[a] = ants[a].x;
    d0[a] = ants[a].dir;
  }
  fresh_world(80, 30, 1234);
  int same = 1;
  for (int a = 0; a < num_ants; a++)
    if (ants[a].x != x0[a] || ants[a].dir != d0[a]) same = 0;
  check(same, "a seeded world lays out identically");

  /* The reflow rebuild keeps the overlapping farm. */
  world[2][3] = E_SAND;
  ants[0].x = 15;
  ants[0].y = 5;
  rebuild_world(30, 12);
  check(world_w == 30 && world_h == 12, "rebuild takes the new size");
  check(world[2][3] == E_SAND, "the overlapping farm survives a rebuild");
  check(all_ants_in_bounds(), "ants are clamped into the new bounds");
  rebuild_world(20, 7);
  check(world_w == 20 && world_h == 7 && all_ants_in_bounds(),
        "shrinking rebuilds too");
}

/* ---- 2. simulation ---------------------------------------------------- */

static void test_simulation_stability(void) {
  fresh_world(120, 60, 99);

  for (int step = 0; step < 4000; step++) {
    move_ants();
    if (num_falling_sands > 0) sand_fall();
    if ((step % 500) == 0) {
      if (!all_ants_in_bounds() || !all_elements_valid()) {
        check(0, "simulation invariants");
        return;
      }
    }
  }
  check(all_ants_in_bounds(), "ants stay in bounds over 4000 steps");
  check(all_elements_valid(), "elements stay air/dirt/sand over 4000 steps");

  /* The farm actually did something: dug or dropped somewhere. */
  int changed = 0, any_sand = 0;
  for (int y = 0; y < world_h && !changed; y++)
    for (int x = 0; x < world_w; x++) {
      if (world[y][x] == E_SAND) any_sand = 1;
      if (world[y][x] != E_DIRT && y >= surface) {
        changed = 1;
        break;
      }
    }
  check(changed || any_sand, "ants dug, carried and dropped");

  /* Whole-run determinism: same seed, same world after the same steps. */
  static unsigned char saved[120 * 60];
  for (int y = 0; y < world_h; y++)
    for (int x = 0; x < world_w; x++)
      saved[y * world_w + x] = world[y][x];
  int ants_saved[10][2];
  for (int a = 0; a < num_ants; a++) {
    ants_saved[a][0] = ants[a].x;
    ants_saved[a][1] = ants[a].y;
  }
  fresh_world(120, 60, 99);
  for (int step = 0; step < 4000; step++) {
    move_ants();
    if (num_falling_sands > 0) sand_fall();
  }
  int identical = 1;
  for (int y = 0; y < world_h && identical; y++)
    for (int x = 0; x < world_w; x++)
      if (world[y][x] != saved[y * world_w + x]) {
        identical = 0;
        break;
      }
  for (int a = 0; a < num_ants && identical; a++)
    if (ants[a].x != ants_saved[a][0] || ants[a].y != ants_saved[a][1])
      identical = 0;
  check(identical, "a seeded run is reproducible");
}

static void test_poke(void) {
  /* An ant pushed away from the cursor moves away and panics. */
  num_ants = 1;
  fresh_world(80, 30, 7);
  ants[0].x = 10;
  ants[0].y = 5;
  ants[0].dir = D_RIGHT_DOWN;
  ants[0].behavior = B_WANDERING;
  ants[0].timer = 4;
  poke(11 * GRID_SIZE, 5 * GRID_SIZE);
  check(ants[0].x == 9 && ants[0].y == 5, "poked ant is pushed away");
  check(ants[0].behavior == B_PANIC, "poked ant panics");
  check(all_ants_in_bounds(), "still in bounds");

  /* Dead on top: pushed randomly, but reproducibly for a seed. */
  int rx[2], ry[2], rd[2];
  for (int round = 0; round < 2; round++) {
    num_ants = 1;
    fresh_world(80, 30, 55);
    ants[0].x = 10;
    ants[0].y = 5;
    ants[0].dir = D_RIGHT_DOWN;
    ants[0].behavior = B_WANDERING;
    ants[0].timer = 4;
    poke(10 * GRID_SIZE + 1, 5 * GRID_SIZE + 1);
    rx[round] = ants[0].x;
    ry[round] = ants[0].y;
    rd[round] = ants[0].dir;
    check(ants[0].x >= 9 && ants[0].x <= 11 && ants[0].y >= 4 && ants[0].y <= 6,
          "dead-on poke nudges by at most one cell");
  }
  check(rx[0] == rx[1] && ry[0] == ry[1] && rd[0] == rd[1],
        "poke randomness is seeded (reproducible)");

  /* A carrying ant drops its sand when poked. */
  num_ants = 1;
  fresh_world(80, 30, 8);
  ants[0].x = 18;
  ants[0].y = 6;
  ants[0].behavior = B_CARRYING;
  ants[0].timer = 5;
  world[6][18] = E_AIR;
  poke(18 * GRID_SIZE, 6 * GRID_SIZE);
  check(world[6][18] == E_SAND, "carrying ant drops its sand when poked");

  /* Poking far away touches nobody. */
  num_ants = 1;
  fresh_world(80, 30, 9);
  ants[0].x = 5;
  ants[0].y = 5;
  poke(60 * GRID_SIZE, 20 * GRID_SIZE);
  check(ants[0].x == 5 && ants[0].y == 5 && ants[0].behavior == B_WANDERING,
        "a far-away poke leaves the ant alone");
}

/* ---- 3. the checkpoint file ------------------------------------------- */

static void test_checkpoint(void) {
  const char *path = "/tmp/ANTFARM.TST";

  fresh_world(160, 60, 42);         /* a 40x15 grid */
  /* A known pattern: sand in a diagonal band, air in a hole. */
  for (int y = 0; y < world_h; y++)
    for (int x = 0; x < world_w; x++)
      world[y][x] = (x == y) ? E_SAND : (y >= surface ? E_DIRT : E_AIR);
  world[1][1] = E_AIR;
  world[9][7] = E_SAND;

  write_world(path);

  fresh_world(80, 40, 43);   /* a different world (20x10) ... */
  checkpoint_file = path;
  rng_seed(44);
  ant_init(160, 60);         /* ... replaced by the checkpoint */
  check(checkpoint_file == path, "(checkpoint path kept)");

  int round_trip = 1;
  for (int y = 0; y < world_h && round_trip; y++) {
    for (int x = 0; x < world_w; x++) {
      unsigned char want = (x == y) ? E_SAND : (y >= surface ? E_DIRT : E_AIR);
      if (x == 1 && y == 1) want = E_AIR;
      if (x == 7 && y == 9) want = E_SAND;
      if (world[y][x] != want) {
        round_trip = 0;
        break;
      }
    }
  }
  check(world_w == 40 && world_h == 15, "checkpoint restored the size");
  check(round_trip, "checkpoint round trips every cell");
  checkpoint_file = 0;
  remove(path);
}

/* ---- 4. the real entry point ------------------------------------------ */

static int drain_fd(int fd, char *buf, int *len, int cap) {
  int got = 0;
  for (;;) {
    if (*len >= cap - 1) break;
    int r = read(fd, buf + *len, cap - 1 - *len);
    if (r <= 0) break;
    *len += r;
    got += r;
    buf[*len] = '\0';
  }
  return got;
}

static int wait_for(int fd, char *buf, int *len, int cap, const char *needle,
                    int ms) {
  for (int waited = 0; waited < ms; waited += 2) {
    drain_fd(fd, buf, len, cap);
    if (strstr(buf, needle) != NULL) return 1;
    usleep(2000);
  }
  drain_fd(fd, buf, len, cap);
  return strstr(buf, needle) != NULL;
}

static int count_occurrences(const char *hay, const char *needle) {
  int n = 0;
  const char *p = hay;
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += strlen(needle);
  }
  return n;
}

static void test_entry_point(void) {
  int in_pipe[2], out_pipe[2];
  char cap[8192];
  int len = 0;
  cap[0] = '\0';

  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
    check(0, "pipe()");
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    /* Child: the real program, talking over the pipes. */
    if (in_pipe[0] != 0) {
      dup2(in_pipe[0], 0);
      close(in_pipe[0]);
    }
    if (out_pipe[1] != 1) {
      dup2(out_pipe[1], 1);
      close(out_pipe[1]);
    }
    close(in_pipe[1]);
    close(out_pipe[0]);
    char *argv[] = { "ANTFARM.BIN", 0 };
    antfarm_main(1, argv);
    _exit(0);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);

  /* Tie the pipe to a non-blocking read end so drain_fd() never hangs. */
  int fl = fcntl(out_pipe[0], F_GETFL, 0);
  fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);

  /* Startup: the window maps (ESC ] X), pointer input is requested
   * (ESC ] P 1) and the title goes out (ESC ] T). */
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "[APP] XANTFARM started", 3000),
        "startup marker printed");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]X 560;400~", 3000),
        "startup sends ]X with the preferred size");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]P 1~", 2000),
        "pointer input requested");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]T antfarm~", 2000),
        "window title sent");

  /* The world only starts after the geometry: from then on the loop
   * animates and every changed patch ends in a flush (ESC ] F ~). */
  int before = count_occurrences(cap, "\033]F~");
  write(in_pipe[1], "\033]G 2;34;320;200~", 18);
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]F~", 3000),
        "first paint after the geometry flushes");

  /* Let it animate a bit: more flushes must arrive on their own. */
  int flushed_after = count_occurrences(cap, "\033]F~");
  for (int waited = 0; waited < 4000 && flushed_after <= before + 1; waited += 2) {
    drain_fd(out_pipe[0], cap, &len, sizeof cap);
    flushed_after = count_occurrences(cap, "\033]F~");
    usleep(2000);
  }
  check(flushed_after > before + 1, "the farm animates without input");

  /* A pointer tracking report must be consumed without trouble and the
   * farm keeps animating (the poke invalidates more cells). */
  int flushes_now = count_occurrences(cap, "\033]F~");
  write(in_pipe[1], "\033[T 100;140~", 13);
  int saw_more = 0;
  for (int waited = 0; waited < 3000; waited += 2) {
    drain_fd(out_pipe[0], cap, &len, sizeof cap);
    if (count_occurrences(cap, "\033]F~") > flushes_now) {
      saw_more = 1;
      break;
    }
    usleep(2000);
  }
  check(saw_more, "tracking report consumed, farm still flushing");

  /* The child should still be running: a crash would have closed the
   * pipes and exited early. */
  int status = 0;
  check(waitpid(pid, &status, WNOHANG) == 0, "still running after the input");
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
        "terminated by the test, not by a fault");

  close(in_pipe[1]);
  close(out_pipe[0]);
}

/* ---- main ------------------------------------------------------------- */

int main(void) {
  /* The X11 library and the fork test need the mock framebuffer up
   * before anything opens a display (like xcalc_test's app_start). */
  graphics_init();
  /* The app's startup defaults (main() sets these; the unit tests call
   * ant_init directly). */
  num_ants = 10;
  cps = 15;
  checkpoint_file = 0;
  test_world_setup();
  test_simulation_stability();
  test_poke();
  test_checkpoint();
  test_entry_point();

  printf("=== %d checks, %d failed ===\n", checks_pass + checks_fail,
         checks_fail);
  if (checks_fail == 0) {
    printf("ALL ANTFARM TESTS PASSED\n");
    return 0;
  }
  return 1;
}
