/*
 * unit.c - Unit Converter for HobbyOS.
 *
 * A windowed text app. Converts a whole-number value between units of five
 * categories (index order):
 *   0 Length      1 Mass      2 Time      3 Data      4 Temperature
 *
 * Value model
 * -----------
 * g_unit.value is a whole number expressed in FROM units. Every tabled unit
 * stores its size in micro-units of the category base (1e-6 of the base unit
 * the user sees), e.g. 1 ft = 0.3048 m = 304800 micro-m. Conversion is
 *
 *     result (in TO units) = value * from_factor / to_factor
 *
 * done entirely in 64-bit integer math (no floats). The result is shown with
 * exactly two decimals, TRUNCATED toward zero (C integer division):
 * whole = num / den, hundredths = |(num % den) * 100 / den|. Examples:
 *   12 ft -> m : 12*304800 = 3657600 -> 3.6576  -> "3.65"
 *   1 mi -> m  : 1609344000            -> 1609.34
 *   10 cm -> in: 100000 / 25400        -> 3.937  -> "3.93"
 *
 * Overflow: if value * from_factor would not fit in int64 the product
 * saturates to INT64_MAX / INT64_MIN and the result line is marked
 * "(clamped)".
 *
 * Temperature is special-cased (C/F/K): conversions run in hundredths of a
 * degree using the 273.15 offset (27315 hundredths) and F = C*9/5 + 32,
 * with every division truncating toward zero. Results are shown as whole
 * degrees (truncated), e.g. 100 C -> "212 F", 32 F -> "0 C" and the spec's
 * example 300 K -> 26.85 -> "26 C".
 *
 * Keys:  left/right = category (wraps)   up/down = FROM unit (wraps)
 *        s = swap FROM/TO                 e = edit value (dialog)
 * Menu 0 "Convert": Next From, Next To, Swap, Edit Value.
 */

#include "libc.h"
#include "gui.h"
#include "dialog.h"

/* ---- constants ---- */

#define NUM_CATS        5
#define UNIT_MSG_MAX    64
#define UNIT_SCREEN_MAX 512

/* Startup defaults: Length, m -> ft (the screen shown in the app spec). */
#define DEF_CAT         0
#define DEF_FROM        2   /* "m"  */
#define DEF_TO          5   /* "ft" */
#define DEF_VALUE       1

/* ---- unit tables ---- */

struct unit_def {
    const char *name;
    long long factor;   /* unit size in micro-units (1e-6) of the base unit */
};

struct unit_cat {
    const char *name;
    const struct unit_def *units;
    int count;
    int temp;           /* 1 = C/F/K, conversions special-cased */
};

static const struct unit_def units_length[] = {
    {"mm", 1000LL}, {"cm", 10000LL}, {"m", 1000000LL}, {"km", 1000000000LL},
    {"in", 25400LL}, {"ft", 304800LL}, {"mi", 1609344000LL},
};

static const struct unit_def units_mass[] = {
    {"mg", 1000LL}, {"g", 1000000LL}, {"kg", 1000000000LL},
    {"oz", 28349523LL}, {"lb", 453592370LL},
};

static const struct unit_def units_time[] = {
    {"s", 1000000LL}, {"min", 60000000LL}, {"h", 3600000000LL},
    {"day", 86400000000LL},
};

static const struct unit_def units_data[] = {
    {"B", 1000000LL}, {"KB", 1024000000LL}, {"MB", 1048576000000LL},
    {"GB", 1073741824000000LL},
};

/* Temperature factors are unused (conversions are special-cased); 1 keeps
 * the generic sanity invariant "every factor is positive" true. */
static const struct unit_def units_temp[] = {
    {"C", 1LL}, {"F", 1LL}, {"K", 1LL},
};

static const struct unit_cat g_cats[NUM_CATS] = {
    {"Length",      units_length, 7, 0},
    {"Mass",        units_mass,   5, 0},
    {"Time",        units_time,   4, 0},
    {"Data",        units_data,   4, 0},
    {"Temperature", units_temp,   3, 1},
};

/* ---- application state ---- */

struct unit_state {
    int cat;                 /* category index 0..NUM_CATS-1 */
    int from;                /* unit index inside the category */
    int to;                  /* unit index inside the category */
    long value;              /* whole number in FROM units */
    char msg[UNIT_MSG_MAX];  /* status/error line, "" = none */
};

static struct unit_state g_unit;

