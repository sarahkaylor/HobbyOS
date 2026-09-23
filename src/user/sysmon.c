/*
 * sysmon.c - SysMon: the HobbyOS System Monitor (desktop app).
 *
 * A windowed system monitor:
 *   - live CPU utilisation and memory bars, sampled from sysinfo(5)/sysinfo(2)
 *   - uptime / CPU count / used+total memory on the info line
 *   - a scrollable process list from sysinfo(3) with short state words
 *   - "Monitor" menu: Kill / Refresh / Sort (PID order <-> name order)
 *   - keys: k = kill selected process (after confirmation), r = refresh,
 *     Up/Down = scroll the process list
 *
 * The window re-samples by itself every SYSMON_REFRESH_MS via
 * gui_read_event_timeout(); the whole screen is re-rendered into a text
 * buffer and pushed with gui_clear() + print(), so a given state always
 * renders to the same text (host tests depend on that).
 *
 * Usage contract (see gui.h):
 *   - Draw with print() after gui_clear() ("\f" clears the window)
 *   - Read input with gui_read_event() / gui_read_event_timeout()
 *   - Register menus with gui_add_menu() (libc.h)
 *
 * Host unit tests (src/host/sysmon_test.c) include this file with HOST_TEST
 * defined and drive the sm_* helpers directly; the g_confirm/g_kill hooks
 * exist so the kill path can be tested without real dialogs or signals.
 */

#include "libc.h"
#include "gui.h"
#include "dialog.h"

/* ---- Window / menu constants ---- */

#define SYSMON_TITLE      "SysMon"
#define SYSMON_MARKER     "[APP] SYSMON started\n"
#define SYSMON_MENU_NAME  "Monitor"
#define SYSMON_MENU_ITEMS "Kill,Refresh,Sort"
#define SYSMON_REFRESH_MS 1000   /* idle re-sample period */

/* ---- Layout constants ---- */

#define SYSMON_MAX_PROCS  64     /* fits the kernel's process table */
#define SYSMON_VISIBLE    12     /* process rows drawn at once */
#define SYSMON_BAR_WIDTH  24     /* CPU / memory bar width in columns */
#define SYSMON_RULE_WIDTH 44     /* separator width */
#define SYSMON_PID_WIDTH  4      /* PID column width (right aligned) */
#define SYSMON_NAME_WIDTH 16     /* process name column width (clipped) */
#define SYSMON_LIST_TOP_ROW 5    /* content row of the first process row */
#define SYSMON_SCREEN_MAX 2048   /* one rendered screen (fits the window) */

/* Process states, mirroring PROC_STATE_* in src/include/process.h.
 * The host test checks them against that header so they cannot drift. */
#define SYSMON_ST_FREE       0   /* PROC_STATE_FREE */
#define SYSMON_ST_ALLOCATED  1   /* PROC_STATE_ALLOCATED */
#define SYSMON_ST_READY      2   /* PROC_STATE_READY */
#define SYSMON_ST_RUNNING    3   /* PROC_STATE_RUNNING */
#define SYSMON_ST_EXITED     4   /* PROC_STATE_EXITED */
#define SYSMON_ST_BLOCKED    5   /* PROC_STATE_BLOCKED */
#define SYSMON_ST_WAIT_SPAWN 6   /* PROC_STATE_WAIT_SPAWN */

/* Sort modes for the "Sort" menu item */
#define SYSMON_SORT_PID  0
#define SYSMON_SORT_NAME 1

/* ---- CPU sample (two of these give a busy percentage) ---- */

struct sysmon_cpu_sample {
    uint64_t uptime_ms;
    uint64_t idle_ms;
    int num_cpus;
};

/* ---- Application state ---- */

static struct sys_procinfo g_procs[SYSMON_MAX_PROCS]; /* process table */
static int g_count = 0;                               /* valid entries */
static struct gui_list g_list = { 0, 0, 0, SYSMON_VISIBLE }; /* selection */

static struct sysmon_cpu_sample g_prev_sample; /* previous CPU sample */
static int g_have_prev = 0;                    /* 0 until the first sample */
static int g_cpu_pct = 0;                      /* busy % of the last interval */
static int g_num_cpus = 0;
static uint64_t g_uptime_ms = 0;
static uint64_t g_mem_total = 0;
static uint64_t g_mem_used = 0;
static int g_mem_pct = 0;
static int g_sort_mode = SYSMON_SORT_PID;

/* Hooks: the real dialog/signal functions in the OS build, replaceable in
 * host tests so the kill path can be exercised without blocking on stdin
 * or signalling host processes. */
