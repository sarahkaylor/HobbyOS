/*
 * unit_test.c - Host unit tests for src/user/unit.c (Unit Converter).
 *
 * Runs on the host (HOST_TEST): the app source is included directly so the
 * tests can drive its state, conversions, parsing, rendering and event
 * handling without a real window. The app's blocking value dialog ('e' /
 * menu "Edit Value") is exercised via unit_apply_value_text(), never through
 * stdin, so the test can never hang on dialog input.
 *
 * Coverage:
 *   - unit / category table sanity (names, counts, positive + ordered factors)
 *   - known conversions for Length, Mass, Time, Data and Temperature
 *   - zero and negative values, 2-decimal truncation cases
 *   - int64 saturation ("(clamped)") on the multiply paths
 *   - value parsing (junk, empty, signs, spaces, overflow)
 *   - category / unit cycling with wraparound in both directions
 *   - swap, error-message line behaviour
 *   - full-screen rendering (exact spec screen, temperature screen, error
 *     line, clamped marker, size/line limits, determinism)
 *   - event handling (arrows, char keys, menu selections, mouse clicks)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

/* Rename the app entry point so the test can define its own main(). */
#define main unit_app_main
#include "../user/unit.c"
#undef main

/* ================= tiny check framework ================= */

static int checks_run = 0;
static int checks_failed = 0;

static void section(const char *name) {
    printf("\n-- %s\n", name);
}

static void check(int ok, const char *what, int line) {
    checks_run++;
    if (ok) {
        printf("  PASS: %s\n", what);
    } else {
        printf("  FAIL: %s  (line %d)\n", what, line);
        checks_failed++;
    }
}

#define CHECK(cond, what) check(!!(cond), (what), __LINE__)

static void check_str(const char *what, const char *got, const char *want, int line) {
    checks_run++;
    if (got != NULL && strcmp(got, want) == 0) {
        printf("  PASS: %s -> \"%s\"\n", what, got);
    } else {
        printf("  FAIL: %s -> got \"%s\", want \"%s\"  (line %d)\n",
               what, got ? got : "(null)", want, line);
        checks_failed++;
    }
}

#define CHECK_STR(what, got, want) check_str((what), (got), (want), __LINE__)

/* ================= helpers ================= */

static int find_unit_idx(int cat, const char *name) {
    const struct unit_cat *c = &g_cats[cat];
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->units[i].name, name) == 0) return i;
    return -1;
}

/* Configure the state by unit names and format the converted number. */
static void conv_str(int cat, const char *from, const char *to, long value, char *out) {
    struct conv_out r;
    g_unit.cat = cat;
    g_unit.from = find_unit_idx(cat, from);
    g_unit.to = find_unit_idx(cat, to);
    g_unit.value = value;
    g_unit.msg[0] = '\0';
    unit_convert(&r);
    unit_format_value(out, g_cats[cat].temp, &r);
}

/* Configure the state by raw indices and format the converted number. */
static void conv_idx(int cat, int from, int to, long value, char *out) {
    struct conv_out r;
    g_unit.cat = cat;
    g_unit.from = from;
    g_unit.to = to;
    g_unit.value = value;
    g_unit.msg[0] = '\0';
    unit_convert(&r);
    unit_format_value(out, g_cats[cat].temp, &r);
}

static const char *render(void) {
    static char buf[UNIT_SCREEN_MAX];
    unit_render_to(buf, (int)sizeof(buf));
    return buf;
}

static struct gui_event ev_key(int type) {
    struct gui_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    return e;
}

static struct gui_event ev_char(int ch) {
    struct gui_event e = ev_key(GUI_EV_CHAR);
    e.ch = ch;
    return e;
}

static struct gui_event ev_menu(int menu, int item) {
    struct gui_event e = ev_key(GUI_EV_MENU);
    e.menu = menu;
    e.item = item;
    return e;
}

