/*
 * FATBIG_T.BIN — in-OS acceptance for the >1 MiB fat16 create-path defect
 * (L7 lane, browser.md §11 / P2.5: "attr=0 entries"; "v2 loader: bad image
 * size 0").
 *
 * Creates a file through the kernel's FAT16 create path, writes 2 MiB of a
 * position-dependent byte pattern in 4 KiB chunks, closes, reopens, and
 * verifies (a) the size reported by lseek(SEEK_END) and (b) a checksum of
 * the tail.  On the pre-fix kernel the on-disk directory entry reads back
 * with file_size 0 for > ~1 MiB, so the reopen size is 0 and the tail read
 * returns nothing — the same shape that makes the image loader reject a
 * freshly built large image.  Status goes to the console (print_console),
 * matching the other wave tests; stdout is a harness pipe.
 */
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "libc.h" /* print_console() */

static int failures = 0;

static void check(const char *what, int got, int want) {
  if (got == want) {
    print_console("[FATBIG] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[FATBIG] FAIL ");
    print_console(what);
    print_console(" (got=");
    print_hex((long)got);
    print_console(" want=");
    print_hex((long)want);
    print_console(")\n");
    failures++;
  }
}

__attribute__((section(".text._start")))
void _start(void) {
  print_console("\n[FATBIG] >1 MiB fat16 create/write/reopen round-trip\n");

  const char *path = "/BIGFILE.BIN";
  const int TARGET = 2 * 1024 * 1024; /* 2 MiB, above the ~1 MiB boundary */
  char chunk[4096];

  /* 1. Create through the kernel create path. */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[FATBIG] FAIL open-create\n");
    exit(0);
  }

  /* 2. Write the full 2 MiB; byte at absolute position p is
     ((p>>8) ^ (p&0xFF)) so any truncation/size-0 misread is detectable. */
  int written = 0;
  while (written < TARGET) {
    int n = TARGET - written;
    if (n > (int)sizeof chunk) n = (int)sizeof chunk;
    for (int i = 0; i < n; i++) {
      int p = written + i;
      chunk[i] = (char)((p >> 8) ^ (p & 0xFF));
    }
    int w = write(fd, chunk, n);
    if (w != n) {
      print_console("[FATBIG] FAIL short write\n");
      exit(0);
    }
    written += w;
  }
  check("write full 2 MiB", written, TARGET);
  close(fd);

  /* 3. Reopen read-only; the size must survive the close (this is the
     path the loader uses: reopen -> entry.file_size -> map). */
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    print_console("[FATBIG] FAIL reopen\n");
    exit(0);
  }
  long sz = lseek(fd, 0, SEEK_END);
  check("reopen size == 2 MiB", (int)sz, TARGET);

  /* 4. Read the tail 4 KiB and checksum it. */
  if (lseek(fd, TARGET - 4096, SEEK_SET) < 0) {
    print_console("[FATBIG] FAIL seek tail\n");
    exit(0);
  }
  int rn = (int)read(fd, chunk, (int)sizeof chunk);
  check("tail read 4096 bytes", rn, 4096);
  if (rn == 4096) {
    int bad = 0;
    for (int i = 0; i < rn; i++) {
      int p = TARGET - 4096 + i;
      if ((chunk[i] & 0xFF) != (char)((p >> 8) ^ (p & 0xFF))) bad++;
    }
    check("tail checksum clean", bad, 0);
  }
  close(fd);

  if (failures == 0) {
    print_console("[FATBIG] ALL PASSED\n");
  } else {
    print_console("[FATBIG] FAILED: ");
    print_hex((long)failures);
    print_console("\n");
  }
  exit(0);
}
