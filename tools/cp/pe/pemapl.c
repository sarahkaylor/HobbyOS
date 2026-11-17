/*
 * tools/cp/pe/pemapl.c - C1-pe: native launcher for linux_probe_pemap over
 * NFS and ext2 (docs/cp-pe.md).  Built and staged by tools/cp/pe/run-pemap.sh.
 *
 * Mounts the localhost-only export PE_EXPORT at /CP (the harness filled it
 * with pemapNN.bin data files), then runs /PEMAP.ELF (the Linux probe) PE_RUNS
 * times with the argument string PE_ARGS, e.g.
 *   "-p 8 -c 64 -i 6 /CP /EXT/pe"
 * Each run's own rows ("[LP] ...") reach the console; this launcher adds
 * "[PE] run k status=s ms=t" and a final "[PE] done runs=R bad=B".
 */
#include <errno.h>
#include <stdint.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef PE_EXPORT
#define PE_EXPORT "/srv/nfs/cp-pe-data"
#endif
#ifndef PE_ARGS
#define PE_ARGS "-p 8 -c 32 -i 4 /CP"
#endif
#ifndef PE_RUNS
#define PE_RUNS 3
#endif
#define MNT "/CP"

static void pc(const char *s) { print_console(s); }
static void pd(long v) {
  char t[24];
  int m = 0;
  unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
  if (v < 0) pc("-");
  if (u == 0) t[m++] = '0';
  while (u) { t[m++] = (char)('0' + u % 10); u /= 10; }
  char b[24];
  int n = 0;
  while (m) b[n++] = t[--m];
  b[n] = 0;
  pc(b);
}
static uint64_t now_ms(void) { return (uint64_t)sysinfo(1, 0, 0); }

static char *envp[] = {(char *)"PATH=/", (char *)"HOME=/", 0};

int main(void) {
  pc("[PE] pemap launcher (C1-pe) args=\"" PE_ARGS "\"\n");
  char src[200];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; PE_EXPORT[i]; i++) src[n++] = PE_EXPORT[i];
  src[n] = 0;
  if (mount(src, MNT) != 0) {
    pc("[PE] mount FAILED errno=");
    pd(errno);
    pc("\n[PE] done runs=0 bad=1\n");
    return 1;
  }
  pc("[PE] mount ");
  pc(src);
  pc(" -> " MNT " ok\n");
  /* split PE_ARGS on spaces */
  static char args[] = PE_ARGS;
  char *argv[40];
  int ac = 0;
  argv[ac++] = (char *)"pemap";
  for (char *p = args; *p && ac < 39;) {
    while (*p == ' ') *p++ = 0;
    if (!*p) break;
    argv[ac++] = p;
    while (*p && *p != ' ') p++;
  }
  argv[ac] = 0;
  int bad = 0;
  for (int k = 0; k < PE_RUNS; k++) {
    uint64_t t0 = now_ms();
    int pid = spawn_ex("/PEMAP.ELF", argv, envp, 0, 0);
    if (pid < 0) {
      pc("[PE] run ");
      pd(k);
      pc(" spawn failed errno=");
      pd(errno);
      pc("\n");
      bad++;
      continue;
    }
    int status = -1;
    waitpid(pid, &status, 0);
    pc("[PE] run ");
    pd(k);
    pc(" status=");
    pd(status);
    pc(" ms=");
    pd((long)(now_ms() - t0));
    pc("\n");
    if (status != 0) bad++;
  }
  pc("[PE] done runs=");
  pd(PE_RUNS);
  pc(" bad=");
  pd(bad);
  pc("\n");
  return 0;
}