static struct gui_event ev_mouse(int x, int y, int button) {
    struct gui_event e = ev_key(GUI_EV_MOUSE);
    e.x = x;
    e.y = y;
    e.button = button;
    return e;
}

/* ================= tests ================= */

static void test_unit_tables(void) {
    section("unit tables");

    CHECK(strcmp(g_cats[0].name, "Length") == 0, "category 0 is Length");
    CHECK(strcmp(g_cats[1].name, "Mass") == 0, "category 1 is Mass");
    CHECK(strcmp(g_cats[2].name, "Time") == 0, "category 2 is Time");
    CHECK(strcmp(g_cats[3].name, "Data") == 0, "category 3 is Data");
    CHECK(strcmp(g_cats[4].name, "Temperature") == 0, "category 4 is Temperature");

    CHECK(g_cats[0].count == 7, "Length has 7 units (mm cm m km in ft mi)");
    CHECK(g_cats[1].count == 5, "Mass has 5 units (mg g kg oz lb)");
    CHECK(g_cats[2].count == 4, "Time has 4 units (s min h day)");
    CHECK(g_cats[3].count == 4, "Data has 4 units (B KB MB GB)");
    CHECK(g_cats[4].count == 3, "Temperature has 3 units (C F K)");

    CHECK(g_cats[4].temp == 1, "Temperature flagged as special-cased");
    CHECK(!g_cats[0].temp && !g_cats[1].temp && !g_cats[2].temp && !g_cats[3].temp,
          "tabled categories are not special-cased");

    int all_pos = 1;
    for (int c = 0; c < NUM_CATS; c++)
        for (int i = 0; i < g_cats[c].count; i++)
            if (g_cats[c].units[i].factor <= 0) all_pos = 0;
    CHECK(all_pos, "every unit factor is positive");

    CHECK(units_length[0].factor < units_length[1].factor &&
          units_length[1].factor < units_length[2].factor &&
          units_length[2].factor < units_length[3].factor,
          "length factors strictly increase mm < cm < m < km");
    CHECK(units_mass[0].factor < units_mass[1].factor &&
          units_mass[1].factor < units_mass[2].factor,
          "mass factors strictly increase mg < g < kg");
    CHECK(units_time[0].factor < units_time[1].factor &&
          units_time[1].factor < units_time[2].factor &&
          units_time[2].factor < units_time[3].factor,
          "time factors strictly increase s < min < h < day");
    CHECK(units_data[0].factor < units_data[1].factor &&
          units_data[1].factor < units_data[2].factor &&
          units_data[2].factor < units_data[3].factor,
          "data factors strictly increase B < KB < MB < GB");

    CHECK(units_length[0].factor == 1000LL && units_length[1].factor == 10000LL &&
          units_length[2].factor == 1000000LL && units_length[3].factor == 1000000000LL,
          "mm/cm/m/km factors match the spec");
    CHECK(units_length[4].factor == 25400LL && units_length[5].factor == 304800LL &&
          units_length[6].factor == 1609344000LL,
          "in/ft/mi factors match the spec");
    CHECK(units_mass[3].factor == 28349523LL && units_mass[4].factor == 453592370LL,
          "oz/lb factors match the spec");
    CHECK(units_time[3].factor == 86400000000LL, "day factor matches the spec");
    CHECK(units_data[2].factor == 1048576000000LL &&
          units_data[3].factor == 1073741824000000LL,
          "MB/GB factors match the spec");

    CHECK(strcmp(units_temp[0].name, "C") == 0 &&
          strcmp(units_temp[1].name, "F") == 0 &&
          strcmp(units_temp[2].name, "K") == 0,
          "temperature units are C / F / K");
    CHECK(find_unit_idx(0, "mi") == 6, "\"mi\" is length unit 6");
    CHECK(find_unit_idx(3, "GB") == 3, "\"GB\" is data unit 3");
    CHECK(find_unit_idx(0, "nope") == -1, "unknown unit name -> -1");
}

