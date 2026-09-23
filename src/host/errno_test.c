/* src/host/errno_test.c — Phase 0 POSIX errno convention, host tier.
 *
 * The host build does not link src/user/libc.c: user code calls the ho_*
 * mocks in src/host/compat.c, which wrap the Linux C library. Under HOST_TEST
 * the libc.h renames make close()/read()/write()/kill() resolve to those
 * mocks, whose errno values come straight from Linux — and our errno.h uses
 * Linux numbering, so the two must agree exactly. This locks in the
 * kernel-side convention (posix.md §2.2): failures return -1 with errno set,
 * successes leave errno untouched. */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "../user_include/libc.h"

static int failures = 0;

#define CHECK(cond, name) do {                              \
    if (cond) { printf("  PASS %s\n", name); }              \
    else { printf("  FAIL %s\n", name); failures++; }       \
} while (0)

int main(void) {
    errno = 0;
    CHECK(errno == 0, "errno starts at 0");

    /* close() on a bad fd -> EBADF */
    errno = 0;
    int r = close(-1);
    CHECK(r == -1 && errno == EBADF, "close(-1) -> -1, errno=EBADF");

    /* read()/write() on a bad fd -> EBADF */
    errno = 0;
    char c;
    r = read(-1, &c, 1);
    CHECK(r == -1 && errno == EBADF, "read(-1) -> -1, errno=EBADF");
    errno = 0;
    r = write(9999, "x", 1);
    CHECK(r == -1 && errno == EBADF, "write(9999) -> -1, errno=EBADF");

    /* kill(pid, 0) on a nonexistent pid -> ESRCH */
    errno = 0;
    r = kill(4194303, 0);
    CHECK(r == -1 && errno == ESRCH, "kill(nonexistent,0) -> -1, errno=ESRCH");

    /* getcwd with a buffer too small -> NULL + ERANGE (compat mirrors
     * the kernel's -ERANGE return from SYS_GETCWD) */
    errno = 0;
    char tiny[1];
    char *cw = getcwd(tiny, sizeof tiny);
    CHECK(cw == NULL && errno == ERANGE, "getcwd(tiny buf) -> NULL, errno=ERANGE");

    /* A successful call must not touch errno */
    errno = 0;
    char big[256];
    cw = getcwd(big, sizeof big);
    CHECK(cw == big && errno == 0, "successful getcwd returns buf, errno stays 0");

    if (failures == 0) {
        printf("ALL ERRNO HOST TESTS PASSED\n");
        return 0;
    }
    printf("ERRNO HOST TESTS FAILED (%d)\n", failures);
    return 1;
}
