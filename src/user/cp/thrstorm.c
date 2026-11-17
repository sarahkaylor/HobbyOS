/* src/user/cp/thrstorm.c - C1-k5 thread/handle storm under Wine
 * (docs/cp-k5.md, docs/cyberpunk.md §7.2e C1-k5).
 *
 * A Windows x86_64 console program built like winprobe.exe
 * (tools/cp/k5/build-thrstorm.sh: clang -target x86_64-windows, Wine's own
 * headers and PE import libraries, no MinGW).  Cyberpunk runs 173-190 threads
 * at its menu; every Wine thread costs the process three pipes to wineserver
 * (request, reply, wait) and the server the other ends, so a thread storm is
 * an fd storm in both processes.
 *
 *   T1 start    256 worker threads (CreateThread, 256 KiB stack reservation);
 *               every one reports in; the process' thread count (Toolhelp)
 *               is >= 257
 *   T2 storm    for SECS seconds (default 60): each round writes a token to
 *               every worker's request channel and SetEvent()s its own
 *               auto-reset event; the worker wakes from
 *               WaitForMultipleObjects({event, stop}), reads the token,
 *               answers token^0x5a on its reply channel, and the main thread
 *               collects all 256 answers.  Workers 0,2,4.. use two anonymous
 *               pipes (CreatePipe); workers 1,3,5.. one duplex named pipe
 *               (\\.\pipe\k5ts-N, server end in the worker, client end in
 *               the main thread).  A row every 10 s.
 *   T3 stop     a manual-reset stop event ends every worker; each exit code
 *               equals its own round count, which must equal the main
 *               thread's (no lost or duplicated wake)
 *
 * Output: "[TS] PASS|FAIL <name> (<value>)" rows, "[TS] t=<s>s ..." progress
 * rows, and a final "[TS] done rc=<failures>"; the exit code is the failure
 * count.  Every row is ONE WriteFile.
 */
#include <windows.h>
#include <tlhelp32.h>

#define NT 256
#define STACK_RESERVE (256 * 1024)

static int fails;

static void put(const char *s) {
  DWORD n = 0;
  int len = 0;
  while (s[len]) len++;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)len, &n, NULL);
}

struct row {
  char b[320];
  int n;
};
static void r_s(struct row *r, const char *s) {
  while (*s && r->n < (int)sizeof r->b - 2) r->b[r->n++] = *s++;
  r->b[r->n] = 0;
}
static void r_u(struct row *r, unsigned long long v) {
  char t[24];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && n < 23);
  while (n && r->n < (int)sizeof r->b - 2) r->b[r->n++] = t[--n];
  r->b[r->n] = 0;
}
static void r_end(struct row *r) {
  r_s(r, "\n");
  put(r->b);
}

static void ck(int ok, const char *what, const char *k1, unsigned long long v1, const char *k2,
               unsigned long long v2) {
  struct row r = {{0}, 0};
  r_s(&r, ok ? "[TS] PASS " : "[TS] FAIL ");
  r_s(&r, what);
  r_s(&r, " (");
  r_s(&r, k1);
  r_s(&r, "=");
  r_u(&r, v1);
  if (k2) {
    r_s(&r, " ");
    r_s(&r, k2);
    r_s(&r, "=");
    r_u(&r, v2);
  }
  r_s(&r, ")");
  r_end(&r);
  if (!ok) fails++;
}

struct worker {
  HANDLE ev;              /* auto-reset: "a token is waiting" */
  HANDLE w_in, w_out;     /* worker side: read requests, write replies */
  HANDLE m_in, m_out;     /* main side: read replies, write requests */
  HANDLE th;
  DWORD tid;
  int named;
  volatile LONG rounds;
  volatile LONG err;      /* first failure code inside the worker */
};

static struct worker W[NT];
static HANDLE g_stop;
static volatile LONG g_started;