static void test_defaults(void) {
    section("default state");

    unit_reset();
    CHECK(g_unit.cat == 0, "default category is Length");
    CHECK(g_unit.from == 2 && strcmp(g_cats[0].units[g_unit.from].name, "m") == 0,
          "default FROM unit is m");
    CHECK(g_unit.to == 5 && strcmp(g_cats[0].units[g_unit.to].name, "ft") == 0,
          "default TO unit is ft");
    CHECK(g_unit.value == 1, "default value is 1");
    CHECK(g_unit.msg[0] == '\0', "default message line is empty");
}

static void test_default_screen(void) {
    section("rendering: default screen (matches the app-spec layout)");

    char out[32];
    unit_reset();
    g_unit.value = 12;

    const char *want =
        "=== Unit Converter ===\n"
        "Category: Length   (< > to change)\n"
        "From: 12 [m]      To: [ft]\n"
        "Result: 39.37 ft\n"
        "e=edit value  s=swap  <>=category  ^v=from unit\n";
    CHECK_STR("full default screen with value 12", render(), want);

    conv_idx(0, g_unit.from, g_unit.to, 12, out);
    CHECK_STR("12 m -> ft (spec layout number)", out, "39.37");
}

static void test_length(void) {
    section("conversions: Length");
    char out[32];

    conv_str(0, "mi", "m", 1, out);
    CHECK_STR("1 mi -> 1609.34 m", out, "1609.34");
    conv_str(0, "mi", "km", 1, out);
    CHECK_STR("1 mi -> 1.60 km", out, "1.60");
    conv_str(0, "ft", "m", 12, out);
    CHECK_STR("12 ft -> 3.65 m (truncated)", out, "3.65");
    conv_str(0, "m", "ft", 12, out);
    CHECK_STR("12 m -> 39.37 ft", out, "39.37");
    conv_str(0, "m", "ft", 1, out);
    CHECK_STR("1 m -> 3.28 ft", out, "3.28");
    conv_str(0, "cm", "in", 10, out);
    CHECK_STR("10 cm -> 3.93 in (truncated)", out, "3.93");
    conv_str(0, "mm", "m", 1000, out);
    CHECK_STR("1000 mm -> 1.00 m", out, "1.00");
    conv_str(0, "km", "mi", 1, out);
    CHECK_STR("1 km -> 0.62 mi", out, "0.62");
    conv_str(0, "in", "cm", 1, out);
    CHECK_STR("1 in -> 2.54 cm", out, "2.54");
    conv_str(0, "ft", "mi", 5280, out);
    CHECK_STR("5280 ft -> 1.00 mi", out, "1.00");
    conv_str(0, "mm", "mm", 5, out);
    CHECK_STR("5 mm -> 5.00 mm (same unit)", out, "5.00");
    conv_str(0, "ft", "m", 0, out);
    CHECK_STR("0 ft -> 0.00 m", out, "0.00");
    conv_str(0, "ft", "m", -12, out);
    CHECK_STR("-12 ft -> -3.65 m", out, "-3.65");
    conv_str(0, "ft", "m", -1, out);
    CHECK_STR("-1 ft -> -0.30 m (sign kept on sub-1)", out, "-0.30");
    conv_str(0, "mm", "m", -1, out);
    CHECK_STR("-1 mm -> 0.00 m (negative zero collapses)", out, "0.00");
}

static void test_mass(void) {
    section("conversions: Mass");
    char out[32];

    conv_str(1, "kg", "g", 1, out);
    CHECK_STR("1 kg -> 1000.00 g", out, "1000.00");
    conv_str(1, "lb", "kg", 1, out);
    CHECK_STR("1 lb -> 0.45 kg", out, "0.45");
    conv_str(1, "oz", "g", 1, out);
    CHECK_STR("1 oz -> 28.34 g", out, "28.34");
    conv_str(1, "g", "oz", 100, out);
    CHECK_STR("100 g -> 3.52 oz", out, "3.52");
    conv_str(1, "mg", "g", 1000, out);
    CHECK_STR("1000 mg -> 1.00 g", out, "1.00");
    conv_str(1, "kg", "lb", 1, out);
    CHECK_STR("1 kg -> 2.20 lb", out, "2.20");
    conv_str(1, "lb", "mg", 0, out);
    CHECK_STR("0 lb -> 0.00 mg", out, "0.00");
}

