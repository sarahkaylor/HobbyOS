/*
 * src/user/linux_probe_pfstat.c -- the kernel's [PF*] fault-cost rows
 * (H7-fault; src/kernel/vm_pfstat.c, docs/cp-fault.md §2).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 *   A1  touch 4096 fresh anonymous pages (16 MiB)       -> anon   >= 4096
 *   C1  fork; the child writes 256 inherited pages      -> cow    >= 256
 *   F1  mmap this program's own file, read 8 pages      -> fat|ext2|nfs >= 8
 *   R1  refault: munmap + re-touch the same 64 pages    -> anon grows by 64
 * then sleeps past the kernel's 10 s row interval and takes one more fault,
 * so the interval holding the work is printed.  The verdict is the console's:
 * tools/gate/lp_check_pfstat.py sums every [PFK]/[PFH]/[PFL]/[PFI] row and
 * checks the counts above, that each row's kinds and histogram add up to its
 * n=, and that frames were zeroed and vm_lock pairs counted.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PG 4096

static void sleep_ms(long ms) {
  struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&t, 0);
}

int main(int argc, char **argv) {
  (void)argc;
  setvbuf(stdout, NULL, _IOLBF, 0);
  int fails = 0;

  /* A1: fresh anonymous pages, one write each */
  size_t n = 4096;
  volatile uint8_t *a = mmap(0, n * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (a == MAP_FAILED) {
    printf("[LP] A1 mmap anon: FAIL\n");
    fails++;
  } else {
    for (size_t i = 0; i < n; i++) a[i * PG] = (uint8_t)i;
    printf("[LP] A1 touched %zu anon pages\n", n);
  }

  /* C1: COW after fork */
  if (a != MAP_FAILED) {
    pid_t c = fork();
    if (c == 0) {
      for (size_t i = 0; i < 256; i++) a[i * PG] = (uint8_t)(i + 1);
      _exit(0);
    }
    int st = 0;
    if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      printf("[LP] C1 fork child: FAIL\n");
      fails++;
    } else {
      printf("[LP] C1 child wrote 256 inherited pages\n");
    }
  }

  /* F1: file-backed pages of this program */
  int fd = open(argv[0], O_RDONLY);
  if (fd < 0) fd = open("/PROBE.ELF", O_RDONLY);
  off_t sz = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
  if (fd < 0 || sz < 8 * PG) {
    printf("[LP] F1 open self: FAIL (fd=%d size=%ld)\n", fd, (long)sz);
    fails++;
  } else {
    volatile uint8_t *f = mmap(0, 8 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    if (f == MAP_FAILED) {
      printf("[LP] F1 mmap file: FAIL\n");
      fails++;
    } else {
      unsigned sum = 0;
      for (int i = 0; i < 8; i++) sum += f[i * PG];
      printf("[LP] F1 read 8 file pages (sum %u)\n", sum);
    }
    close(fd);
  }

  /* R1: the same 64 pages unmapped and touched again */
  if (a != MAP_FAILED) {
    munmap((void *)a, 64 * PG);
    volatile uint8_t *b = mmap((void *)a, 64 * PG, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (b == MAP_FAILED) {
      printf("[LP] R1 remap: FAIL\n");
      fails++;
    } else {
      for (int i = 0; i < 64; i++) b[i * PG] = 1;
      printf("[LP] R1 re-touched 64 pages\n");
    }
  }

  /* close the kernel's interval: one more fault after 10 s */
  sleep_ms(10500);
  volatile uint8_t *z = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (z != MAP_FAILED) z[0] = 1;
  sleep_ms(200);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