static void unit_reset(void) {
    g_unit.cat = DEF_CAT;
    g_unit.from = DEF_FROM;
    g_unit.to = DEF_TO;
    g_unit.value = DEF_VALUE;
    g_unit.msg[0] = '\0';
}

/* ---- saturating integer helpers ----
 * Both set *clamped = 1 and return the saturated end of the int64 range
 * instead of overflowing. */

static long long sat_mul(long long a, long long b, int *clamped) {
    if (a == 0 || b == 0) return 0;
    if (a > 0) {
        if (b > 0) {
            if (a > 9223372036854775807LL / b) { *clamped = 1; return 9223372036854775807LL; }
        } else {
            if (b < (-9223372036854775807LL - 1) / a) { *clamped = 1; return (-9223372036854775807LL - 1); }
        }
    } else {
        if (b > 0) {
            if (a < (-9223372036854775807LL - 1) / b) { *clamped = 1; return (-9223372036854775807LL - 1); }
        } else {
            /* b < 0: a*b > INT64_MAX  <=>  a < INT64_MAX/b */
            if (a < 9223372036854775807LL / b) { *clamped = 1; return 9223372036854775807LL; }
        }
    }
    return a * b;
}

static long long sat_add(long long a, long long b, int *clamped) {
    if (b > 0 && a > 9223372036854775807LL - b) { *clamped = 1; return 9223372036854775807LL; }
    if (b < 0 && a < (-9223372036854775807LL - 1) - b) { *clamped = 1; return (-9223372036854775807LL - 1); }
    return a + b;
}

/* ---- state transitions ---- */

/* Move category by delta (wraps). The unit tables differ per category, so
 * from/to restart at the first two units. */
static void unit_cycle_cat(int delta) {
    int n = NUM_CATS;
    g_unit.cat = ((g_unit.cat + delta) % n + n) % n;
    g_unit.from = 0;
    g_unit.to = 1;  /* every category has at least two units */
    g_unit.msg[0] = '\0';
}

/* Move the FROM unit by delta inside the category (wraps). */
static void unit_cycle_from(int delta) {
    int n = g_cats[g_unit.cat].count;
    g_unit.from = ((g_unit.from + delta) % n + n) % n;
    g_unit.msg[0] = '\0';
}

/* Move the TO unit by delta inside the category (wraps). */
static void unit_cycle_to(int delta) {
    int n = g_cats[g_unit.cat].count;
    g_unit.to = ((g_unit.to + delta) % n + n) % n;
    g_unit.msg[0] = '\0';
}

/* Exchange FROM and TO (the value keeps its meaning in FROM units). */
static void unit_swap(void) {
    int t = g_unit.from;
    g_unit.from = g_unit.to;
    g_unit.to = t;
    g_unit.msg[0] = '\0';
}

/* ---- value parsing ---- */

/* Parse a signed decimal integer from `text`.
 * Accepts optional surrounding spaces, an optional '+'/'-' sign and at least
 * one digit; anything else is rejected. On magnitude overflow the value is
 * clamped to the int64 limits and *clamped is set to 1.
 * Returns 0 on success, -1 on invalid input. */
static int unit_parse_value(const char *text, long *out, int *clamped) {
    int i = 0;
    int neg = 0;
    int overflow = 0;
    unsigned long long mag = 0;
    unsigned long long limit;

    *clamped = 0;
    while (text[i] == ' ') i++;
    if (text[i] == '+' || text[i] == '-') {
        neg = (text[i] == '-');
        i++;
    }
    if (text[i] < '0' || text[i] > '9') return -1;   /* need >= 1 digit */

    limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    while (text[i] >= '0' && text[i] <= '9') {
        unsigned long long d = (unsigned long long)(text[i] - '0');
        if (mag > (limit - d) / 10ULL) overflow = 1;
        else mag = mag * 10ULL + d;
        i++;
    }
    while (text[i] == ' ') i++;
    if (text[i] != '\0') return -1;                  /* junk after the number */

    if (overflow) {
        *clamped = 1;
        mag = limit;
    }
    if (neg) {
        if (mag == 9223372036854775808ULL)
            *out = (long)(-(9223372036854775807LL) - 1);  /* -2^63, exact */
        else
            *out = -(long)mag;
    } else {
        *out = (long)mag;
    }
    return 0;
}

/* Apply the text entered in the value dialog. Returns 1 if the value was
 * set, 0 if the text was rejected (an error line is left in g_unit.msg). */
