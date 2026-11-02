/* GB-016: uname(2) over the Linux-ABI compat surface, from inside the guest.
 *
 * The identity itself is produced by the kernel's linux_fill_utsname()
 * (src/kernel/linux_uname.c); this probe only proves the DISPATCH reached it,
 * because that wiring is what the card was missing -- the fill and its T2 test
 * have been on main since PR #61.
 *
 * Built with musl-gcc -static-pie, exactly like the other W0 compat vehicles,
 * so the process carries PROC_FLAG_LINUX_ABI and its syscalls arrive as Linux
 * numbers (uname = 63).
 *
 * Rows are single-line and greppable.  The falsifiers:
 *   - a dispatch that did nothing returns -ENOSYS (38), not 0;
 *   - a dispatch that copied nothing leaves the buffer zeroed, so sysname is
 *     empty rather than "Linux";
 *   - a wrong machine string fails the per-arch row;
 *   - a missing user-range check SEGFAULTS instead of returning -EFAULT, and a
 *     present one returns exactly -1/EFAULT on a bad pointer.
 */
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

/* musl's own struct utsname is 6 x 65 bytes; declare the Linux layout here so
   the probe does not depend on a header the guest toolchain may not carry. */
struct utsname_l {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

static long sys_uname(struct utsname_l *u) {
  long r;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(63L), "D"(u)
                   : "rcx", "r11", "memory");
  return r;
}

static void emit(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  long r = write(1, s, n);
  (void)r;
}

int main(void) {
  struct utsname_l u;
  memset(&u, 0, sizeof(u));
  long r = sys_uname(&u);
  char line[256];
  int k;

  emit("gb016 BEGIN\n");

  /* rc row: 0 means the dispatch exists and copied; -38 is the no-dispatch
     signature, which is exactly what this card was filed about. */
  k = 0;
  {
    const char *p = "gb016 rc=";
    while (*p) line[k++] = *p++;
  }
  if (r == 0) { line[k++] = '0'; }
  else { line[k++] = '-'; line[k++] = (char)('0' + (int)(-r) / 10); line[k++] = (char)('0' + (int)(-r) % 10); }
  line[k++] = '\n';
  line[k] = 0;
  emit(line);

  /* the three fields the DoD names, printed verbatim so the gate can compare */
  emit("gb016 sysname=");
  emit(u.sysname);
  emit("\n");
  emit("gb016 machine=");
  emit(u.machine);
  emit("\n");
  emit("gb016 release=");
  emit(u.release);
  emit("\n");

  /* falsifier: a bad pointer must be -EFAULT (14), not a fault and not 0 */
  {
    long er = sys_uname((struct utsname_l *)(uintptr_t)1);
    r = er;
    k = 0;
    {
      const char *p = "gb016 efaul rc=";
      while (*p) line[k++] = *p++;
    }
    line[k++] = '-';
    line[k++] = (char)('0' + (int)(-r) / 10);
    line[k++] = (char)('0' + (int)(-r) % 10);
    line[k++] = '\n';
    line[k] = 0;
    emit(line);
  }

  emit("gb016 END\n");
  return 0;
}
