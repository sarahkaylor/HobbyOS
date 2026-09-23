/*
 * LKSTEST.BIN — Phase 3 acceptance: real lseek/stat/fstat + FILE-layer
 * fseek/ftell/rewind/ungetc against the HobbyOS kernel.
 *
 * Creates /LKSTEST.TXT (31 bytes: 2 lines) and verifies:
 *   - fstat: st_size == expected, S_ISREG, st_blocks rounding
 *   - stat(path): same via path resolution
 *   - lseek SEEK_SET/SEEK_CUR/SEEK_END positions + read-back
 *   - fseek/ftell/rewind through the buffered FILE layer
 *   - ungetc push-back peek
 *   - error paths: lseek on a bad fd -> EBADF
 *
 * Fixture content (exactly, 31 bytes):
 *   "alpha beta\ngamma delta epsilon\n"
 *     line 1 = "alpha beta\n"          (bytes 0..10)
 *     line 2 = "gamma delta epsilon\n" (bytes 11..30)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "libc.h" /* print_console, pipe(), spawn2() */

#define FIXTURE "alpha beta\ngamma delta epsilon\n"
#define FIXLEN  31 /* strlen(FIXTURE) */

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

static void check(const char *what, int ok)
{
    print_console(ok ? "[LKSTEST] PASS " : "[LKSTEST] FAIL ");
    print_console(what);
    print_console("\n");
    if (!ok) failures++;
}

#define CHECK(what, expr) check(what, (expr))

int main(void)
{
    char buf[16];
    struct stat st;

    print_console("[LKSTEST] starting\n");

    /* fixture */
    int fd = open("/LKSTEST.TXT", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK("open fixture", fd >= 0);
    if (fd < 0) { print_console("[LKSTEST] cannot create fixture\n"); return 1; }
    if (write(fd, FIXTURE, FIXLEN) != FIXLEN) failures++;

    /* --- fstat on the freshly-written (cursor at end) fd --- */
    memset(&st, 0, sizeof st);
    CHECK("fstat ok", fstat(fd, &st) == 0);
    CHECK("fstat size", st.st_size == FIXLEN);
    CHECK("fstat S_ISREG", S_ISREG(st.st_mode));
    CHECK("fstat mode perms", (st.st_mode & 0777) == 0644);
    CHECK("fstat blocks rounding", st.st_blocks == 1);
    CHECK("fstat nlink", st.st_nlink == 1);
    CHECK("fstat blksize", st.st_blksize == 512);

    /* --- stat(path) --- */
    memset(&st, 0, sizeof st);
    CHECK("stat ok", stat("/LKSTEST.TXT", &st) == 0);
    CHECK("stat size", st.st_size == FIXLEN);
    CHECK("stat S_ISREG", S_ISREG(st.st_mode));
    memset(&st, 0, sizeof st);
    CHECK("stat missing -> ENOENT",
          stat("/LKSTEST.NOPE", &st) == -1 && errno == ENOENT);

    /* --- lseek positions on the raw fd (cursor is at 31 after write) --- */
    CHECK("lseek SEEK_SET 6", lseek(fd, 6, SEEK_SET) == 6);
    CHECK("lseek SEEK_CUR +5", lseek(fd, 5, SEEK_CUR) == 11);
    memset(buf, 0, sizeof buf);
    CHECK("read 5 @11", read(fd, buf, 5) == 5);
    CHECK("read-back 'gamma'", memcmp(buf, "gamma", 5) == 0);
    CHECK("lseek SEEK_END 0", lseek(fd, 0, SEEK_END) == FIXLEN);
    CHECK("lseek SEEK_END -4", lseek(fd, -4, SEEK_END) == 27);
    memset(buf, 0, sizeof buf);
    CHECK("read 4 @27", read(fd, buf, 4) == 4);
    CHECK("read-back 'lon\\n'", memcmp(buf, "lon\n", 4) == 0);
    CHECK("lseek beyond EOF", lseek(fd, 1000, SEEK_SET) == 1000);
    memset(buf, 0, sizeof buf);
    CHECK("read past EOF -> 0", read(fd, buf, 5) == 0);
    CHECK("lseek bad fd -> EBADF", lseek(31, 0, SEEK_SET) == -1 && errno == EBADF);
    CHECK("lseek bad whence -> EINVAL", lseek(fd, 0, 7) == -1 && errno == EINVAL);
    close(fd);

    /* --- FILE layer: fseek/ftell/rewind/ungetc --- */
    FILE *f = fopen("/LKSTEST.TXT", "r");
    CHECK("fopen", f != NULL);
    if (f) {
        CHECK("ftell init 0", ftell(f) == 0);
        CHECK("fseek END 0", fseek(f, 0, SEEK_END) == 0);
        CHECK("ftell at END", ftell(f) == FIXLEN);
        CHECK("rewind", fseek(f, 0, SEEK_SET) == 0);
        CHECK("fgets line 1", fgets(buf, sizeof buf, f) != NULL);
        CHECK("line 1 content", strcmp(buf, "alpha beta\n") == 0);
        CHECK("ftell after line 1", ftell(f) == 11);
        CHECK("fseek SEEK_CUR +5", fseek(f, 5, SEEK_CUR) == 0);
        CHECK("ftell after +5", ftell(f) == 16);
        CHECK("fseek SEEK_SET 23", fseek(f, 23, SEEK_SET) == 0);
        memset(buf, 0, sizeof buf);
        CHECK("fread 7 @23", fread(buf, 1, 7, f) == 7);
        CHECK("fread 'epsilon'", memcmp(buf, "epsilon", 7) == 0);
        /* ungetc: push 'X' back (one slot), next fgetc sees it */
        CHECK("ungetc", ungetc('X', f) == 'X');
        CHECK("ungetc one-slot full (2nd rejected)", ungetc('Y', f) == EOF);
        int c = fgetc(f);
        CHECK("fgetc sees pushed back", c == 'X');
        rewind(f);
        CHECK("rewind resets ftell", ftell(f) == 0);
        CHECK("fclose ok", fclose(f) == 0);
    }

    /* cleanup */
    unlink("/LKSTEST.TXT");

    if (failures == 0) {
        print_console("[LKSTEST] ALL PASSED\n");
        exit(0);
    }
    print_console("[LKSTEST] FAILURES: ");
    con_int(failures);
    print_console("\n");
    exit(1);
}
