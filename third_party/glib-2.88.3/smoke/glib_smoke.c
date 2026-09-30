/* glib_smoke.c -- deterministic static-link smoke for vendored GLib 2.88.3
 * (L6 lane browser/l6-glib; see ../README.md).
 *
 * Covers the mission list: GString, GHashTable, GPtrArray, g_ascii, GRegex
 * (PCRE2-backed), GDateTime, GMutex + GThread, GThreadPool.  Additionally
 * exercises the component libs the WPE 2.54.0 port links (GObject/Thread/
 * Module/GioUnix-equivalents): GObject class + signal (via the generic,
 * ffi-backed closure marshaller), GModule open/symbol, GIO streams + GFile.
 *
 * Deterministic output; exits 0 and prints "ALL TESTS PASSED SUCCESSFULLY!"
 * as the final line on success. */
#include <glib.h>
#include <glib-object.h>
#include <gmodule.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static unsigned checks = 0, failures = 0;
static void check(int ok, const char *name)
{
    checks++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        failures++;
}

/* g_ptr_array_sort comparators get pointers-to-elements */
static int cmp_strptr(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char * const *)a, *(char * const *)b);
}

/* ---------------- GObject test type (signal + ffi-backed closure) --------- */
typedef struct { GObject parent; } SmokeObj;
typedef struct { GObjectClass parent_class; } SmokeObjClass;
G_DEFINE_TYPE(SmokeObj, smoke_obj, G_TYPE_OBJECT)

static void smoke_obj_class_init(SmokeObjClass *klass)
{
    g_signal_new("ping", G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                 g_cclosure_marshal_generic,      /* ffi-backed fallback */
                 G_TYPE_NONE, 1, G_TYPE_INT);
}
static void smoke_obj_init(SmokeObj *self) { (void)self; }

static int ping_payload = -1;
static void on_ping(SmokeObj *self, int value, gpointer user_data)
{
    (void)self; (void)user_data;
    ping_payload = value;
}

/* ---------------- thread helpers ------------------------------------------ */
static GMutex thread_mutex;
static gint64 thread_counter;
static gpointer thread_inc(gpointer data)
{
    (void)data;
    for (int i = 0; i < 10000; i++) {
        g_mutex_lock(&thread_mutex);
        thread_counter++;
        g_mutex_unlock(&thread_mutex);
    }
    return NULL;
}

static GMutex pool_mutex;
static guint64 pool_sum;
static void pool_task(gpointer data, gpointer user_data)
{
    guint64 n;
    (void)user_data;
    n = (guint64)GPOINTER_TO_UINT(data);
    g_mutex_lock(&pool_mutex);
    pool_sum += n * n;
    g_mutex_unlock(&pool_mutex);
}