static void test_time(void) {
    section("conversions: Time");
    char out[32];

    conv_str(2, "day", "h", 1, out);
    CHECK_STR("1 day -> 24.00 h", out, "24.00");
    conv_str(2, "min", "h", 90, out);
    CHECK_STR("90 min -> 1.50 h", out, "1.50");
    conv_str(2, "s", "h", 3600, out);
    CHECK_STR("3600 s -> 1.00 h", out, "1.00");
    conv_str(2, "h", "s", 1, out);
    CHECK_STR("1 h -> 3600.00 s", out, "3600.00");
    conv_str(2, "day", "min", 1, out);
    CHECK_STR("1 day -> 1440.00 min", out, "1440.00");
    conv_str(2, "s", "s", 0, out);
    CHECK_STR("0 s -> 0.00 s (same unit)", out, "0.00");
}

static void test_data(void) {
    section("conversions: Data");
    char out[32];

    conv_str(3, "MB", "GB", 1024, out);
    CHECK_STR("1024 MB -> 1.00 GB", out, "1.00");
    conv_str(3, "GB", "MB", 1, out);
    CHECK_STR("1 GB -> 1024.00 MB", out, "1024.00");
    conv_str(3, "KB", "B", 500, out);
    CHECK_STR("500 KB -> 512000.00 B", out, "512000.00");
    conv_str(3, "B", "KB", 2048, out);
    CHECK_STR("2048 B -> 2.00 KB", out, "2.00");
    conv_str(3, "GB", "B", 1, out);
    CHECK_STR("1 GB -> 1073741824.00 B", out, "1073741824.00");
    conv_str(3, "B", "GB", 0, out);
    CHECK_STR("0 B -> 0.00 GB", out, "0.00");
}

static void test_temperature(void) {
    section("conversions: Temperature");
    char out[32];

    conv_str(4, "C", "F", 100, out);
    CHECK_STR("100 C -> 212 F (exact)", out, "212");
    conv_str(4, "F", "C", 32, out);
    CHECK_STR("32 F -> 0 C (exact)", out, "0");
    conv_str(4, "K", "C", 300, out);
    CHECK_STR("300 K -> 26 C (spec example)", out, "26");
    conv_str(4, "K", "C", 273, out);
    CHECK_STR("273 K -> 0 C", out, "0");
    conv_str(4, "C", "K", 0, out);
    CHECK_STR("0 C -> 273 K", out, "273");
    conv_str(4, "C", "K", 100, out);
    CHECK_STR("100 C -> 373 K", out, "373");
    conv_str(4, "C", "K", 37, out);
    CHECK_STR("37 C -> 310 K", out, "310");
    conv_str(4, "C", "F", -40, out);
    CHECK_STR("-40 C -> -40 F (exact)", out, "-40");
    conv_str(4, "F", "C", -40, out);
    CHECK_STR("-40 F -> -40 C (exact)", out, "-40");
    conv_str(4, "F", "C", 50, out);
    CHECK_STR("50 F -> 10 C (exact)", out, "10");
    conv_str(4, "F", "C", 98, out);
    CHECK_STR("98 F -> 36 C (36.66 truncated)", out, "36");
    conv_str(4, "K", "F", 300, out);
    CHECK_STR("300 K -> 80 F (via C)", out, "80");
    conv_str(4, "K", "C", 0, out);
    CHECK_STR("0 K -> -273 C", out, "-273");
    conv_str(4, "C", "C", 21, out);
    CHECK_STR("21 C -> 21 C (identity)", out, "21");
    conv_str(4, "F", "F", 32, out);
    CHECK_STR("32 F -> 32 F (identity)", out, "32");
}

