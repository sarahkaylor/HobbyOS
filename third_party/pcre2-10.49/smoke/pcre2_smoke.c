/* pcre2_smoke.c -- deterministic static-link smoke for vendored pcre2 10.49
 * (L6 lane browser/l6-glib).
 *
 * Proves that libpcre2-8.a links standalone and that the interpreter + JIT +
 * named groups + lookbehind + substitute all behave.  This is the regex
 * engine behind GLib's GRegex, which WebCore uses.
 *
 * Output is deterministic; exits 0 and prints "ALL TESTS PASSED SUCCESSFULLY!"
 * as the final line on success. */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
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

int main(void)
{
    PCRE2_UCHAR vbuf[64];
    uint32_t jit = 0;

    pcre2_config(PCRE2_CONFIG_VERSION, vbuf);
    printf("pcre2 version: %s\n", (char *)vbuf);
    pcre2_config(PCRE2_CONFIG_JIT, &jit);
    printf("pcre2 jit support: %u\n", jit);
    check(strncmp((char *)vbuf, "10.49", 5) == 0, "pcre2 version is 10.49");

    int errcode = 0;
    PCRE2_SIZE erroff = 0;
    const char *pat = "^(?<tool>[a-z0-9]+)-(?<lane>[0-9]+[a-z]?)$";
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pat, PCRE2_ZERO_TERMINATED, 0,
                                   &errcode, &erroff, NULL);
    check(re != NULL, "compile named-group pattern");
    if (re == NULL) {
        printf("compile error %d at offset %zu\n", errcode, (size_t)erroff);
        return 2;
    }

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)"libffi-348", PCRE2_ZERO_TERMINATED,
                         0, 0, md, NULL);
    check(rc == 3, "match 'libffi-348' returns 3 captured groups");

    PCRE2_UCHAR *g = NULL;
    PCRE2_SIZE glen = 0;
    if (pcre2_substring_get_byname(md, (PCRE2_SPTR)"tool", &g, &glen) == 0) {
        check(glen == 6 && memcmp(g, "libffi", 6) == 0, "named group tool == libffi");
        pcre2_substring_free(g);
    } else {
        check(0, "named group tool fetch");
    }
    if (pcre2_substring_get_byname(md, (PCRE2_SPTR)"lane", &g, &glen) == 0) {
        check(glen == 3 && memcmp(g, "348", 3) == 0, "named group lane == 348");
        pcre2_substring_free(g);
    } else {
        check(0, "named group lane fetch");
    }

    rc = pcre2_match(re, (PCRE2_SPTR)"no match here!", PCRE2_ZERO_TERMINATED,
                     0, 0, md, NULL);
    check(rc == PCRE2_ERROR_NOMATCH, "non-matching subject -> NOMATCH");

#if defined(SUPPORT_JIT) || 1
    rc = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    check(jit == 1 && rc == 0, "jit_compile complete");
    rc = pcre2_match(re, (PCRE2_SPTR)"pcre2-49", PCRE2_ZERO_TERMINATED,
                     0, 0, md, NULL);
    check(rc == 3, "JIT match 'pcre2-49'");
#endif

    /* lookbehind: a PCRE feature, not POSIX ERE -- proves real pcre2 */
    pcre2_code *lb = pcre2_compile((PCRE2_SPTR)"(?<=browser/)l6-glib$",
                                   PCRE2_ZERO_TERMINATED, 0, &errcode, &erroff, NULL);
    check(lb != NULL, "compile lookbehind pattern");
    if (lb != NULL) {
        pcre2_match_data *md2 = pcre2_match_data_create_from_pattern(lb, NULL);
        rc = pcre2_match(lb, (PCRE2_SPTR)"wl/browser/l6-glib", PCRE2_ZERO_TERMINATED,
                         0, 0, md2, NULL);
        check(rc == 1, "lookbehind matches 'wl/browser/l6-glib'");
        rc = pcre2_match(lb, (PCRE2_SPTR)"wl/browser/l6-icu", PCRE2_ZERO_TERMINATED,
                         0, 0, md2, NULL);
        check(rc == PCRE2_ERROR_NOMATCH, "lookbehind rejects 'wl/browser/l6-icu'");
        pcre2_match_data_free(md2);
        pcre2_code_free(lb);
    }

    /* substitute $2/$1 (numbered groups) */
    PCRE2_UCHAR out[64];
    PCRE2_SIZE outlen = sizeof(out);
    rc = pcre2_substitute(re, (PCRE2_SPTR)"pcre2-1049", PCRE2_ZERO_TERMINATED,
                          0, PCRE2_SUBSTITUTE_GLOBAL, md, NULL,
                          (PCRE2_SPTR)"$2/$1", PCRE2_ZERO_TERMINATED,
                          out, &outlen);
    check(rc >= 0 && strcmp((char *)out, "1049/pcre2") == 0,
          "substitute '$2/$1' -> 1049/pcre2");

    pcre2_match_data_free(md);
    pcre2_code_free(re);

    printf("%u checks, %u failures\n", checks, failures);
    if (failures == 0) {
        printf("ALL TESTS PASSED SUCCESSFULLY!\n");
        return 0;
    }
    printf("PCRE2 SMOKE FAILED\n");
    return 1;
}
