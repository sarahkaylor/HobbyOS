/*
 * STAGE4K_T.BIN — GB-067 byte-exact read-back of a host-staged 4 KiB file.
 *
 * GB-067's DoD: "post-build stage of a 4 KiB file, read back in-guest
 * byte-exact; attr != 0 on any entry the driver creates".  The attr half is on
 * main (PR #48: new_entry.attr = 0x20).  This program closes the other half:
 * the wave stages a deterministic 4 KiB fixture `/STAGE4K.BIN` on the host
 * (post-build, via T3_STAGE) and this guest reader opens it, lseek-verifies the
 * size is exactly 4096, reads every byte, and byte-compares it against the
 * same position-dependent pattern the host wrote.  Full-content compare, not a
 * header check — the loader's first-N-bytes read is explicitly the thing the
 * DoD says is not enough.
 *
 * The fixture is 4096 bytes: byte[i] = (unsigned char)(i % 251).  The host
 * stages it with the identical expression via `perl -e` (see the stage
 * command in the evidence), so a match means the staged-on-disk bytes were
 * read back in-guest unchanged.
 */
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "libc.h" /* print_console(), print_hex() */

#define STAGE_PATH "/STAGE4K.BIN"
#define FIXTURE_SIZE 4096

static int failures = 0;

static void check(const char *what, int got, int want) {
  if (got == want) {
    print_console("[STAGE4K] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[STAGE4K] FAIL ");
    print_console(what);
    print_console(" (got=");
    print_hex((long)got);
    print_console(" want=");
    print_hex((long)want);
    print_console(")\n");
    failures++;
  }
}

static void failmsg(const char *msg) {
  print_console("[STAGE4K] FAIL ");
  print_console(msg);
  print_console("\n");
  failures++;
}

__attribute__((section(".text._start")))
void _start(void) {
  int fd = open(STAGE_PATH, O_RDONLY);
  if (fd < 0) {
    failmsg("open /STAGE4K.BIN");
    print_console("[STAGE4K] FAILED\n");
    exit(0);
  }

  /* 1. The size must be exactly 4096 through the FAT16 entry. */
  long sz = lseek(fd, 0, SEEK_END);
  check("size == 4096", (int)sz, FIXTURE_SIZE);

  /* 2. Read the whole 4 KiB back to the start. */
  if (lseek(fd, 0, SEEK_SET) < 0) {
    failmsg("seek to 0");
  }
  unsigned char buf[FIXTURE_SIZE];
  size_t got = 0;
  while (got < FIXTURE_SIZE) {
    int n = (int)read(fd, buf + got, FIXTURE_SIZE - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  check("read full 4096 bytes", (int)got, FIXTURE_SIZE);

  /* 3. Byte-exact compare against the staged pattern (unsigned bytes: `char`
     is signed on x86_64, so cast both sides — the GB-062 lesson). */
  int bad = 0;
  for (int i = 0; i < FIXTURE_SIZE; i++) {
    unsigned char want = (unsigned char)(i % 251);
    if ((unsigned char)buf[i] != want) bad++;
  }
  check("byte-exact content (0 mismatches)", bad, 0);

  close(fd);

  if (failures == 0) {
    print_console("[STAGE4K] ALL PASSED\n");
  } else {
    print_console("[STAGE4K] FAILED: ");
    print_hex((long)failures);
    print_console("\n");
  }
  exit(0);
}