static void test_parse(void) {
    section("value parsing");
    long v = 123;
    int clamped = 9;

    CHECK(unit_parse_value("", &v, &clamped) == -1, "rejects empty string");
    CHECK(unit_parse_value("   ", &v, &clamped) == -1, "rejects blank string");
    CHECK(unit_parse_value("abc", &v, &clamped) == -1, "rejects letters");
    CHECK(unit_parse_value("12x", &v, &clamped) == -1, "rejects trailing junk");
    CHECK(unit_parse_value("1 2", &v, &clamped) == -1, "rejects embedded spaces");
    CHECK(unit_parse_value("1.5", &v, &clamped) == -1, "rejects decimals");
    CHECK(unit_parse_value("+", &v, &clamped) == -1, "rejects lone +");
    CHECK(unit_parse_value("-", &v, &clamped) == -1, "rejects lone -");
    CHECK(unit_parse_value("--5", &v, &clamped) == -1, "rejects double sign");
    CHECK(unit_parse_value("++5", &v, &clamped) == -1, "rejects double plus");

    CHECK(unit_parse_value("42", &v, &clamped) == 0 && v == 42 && clamped == 0,
          "accepts 42");
    CHECK(unit_parse_value("+12", &v, &clamped) == 0 && v == 12 && clamped == 0,
          "accepts +12");
    CHECK(unit_parse_value("-12", &v, &clamped) == 0 && v == -12 && clamped == 0,
          "accepts -12");
    CHECK(unit_parse_value("0", &v, &clamped) == 0 && v == 0, "accepts 0");
    CHECK(unit_parse_value("007", &v, &clamped) == 0 && v == 7, "accepts 007");
    CHECK(unit_parse_value("  7  ", &v, &clamped) == 0 && v == 7,
          "accepts padded \"  7  \"");
    CHECK(unit_parse_value("-0", &v, &clamped) == 0 && v == 0, "accepts -0");

    CHECK(unit_parse_value("9223372036854775807", &v, &clamped) == 0 &&
          clamped == 0 && v == (long)9223372036854775807LL,
          "accepts INT64_MAX exactly");
    CHECK(unit_parse_value("-9223372036854775808", &v, &clamped) == 0 &&
          clamped == 0 && v == (long)(-9223372036854775807LL - 1),
          "accepts INT64_MIN exactly");
    CHECK(unit_parse_value("9223372036854775808", &v, &clamped) == 0 &&
          clamped == 1 && v == (long)9223372036854775807LL,
          "clamps INT64_MAX+1");
    CHECK(unit_parse_value("-9223372036854775809", &v, &clamped) == 0 &&
          clamped == 1 && v == (long)(-9223372036854775807LL - 1),
          "clamps INT64_MIN-1");
    CHECK(unit_parse_value("99999999999999999999", &v, &clamped) == 0 && clamped == 1,
          "clamps 20-digit input");
}

static void test_apply_value_text(void) {
    section("dialog value application");
    unit_reset();
    g_unit.value = 7;

    CHECK(unit_apply_value_text("abc") == 0, "apply rejects junk");
    CHECK(g_unit.value == 7, "rejected edit leaves the value unchanged");
    CHECK(strstr(g_unit.msg, "Invalid value") != NULL,
          "rejected edit sets the error line");

    CHECK(unit_apply_value_text("-12") == 1 && g_unit.value == -12,
          "apply accepts -12");
    CHECK(g_unit.msg[0] == '\0', "successful edit clears the error line");

    CHECK(unit_apply_value_text("99999999999999999999") == 1 &&
          g_unit.value == (long)9223372036854775807LL,
          "apply clamps out-of-range input");
    CHECK(strstr(g_unit.msg, "clamped") != NULL,
          "clamped edit sets the notice line");

    CHECK(unit_apply_value_text("5") == 1 && g_unit.value == 5 &&
          g_unit.msg[0] == '\0',
          "next valid edit clears the clamp notice");
}

