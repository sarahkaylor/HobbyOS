/*
 * WCTEST.BIN — in-OS end-to-end acceptance for the ported GNU wc.
 *
 * Spawns WC.BIN through the real spawn2/pipe path (crt0 main() ->
 * get_args -> getopt_long -> open/read -> counting -> our printf ->
 * write) and compares the captured stdout byte-for-byte against the GNU
 * coreutils layout (e.g. "      4       7      36 /WCTEST.TXT" — %7s
 * right-aligned counts).
 *
 * Six checks covering file mode, stdin mode, multi-file totals, and
 * long options.
 *
 * Note: all status output goes to the console (print_console), matching
 * the rest of the test programs — stdout is a pipe owned by the harness
 * and is not echoed to the serial log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "libc.h" /* pipe(), spawn2(), print_console() */

static int failures = 0;

/* tiny signed-int console printer (no libc/sysroot dependency) */
static void con_int(int v)
{
    char b[16];
    int i = 15, neg = 0;
    if (v < 0) { neg = 1; v = (v == -2147483648) ? 2147483647 : -v; }
    b[i--] = 0;
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = (char)('0' + v % 10); v /= 10; }
    if (neg) b[i--] = '-';
    print_console(&b[i + 1]);
}

static void check(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) == 0) {
        print_console("[WCTEST] PASS ");
        print_console(what);
        print_console("\n");
    } else {
        print_console("[WCTEST] FAIL ");
        print_console(what);
        print_console("\n  got:  [");
        print_console(got);
        print_console("]\n  want: [");
        print_console(want);
        print_console("]\n");
        failures++;
    }
}

/* Run "wc <args>" with the given stdin content; returns the captured
   stdout in out (NUL-terminated, outsize bytes max). */
static void run_wc(const char *args, const char *stdin_data, char *out,
                   size_t outsize)
{
    int in_p[2], out_p[2];
    int pid;
    size_t n = 0;

    if (pipe(in_p) != 0 || pipe(out_p) != 0) {
        print_console("[WCTEST] pipe failed\n");
        failures++;
        return;
    }
    pid = spawn2("WC.BIN", in_p[0], out_p[1], -1, args);
    if (pid <= 0) {
        /* process-table pressure from concurrently-running tests can make
           the first spawn fail (-1); retry briefly before giving up */
        int tries = 0;
        while (pid <= 0 && tries < 25) {
            usleep(20000); /* 20 ms */
            pid = spawn2("WC.BIN", in_p[0], out_p[1], -1, args);
            tries++;
        }
    }
    if (pid <= 0) {
        print_console("[WCTEST] spawn WC.BIN failed\n");
        failures++;
        return;
    }
    /* feed stdin, then let the child see EOF */
    close(in_p[0]);
    write(in_p[1], stdin_data, strlen(stdin_data));
    close(in_p[1]);

    close(out_p[1]);
    while (n + 1 < outsize) {
        int r = read(out_p[0], out + n, outsize - n - 1);
        if (r <= 0)
            break; /* EOF once the child exits and its pipe fd closes */
        n += (size_t)r;
    }
    close(out_p[0]);
    out[n] = '\0';
}

static void write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        print_console("[WCTEST] cannot create ");
        print_console(path);
        print_console("\n");
        failures++;
        return;
    }
    write(fd, content, strlen(content));
    close(fd);
}

int main(void)
{
    char out[512];

    print_console("[WCTEST] starting\n");

    /* /WCTEST.TXT : lines=4 words=7 bytes=36 */
    write_file("/WCTEST.TXT",
               "line one\nline two word\n\nhello world\n");
    /* /WCTEST2.TXT : lines=2 words=3 bytes=6 */
    write_file("/WCTEST2.TXT", "x y\nz\n");

    run_wc("-l -w -c /WCTEST.TXT", "", out, sizeof out);
    check("-l -w -c file", out, "      4       7      36 /WCTEST.TXT\n");

    run_wc("-c /WCTEST.TXT", "", out, sizeof out);
    check("-c file", out, "     36 /WCTEST.TXT\n");

    run_wc("-wcL /WCTEST.TXT", "a b\nc d e\nf\n", out, sizeof out);
    check("-wcL file", out, "      7      36      13 /WCTEST.TXT\n");

    run_wc("-", "a b\nc\n", out, sizeof out);
    check("stdin '-'", out, "      2       3       6 -\n");

    run_wc("-l -w -c /WCTEST.TXT /WCTEST2.TXT", "", out, sizeof out);
    check("multi-file total",
          out,
          "      4       7      36 /WCTEST.TXT\n"
          "      2       3       6 /WCTEST2.TXT\n"
          "      6      10      42 total\n");

    run_wc("--words --bytes /WCTEST2.TXT", "", out, sizeof out);
    check("long opts", out, "      3       6 /WCTEST2.TXT\n");

    if (failures == 0) {
        print_console("[WCTEST] ALL PASSED (6 checks)\n");
        exit(0);
    }
    print_console("[WCTEST] FAILURES: ");
    con_int(failures);
    print_console("\n");
    exit(1);
}
