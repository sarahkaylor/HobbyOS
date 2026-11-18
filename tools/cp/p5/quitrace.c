/* tools/cp/p5/quitrace.c - H3-winefix: the wineserver-SIGQUIT-vs-thread-exit
 * race, amplified (docs/cp-winefix.md §2).
 *
 * A Windows x86_64 console program (tools/cp/p5/run-quitrace.sh builds it the
 * build-winprobe.sh way: clang -target x86_64-windows, Wine's headers and PE
 * import libs).  wineserver's kill_thread(violent_death) SIGQUITs a thread that
 * is still on its list; a thread that exits ON ITS OWN (NtTerminateThread(self)
 * -> exit_thread -> pthread_exit) stays on that list until the server sees its
 * request fd close.  Stock Wine 11.0 does not block SIGQUIT in that window, so
 * quit_handler runs inside the exit (after glibc cleared the TEB TSD slot it
 * reads NULL+0x3a8).  This program makes the window frequent:
 *
 *   quitrace.exe thr <rounds> [pairs]
 *     <pairs> killer threads (default 4) each run <rounds> times:
 *       CreateThread(worker); the worker sets its "leaving" flag and calls
 *       NtTerminateThread(self) (-> exit_thread); the killer waits for
 *       the flag, spins a delay that sweeps 0..255 us, then TerminateThread()s
 *       the worker (the server SIGQUITs it if it is still listed), waits for
 *       it and closes the handle.
 *     A "[QR] thr t=<s>s rounds=<n>" row every 5 s; "[QR] done rc=0" at the
 *     end; "[QR] thr HUNG ..." + exit 3 when no round finishes for 60 s.  Stock Wine: the process dies of the fault (or, when a SIGQUIT
 *     re-enters abort_thread and nb_threads is decremented twice, exits
 *     early without the done row).
 *
 *   quitrace.exe proc <n> [ms_max]
 *     the P5 shape: <n> times start "quitrace.exe churn" (16 threads that
 *     create threads which exit on their own, forever), let it run a random
 *     150..ms_max ms (default 900), TerminateProcess() it (the server's
 *     terminate_process SIGQUITs every listed thread).  A fault shows only on
 *     the serial console ([KERNEL] ... memory fault); the rows report the
 *     exit codes seen.
 *
 * Every row is ONE WriteFile.
 */
#include <windows.h>
#include <winternl.h>

struct row { char b[256]; int n; };
static void r_s(struct row *r, const char *s) {
  while (*s && r->n < (int)sizeof r->b - 3) r->b[r->n++] = *s++;
}
static void r_d(struct row *r, long long v) {
  char t[24];
  int k = 0;
  unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
  if (v < 0) r_s(r, "-");
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 23);
  while (k && r->n < (int)sizeof r->b - 3) r->b[r->n++] = t[--k];
}
static void r_end(struct row *r) {
  DWORD n = 0;
  r->b[r->n++] = '\r';
  r->b[r->n++] = '\n';
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), r->b, (DWORD)r->n, &n, NULL);
  r->n = 0;
}

static long long atoll_(const char *s, long long def) {
  long long v = 0;
  int any = 0;
  if (!s) return def;
  while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
  return any ? v : def;
}

static LARGE_INTEGER qpf;
static long long now_us(void) {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (long long)(c.QuadPart * 1000000 / qpf.QuadPart);
}
static void spin_us(long long us) {
  long long end = now_us() + us;
  while (now_us() < end) YieldProcessor();
}

/* ------------------------------------------------------------- thr mode */
struct pair {
  volatile LONG leaving;
  long long rounds, done, create_fail, wait_fail, code_killed, code_self, stuck;
  unsigned seed;
  HANDLE th;
};

static DWORD WINAPI worker(void *arg) {
  struct pair *p = arg;
  InterlockedExchange(&p->leaving, 1);
  /* NtTerminateThread(self) directly, not RtlExitUserThread: the latter runs
     LdrShutdownThread under the loader lock, and a TerminateThread() there
     orphans the lock (every later CreateThread then hangs in thread attach;
     seen in run stock1). */
  NtTerminateThread(GetCurrentThread(), 0);
  return 0;
}

