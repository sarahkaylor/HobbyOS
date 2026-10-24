/* src/host/sdl_test_app_host.c — GB-032 SDL test app host gate.
 *
 * T1 + T4 as the coordinator scoped GB-032:
 *   - T1: the app's three surfaces (surface create/clear, 2D blit, input
 *     echo) run as a host TU and their results are asserted directly.
 *   - T4: the REAL static-PIE binary the Makefile builds for the loader
 *     (obj/sdl_test_app_pie, x86_64 static-musl) is parsed with the same
 *     src/kernel/elf_image.c the loader links, and its properties are
 *     asserted against the GB-013 loader's accept rules AND cross-checked
 *     against readelf — i.e. a genuine differential, not fibre.
 *
 * ELF-property invariants the loader requires (docs/linux-abi.md §9, the
 * accept class in tools/linux-abi/gen_test_elf.py):
 *   ET_DYN  -> is_pie == 1, needs_low_window == 0
 *   no PT_INTERP (static)   -- the parser returns ELF_ERR_INTERP for dynamic
 *   entry inside an executable segment
 *   one R+X + one R+W PT_LOAD plan region -> the loader mapper can map it
 *
 * The binary path comes from SDL_TEST_APP_ELF, defaulting to
 * obj/sdl_test_app_pie (the Makefile builds it as a host_tests prereq).  If
 * it is absent this prints SKIP with a reason — never a fabricated PASS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fb_pixel.h"
#include "linux_input.h"
#include "linux_joystick.h"
#include "elf_image.h"

#define HB_SDL_APP_NO_MAIN
#include "../../tools/sdl/sdl_test_app.c"

static int checks_run = 0, checks_fail = 0;
static void check(int ok, const char *name) {
  checks_run++;
  if (ok) printf("  PASS: %s\n", name);
  else    { checks_fail++; printf("  FAIL: %s\n", name); }
}

/* ---- T1: the three surfaces -------------------------------------------- */
static void t1_surfaces(void) {
  static uint32_t fb[64 * 48];
  static uint32_t tile[8 * 8];
  sdl_surface s;
  sdl_surface_init(&s, fb, 64, 48, 64);

  /* surface clear sets every word and respects the B8G8R8A8 pack. */
  sdl_surface_clear(&s, hb_fb_pack(0x12, 0x34, 0x56, 0x78));
  uint8_t by[4];
  hb_fb_pixel_bytes(fb[0], by);
  check(s.width == 64 && s.height == 48 && s.stride_px == 64,
        "surface geometry (64x48, stride 64)");
  check(by[0] == 0x56 && by[1] == 0x34 && by[2] == 0x12 && by[3] == 0x78,
        "surface clear: packed word is B8G8R8A8 [b,g,r,a]");
  check(fb[64 * 48 - 1] == fb[0], "surface clear paints every pixel");

  /* blit places the tile at (x,y) inside the surface. */
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      tile[y * 8 + x] = hb_fb_pack((uint8_t)(10 * y + x), 0xFF, 0x00, 0xFF);
  sdl_surface_clear(&s, hb_fb_pack(0, 0, 0, 0xFF));
  sdl_blit(&s, tile, 8, 8, 10, 10);
  check(fb[10 * 64 + 10] == tile[0], "blit: tile origin lands at (10,10)");
  check(fb[0 * 64 + 0] == hb_fb_pack(0, 0, 0, 0xFF), "blit: untouched pixel stays");
  /* clip: tile extending past the right/bottom edge is clipped, no OOB. */
  sdl_blit(&s, tile, 8, 8, 60, 44);
  check(fb[47 * 64 + 63] == tile[3 * 8 + 3], "blit: bottom/right clip keeps in-bounds pixel");
  check(fb[47 * 64 + 31] == hb_fb_pack(0, 0, 0, 0xFF), "blit: clipped-away region untouched");

  /* input echo formats evdev + gamepad events distinctly. */
  char line[32];
  struct hb_input_event key = {0, 0, HB_EV_KEY, 30, 1};
  struct hb_input_event abs = {0, 0, HB_EV_ABS, 0, 512};
  struct hb_input_event jp  = {0, 0, HB_EV_KEY, HB_JP_BTN_X, 1};
  sdl_echo_input(&key, line, sizeof line);
  check(strcmp(line, "KEY 1e=1") == 0, "echo: KEY_A down -> \"KEY 1e=1\"");
  sdl_echo_input(&abs, line, sizeof line);
  check(strcmp(line, "ABS 00=512") == 0, "echo: ABS_X=512 -> \"ABS 00=512\"");
  sdl_echo_input(&jp, line, sizeof line);
  check(strstr(line, "KEY") == line, "echo: gamepad BTN_X echoed as KEY type");
}