static void test_cycling(void) {
    section("category / unit cycling");
    struct gui_event e;

    unit_reset();
    e = ev_key(GUI_EV_RIGHT);
    unit_handle_event(&e);
    CHECK(g_unit.cat == 1, "RIGHT moves Length -> Mass");
    CHECK(g_unit.from == 0 && g_unit.to == 1,
          "category change resets from/to to 0/1");

    e = ev_key(GUI_EV_LEFT);
    unit_handle_event(&e);
    CHECK(g_unit.cat == 0, "LEFT moves back to Length");

    e = ev_key(GUI_EV_LEFT);
    unit_handle_event(&e);
    CHECK(g_unit.cat == 4, "LEFT from Length wraps to Temperature");
    CHECK(g_unit.from == 0 && strcmp(g_cats[4].units[0].name, "C") == 0,
          "Temperature FROM starts at C");
    e = ev_key(GUI_EV_RIGHT);
    unit_handle_event(&e);
    CHECK(g_unit.cat == 0, "RIGHT from Temperature wraps to Length");

    unit_reset();
    g_unit.from = 6;
    unit_cycle_from(1);
    CHECK(g_unit.from == 0, "cycle_from(+1) wraps mi -> mm");
    unit_cycle_from(-1);
    CHECK(g_unit.from == 6, "cycle_from(-1) wraps mm -> mi");
    g_unit.from = 3;
    unit_cycle_from(1);
    CHECK(g_unit.from == 4, "cycle_from(+1) km -> in");

    unit_reset();
    g_unit.to = 6;
    unit_cycle_to(1);
    CHECK(g_unit.to == 0, "cycle_to(+1) wraps ft(6) -> mm");
    CHECK(g_unit.from == 2, "cycling TO leaves FROM alone");
    unit_cycle_to(-1);
    CHECK(g_unit.to == 6, "cycle_to(-1) wraps back");

    unit_reset();
    unit_cycle_cat(4);                        /* Temperature, from=0, to=1 */
    g_unit.from = 2;
    unit_cycle_from(1);
    CHECK(g_unit.from == 0, "temperature FROM wraps K -> C (3 units)");
    CHECK(g_unit.to == 1, "cycling FROM leaves TO alone");

    unit_reset();
    unit_apply_value_text("zzz");
    CHECK(g_unit.msg[0] != '\0', "error line set before cycling");
    unit_cycle_from(1);
    CHECK(g_unit.msg[0] == '\0', "cycling clears the error line");
}

static void test_swap(void) {
    section("swap");
    char out[32];
    struct gui_event e;

    unit_reset();
    g_unit.value = 12;
    unit_swap();
    CHECK(g_unit.from == 5 && g_unit.to == 2, "swap exchanges FROM and TO");
    CHECK(g_unit.value == 12, "swap leaves the value alone");
    conv_idx(0, g_unit.from, g_unit.to, g_unit.value, out);
    CHECK_STR("12 ft -> m after swap", out, "3.65");
    unit_swap();
    CHECK(g_unit.from == 2 && g_unit.to == 5, "swap twice restores FROM/TO");

    unit_apply_value_text("bad");
    unit_swap();
    CHECK(g_unit.msg[0] == '\0', "swap clears the error line");

    unit_reset();
    g_unit.value = 12;
    e = ev_char('s');
    CHECK(unit_handle_event(&e) == 1 && g_unit.from == 5 && g_unit.to == 2,
          "'s' key swaps FROM/TO");
    CHECK(strstr(render(), "From: 12 [ft]      To: [m]\n") != NULL,
          "swap updates the From/To screen line");
    CHECK(strstr(render(), "Result: 3.65 m\n") != NULL,
          "swap result line is 12 ft -> 3.65 m");
}

