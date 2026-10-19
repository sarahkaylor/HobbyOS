/*
 * memfd_test_host.c - T4 differential host tests for the memfd_create +
 * shared-memory ABI surface (GB-030).  The code under test is the Linux
 * memfd/shared-mem contract the compat layer must satisfy for SDL
 * surfaces (docs/linux-abi.md §2.2 #4 / gaming.md §2.2):
 *
 *   memfd_create(name, flags) -> fd  (Linux x86_64 syscall 319)
 *   mmap(fd, MAP_SHARED) -> a MAP_SHARED fd-backed mapping whose writes are
 *   visible to any process with a mapping of the same memfd (SDL surfaces,
 *   XSharedMemory, game shared state).
 *
 * This is a live differential test: it RUNS the real Linux memfd_create /
 * fcntl(F_ADD_SEALS) / mmap(MAP_SHARED) on the host and asserts the
 * concrete behaviours and numeric ABI the HobbyOS compat layer must match
 * (memfd_create syscall number, MFD_* flags, F_ADD_SEALS/F_GET_SEALS
 * command numbers, F_SEAL_* bits, and two-process shared-memory write
 * visibility after fork).  These are the same numbers locked in
 * src/include/fs.h (K_MFD_*, K_F_ADD_SEALS, K_F_SEAL_*) and the Linux
 * syscall table (L_SYS_memfd_create = 319).
 *
 * Runs only where memfd_create is available (Linux 3.17+; disable with
 * the __linux__ guard if a host lacks it).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>

/* The numeric ABI surface from src/include/fs.h + src/include/linux_syscall.h
 * (locked via the T1 golden suite); this T4 test proves the *behaviour*. */
#define K_MEMFD_SYSCALL   319
#define K_MFD_CLOEXEC     0x0001
#define K_MFD_ALLOW_SEALING 0x0002
#define K_F_ADD_SEALS     1033
#define K_F_GET_SEALS     1034
#define K_F_SEAL_SEAL     0x0001
#define K_F_SEAL_SHRINK   0x0002
#define K_F_SEAL_GROW     0x0004
#define K_F_SEAL_WRITE    0x0008

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *name) {
  checks_run++;
  if (ok) {
    printf("  PASS: %s\n", name);
  } else {
    checks_failed++;
    printf("  FAIL: %s\n", name);
  }
}

/* memfd_create via the raw syscall (same number the compat layer traps). */
static long raw_memfd_create(const char *name, unsigned int flags) {
  return syscall(K_MEMFD_SYSCALL, name, flags);
}

static void test_syscall_number(void) {
  /* The syscall number on this host must be 319 (the value locked in the
     golden / our table) and must actually create an fd. */
  long fd = raw_memfd_create("t4-gb030", 0);
  check(fd >= 0, "memfd_create: syscall 319 creates an fd");
  if (fd >= 0) { close((int)fd); }
  /* Wrong number (318 = stat) would not be memfd; sanity only. */
  (void)0;
}

static void test_flags(void) {
  /* MFD_CLOEXEC: the fd must have FD_CLOEXEC set.  MFD_ALLOW_SEALING must
     be accepted and then F_GET_SEALS == 0 until a seal is added. */
  long fd = raw_memfd_create("t4-flags", K_MFD_CLOEXEC);
  if (fd < 0) { check(0, "flags: MFD_CLOEXEC create"); return; }
  int fl = fcntl((int)fd, F_GETFD);
  check((fl & FD_CLOEXEC) != 0,
        "flags: MFD_CLOEXEC -> FD_CLOEXEC set on the fd");
  close((int)fd);

  fd = raw_memfd_create("t4-seal", K_MFD_ALLOW_SEALING);
  if (fd < 0) { check(0, "flags: MFD_ALLOW_SEALING create"); return; }
  long seals = fcntl((int)fd, K_F_GET_SEALS);
  check(seals == 0, "flags: F_GET_SEALS == 0 on a fresh ALLOW_SEALING memfd");
  /* F_ADD_SEALS of F_SEAL_SEAL|F_SEAL_WRITE sticks. */
  long add = fcntl((int)fd, K_F_ADD_SEALS, K_F_SEAL_SEAL | K_F_SEAL_WRITE);
  check(add == 0, "flags: F_ADD_SEALS(SEAL|WRITE) succeeds");
  seals = fcntl((int)fd, K_F_GET_SEALS);
  check((seals & (K_F_SEAL_SEAL | K_F_SEAL_WRITE)) ==
            (K_F_SEAL_SEAL | K_F_SEAL_WRITE),
        "flags: F_GET_SEALS reflects added SEAL|WRITE");
  close((int)fd);
}

static void test_shared_visibility(void) {
  /* The core SDL-surfaces requirement: a MAP_SHARED fd-backed memfd mapping
     written by the parent is visible in a fresh child mapping (fork). */
  long fd = raw_memfd_create("t4-shm", 0);
  if (fd < 0) { check(0, "shm: memfd_create"); return; }
  const char *buf = "gb030-shared-memory-write";
  /* grow the memfd (memfd starts at size 0; mmap of size 0 is useless). */
  if (ftruncate((int)fd, 4096) != 0) { check(0, "shm: ftruncate"); close((int)fd); return; }
  volatile long *p = (volatile long *)mmap(NULL, 4096,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, (int)fd, 0);
  if (p == MAP_FAILED) { check(0, "shm: mmap MAP_SHARED"); close((int)fd); return; }
  p[0] = 0;
  p[1] = 0;
  (void)buf;

  /* Parent writes first; the child's own MAP_SHARED mapping of the same
     memfd (created after fork) must see this byte — the SDL-surfaces /
     shared-state requirement. */
  p[0] = 0x12345678;

  pid_t pid = fork();
  if (pid == 0) {
    /* child: its own mapping of the same fd must see the parent's write. */
    volatile long *q = (volatile long *)mmap(NULL, 4096,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, (int)fd, 0);
    _exit((q != MAP_FAILED && q[0] == 0x12345678) ? 0 : 1);
  }
  if (pid > 0) {
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "shm: child MAP_SHARED mapping sees the parent's write (after fork)");
  }
  munmap((void *)p, 4096);
  close((int)fd);
}

int main(void) {
  printf("memfd_test_host:\n");
  test_syscall_number();
  test_flags();
  test_shared_visibility();

  printf("memfd_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
