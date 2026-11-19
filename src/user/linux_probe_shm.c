/*
 * src/user/linux_probe_shm.c - C4-x3: System V shared memory (shmget/shmat/
 * shmdt/shmctl), the segment form Wine's winex11 hands the X server through
 * MIT-SHM (XShmCreateImage + shmget(IPC_PRIVATE) + shmat + XShmAttach, then
 * IPC_RMID right after the server attached).  docs/cp-x3.md §2.
 *
 * ORDINARY musl program; same ELF on strix (Linux) and in the guest.  Rows
 * "[LP] PASS|FAIL ...", last row "[LP] done rc=<fails>".  On a kernel with no
 * SysV shm every row fails (shmget is ENOSYS).
 *
 *  M1  shmget(IPC_PRIVATE, 1 MiB, IPC_CREAT|0600) returns an id
 *  M2  shmat(id, 0, 0) maps it; a pattern written there reads back
 *  M3  IPC_STAT: shm_segsz = 1 MiB, mode 0600, nattch = 1, cpid = getpid()
 *  M4  a forked child shmat()s the SAME id, sees the parent's pattern, writes
 *      its own into the second half; the parent reads the child's bytes
 *      through its own mapping (one set of frames for two processes)
 *  M5  nattch counts the inherited mapping of a fork (2 while the child
 *      lives, 1 after it exits)
 *  M6  winex11's order: IPC_RMID while attached; the segment survives, a
 *      second shmat on the id still works, bytes are intact; after the last
 *      shmdt the id is gone (IPC_STAT -> EINVAL)
 *  M7  keyed segments: IPC_CREAT|IPC_EXCL creates, a repeat is EEXIST, a
 *      lookup without IPC_CREAT finds the same id, a bigger size is EINVAL,
 *      an unknown key is ENOENT; IPC_SET changes the mode
 *  M8  shmdt of a non-shm address and shmat of a bad id are EINVAL
 *  M9  SHM_RDONLY: a write faults (SIGSEGV) in a child
 *  M10 40 rounds of create/attach/touch 16 MiB/RMID/detach leave free memory
 *      within 64 MiB of where it started (a destroyed segment is released)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>

static void out(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static int fails;
static void row(int ok, const char *fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  out("[LP] %s %s\n", ok ? "PASS" : "FAIL", b);
  if (!ok) fails++;
}

#define SEG (1u << 20)

static long long freeram(void) {
  struct sysinfo si;
  if (sysinfo(&si) != 0) return -1;
  return (long long)si.freeram * (long long)si.mem_unit;
}

static int nattch(int id) {
  struct shmid_ds ds;
  if (shmctl(id, IPC_STAT, &ds) != 0) return -errno;
  return (int)ds.shm_nattch;
}

int main(void) {
  out("[LP] shm probe (C4-x3): System V shared memory\n");

  /* M1 */
  int id = shmget(IPC_PRIVATE, SEG, IPC_CREAT | 0600);
  row(id >= 0, "M1 shmget(IPC_PRIVATE, 1 MiB) id=%d errno=%d", id, id < 0 ? errno : 0);
  if (id < 0) {
    out("[LP] done rc=%d\n", fails + 9);
    return 1;
  }

  /* M2 */
  uint8_t *a = shmat(id, 0, 0);
  int ok = a != (void *)-1;
  if (ok) {
    for (unsigned i = 0; i < SEG; i++) a[i] = (uint8_t)(i * 7 + 3);
    for (unsigned i = 0; i < SEG; i++)
      if (a[i] != (uint8_t)(i * 7 + 3)) { ok = 0; break; }
  }
  row(ok, "M2 shmat -> %p, 1 MiB pattern reads back", (void *)a);
  if (a == (void *)-1) {
    out("[LP] done rc=%d\n", fails + 8);
    return 1;
  }

  /* M3 */
  struct shmid_ds ds;
  memset(&ds, 0, sizeof ds);
  int r = shmctl(id, IPC_STAT, &ds);
  row(r == 0 && ds.shm_segsz == SEG && (ds.shm_perm.mode & 0777) == 0600 &&
          ds.shm_nattch == 1 && ds.shm_cpid == getpid(),
      "M3 IPC_STAT rc=%d segsz=%zu mode=%o nattch=%lu cpid=%d (me %d)", r,
      (size_t)ds.shm_segsz, (unsigned)ds.shm_perm.mode & 0777,
      (unsigned long)ds.shm_nattch, (int)ds.shm_cpid, (int)getpid());

  /* M4 + M5 */
  int pfd[2];
  (void)!pipe(pfd);
  pid_t c = fork();
  if (c == 0) {
    close(pfd[0]);
    uint8_t *b = shmat(id, 0, 0);
    int good = b != (void *)-1;
    if (good) {
      for (unsigned i = 0; i < SEG / 2; i++)
        if (b[i] != (uint8_t)(i * 7 + 3)) { good = 0; break; }
      for (unsigned i = SEG / 2; i < SEG; i++) b[i] = (uint8_t)(i ^ 0x5A);
    }
    int n = nattch(id); /* inherited + own = 2 here, parent's = 1: 3 */
    char msg[2] = {(char)(good ? 'Y' : 'N'), (char)n};
    (void)!write(pfd[1], msg, 2);
    if (b != (void *)-1) shmdt(b);
    sleep(1); /* parent samples nattch while we still hold the inherited map */
    _exit(0);
  }
  close(pfd[1]);
  char msg[2] = {0, 0};
  (void)!read(pfd[0], msg, 2);
  usleep(200 * 1000);
  int mid = nattch(id); /* parent + child's inherited mapping */
  int st = 0;
  waitpid(c, &st, 0);
  /* Linux drops the child's mappings before waitpid returns.  HobbyOS may
     keep a just-exited child's address space until the CPU it ran on has
     switched away (process.c: an AS another CPU still translates through is
     never freed), so allow it a few seconds and report how long it took. */
  int after = nattch(id), waited = 0;
  while (after > 1 && waited < 5000) {
    usleep(50 * 1000);
    waited += 50;
    after = nattch(id);
  }
  ok = msg[0] == 'Y';
  for (unsigned i = SEG / 2; ok && i < SEG; i++)
    if (a[i] != (uint8_t)(i ^ 0x5A)) ok = 0;
  row(ok, "M4 child shmat of the same id: saw parent's bytes=%c, parent sees child's bytes=%s",
      msg[0] ? msg[0] : '?', ok ? "yes" : "no");
  row(msg[1] == 3 && mid == 2 && after == 1,
      "M5 nattch: in child %d (want 3), after child's shmdt %d (want 2), after exit %d (want 1; %d ms after waitpid)",
      (int)msg[1], mid, after, waited);

  /* M6 */
  r = shmctl(id, IPC_RMID, 0);
  uint8_t *a2 = shmat(id, 0, 0);
  ok = r == 0 && a2 != (void *)-1 && a2[5] == (uint8_t)(5 * 7 + 3) &&
       a2[SEG - 1] == (uint8_t)((SEG - 1) ^ 0x5A);
  int n2 = nattch(id);
  int d1 = a2 != (void *)-1 ? shmdt(a2) : -1;
  int d2 = shmdt(a);
  errno = 0;
  int gone = shmctl(id, IPC_STAT, &ds);
  int gone_errno = errno;
  row(ok && n2 == 2 && d1 == 0 && d2 == 0 && gone == -1 && gone_errno == EINVAL,
      "M6 IPC_RMID while attached: rmid=%d reattach=%s nattch=%d dt=%d,%d then IPC_STAT=%d errno=%d",
      r, ok ? "ok" : "bad", n2, d1, d2, gone, gone_errno);

  /* M7 */
  key_t key = 0x48423358; /* "HB3X" */
  int k1 = shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0640);
  errno = 0;
  int k2 = shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0640);
  int e2 = errno;
  int k3 = shmget(key, 4096, 0);
  errno = 0;
  int k4 = shmget(key, 65536, 0);
  int e4 = errno;
  errno = 0;
  int k5 = shmget(0x48420000, 4096, 0);
  int e5 = errno;
  struct shmid_ds ks;
  memset(&ks, 0, sizeof ks);
  shmctl(k1, IPC_STAT, &ks);
  ks.shm_perm.mode = 0604;
  int sr = shmctl(k1, IPC_SET, &ks);
  memset(&ks, 0, sizeof ks);
  shmctl(k1, IPC_STAT, &ks);
  row(k1 >= 0 && k2 == -1 && e2 == EEXIST && k3 == k1 && k4 == -1 && e4 == EINVAL &&
          k5 == -1 && e5 == ENOENT && sr == 0 && (ks.shm_perm.mode & 0777) == 0604 &&
          ks.shm_perm.__key == key,
      "M7 keyed: create=%d excl=%d/%d find=%d big=%d/%d unknown=%d/%d set=%d mode=%o",
      k1, k2, e2, k3, k4, e4, k5, e5, sr, (unsigned)ks.shm_perm.mode & 0777);
  if (k1 >= 0) shmctl(k1, IPC_RMID, 0);

  /* M8 */
  static uint8_t notshm[8192];
  errno = 0;
  int b1 = shmdt(notshm + 4096 - ((uintptr_t)notshm & 4095));
  int eb1 = errno;
  errno = 0;
  void *b2 = shmat(0x7FFFFF00, 0, 0);
  int eb2 = errno;
  row(b1 == -1 && eb1 == EINVAL && b2 == (void *)-1 && eb2 == EINVAL,
      "M8 shmdt(non-shm)=%d/%d shmat(bad id)=%p/%d", b1, eb1, b2, eb2);

  /* M9 */
  int rid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
  c = fork();
  if (c == 0) {
    signal(SIGSEGV, SIG_DFL);
    volatile uint8_t *ro = shmat(rid, 0, SHM_RDONLY);
    if (ro == (void *)-1) _exit(3);
    (void)ro[0];
    ro[0] = 1; /* must fault */
    _exit(4);
  }
  st = 0;
  waitpid(c, &st, 0);
  shmctl(rid, IPC_RMID, 0);
  row(rid >= 0 && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
      "M9 SHM_RDONLY write: child status 0x%x (want SIGSEGV)", st);

  /* M10 */
  long long f0 = freeram();
  int bad = 0;
  for (int it = 0; it < 40; it++) {
    int t = shmget(IPC_PRIVATE, 16u << 20, IPC_CREAT | 0600);
    uint8_t *m = t >= 0 ? shmat(t, 0, 0) : (void *)-1;
    if (t < 0 || m == (void *)-1) { bad++; break; }
    for (unsigned i = 0; i < (16u << 20); i += 4096) m[i] = (uint8_t)it;
    shmctl(t, IPC_RMID, 0);
    shmdt(m);
  }
  (void)shmget(0x48420001, 4096, 0); /* ENOENT; any shm call reaps on HobbyOS */
  long long f1 = freeram();
  long long drop = f0 - f1;
  row(!bad && f0 > 0 && drop < (64ll << 20),
      "M10 40 x 16 MiB segments: free before %lld MiB, after %lld MiB (drop %lld MiB)",
      f0 >> 20, f1 >> 20, drop >> 20);

  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