typedef int (*sysmon_confirm_fn)(const char *title, const char *msg);
typedef int (*sysmon_kill_fn)(int pid, int sig);
static sysmon_confirm_fn g_confirm = dialog_confirm;
static sysmon_kill_fn g_kill = kill;

/* ================================================================== */
/* Small helpers                                                       */
/* ================================================================== */

/* Append s to out[cap] at position len; always NUL-terminated, never
 * overflows. Returns the new length. */
static int sm_put(char *out, int cap, int len, const char *s) {
    if (out == 0 || cap <= 0) return 0;
    if (len < 0) len = 0;
    if (len > cap - 1) len = cap - 1;
    for (int i = 0; s[i] != '\0' && len < cap - 1; i++) out[len++] = s[i];
    out[len] = '\0';
    return len;
}

/* floor(num * 100 / den) for num <= den, without 64-bit overflow.
 * Both operands are scaled down together when they are huge enough that
 * num * 100 would wrap (the ratio is preserved to ~1e-17). */
static int sm_pct_of(uint64_t num, uint64_t den) {
    if (den == 0) return 0;
    if (num >= den) return 100;
    while (num > UINT64_MAX / 100ULL) {
        num >>= 1;
        den >>= 1;
        if (den == 0) return 100;
    }
    return (int)(num * 100ULL / den);
}

/* Memory used percentage: 0 when empty/inconsistent, clamped to 0..100. */
static int sm_mem_pct(uint64_t total, uint64_t free_bytes) {
    if (total == 0) return 0;
    if (free_bytes >= total) return 0;
    return sm_pct_of(total - free_bytes, total);
}

/* CPU busy percentage between two samples. The first sample (prev == 0)
 * reports 0%; zero-time and bogus samples are guarded. */
static int sm_cpu_pct(const struct sysmon_cpu_sample *prev,
                      const struct sysmon_cpu_sample *cur) {
    if (prev == 0 || cur == 0) return 0;
    if (cur->uptime_ms <= prev->uptime_ms) return 0; /* no time passed */
    if (cur->num_cpus <= 0) return 0;
    uint64_t dup = cur->uptime_ms - prev->uptime_ms;
    uint64_t ncpu = (uint64_t)cur->num_cpus;
    if (dup > UINT64_MAX / ncpu) return 0;           /* bogus sample */
    uint64_t dtotal = dup * ncpu;
    if (dtotal == 0) return 0;                       /* divide-by-zero guard */
    uint64_t didle = 0;
    if (cur->idle_ms > prev->idle_ms) didle = cur->idle_ms - prev->idle_ms;
    if (didle > dtotal) didle = dtotal;              /* clamp inconsistent data */
    return sm_pct_of(dtotal - didle, dtotal);
}

/* Format an uptime in milliseconds as "HH:MM:SS", or "Nd HH:MM:SS" once a
 * full day has elapsed. out must hold at least 32 bytes. */
static void sm_fmt_uptime(uint64_t ms, char *out) {
    uint64_t secs = ms / 1000ULL;
    uint64_t days = secs / 86400ULL;
    uint64_t rem  = secs % 86400ULL;
    char tmp[24];
    int n = 0;
    out[0] = '\0';
    if (days > 0) {
        gui_uitoa((long)days, tmp);
        n = sm_put(out, 32, n, tmp);
        n = sm_put(out, 32, n, "d ");
    }
    gui_uitoa_z((unsigned long)(rem / 3600ULL), tmp, 2);
    out[n++] = tmp[0];
    out[n++] = tmp[1];
    out[n++] = ':';
    gui_uitoa_z((unsigned long)((rem % 3600ULL) / 60ULL), tmp, 2);
    out[n++] = tmp[0];
    out[n++] = tmp[1];
    out[n++] = ':';
    gui_uitoa_z((unsigned long)(rem % 60ULL), tmp, 2);
    out[n++] = tmp[0];
    out[n++] = tmp[1];
    out[n] = '\0';
}

/* Short word for a process state (PROC_STATE_* values); unknown values are
 * rendered as their number. out must hold at least 24 bytes. */
static int sm_state_word(int state, char *out) {
    const char *w = 0;
    switch (state) {
        case SYSMON_ST_FREE:       w = "free";    break;
        case SYSMON_ST_ALLOCATED:  w = "alloc";   break;
        case SYSMON_ST_READY:      w = "ready";   break;
        case SYSMON_ST_RUNNING:    w = "run";     break;
        case SYSMON_ST_EXITED:     w = "zombie";  break;
        case SYSMON_ST_BLOCKED:    w = "blocked"; break;
        case SYSMON_ST_WAIT_SPAWN: w = "wait";    break;
        default: break;
    }
    if (w != 0) {
        int i = 0;
        while (w[i] != '\0') { out[i] = w[i]; i++; }
        out[i] = '\0';
        return i;
    }
    return gui_itoa(state, out);
}