/* ---------------- smoke --------------------------------------------------- */
int main(void)
{
    guint sig_id;

    printf("glib compile-time version: %d.%d.%d (runtime %u.%u.%u)\n",
           GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION,
           glib_major_version, glib_minor_version, glib_micro_version);
    check(GLIB_CHECK_VERSION(2, 70, 0), "GLib >= 2.70.0 (WPE 2.54.0 floor)");
    check(glib_major_version == 2 && glib_minor_version == 88 && glib_micro_version == 3,
          "runtime version is 2.88.3");

    /* --- GString ---------------------------------------------------------- */
    {
        GString *s = g_string_new("HobbyOS");
        g_string_append(s, "+");
        g_string_append(s, "GLib");
        g_string_append_printf(s, "-%d.%d", 2, 88);
        check(strcmp(s->str, "HobbyOS+GLib-2.88") == 0, "GString append/append_printf");
        g_string_prepend(s, "[");
        g_string_append_c(s, ']');
        check(g_str_equal(s->str, "[HobbyOS+GLib-2.88]"), "GString prepend/append_c");
        g_string_free(s, TRUE);
    }

    /* --- GHashTable ------------------------------------------------------- */
    {
        const char *words[] = { "pcre2", "libffi", "glib", "zlib" };
        GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        GList *keys, *l;
        GString *join;
        char *v;
        for (guint i = 0; i < G_N_ELEMENTS(words); i++)
            g_hash_table_insert(h, g_strdup(words[i]),
                                g_strdup_printf("%u", (unsigned)strlen(words[i])));
        check(g_hash_table_size(h) == 4, "GHashTable size == 4");
        v = g_hash_table_lookup(h, "glib");
        check(v != NULL && strcmp(v, "4") == 0, "GHashTable lookup glib -> 4");
        g_hash_table_remove(h, "zlib");
        check(g_hash_table_size(h) == 3 && g_hash_table_lookup(h, "zlib") == NULL,
              "GHashTable remove");
        keys = g_list_sort(g_hash_table_get_keys(h), (GCompareFunc)strcmp);
        join = g_string_new("");
        for (l = keys; l != NULL; l = l->next) {
            if (join->len > 0)
                g_string_append_c(join, '+');
            g_string_append(join, (const char *)l->data);
        }
        check(g_str_equal(join->str, "glib+libffi+pcre2"),
              "GHashTable sorted key join: glib+libffi+pcre2");
        g_list_free(keys);
        g_string_free(join, TRUE);
        g_hash_table_destroy(h);
    }

    /* --- GPtrArray -------------------------------------------------------- */
    {
        GPtrArray *pa = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(pa, g_strdup("aarch64-none-elf"));
        g_ptr_array_add(pa, g_strdup("x86_64"));
        g_ptr_array_add(pa, g_strdup("riscv64"));
        check(pa->len == 3, "GPtrArray len == 3");
        g_ptr_array_sort(pa, cmp_strptr);
        check(strcmp(g_ptr_array_index(pa, 0), "aarch64-none-elf") == 0 &&
              strcmp(g_ptr_array_index(pa, 1), "riscv64") == 0 &&
              strcmp(g_ptr_array_index(pa, 2), "x86_64") == 0,
              "GPtrArray sort");
        check(g_ptr_array_find_with_equal_func(pa, "x86_64", g_str_equal, NULL),
              "GPtrArray find x86_64");
        g_ptr_array_remove_index(pa, 1);
        check(pa->len == 2, "GPtrArray remove_index");
        g_ptr_array_free(pa, TRUE);
    }

    /* --- g_ascii ---------------------------------------------------------- */
    {
        char *up = g_ascii_strup("wave1d", -1);
        char *dn = g_ascii_strdown("GLIB-DEPS", -1);
        char *end = NULL;
        double dv = g_ascii_strtod("2.5400", &end);
        check(g_strcmp0(up, "WAVE1D") == 0, "g_ascii_strup");
        check(g_strcmp0(dn, "glib-deps") == 0, "g_ascii_strdown");
        check(g_ascii_strcasecmp("GlIb", "gLiB") == 0, "g_ascii_strcasecmp");
        check(g_ascii_isdigit('7') && g_ascii_isalpha('x') && !g_ascii_isspace('0'),
              "g_ascii_isdigit/isalpha/isspace");
        check(dv == 2.54 && end != NULL && *end == '\0', "g_ascii_strtod 2.5400");
        g_free(up);
        g_free(dn);
    }

    /* --- GRegex (PCRE2-backed) -------------------------------------------- */
    {
        GError *err = NULL;
        GRegex *re = g_regex_new("^(?<tool>[a-z0-9]+)-(?<lane>[0-9]+[a-z]?)$",
                                 G_REGEX_OPTIMIZE, 0, &err);
        check(re != NULL && err == NULL, "GRegex compile named-group pattern");
        if (re != NULL) {
            GMatchInfo *mi = NULL;
            char *tool, *lane, *rep1, *rep2;
            check(g_regex_match(re, "libffi-348", 0, &mi),
                  "GRegex match 'libffi-348'");
            tool = g_match_info_fetch_named(mi, "tool");
            lane = g_match_info_fetch_named(mi, "lane");
            check(g_strcmp0(tool, "libffi") == 0, "GRegex named group tool == libffi");
            check(g_strcmp0(lane, "348") == 0, "GRegex named group lane == 348");
            g_free(tool); g_free(lane);
            g_match_info_free(mi);
            check(!g_regex_match(re, "not a match!", 0, NULL),
                  "GRegex rejects non-match");
            rep1 = g_regex_replace(re, "pcre2-1049", -1, 0, "\\2/\\1", 0, &err);
            rep2 = g_regex_replace(re, "pcre2-1049", -1, 0, "\\g<lane>/\\g<tool>", 0, &err);
            check(g_strcmp0(rep1, "1049/pcre2") == 0, "GRegex replace \\2/\\1 -> 1049/pcre2");
            check(g_strcmp0(rep2, "1049/pcre2") == 0, "GRegex replace \\g<lane>/\\g<tool> -> 1049/pcre2");
            g_free(rep1); g_free(rep2);
            g_regex_unref(re);
        }
        /* lookbehind: a PCRE feature, not POSIX ERE */
        {
            GRegex *lb = g_regex_new("(?<=browser/)l6-glib$", 0, 0, NULL);
            check(lb != NULL && g_regex_match(lb, "wl/browser/l6-glib", 0, NULL),
                  "GRegex lookbehind match");
            check(lb != NULL && !g_regex_match(lb, "wl/browser/l6-icu", 0, NULL),
                  "GRegex lookbehind reject");
            if (lb != NULL)
                g_regex_unref(lb);
        }
        /* escaped literal roundtrip */
        {
            char *esc = g_regex_escape_string("a+b*c?", -1);
            GRegex *lit = g_regex_new(esc, 0, 0, NULL);
            check(lit != NULL && g_regex_match(lit, "xxa+b*c?xx", 0, NULL) &&
                  !g_regex_match(lit, "axbxc", 0, NULL),
                  "GRegex escape_string literal roundtrip");
            g_free(esc);
            if (lit != NULL)
                g_regex_unref(lit);
        }
    }

    /* --- GDateTime -------------------------------------------------------- */
    {
        GDateTime *dt = g_date_time_new_utc(2026, 9, 30, 12, 34, 56);
        GDateTime *d2;
        char *s1, *s1z, *s2;
        check(dt != NULL, "GDateTime new_utc");
        s1 = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%S");
        s1z = g_strconcat(s1, "Z", NULL);
        check(g_strcmp0(s1z, "2026-09-30T12:34:56Z") == 0,
              "GDateTime format -> 2026-09-30T12:34:56Z");
        check(g_date_time_get_day_of_year(dt) == 273, "GDateTime day_of_year == 273");
        check(g_date_time_get_utc_offset(dt) == 0, "GDateTime utc offset == 0");
        d2 = g_date_time_add_days(dt, 1);
        s2 = g_date_time_format(d2, "%Y-%m-%d");
        check(g_strcmp0(s2, "2026-10-01") == 0, "GDateTime +1 day -> 2026-10-01");
        check(g_date_time_difference(d2, dt) == G_TIME_SPAN_DAY,
              "GDateTime difference == 1 day");
        g_free(s1); g_free(s1z); g_free(s2);
        g_date_time_unref(d2);
        g_date_time_unref(dt);
    }

    /* --- GMutex + GThread ------------------------------------------------- */
    {
        GThread *threads[4];
        g_mutex_init(&thread_mutex);
        thread_counter = 0;
        for (int i = 0; i < 4; i++)
            threads[i] = g_thread_new("smoke-inc", thread_inc, NULL);
        for (int i = 0; i < 4; i++)
            g_thread_join(threads[i]);
        check(thread_counter == 40000, "GMutex + 4 GThread: counter == 40000");
        g_mutex_clear(&thread_mutex);
    }

    /* --- GThreadPool ------------------------------------------------------ */
    {
        GError *err = NULL;
        GThreadPool *pool = g_thread_pool_new(pool_task, NULL, 4, FALSE, &err);
        gboolean pushed = TRUE;
        check(pool != NULL, "GThreadPool new (4 threads)");
        g_mutex_init(&pool_mutex);
        pool_sum = 0;
        for (guint i = 1; i <= 8; i++)
            pushed = g_thread_pool_push(pool, GUINT_TO_POINTER(i), NULL) && pushed;
        check(pushed, "GThreadPool push 8 tasks");
        g_thread_pool_free(pool, FALSE, TRUE);
        check(pool_sum == 204, "GThreadPool sum(1..8 squares) == 204");
        g_mutex_clear(&pool_mutex);
    }

    /* --- GObject (class, signal, generic ffi-backed marshal) -------------- */
    {
        SmokeObj *obj = g_object_new(smoke_obj_get_type(), NULL);
        GValue v = G_VALUE_INIT;
        sig_id = g_signal_lookup("ping", smoke_obj_get_type());
        check(obj != NULL && G_IS_OBJECT(obj), "GObject instantiate SmokeObj");
        check(sig_id != 0, "GObject signal 'ping' registered");
        g_signal_connect(obj, "ping", G_CALLBACK(on_ping), NULL);
        g_signal_emit(obj, sig_id, 0, 1234);
        check(ping_payload == 1234, "GObject signal payload via g_cclosure_marshal_generic == 1234");
        g_value_init(&v, G_TYPE_INT);
        g_value_set_int(&v, 42);
        check(g_value_get_int(&v) == 42, "GValue int roundtrip");
        g_value_unset(&v);
        g_object_unref(obj);
    }

    /* --- GModule ---------------------------------------------------------- */
    {
        check(g_module_supported(), "GModule supported");
        GModule *mod = g_module_open(NULL, 0);
        check(mod != NULL, "GModule open(NULL) self");
        if (mod != NULL) {
            gpointer sym = NULL;
            check(g_module_symbol(mod, "strlen", &sym) && sym != NULL,
                  "GModule symbol 'strlen' resolved");
            g_module_close(mod);
        }
    }

    /* --- GIO -------------------------------------------------------------- */
    {
        GInputStream *in = g_memory_input_stream_new_from_data("webk", 4, NULL);
        char buf[8] = { 0 };
        gsize nread = 0;
        check(g_input_stream_read_all(in, buf, 4, &nread, NULL, NULL) &&
              nread == 4 && memcmp(buf, "webk", 4) == 0,
              "GIO GMemoryInputStream read_all");
        g_object_unref(in);
        {
            GFile *f = g_file_new_for_path("/opt/browser/l6-glib");
            char *base = g_file_get_basename(f);
            check(g_strcmp0(base, "l6-glib") == 0, "GIO GFile basename");
            g_free(base);
            g_object_unref(f);
        }
    }

    printf("%u checks, %u failures\n", checks, failures);
    if (failures == 0) {
        printf("ALL TESTS PASSED SUCCESSFULLY!\n");
        return 0;
    }
    printf("GLIB SMOKE FAILED\n");
    return 1;
}