static DWORD WINAPI worker_main(LPVOID arg) {
  struct worker *w = (struct worker *)arg;
  InterlockedIncrement(&g_started);
  if (w->named) {
    /* The client end was opened before the thread started, so this answers
       ERROR_PIPE_CONNECTED at once. */
    if (!ConnectNamedPipe(w->w_in, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
      InterlockedCompareExchange(&w->err, 1000 + (LONG)GetLastError(), 0);
      return 0xfffffffe;
    }
  }
  HANDLE hs[2] = {w->ev, g_stop};
  for (;;) {
    DWORD r = WaitForMultipleObjects(2, hs, FALSE, 120000);
    if (r == WAIT_OBJECT_0 + 1) break;
    if (r != WAIT_OBJECT_0) {
      InterlockedCompareExchange(&w->err, 2000 + (LONG)r, 0);
      return 0xfffffffd;
    }
    unsigned char b = 0;
    DWORD n = 0;
    if (!ReadFile(w->w_in, &b, 1, &n, NULL) || n != 1) {
      InterlockedCompareExchange(&w->err, 3000 + (LONG)GetLastError(), 0);
      return 0xfffffffc;
    }
    b ^= 0x5a;
    if (!WriteFile(w->w_out, &b, 1, &n, NULL) || n != 1) {
      InterlockedCompareExchange(&w->err, 4000 + (LONG)GetLastError(), 0);
      return 0xfffffffb;
    }
    InterlockedIncrement(&w->rounds);
  }
  return (DWORD)w->rounds;
}

static int make_channels(int i, struct worker *w) {
  w->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!w->ev) return 1;
  w->named = i & 1;
  if (!w->named) {
    HANDLE r1, w1, r2, w2;
    if (!CreatePipe(&r1, &w1, NULL, 0)) return 2;
    if (!CreatePipe(&r2, &w2, NULL, 0)) return 3;
    w->w_in = r1;  /* requests: main writes w1, worker reads r1 */
    w->m_out = w1;
    w->m_in = r2;  /* replies: worker writes w2, main reads r2 */
    w->w_out = w2;
    return 0;
  }
  WCHAR name[40] = L"\\\\.\\pipe\\k5ts-";
  int k = 0;
  while (name[k]) k++;
  {
    char d[8];
    int n = 0, v = i;
    do {
      d[n++] = (char)('0' + v % 10);
      v /= 10;
    } while (v);
    while (n) name[k++] = (WCHAR)d[--n];
    name[k] = 0;
  }
  HANDLE s = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                              1, 4096, 4096, 0, NULL);
  if (s == INVALID_HANDLE_VALUE) return 4;
  HANDLE c = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (c == INVALID_HANDLE_VALUE) return 5;
  w->w_in = w->w_out = s;
  w->m_in = w->m_out = c;
  return 0;
}