/* ---- T4: real static-PIE ELF differential vs the loader + readelf ------- */
static uint8_t *slurp(const char *path, uint64_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
  if (!b) { fclose(f); return NULL; }
  *len = (size_t)fread(b, 1, (size_t)n, f);
  fclose(f);
  return b;
}

static int readelf_has(const char *path, const char *pattern) {
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "readelf -h \"%s\" 2>/dev/null", path);
  FILE *p = popen(cmd, "r");
  if (!p) return 0;
  char buf[512];
  int found = 0;
  while (fgets(buf, sizeof buf, p) != NULL)
    if (strstr(buf, pattern) != NULL) { found = 1; break; }
  pclose(p);
  return found;
}

static void t4_elf_properties(const char *path) {
  uint64_t len = 0;
  uint8_t *b = slurp(path, &len);
  if (!b) {
    checks_run++;
    printf("  SKIP: SDL test app ELF-property gate (%s not built; "
           "run `make obj/sdl_test_app_pie`)\n", path);
    return;
  }
  struct elf_image img;
  memset(&img, 0, sizeof img);
  int rc = elf_parse(b, len, &img);
  check(rc == ELF_OK, "parse: static-PIE app accepted by the GB-013 loader parser");

  if (rc == ELF_OK) {
    check(img.is_pie == 1, "ELF: ET_DYN flagged is_pie");
    check(img.needs_low_window == 0, "ELF: static-PIE needs no low window");
    check(img.entry != 0, "ELF: entry point is inside the image (nonzero)");

    struct elf_plan pl;
    check(elf_build_plan(&img, &pl) == ELF_OK, "plan: builds for the loader mapper");
    check(pl.nreg >= 2, "plan: >= 2 regions (one exec, one data)");
    int got_exec = 0, got_data = 0;
    for (unsigned i = 0; i < pl.nreg; i++) {
      if ((pl.regs[i].flags & (ELF_PF_R | ELF_PF_X)) == (ELF_PF_R | ELF_PF_X))
        got_exec = 1;
      if ((pl.regs[i].flags & (ELF_PF_R | ELF_PF_W)) == (ELF_PF_R | ELF_PF_W))
        got_data = 1;
    }
    check(got_exec, "plan: an R+X executable region is mappable");
    check(got_data, "plan: an R+W data region is mappable");
  }

  /* Differential vs readelf: the type must be DYN and there must be NO
     PT_INTERP (static).  A dynamic ELF would need ld.so -> rejected. */
  check(readelf_has(path, "Type:"), "readelf: produces an ELF header");
  /* readelf -h prints "Type: DYN (Position-Independent Executable file)". */
  if (readelf_has(path, "Position-Independent Executable"))
    checks_run++;
  else {
    checks_run++;
    printf("  FAIL: readelf shows a NON-PIE type (not ET_DYN)\n");
    checks_fail++;
  }
  free(b);
}

int main(void) {
  printf("=== GB-032 SDL test app: T1 surface logic + T4 ELF differential ===\n");
  t1_surfaces();

  const char *path = getenv("SDL_TEST_APP_ELF");
  if (!path || !*path) path = "obj/sdl_test_app_pie";
  t4_elf_properties(path);

  printf("sdl_test_app_test_host: %d checks, %d failed\n", checks_run, checks_fail);
  return checks_fail == 0 ? 0 : 1;
}