/* One list row: PID right-aligned in 4 columns, one space, the name padded
 * or clipped to 16 columns, one space, the state word. Writes a
 * NUL-terminated string into out (>= 48 bytes) and returns its length. */
static int sm_fmt_proc_row(const struct sys_procinfo *p, char *out) {
    int j = 0;
    j += gui_itoa_pad(p->pid, out + j, SYSMON_PID_WIDTH);
    out[j++] = ' ';
    gui_fit(out + j, SYSMON_NAME_WIDTH, p->name);
    j += SYSMON_NAME_WIDTH;
    out[j++] = ' ';
    j += sm_state_word(p->state, out + j);
    out[j] = '\0';
    return j;
}

/* ================================================================== */
/* Process list: sorting, selection, clamping                          */
/* ================================================================== */

/* Comparator for the active sort mode. Name order is case-sensitive (byte
 * order, like the toolkit's gui_strcmp) with the PID as a deterministic
 * tie-breaker, so equal names keep a stable, repeatable order. */
static int sm_cmp_procs(const struct sys_procinfo *a,
                        const struct sys_procinfo *b) {
    if (g_sort_mode == SYSMON_SORT_NAME) {
        int c = gui_strcmp(a->name, b->name);
        if (c != 0) return c;
    }
    if (a->pid < b->pid) return -1;
    if (a->pid > b->pid) return 1;
    return 0;
}

/* Stable insertion sort of g_procs[0..g_count). The table is tiny and the
 * userland libc has no qsort. */
static void sm_sort_procs(void) {
    for (int i = 1; i < g_count; i++) {
        struct sys_procinfo key = g_procs[i];
        int j = i - 1;
        while (j >= 0 && sm_cmp_procs(&g_procs[j], &key) > 0) {
            g_procs[j + 1] = g_procs[j];
            j--;
        }
        g_procs[j + 1] = key;
    }
}

/* Clamp selection and scroll window after the list changed (refresh, sort
 * or shrink). Never leaves the selection outside [0, count-1] nor the
 * window outside [0, max(0, count-visible)]. */
static void sm_clamp_list(void) {
    if (g_list.visible < 1) g_list.visible = 1;
    if (g_count < 0) g_count = 0;
    g_list.count = g_count;
    if (g_list.count == 0) {
        g_list.selected = 0;
        g_list.top = 0;
        return;
    }
    if (g_list.selected < 0) g_list.selected = 0;
    if (g_list.selected > g_list.count - 1) g_list.selected = g_list.count - 1;
    if (g_list.top < 0) g_list.top = 0;
    int maxtop = g_list.count - g_list.visible;
    if (maxtop < 0) maxtop = 0;
    if (g_list.top > maxtop) g_list.top = maxtop;
    gui_list_ensure_visible(&g_list); /* keep the selection on screen */
    if (g_list.top > maxtop) g_list.top = maxtop;
}

/* PID of the selected process, or -1 when the list is empty or the stored
 * selection index is stale. */
static int sm_selected_pid(void) {
    if (g_count <= 0) return -1;
    if (g_list.selected < 0 || g_list.selected >= g_count) return -1;
    return g_procs[g_list.selected].pid;
}

/* Replace the process table with a snapshot, re-sort it with the current
 * mode, keep the selection on keep_pid when that process is still around,
 * then clamp. keep_pid < 0 keeps the index instead. */
static void sm_load_procs(const struct sys_procinfo *list, int count,
                          int keep_pid) {
    int n = count;
    if (n < 0) n = 0;
    if (n > SYSMON_MAX_PROCS) n = SYSMON_MAX_PROCS;
    for (int i = 0; i < n; i++) g_procs[i] = list[i];
    g_count = n;
    g_list.count = n;
    sm_sort_procs();
    if (keep_pid >= 0) {
        for (int i = 0; i < g_count; i++) {
            if (g_procs[i].pid == keep_pid) { g_list.selected = i; break; }
        }
    }
    sm_clamp_list();
}

/* "Sort" menu item: flip PID order <-> name order, keeping the selection
 * on the same process when it is still present. */