static void test_saturation(void) {
    section("int64 saturation / clamped marker");
    struct conv_out r;
    char out[32];

    unit_reset();
    g_unit.cat = 0;
    g_unit.from = find_unit_idx(0, "mi");
    g_unit.to = find_unit_idx(0, "m");
    g_unit.value = 10000000000L;   /* 1e10 mi * 1609344000 overflows int64 */
    unit_convert(&r);
    CHECK(r.clamped == 1, "1e10 mi -> m saturates on the multiply");
    CHECK(r.whole == 9223372036854775807LL / 1000000LL,
          "clamped whole is INT64_MAX / 1e6");
    unit_format_value(out, 0, &r);
    CHECK_STR("clamped positive result string", out, "9223372036854.77");

    g_unit.value = -10000000000L;
    unit_convert(&r);
    CHECK(r.clamped == 1, "-1e10 mi -> m saturates (negative)");
    CHECK(r.whole == (-9223372036854775807LL - 1) / 1000000LL,
          "clamped whole is INT64_MIN / 1e6");
    unit_format_value(out, 0, &r);
    CHECK_STR("clamped negative result string", out, "-9223372036854.77");

    g_unit.value = (long)(9223372036854775807LL / 1609344000LL);
    unit_convert(&r);
    CHECK(r.clamped == 0, "value = INT64_MAX / mi_factor does not clamp");

    g_unit.cat = 2;
    g_unit.from = 3;   /* day */
    g_unit.to = 2;     /* h   */
    g_unit.value = 1000000L;
    unit_convert(&r);
    CHECK(r.clamped == 0, "1e6 days -> h does not clamp");

    unit_reset();
    g_unit.cat = 0;
    g_unit.from = 6;
    g_unit.to = 2;
    g_unit.value = 10000000000L;
    CHECK(strstr(render(), "Result: 9223372036854.77 m (clamped)\n") != NULL,
          "render marks the clamped result");

    unit_reset();
    unit_cycle_cat(4);                 /* Temperature, from=0 (C), to=1 (F) */
    g_unit.value = (long)9223372036854775807LL;
    unit_convert(&r);
    CHECK(r.clamped == 1, "temperature value * 100 saturates");
}

static void test_render(void) {
    section("rendering");
    char copy[UNIT_SCREEN_MAX];
    const char *scr;

    unit_reset();
    for (int i = 0; i < 4; i++) {
        struct gui_event e = ev_key(GUI_EV_RIGHT);
        unit_handle_event(&e);
    }
    CHECK(g_unit.cat == 4, "four RIGHT events reach Temperature");
    unit_apply_value_text("100");
    scr = render();
    CHECK(strstr(scr, "Category: Temperature   (< > to change)\n") != NULL,
          "temperature category line");
    CHECK(strstr(scr, "From: 100 [C]      To: [F]\n") != NULL,
          "temperature From/To line");
    CHECK(strstr(scr, "Result: 212 F\n") != NULL,
          "temperature result line (100 C -> 212 F)");

    unit_reset();
    unit_apply_value_text("nope");
    scr = render();
    CHECK(strstr(scr, "! Invalid value") != NULL, "render shows the error line");
    unit_apply_value_text("3");
    scr = render();
    CHECK(strstr(scr, "! ") == NULL, "render drops the error line after a valid edit");

    unit_reset();
    strcpy(copy, render());
    CHECK(strcmp(copy, render()) == 0, "rendering is deterministic");

    unit_reset();
    g_unit.value = (long)9223372036854775807LL;
    scr = render();
    CHECK((int)strlen(scr) < 1800, "screen fits the 2048-byte window buffer");
    int maxlen = 0, cur = 0;
    for (const char *p = scr; *p; p++) {
        if (*p == '\n') {
            if (cur > maxlen) maxlen = cur;
            cur = 0;
        } else {
            cur++;
        }
    }
    CHECK(maxlen <= 110, "no rendered line exceeds 110 chars");

    unit_reset();
    unit_cycle_from(1);
    unit_cycle_from(1);                /* m -> km -> in */
    scr = render();
    CHECK(strstr(scr, "From: 1 [in]") != NULL, "cycling FROM updates the screen");

    char tiny[8];
    int n = unit_render_to(tiny, (int)sizeof(tiny));
    CHECK(n == 7 && (int)strlen(tiny) == 7, "render respects a small buffer cap");
}