static unsigned count_my_threads(void) {
  HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (sn == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te;
  te.dwSize = sizeof te;
  unsigned n = 0;
  DWORD me = GetCurrentProcessId();
  if (Thread32First(sn, &te)) {
    do {
      if (te.th32OwnerProcessID == me) n++;
      te.dwSize = sizeof te;
    } while (Thread32Next(sn, &te));
  }
  CloseHandle(sn);
  return n;
}

static int parse_secs(void) {
  const WCHAR *cl = GetCommandLineW();
  /* skip argv[0] (quoted or not) */
  if (*cl == L'"') {
    cl++;
    while (*cl && *cl != L'"') cl++;
    if (*cl) cl++;
  } else {
    while (*cl && *cl != L' ' && *cl != L'\t') cl++;
  }
  while (*cl == L' ' || *cl == L'\t') cl++;
  int v = 0, any = 0;
  while (*cl >= L'0' && *cl <= L'9') {
    v = v * 10 + (*cl++ - L'0');
    any = 1;
  }
  return any && v > 0 ? v : 60;
}

int main(void) {
  int secs = parse_secs();
  ULONGLONG t0 = GetTickCount64();
  {
    struct row r = {{0}, 0};
    r_s(&r, "[TS] thrstorm (C1-k5): threads=");
    r_u(&r, NT);
    r_s(&r, " secs=");
    r_u(&r, (unsigned long long)secs);
    r_s(&r, " pid=");
    r_u(&r, GetCurrentProcessId());
    r_end(&r);
  }
  g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  int made = 0, why = 0;
  for (int i = 0; i < NT; i++) {
    why = make_channels(i, &W[i]);
    if (why) break;
    W[i].th = CreateThread(NULL, STACK_RESERVE, worker_main, &W[i], STACK_SIZE_PARAM_IS_A_RESERVATION,
                           &W[i].tid);
    if (!W[i].th) {
      why = 100;
      break;
    }
    made++;
  }
  DWORD gle = GetLastError();
  ck(made == NT, "T1 create", "threads", (unsigned long long)made, "why", (unsigned long long)(why ? why * 100000ull + gle : 0));
  if (made != NT) {
    SetEvent(g_stop);
    struct row r = {{0}, 0};
    r_s(&r, "[TS] done rc=");
    r_u(&r, (unsigned long long)fails);
    r_end(&r);
    ExitProcess((UINT)fails);
  }
  for (int t = 0; t < 600 && g_started < NT; t++) Sleep(100);
  unsigned nth = count_my_threads();
  ck(g_started == NT && nth >= NT + 1, "T1 start", "started", (unsigned long long)g_started, "toolhelp",
     nth);
  {
    struct row r = {{0}, 0};
    r_s(&r, "[TS] info startup ms=");
    r_u(&r, GetTickCount64() - t0);
    r_end(&r);
  }

  /* T2: the storm */
  ULONGLONG ts = GetTickCount64(), next_row = ts + 10000, end = ts + (ULONGLONG)secs * 1000;
  long rounds = 0;
  int broken = 0;
  ULONGLONG worst = 0;
  while (!broken && GetTickCount64() < end) {
    ULONGLONG r0 = GetTickCount64();
    unsigned char tok = (unsigned char)(rounds * 37 + 11);
    for (int i = 0; i < NT && !broken; i++) {
      unsigned char b = (unsigned char)(tok + i);
      DWORD n = 0;
      if (!WriteFile(W[i].m_out, &b, 1, &n, NULL) || n != 1) {
        ck(0, "T2 request", "worker", (unsigned long long)i, "gle", GetLastError());
        broken = 1;
        break;
      }
      if (!SetEvent(W[i].ev)) {
        ck(0, "T2 SetEvent", "worker", (unsigned long long)i, "gle", GetLastError());
        broken = 1;
      }
    }
    for (int i = 0; i < NT && !broken; i++) {
      unsigned char b = 0;
      DWORD n = 0;
      if (!ReadFile(W[i].m_in, &b, 1, &n, NULL) || n != 1) {
        ck(0, "T2 reply", "worker", (unsigned long long)i, "gle", GetLastError());
        broken = 1;
        break;
      }
      if (b != (unsigned char)((unsigned char)(tok + i) ^ 0x5a)) {
        ck(0, "T2 reply value", "worker", (unsigned long long)i, "got", b);
        broken = 1;
      }
    }
    if (broken) break;
    rounds++;
    ULONGLONG dt = GetTickCount64() - r0;
    if (dt > worst) worst = dt;
    ULONGLONG now = GetTickCount64();
    if (now >= next_row) {
      struct row r = {{0}, 0};
      r_s(&r, "[TS] t=");
      r_u(&r, (now - ts) / 1000);
      r_s(&r, "s rounds=");
      r_u(&r, (unsigned long long)rounds);
      r_s(&r, " trips=");
      r_u(&r, (unsigned long long)rounds * NT);
      r_s(&r, " worst_round_ms=");
      r_u(&r, worst);
      r_s(&r, " threads=");
      r_u(&r, count_my_threads());
      r_end(&r);
      next_row += 10000;
    }
  }
  ULONGLONG ran = GetTickCount64() - ts;
  ck(!broken && ran >= (ULONGLONG)secs * 1000 && rounds > 0, "T2 storm", "rounds", (unsigned long long)rounds,
     "ms", ran);

  /* T3: stop and account */
  SetEvent(g_stop);
  int bad_exit = 0, first_bad = -1;
  for (int base = 0; base < NT; base += MAXIMUM_WAIT_OBJECTS) {
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    int n = 0;
    for (int i = base; i < NT && n < MAXIMUM_WAIT_OBJECTS; i++) hs[n++] = W[i].th;
    DWORD r = WaitForMultipleObjects((DWORD)n, hs, TRUE, 60000);
    if (r == WAIT_TIMEOUT || r == WAIT_FAILED) {
      bad_exit++;
      if (first_bad < 0) first_bad = base;
    }
  }
  for (int i = 0; i < NT; i++) {
    DWORD code = 0;
    if (!GetExitCodeThread(W[i].th, &code) || code != (DWORD)rounds || W[i].err) {
      if (first_bad < 0) first_bad = i;
      bad_exit++;
    }
  }
  ck(bad_exit == 0, "T3 stop", "bad", (unsigned long long)bad_exit, "first",
     (unsigned long long)(first_bad < 0 ? 0 : first_bad));
  if (first_bad >= 0 && first_bad < NT) {
    DWORD code = 0;
    GetExitCodeThread(W[first_bad].th, &code);
    struct row r = {{0}, 0};
    r_s(&r, "[TS] info worker=");
    r_u(&r, (unsigned long long)first_bad);
    r_s(&r, " exit=");
    r_u(&r, code);
    r_s(&r, " rounds=");
    r_u(&r, (unsigned long long)W[first_bad].rounds);
    r_s(&r, " err=");
    r_u(&r, (unsigned long long)W[first_bad].err);
    r_end(&r);
  }
  struct row r = {{0}, 0};
  r_s(&r, "[TS] done rc=");
  r_u(&r, (unsigned long long)fails);
  r_end(&r);
  ExitProcess((UINT)fails);
}