static int unit_apply_value_text(const char *text) {
    long v;
    int clamped;

    if (unit_parse_value(text, &v, &clamped) != 0) {
        gui_strncpy(g_unit.msg,
                    "Invalid value. Enter a whole number like 42 or -12.",
                    UNIT_MSG_MAX);
        return 0;
    }
    g_unit.value = v;
    if (clamped) {
        gui_strncpy(g_unit.msg, "Value out of range; clamped to limit.",
                    UNIT_MSG_MAX);
    } else {
        g_unit.msg[0] = '\0';
    }
    return 1;
}

/* ---- conversion ---- */

struct conv_out {
    long long whole;  /* truncated integer part of the result, signed */
    int frac;         /* magnitude of the hundredths pair, 0..99 */
    int neg;          /* 1 if the (untruncated) result is negative */
    int clamped;      /* 1 if a multiplication saturated */
};

/* Temperature conversions in hundredths of a degree (indices 0=C 1=F 2=K).
 * F <-> C uses *9/5 and +32; C <-> K uses the 273.15 offset (27315
 * hundredths). Every division truncates toward zero. */
static long long temp_convert(int from, int to, long long v100, int *clamped) {
    long long c;   /* value in hundredths of a degree Celsius */
    long long o;   /* value in hundredths of the target unit */

    if (from == 1) {                       /* F -> C */
        long long t = sat_add(v100, -3200LL, clamped);
        t = sat_mul(t, 5LL, clamped);
        c = t / 9LL;
    } else if (from == 2) {                /* K -> C */
        c = sat_add(v100, -27315LL, clamped);
    } else {                               /* C */
        c = v100;
    }

    if (to == 1) {                         /* C -> F */
        long long t = sat_mul(c, 9LL, clamped);
        t = t / 5LL;
        o = sat_add(t, 3200LL, clamped);
    } else if (to == 2) {                  /* C -> K */
        o = sat_add(c, 27315LL, clamped);
    } else {                               /* C */
        o = c;
    }
    return o;
}

/* Compute the conversion of g_unit.value from the current FROM unit to the
 * current TO unit. */
static void unit_convert(struct conv_out *r) {
    const struct unit_cat *cat = &g_cats[g_unit.cat];
    long long whole;
    long long rem;

    r->clamped = 0;
    if (cat->temp) {
        long long v100 = sat_mul(g_unit.value, 100LL, &r->clamped);
        long long o100 = temp_convert(g_unit.from, g_unit.to, v100, &r->clamped);
        whole = o100 / 100;
        rem = o100 % 100;
    } else {
        long long from_f = cat->units[g_unit.from].factor;
        long long to_f = cat->units[g_unit.to].factor;
        long long num = sat_mul(g_unit.value, from_f, &r->clamped);
        whole = num / to_f;
        rem = (num % to_f) * 100 / to_f;   /* hundredths, truncates toward 0 */
    }

    r->whole = whole;
    r->frac = (int)(rem < 0 ? -rem : rem);
    r->neg = (whole < 0) || (rem < 0);
}

/* ---- output ---- */

/* Format the result number.
 * Layout categories: exactly two truncated decimals, e.g. "1609.34",
 * "0.00", "-3.65" ("-0.001" collapses to "0.00", never "-0.00").
 * Temperature: whole degrees, truncated toward zero, e.g. "212", "0". */
static void unit_format_value(char *out, int temp, const struct conv_out *r) {
    int n = 0;
    unsigned long mag;

    if (temp) {
        gui_itoa((long)r->whole, out);
        return;
    }

    if (r->neg && (r->whole != 0 || r->frac != 0)) out[n++] = '-';
    if (r->whole < 0) mag = (unsigned long)(-(r->whole + 1)) + 1UL;
    else mag = (unsigned long)r->whole;
    n += gui_uitoa(mag, out + n);
    out[n++] = '.';
    out[n++] = (char)('0' + (r->frac / 10) % 10);
    out[n++] = (char)('0' + r->frac % 10);
    out[n] = '\0';
}

/* Append s to out (bounded), keeping it NUL-terminated; returns new length. */
static int unit_put(char *out, int cap, int n, const char *s) {
    int i;
    if (cap <= 0) return 0;
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    for (i = 0; s[i] != '\0' && n < cap - 1; i++) out[n++] = s[i];
    out[n] = '\0';
    return n;
}

/* Build the complete screen into out (NUL-terminated); returns its length.
 * The window text buffer is 2048 bytes - the screen is ~250 chars. */