static DWORD WINAPI killer(void *arg) {
  struct pair *p = arg;
  for (long long i = 0; i < p->rounds; i++) {
    p->leaving = 0;
    HANDLE h = CreateThread(NULL, 64 * 1024, worker, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { p->create_fail++; Sleep(1); continue; }
    /* A worker that never reaches its function within 20 s is a hung thread
       start (seen on stock Wine, run stock3): count it, report it, stop this
       killer, so the run ends with a FAIL instead of a hang. */
    long long t_start = now_us();
    while (!p->leaving && now_us() - t_start < 20000000) Sleep(0);
    if (!p->leaving) {
      struct row r = {{0}, 0};
      DWORD st = WaitForSingleObject(h, 0), code = 0;
      GetExitCodeThread(h, &code);
      r_s(&r, "[QR] thr STUCK round="); r_d(&r, i); r_s(&r, " worker_wait="); r_d(&r, st);
      r_s(&r, " worker_code="); r_d(&r, code); r_end(&r);
      p->stuck++;
      CloseHandle(h);
      break;
    }
    p->seed = p->seed * 1103515245u + 12345u;
    spin_us((p->seed >> 16) & 0xff);
    TerminateThread(h, 7);
    if (WaitForSingleObject(h, 10000) != WAIT_OBJECT_0) p->wait_fail++;
    DWORD code = 0;
    GetExitCodeThread(h, &code);
    if (code == 7) p->code_killed++; else p->code_self++;
    CloseHandle(h);
    p->done++;
  }
  return 0;
}

static int mode_thr(long long rounds, int pairs) {
  static struct pair pr[16];
  struct row r = {{0}, 0};
  if (pairs < 1) pairs = 1;
  if (pairs > 16) pairs = 16;
  r_s(&r, "[QR] thr begin rounds="); r_d(&r, rounds); r_s(&r, " pairs="); r_d(&r, pairs); r_end(&r);
  long long t0 = now_us();
  for (int i = 0; i < pairs; i++) {
    pr[i].rounds = rounds;
    pr[i].seed = (unsigned)(t0 ^ (i * 2654435761u));
    pr[i].th = CreateThread(NULL, 0, killer, &pr[i], 0, NULL);
  }
  long long next = t0 + 5000000, last_d = -1, last_change = t0;
  for (;;) {
    int alive = 0;
    for (int i = 0; i < pairs; i++)
      if (WaitForSingleObject(pr[i].th, 0) == WAIT_TIMEOUT) alive++;
    if (!alive) break;
    Sleep(100);
    long long d = 0;
    for (int i = 0; i < pairs; i++) d += pr[i].done;
    if (d != last_d) { last_d = d; last_change = now_us(); }
    if (now_us() >= next) {
      r_s(&r, "[QR] thr t="); r_d(&r, (now_us() - t0) / 1000000); r_s(&r, "s rounds="); r_d(&r, d); r_end(&r);
      next += 5000000;
    }
    /* Watchdog: no round finished for 60 s = the killers are deadlocked (on
       stock Wine: a worker SIGQUIT'd inside exit_thread() while it held a
       lock, seen in runs stock3 / ab-stock-1).  Report and kill ourselves. */
    if (now_us() - last_change > 60000000) {
      r_s(&r, "[QR] thr HUNG rounds="); r_d(&r, d); r_s(&r, " alive_killers="); r_d(&r, alive);
      r_s(&r, " s="); r_d(&r, (now_us() - t0) / 1000000); r_end(&r);
      r_s(&r, "[QR] done rc=3"); r_end(&r);
      TerminateProcess(GetCurrentProcess(), 3);
    }
  }
  long long d = 0, cf = 0, wf = 0, ck = 0, cs = 0, sk = 0;
  for (int i = 0; i < pairs; i++) {
    d += pr[i].done; cf += pr[i].create_fail; wf += pr[i].wait_fail;
    ck += pr[i].code_killed; cs += pr[i].code_self; sk += pr[i].stuck;
  }
  int rc = (d != rounds * pairs) || wf || sk;
  r_s(&r, "[QR] thr end rounds="); r_d(&r, d); r_s(&r, " create_fail="); r_d(&r, cf);
  r_s(&r, " wait_fail="); r_d(&r, wf); r_s(&r, " exit_code_terminate="); r_d(&r, ck);
  r_s(&r, " exit_code_self="); r_d(&r, cs); r_s(&r, " stuck="); r_d(&r, sk); r_s(&r, " ms="); r_d(&r, (now_us() - t0) / 1000); r_end(&r);
  r_s(&r, "[QR] done rc="); r_d(&r, rc); r_end(&r);
  return rc;
}

/* ----------------------------------------------------- churn / proc mode */
static DWORD WINAPI churn_leaf(void *arg) { (void)arg; return 0; }
static DWORD WINAPI churner(void *arg) {
  (void)arg;
  for (;;) {
    HANDLE h = CreateThread(NULL, 64 * 1024, churn_leaf, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (h) CloseHandle(h); else Sleep(1);
  }
  return 0;
}

static int mode_churn(void) {
  for (int i = 0; i < 16; i++) CloseHandle(CreateThread(NULL, 0, churner, NULL, 0, NULL));
  Sleep(INFINITE);
  return 0;
}

static int mode_proc(long long n, long long ms_max) {
  struct row r = {{0}, 0};
  char self[MAX_PATH], cmd[MAX_PATH + 16];
  unsigned seed = (unsigned)now_us();
  long long started = 0, fail = 0, t0 = now_us();
  if (ms_max < 200) ms_max = 200;
  GetModuleFileNameA(NULL, self, sizeof self);
  r_s(&r, "[QR] proc begin n="); r_d(&r, n); r_s(&r, " ms_max="); r_d(&r, ms_max); r_end(&r);
  for (long long i = 0; i < n; i++) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    int k = 0;
    cmd[k++] = '"';
    for (int j = 0; self[j] && k < MAX_PATH; j++) cmd[k++] = self[j];
    const char *tail = "\" churn";
    for (int j = 0; tail[j]; j++) cmd[k++] = tail[j];
    cmd[k] = 0;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
      fail++;
      r_s(&r, "[QR] proc CreateProcess failed err="); r_d(&r, GetLastError()); r_end(&r);
      Sleep(200);
      continue;
    }
    started++;
    seed = seed * 1103515245u + 12345u;
    Sleep(150 + (DWORD)((seed >> 8) % (unsigned)(ms_max - 150)));
    TerminateProcess(pi.hProcess, 9);
    DWORD wr = WaitForSingleObject(pi.hProcess, 30000), code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (wr != WAIT_OBJECT_0 || code != 9) {
      fail++;
      r_s(&r, "[QR] proc #"); r_d(&r, i); r_s(&r, " wait="); r_d(&r, wr); r_s(&r, " code="); r_d(&r, code); r_end(&r);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if ((i + 1) % 10 == 0) {
      r_s(&r, "[QR] proc t="); r_d(&r, (now_us() - t0) / 1000000); r_s(&r, "s killed="); r_d(&r, i + 1); r_end(&r);
    }
  }
  r_s(&r, "[QR] proc end started="); r_d(&r, started); r_s(&r, " fail="); r_d(&r, fail);
  r_s(&r, " ms="); r_d(&r, (now_us() - t0) / 1000); r_end(&r);
  r_s(&r, "[QR] done rc="); r_d(&r, fail != 0); r_end(&r);
  return fail != 0;
}

int main(int argc, char **argv) {
  QueryPerformanceFrequency(&qpf);
  const char *m = argc > 1 ? argv[1] : "thr";
  if (!lstrcmpA(m, "churn")) return mode_churn();
  if (!lstrcmpA(m, "proc")) return mode_proc(atoll_(argc > 2 ? argv[2] : 0, 100), atoll_(argc > 3 ? argv[3] : 0, 900));
  return mode_thr(atoll_(argc > 2 ? argv[2] : 0, 20000), (int)atoll_(argc > 3 ? argv[3] : 0, 4));
}
