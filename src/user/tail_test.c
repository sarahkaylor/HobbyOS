/*
 * TAILTEST.BIN — in-OS acceptance for the ported GNU tail (TAILGN.BIN,
 * distinct from the shell's legacy TAIL.BIN).
 *
 * The exhaustive byte-exact matrix (222 cases vs genuine GNU tail) lives
 * in the HOST parity harness (src/host/tail_parity.sh).  This in-OS test
 * proves the device path: spawns TAILGN.BIN through the real spawn2/pipe
 * machinery and compares captured stdout byte-for-byte for the
 * representative paths: -n, -c, old -Nc syntax, multi-file headers, and
 * stdin-pipe (tail reads the whole pipe and tails it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "libc.h" /* pipe(), spawn2(), print_console() */

static int failures = 0;

static void con_int(long v)
{
    char b[20];
    int i = 19, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    b[i--] = 0;
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = (char)('0' + v % 10); v /= 10; }
    if (neg) b[i--] = '-';
    print_console(&b[i + 1]);
}

static void check(const char *what, const char *got, const char *want)
{
    if (got && strcmp(got, want) == 0) {
        print_console("[TAILTEST] PASS ");
        print_console(what);
        print_console("\n");
    } else {
        print_console("[TAILTEST] FAIL ");
        print_console(what);
        print_console("\n  got:  [");
        print_console(got ? got : "(null)");
        print_console("]\n  want: [");
        print_console(want);
        print_console("]\n");
        failures++;
    }
}

/* Run "tail <args>" with the given stdin content; capture stdout. */
static void run_tail(const char *args, const char *stdin_data, char *out,
                     size_t outsize)
{
    int in_p[2], out_p[2];
    int pid;
    size_t n = 0;

    /* let any previous TAILGN child exit and free its process slot */
    usleep(50000);

    if (pipe(in_p) != 0 || pipe(out_p) != 0) {
        print_console("[TAILTEST] pipe failed\n");
        failures++;
        return;
    }
    pid = spawn2("TAILGN.BIN", in_p[0], out_p[1], -1, args);
    if (pid <= 0) {
        int tries = 0;
        while (pid <= 0 && tries < 200) {
            usleep(50000); /* 50 ms; up to ~10 s of patience */
            pid = spawn2("TAILGN.BIN", in_p[0], out_p[1], -1, args);
            tries++;
        }
    }
    if (pid <= 0) {
        print_console("[TAILTEST] spawn TAILGN.BIN failed\n");
        failures++;
        return;
    }
    close(in_p[0]);
    write(in_p[1], stdin_data, strlen(stdin_data));
    close(in_p[1]);

    close(out_p[1]);
    while (n + 1 < outsize) {
        int r = read(out_p[0], out + n, outsize - n - 1);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(out_p[0]);
    out[n] = '\0';
}

static void write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        print_console("[TAILTEST] cannot create ");
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
    char out[2048];

    print_console("[TAILTEST] starting\n");

    /* 12 lines: last 3 are kappa/lambda/mu; last 8 bytes are "mbda\nmu\n" */
    write_file("/TLTEST1.TXT",
               "alpha\n"
               "beta\n"
               "gamma\n"
               "delta\n"
               "epsilon\n"
               "zeta\n"
               "eta\n"
               "theta\n"
               "iota\n"
               "kappa\n"
               "lambda\n"
               "mu\n");
    write_file("/TLTEST2.TXT", "one\ntwo\nthree\n");

    /* -n 3: last 3 lines */
    run_tail("-n 3 /TLTEST1.TXT", "", out, sizeof out);
    check("-n 3", out, "kappa\nlambda\nmu\n");

    /* -c 8: last 8 bytes */
    run_tail("-c 8 /TLTEST1.TXT", "", out, sizeof out);
    check("-c 8", out, "mbda\nmu\n");

    /* old syntax -2c: last 2 bytes */
    run_tail("-2c /TLTEST1.TXT", "", out, sizeof out);
    check("old -2c", out, "u\n");

    /* multi-file: header banners + last 10 lines of each */
    run_tail("/TLTEST1.TXT /TLTEST2.TXT", "", out, sizeof out);
    check("multi-file headers", out,
          "==> /TLTEST1.TXT <==\n"
          "gamma\ndelta\nepsilon\nzeta\neta\ntheta\niota\nkappa\nlambda\nmu\n"
          "\n"
          "==> /TLTEST2.TXT <==\n"
          "one\ntwo\nthree\n");

    /* stdin-pipe: tail reads the whole pipe, then tails it */
    run_tail("-", "one\ntwo\nthree\n", out, sizeof out);
    check("stdin '-' with pipe data", out, "one\ntwo\nthree\n");

    if (failures == 0) {
        print_console("[TAILTEST] ALL PASSED (5 checks)\n");
        exit(0);
    }
    print_console("[TAILTEST] FAILURES: ");
    con_int(failures);
    print_console("\n");
    exit(1);
}