static int unit_render_to(char *out, int cap) {
    const struct unit_cat *cat = &g_cats[g_unit.cat];
    struct conv_out r;
    char num[24];
    int n = 0;

    if (cap <= 0) return 0;
    out[0] = '\0';

    n = unit_put(out, cap, n, "=== Unit Converter ===\n");
    n = unit_put(out, cap, n, "Category: ");
    n = unit_put(out, cap, n, cat->name);
    n = unit_put(out, cap, n, "   (< > to change)\n");

    n = unit_put(out, cap, n, "From: ");
    gui_itoa(g_unit.value, num);
    n = unit_put(out, cap, n, num);
    n = unit_put(out, cap, n, " [");
    n = unit_put(out, cap, n, cat->units[g_unit.from].name);
    n = unit_put(out, cap, n, "]      To: [");
    n = unit_put(out, cap, n, cat->units[g_unit.to].name);
    n = unit_put(out, cap, n, "]\n");

    unit_convert(&r);
    unit_format_value(num, cat->temp, &r);
    n = unit_put(out, cap, n, "Result: ");
    n = unit_put(out, cap, n, num);
    n = unit_put(out, cap, n, " ");
    n = unit_put(out, cap, n, cat->units[g_unit.to].name);
    if (r.clamped) n = unit_put(out, cap, n, " (clamped)");
    n = unit_put(out, cap, n, "\n");

    n = unit_put(out, cap, n,
                 "e=edit value  s=swap  <>=category  ^v=from unit\n");

    if (g_unit.msg[0] != '\0') {
        n = unit_put(out, cap, n, "! ");
        n = unit_put(out, cap, n, g_unit.msg);
        n = unit_put(out, cap, n, "\n");
    }
    return n;
}

/* Draw the whole screen (gui_clear() first, per the toolkit contract). */
static void unit_render(void) {
    char buf[UNIT_SCREEN_MAX];
    unit_render_to(buf, (int)sizeof(buf));
    print(buf);
}

/* ---- value dialog ---- */

/* Ask for a new value; only this path blocks on the dialog library. */
static void unit_edit_value(void) {
    char buf[24];
    buf[0] = '\0';
    if (dialog_prompt("Value", "Enter value:", buf, (int)sizeof(buf))) {
        unit_apply_value_text(buf);
    }
}

/* ---- event handling ---- */

/* Apply one event. Returns 1 if the screen needs a redraw. */
static int unit_handle_event(const struct gui_event *ev) {
    switch (ev->type) {
    case GUI_EV_LEFT:
        unit_cycle_cat(-1);
        return 1;
    case GUI_EV_RIGHT:
        unit_cycle_cat(1);
        return 1;
    case GUI_EV_UP:
        unit_cycle_from(1);
        return 1;
    case GUI_EV_DOWN:
        unit_cycle_from(-1);
        return 1;
    case GUI_EV_MENU:
        if (ev->menu != 0) return 0;
        switch (ev->item) {
        case 0: unit_cycle_from(1); return 1;   /* Next From  */
        case 1: unit_cycle_to(1);   return 1;   /* Next To    */
        case 2: unit_swap();        return 1;   /* Swap       */
        case 3: unit_edit_value();  return 1;   /* Edit Value */
        default: return 0;
        }
    case GUI_EV_MOUSE:
        if (ev->button != 1) return 0;
        if (ev->y == 1) {                       /* "Category:" row */
            unit_cycle_cat(1);
            return 1;
        }
        if (ev->y == 2) {                       /* "From: ... To: ..." row */
            if (ev->x < 18) unit_cycle_from(1);
            else unit_cycle_to(1);
            return 1;
        }
        return 0;
    case GUI_EV_CHAR:
        if (ev->ch == 's' || ev->ch == 'S') { unit_swap(); return 1; }
        if (ev->ch == 'e' || ev->ch == 'E') { unit_edit_value(); return 1; }
        return 0;
    default:
        return 0;   /* ESC and unknown events change nothing */
    }
}

/* ---- entry point ---- */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
    gui_set_title("Unit Converter");
    gui_enable_mouse();
    gui_add_menu(0, "Convert", "Next From,Next To,Swap,Edit Value");
    print_console("[APP] UNIT started\n");

    unit_reset();
    gui_clear();
    unit_render();

    for (;;) {
        struct gui_event ev;
        if (gui_read_event(&ev)) {
            if (unit_handle_event(&ev)) {
                gui_clear();
                unit_render();
            }
        }
    }

#ifdef HOST_TEST
    return 0;
#endif
}