static void sm_toggle_sort(void) {
    int pid = sm_selected_pid();
    g_sort_mode = (g_sort_mode == SYSMON_SORT_PID) ? SYSMON_SORT_NAME
                                                   : SYSMON_SORT_PID;
    sm_sort_procs();
    if (pid >= 0) {
        for (int i = 0; i < g_count; i++) {
            if (g_procs[i].pid == pid) { g_list.selected = i; break; }
        }
    }
    sm_clamp_list();
}

/* ================================================================== */
/* Data sampling                                                       */
/* ================================================================== */

/* Re-read every system data source and rebuild the process list. */
static void sm_refresh(void) {
    struct sys_meminfo mem;
    struct sys_cpuinfo cpu;
    struct sysmon_cpu_sample cur;
    int keep = sm_selected_pid();

    /* memory (cmd 2) */
    if (sysinfo(2, &mem, (int)sizeof(mem)) == 0) {
        g_mem_total = mem.total_bytes;
        g_mem_used = (mem.free_bytes < mem.total_bytes)
                         ? mem.total_bytes - mem.free_bytes : 0;
        g_mem_pct = sm_mem_pct(mem.total_bytes, mem.free_bytes);
    } else {
        g_mem_total = 0;
        g_mem_used = 0;
        g_mem_pct = 0;
    }

    /* uptime in ms (cmd 1, returned by value) */
    int up_ms = sysinfo(1, 0, 0);
    g_uptime_ms = (up_ms > 0) ? (uint64_t)up_ms : 0;

    /* CPU sample (cmd 5): percentage needs two samples */
    if (sysinfo(5, &cpu, (int)sizeof(cpu)) == 0) {
        cur.uptime_ms = cpu.uptime_ms;
        cur.idle_ms = cpu.total_idle_ms;
        cur.num_cpus = (cpu.num_cpus > 0) ? cpu.num_cpus : 0;
        g_cpu_pct = g_have_prev ? sm_cpu_pct(&g_prev_sample, &cur) : 0;
        g_prev_sample = cur;
        g_have_prev = 1;
        g_num_cpus = cur.num_cpus;
    } else {
        g_cpu_pct = 0;
        g_have_prev = 0;
        g_num_cpus = 0;
    }

    /* process list (cmd 3); returns the number of entries written */
    int n = sysinfo(3, g_procs, (int)sizeof(g_procs));
    if (n < 0) n = 0;
    sm_load_procs(g_procs, n, keep);
}

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

/*
 * Render the complete screen into out (NUL-terminated) and return its
 * length. Layout:
 *   === System Monitor ===
 *   CPU [########....]   NN%    Mem [#####.......]   NN%
 *   Uptime: HH:MM:SS   CPUs: N   Mem: used/total
 *   +------------------------------------------+
 *   PID  NAME             STATE
 *   ...up to 12 rows, the selected one gets a trailing " <"...
 *   k=kill  r=refresh  arrows=scroll
 */
static int sm_render(char *out, int cap) {
    char tmp[64];
    char uptime[40];
    int n = 0;
    if (out == 0 || cap <= 0) return 0;
    out[0] = '\0';

    n = sm_put(out, cap, n, "=== System Monitor ===\n");

    n = sm_put(out, cap, n, "CPU ");
    gui_bar(tmp, SYSMON_BAR_WIDTH, g_cpu_pct);
    n = sm_put(out, cap, n, tmp);
    n = sm_put(out, cap, n, "   Mem ");
    gui_bar(tmp, SYSMON_BAR_WIDTH, g_mem_pct);
    n = sm_put(out, cap, n, tmp);
    n = sm_put(out, cap, n, "\n");

    {
        char used[24];
        char total[24];
        char cpus[24];
        char line[128];
        int m = 0;
        gui_size_str(g_mem_used, used);
        gui_size_str(g_mem_total, total);
        gui_itoa((long)g_num_cpus, cpus);
        sm_fmt_uptime(g_uptime_ms, uptime);
        line[0] = '\0';
        m = sm_put(line, (int)sizeof(line), m, "Uptime: ");
        m = sm_put(line, (int)sizeof(line), m, uptime);
        m = sm_put(line, (int)sizeof(line), m, "   CPUs: ");
        m = sm_put(line, (int)sizeof(line), m, cpus);
        m = sm_put(line, (int)sizeof(line), m, "   Mem: ");
        m = sm_put(line, (int)sizeof(line), m, used);
        m = sm_put(line, (int)sizeof(line), m, "/");
        m = sm_put(line, (int)sizeof(line), m, total);
        m = sm_put(line, (int)sizeof(line), m, "\n");
        n = sm_put(out, cap, n, line);
    }

    gui_rule(tmp, SYSMON_RULE_WIDTH);
    n = sm_put(out, cap, n, tmp);
    n = sm_put(out, cap, n, "\n");
    n = sm_put(out, cap, n, "PID  NAME             STATE\n");

    if (g_count <= 0) {
        n = sm_put(out, cap, n, "  (no processes)\n");
    } else {
        /* Defensive: never index outside the table even if the scroll
         * window is stale (all normal paths clamp it before rendering). */
        int top = g_list.top;
        int maxtop = g_count - g_list.visible;
        if (maxtop < 0) maxtop = 0;
        if (top > maxtop) top = maxtop;
        if (top < 0) top = 0;
        int last = top + g_list.visible;
        if (last > g_count) last = g_count;
        for (int i = top; i < last; i++) {
            sm_fmt_proc_row(&g_procs[i], tmp);
            n = sm_put(out, cap, n, tmp);
            if (i == g_list.selected) n = sm_put(out, cap, n, " <");
            n = sm_put(out, cap, n, "\n");
        }
    }
    n = sm_put(out, cap, n, "k=kill  r=refresh  arrows=scroll\n");
    return n;
}