static void test_events(void) {
    section("event handling");
    struct gui_event e;

    /* arrows */
    unit_reset();
    e = ev_key(GUI_EV_UP);
    CHECK(unit_handle_event(&e) == 1 && g_unit.from == 3,
          "UP cycles FROM toward the higher unit");
    e = ev_key(GUI_EV_DOWN);
    CHECK(unit_handle_event(&e) == 1 && g_unit.from == 2,
          "DOWN cycles FROM back");
    e = ev_key(GUI_EV_ESC);
    CHECK(unit_handle_event(&e) == 0, "ESC needs no redraw");

    /* menu 0 "Convert": Next From, Next To, Swap, Edit Value */
    unit_reset();
    e = ev_menu(0, 0);
    CHECK(unit_handle_event(&e) == 1 && g_unit.from == 3,
          "menu Convert > Next From");
    e = ev_menu(0, 1);
    CHECK(unit_handle_event(&e) == 1 && g_unit.to == 6,
          "menu Convert > Next To");
    e = ev_menu(0, 2);
    CHECK(unit_handle_event(&e) == 1 && g_unit.from == 6 && g_unit.to == 3,
          "menu Convert > Swap");
    e = ev_menu(1, 0);
    CHECK(unit_handle_event(&e) == 0, "unknown menu index ignored");
    e = ev_menu(0, 9);
    CHECK(unit_handle_event(&e) == 0, "unknown menu item ignored");
    /* menu item 3 (Edit Value) opens the blocking dialog; covered through
     * unit_apply_value_text() instead (see test_apply_value_text). */

    /* char keys */
    unit_reset();
    e = ev_char('x');
    CHECK(unit_handle_event(&e) == 0, "unknown char ignored");
    e = ev_char('S');
    CHECK(unit_handle_event(&e) == 1 && g_unit.from == 5 && g_unit.to == 2,
          "'S' (shift-s) also swaps");
    e = ev_char('\n');
    CHECK(unit_handle_event(&e) == 0, "newline ignored");

    /* mouse hits */
    unit_reset();
    e = ev_mouse(4, 2, 1);
    CHECK(unit_handle_event(&e) == 1 && g_unit.from == 3,
          "left click on the From cell cycles FROM");
    e = ev_mouse(30, 2, 1);
    CHECK(unit_handle_event(&e) == 1 && g_unit.to == 6,
          "left click on the To cell cycles TO");
    e = ev_mouse(4, 1, 1);
    CHECK(unit_handle_event(&e) == 1 && g_unit.cat == 1,
          "left click on the category row cycles the category");
    e = ev_mouse(4, 2, 2);
    CHECK(unit_handle_event(&e) == 0, "right click ignored");
    e = ev_mouse(4, 9, 1);
    CHECK(unit_handle_event(&e) == 0, "click outside interactive rows ignored");
}

/* ================= main ================= */

int main(void) {
    printf("=== unit.c host tests (Unit Converter) ===\n");

    test_unit_tables();
    test_defaults();
    test_default_screen();
    test_length();
    test_mass();
    test_time();
    test_data();
    test_temperature();
    test_parse();
    test_apply_value_text();
    test_cycling();
    test_swap();
    test_saturation();
    test_render();
    test_events();

    printf("\n=== Test summary ===\n");
    printf("checks run:    %d\n", checks_run);
    printf("checks passed: %d\n", checks_run - checks_failed);
    printf("checks failed: %d\n", checks_failed);
    if (checks_failed == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\nTESTS FAILED\n");
    return 1;
}