/* Push the current state onto the window. */
static void sm_draw(void) {
    static char screen[SYSMON_SCREEN_MAX];
    gui_clear();
    sm_render(screen, (int)sizeof(screen));
    print(screen);
}

/* ================================================================== */
/* Actions                                                             */
/* ================================================================== */

/* 'k' / menu Kill: confirm, then SIGKILL the selected PID and refresh. */
static void sm_act_kill(void) {
    int pid = sm_selected_pid();
    if (pid < 0) return; /* empty list or stale selection: nothing to do */
    char num[24];
    char msg[48];
    int m = 0;
    gui_itoa(pid, num);
    msg[0] = '\0';
    m = sm_put(msg, (int)sizeof(msg), m, "Kill PID ");
    m = sm_put(msg, (int)sizeof(msg), m, num);
    m = sm_put(msg, (int)sizeof(msg), m, "?");
    if (!g_confirm("Kill", msg)) return; /* user declined */
    g_kill(pid, 9);
    sm_refresh();                        /* the process should be gone */
}

/* Handle one input event; returns 1 when the screen must be redrawn. */
static int sm_handle_event(const struct gui_event *ev) {
    if (ev == 0) return 0;
    if (ev->type == GUI_EV_CHAR) {
        if (ev->ch == 'r') { sm_refresh();   return 1; }
        if (ev->ch == 'k') { sm_act_kill();  return 1; }
        return 0;
    }
    if (ev->type == GUI_EV_UP) {
        gui_list_move(&g_list, -1);
        sm_clamp_list();
        return 1;
    }
    if (ev->type == GUI_EV_DOWN) {
        gui_list_move(&g_list, 1);
        sm_clamp_list();
        return 1;
    }
    if (ev->type == GUI_EV_MENU) {
        if (ev->menu != 0) return 0;                 /* only one menu */
        if (ev->item == 0) { sm_act_kill();   return 1; } /* Kill */
        if (ev->item == 1) { sm_refresh();    return 1; } /* Refresh */
        if (ev->item == 2) { sm_toggle_sort(); return 1; } /* Sort */
        return 0;
    }
    if (ev->type == GUI_EV_MOUSE) {
        if (ev->button != 1 || ev->state != GUI_MOUSE_PRESS) return 0; /* left press only */
        int idx = gui_list_click_row(&g_list, ev->y, SYSMON_LIST_TOP_ROW);
        if (idx < 0) return 0;                        /* outside the list */
        g_list.selected = idx;
        sm_clamp_list();
        return 1;
    }
    return 0;
}

/* One-time setup: title bar, menu registration, mouse, serial marker. */
static void sm_startup(void) {
    gui_set_title(SYSMON_TITLE);
    gui_add_menu(0, SYSMON_MENU_NAME, SYSMON_MENU_ITEMS);
    gui_enable_mouse();
    print_console(SYSMON_MARKER);
}

/* ================================================================== */
/* Entry point                                                         */
/* ================================================================== */

#ifdef HOST_TEST
int main(void) {
#else
__attribute__((section(".text._start")))
void _start(void) {
#endif
    sm_startup();
    sm_refresh();
    sm_draw();

    for (;;) {
        struct gui_event ev;
        if (gui_read_event_timeout(&ev, SYSMON_REFRESH_MS)) {
            if (sm_handle_event(&ev)) sm_draw();
        } else {
            sm_refresh(); /* periodic re-sample */
            sm_draw();
        }
    }
}
