/* src/user/cp/winprobe.c - Gate P2 Windows console probe (lane C2-wine2,
 * docs/cyberpunk.md §6 P2).
 *
 * A Windows x86_64 console program, built with clang -target
 * x86_64-windows against Wine 11.0's own headers and PE import libraries
 * (tools/cp/build-winprobe.sh; no MinGW).  Under Wine it exercises the pieces
 * of the Windows ABI that the game's process depends on before any graphics:
 *
 *   W1 TEB       %gs:[0x30] is the TEB: Self, ClientId, PEB, and the same
 *                answer as NtCurrentTeb(); a second thread has its own TEB
 *   W2 threads   CreateThread x4, InterlockedIncrement, exit codes,
 *                WaitForMultipleObjects
 *   W3 TLS       TlsAlloc/TlsSetValue/TlsGetValue isolation across threads,
 *                and the TEB's TlsSlots[] agree with TlsGetValue
 *   W4 SEH       __try/__except on an access violation and an integer divide
 *                by zero; a vectored handler that fixes up both and resumes
 *   W5 QPC       QueryPerformanceFrequency/Counter monotonic, Sleep advances it
 *   W6 KUSER     GetTickCount64 advances across Sleep; KUSER_SHARED_DATA at
 *                0x7ffe0000 is readable and the wineserver keeps updating it
 *                (TickCount and InterruptTime move: the coherent MAP_PRIVATE
 *                read-through of a server-written page)
 *   W7 C:\ I/O   create/write/flush/close, reopen/read/compare, size, a
 *                directory listing that finds it, delete
 *
 * Output is one "[WP] PASS|FAIL <name> (<value>)" row per check and a final
 * "[WP] done rc=<failures>"; the exit code is the failure count, so the gate
 * needs both "rc=0" and exit status 0.
 */
#include <windows.h>
#include <winternl.h>

static int fails;

static void put(const char *s) {
  DWORD n = 0;
  int len = 0;
  while (s[len]) len++;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)len, &n, NULL);
}

/* Minimal formatting (no CRT): decimal / hex of a 64-bit value. */
static void fmt_u64(char *b, unsigned long long v, int hex) {
  char t[24];
  int n = 0;
  do {
    unsigned d = (unsigned)(hex ? (v & 15) : (v % 10));
    t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v = hex ? (v >> 4) : (v / 10);
  } while (v && n < 23);
  int k = 0;
  if (hex) { b[k++] = '0'; b[k++] = 'x'; }
  while (n) b[k++] = t[--n];
  b[k] = 0;
}

static void ck(int ok, const char *what, unsigned long long v) {
  char num[32];
  fmt_u64(num, v, v > 0xffff);
  put("[WP] ");
  put(ok ? "PASS " : "FAIL ");
  put(what);
  put(" (");
  put(num);
  put(")\r\n");
  if (!ok) fails++;
}

static void *teb_gs(void) {
  void *t;
  __asm__ volatile("movq %%gs:0x30, %0" : "=r"(t));
  return t;
}

/* ---- W1/W2/W3: TEB, threads, TLS ------------------------------------ */

static DWORD tls_idx;
static volatile LONG counter;

struct tinfo {
  int id;
  void *teb;          /* %gs:[0x30] seen by the thread */
  DWORD tid;          /* GetCurrentThreadId() */
  DWORD teb_tid;      /* TEB->ClientId.UniqueThread */
  int tls_ok;
};

static DWORD WINAPI worker(LPVOID arg) {
  struct tinfo *ti = (struct tinfo *)arg;
  unsigned char *t = (unsigned char *)teb_gs();
  ti->teb = t;
  ti->tid = GetCurrentThreadId();
  ti->teb_tid = (DWORD)(ULONG_PTR)*(void **)(t + 0x48); /* ClientId.UniqueThread */
  /* TLS: a fresh thread starts with NULL, its own value never leaks. */
  int ok = TlsGetValue(tls_idx) == NULL;
  TlsSetValue(tls_idx, (LPVOID)(ULONG_PTR)(0x1000 + ti->id));
  for (int i = 0; i < 1000; i++) {
    InterlockedIncrement(&counter);
    if (TlsGetValue(tls_idx) != (LPVOID)(ULONG_PTR)(0x1000 + ti->id)) ok = 0;
  }
  /* TEB TlsSlots[] (offset 0x1480) mirror TlsGetValue for index < 64 */
  if (tls_idx < 64 && *(void **)(t + 0x1480 + 8 * tls_idx) != TlsGetValue(tls_idx)) ok = 0;
  ti->tls_ok = ok;
  return 100 + (DWORD)ti->id;
}

static void w1_w3(void) {
  unsigned char *t = (unsigned char *)teb_gs();
  ck(t != NULL, "W1 %gs:[0x30] non-null", (ULONG_PTR)t);
  ck((void *)t == (void *)NtCurrentTeb(), "W1 %gs:[0x30] == NtCurrentTeb()", (ULONG_PTR)t);
  ck(*(void **)(t + 0x30) == (void *)t, "W1 TEB->Self == TEB", (ULONG_PTR)*(void **)(t + 0x30));
  ck((DWORD)(ULONG_PTR)*(void **)(t + 0x40) == GetCurrentProcessId(), "W1 TEB ClientId.UniqueProcess",
     (ULONG_PTR)*(void **)(t + 0x40));
  ck((DWORD)(ULONG_PTR)*(void **)(t + 0x48) == GetCurrentThreadId(), "W1 TEB ClientId.UniqueThread",
     (ULONG_PTR)*(void **)(t + 0x48));
  ck(*(void **)(t + 0x60) != NULL, "W1 TEB->ProcessEnvironmentBlock", (ULONG_PTR)*(void **)(t + 0x60));
  /* NT_TIB stack bounds: this frame lies inside [StackLimit, StackBase) */
  {
    volatile int here = 0;
    ULONG_PTR base = (ULONG_PTR)*(void **)(t + 0x08), lim = (ULONG_PTR)*(void **)(t + 0x10);
    ULONG_PTR sp = (ULONG_PTR)&here;
    ck(lim < sp && sp < base, "W1 TIB stack bounds contain the stack", sp);
  }

  tls_idx = TlsAlloc();
  ck(tls_idx != TLS_OUT_OF_INDEXES, "W3 TlsAlloc", tls_idx);
  TlsSetValue(tls_idx, (LPVOID)0x5151);

  struct tinfo ti[4];
  HANDLE h[4];
  int created = 0;
  for (int i = 0; i < 4; i++) {
    ti[i].id = i;
    ti[i].teb = 0;
    ti[i].tls_ok = 0;
    h[i] = CreateThread(NULL, 0, worker, &ti[i], 0, NULL);
    if (h[i]) created++;
  }
  ck(created == 4, "W2 CreateThread x4", created);
  if (created != 4) return;
  DWORD w = WaitForMultipleObjects(4, h, TRUE, 30000);
  ck(w <= WAIT_OBJECT_0 + 3, "W2 WaitForMultipleObjects(all)", w);
  int codes = 1, tebs = 1, tls = 1;
  for (int i = 0; i < 4; i++) {
    DWORD code = 0;
    if (!GetExitCodeThread(h[i], &code) || code != 100u + (DWORD)i) codes = 0;
    if (!ti[i].teb || ti[i].teb == (void *)t || ti[i].teb_tid != ti[i].tid) tebs = 0;
    for (int j = 0; j < i; j++) if (ti[j].teb == ti[i].teb) tebs = 0;
    if (!ti[i].tls_ok) tls = 0;
    CloseHandle(h[i]);
  }
  ck(codes, "W2 thread exit codes", codes);
  ck(counter == 4000, "W2 InterlockedIncrement 4x1000", (ULONG_PTR)counter);
  ck(tebs, "W1 each thread has its own TEB via %gs", tebs);
  ck(tls, "W3 TLS isolated per thread (+ TEB TlsSlots)", tls);
  ck(TlsGetValue(tls_idx) == (LPVOID)0x5151, "W3 main thread TLS value kept",
     (ULONG_PTR)TlsGetValue(tls_idx));
  ck(TlsFree(tls_idx), "W3 TlsFree", 0);
}

/* ---- W4: SEH -------------------------------------------------------- */

static volatile int *volatile null_ptr;
static volatile int zero_div;

static int seh_av(void) {
  int got = 0;
  __try {
    got = *null_ptr;
    got = 1;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
    got = 2;
  }
  return got;
}

static int seh_div(void) {
  int got = 0;
  __try {
    volatile int a = 7;
    got = a / zero_div;
    got = 1;
  } __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
    got = 2;
  }
  return got;
}

static volatile LONG veh_av, veh_div;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
  DWORD c = ep->ExceptionRecord->ExceptionCode;
  unsigned char *ip = (unsigned char *)ep->ContextRecord->Rip;
  /* The faulting instructions below are fixed 2-byte encodings:
     8b 00 = mov (%rax),%eax   f7 f1 = div %ecx */
  if (c == EXCEPTION_ACCESS_VIOLATION && ip[0] == 0x8b && ip[1] == 0x00) {
    veh_av++;
    ep->ContextRecord->Rax = 0x4156; /* "AV" */
    ep->ContextRecord->Rip += 2;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (c == EXCEPTION_INT_DIVIDE_BY_ZERO && ip[0] == 0xf7 && ip[1] == 0xf1) {
    veh_div++;
    ep->ContextRecord->Rax = 0x4456; /* "DV" */
    ep->ContextRecord->Rip += 2;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static void w4(void) {
  ck(seh_av() == 2, "W4 __try/__except catches an access violation", 0);
  ck(seh_div() == 2, "W4 __try/__except catches an integer divide by zero", 0);
  void *hv = AddVectoredExceptionHandler(1, veh);
  ck(hv != NULL, "W4 AddVectoredExceptionHandler", (ULONG_PTR)hv);
  unsigned long long r1 = 0, r2 = 0;
  __asm__ volatile("xorl %%eax, %%eax\n\t"
                   ".byte 0x8b, 0x00\n\t" /* mov (%rax),%eax with rax = 0 */
                   : "=a"(r1) : : "memory");
  ck(veh_av == 1 && r1 == 0x4156, "W4 vectored handler resumes after AV", r1);
  __asm__ volatile("movl $9, %%eax\n\t"
                   "xorl %%edx, %%edx\n\t"
                   "xorl %%ecx, %%ecx\n\t"
                   ".byte 0xf7, 0xf1\n\t" /* div %ecx with ecx = 0 */
                   : "=a"(r2) : : "rcx", "rdx", "memory");
  ck(veh_div == 1 && r2 == 0x4456, "W4 vectored handler resumes after #DE", r2);
  ck(RemoveVectoredExceptionHandler(hv) != 0, "W4 RemoveVectoredExceptionHandler", 0);
  /* with the VEH gone, SEH still works */
  ck(seh_av() == 2, "W4 __except again after VEH removal", 0);
}

/* ---- W5/W6: QPC, GetTickCount64, KUSER_SHARED_DATA ------------------- */

#define KUSER ((volatile unsigned char *)0x7ffe0000)

static unsigned long long kuser_tick(void) {
  /* KSYSTEM_TIME TickCount at 0x320: LowPart, High1Time, High2Time */
  for (;;) {
    LONG hi1 = *(volatile LONG *)(KUSER + 0x324);
    ULONG lo = *(volatile ULONG *)(KUSER + 0x320);
    LONG hi2 = *(volatile LONG *)(KUSER + 0x328);
    if (hi1 == hi2) return ((unsigned long long)(ULONG)hi1 << 32) | lo;
  }
}
static unsigned long long kuser_itime(void) {
  /* KSYSTEM_TIME InterruptTime at 0x008 (100 ns units) */
  for (;;) {
    LONG hi1 = *(volatile LONG *)(KUSER + 0x00c);
    ULONG lo = *(volatile ULONG *)(KUSER + 0x008);
    LONG hi2 = *(volatile LONG *)(KUSER + 0x010);
    if (hi1 == hi2) return ((unsigned long long)(ULONG)hi1 << 32) | lo;
  }
}

static void w5_w6(void) {
  LARGE_INTEGER f, a, b;
  f.QuadPart = 0;
  BOOL qf = QueryPerformanceFrequency(&f);
  ck(qf && f.QuadPart > 0, "W5 QueryPerformanceFrequency", f.QuadPart);
  QueryPerformanceCounter(&a);
  int mono = 1;
  LARGE_INTEGER prev = a, cur;
  for (int i = 0; i < 10000; i++) {
    QueryPerformanceCounter(&cur);
    if (cur.QuadPart < prev.QuadPart) mono = 0;
    prev = cur;
  }
  ck(mono, "W5 QueryPerformanceCounter monotonic x10000", mono);
  ULONGLONG t0 = GetTickCount64();
  unsigned long long k0 = kuser_tick(), i0 = kuser_itime();
  Sleep(200);
  QueryPerformanceCounter(&b);
  ULONGLONG t1 = GetTickCount64();
  unsigned long long k1 = kuser_tick(), i1 = kuser_itime();
  unsigned long long qms = (unsigned long long)(b.QuadPart - a.QuadPart) * 1000ull /
                           (unsigned long long)(f.QuadPart ? f.QuadPart : 1);
  ck(qms >= 150 && qms < 5000, "W5 QPC advances ~200 ms across Sleep(200)", qms);
  ck(t1 > t0 && t1 - t0 >= 100 && t1 - t0 < 5000, "W6 GetTickCount64 advances across Sleep(200)", t1 - t0);
  ULONG mult = *(volatile ULONG *)(KUSER + 0x004);
  ck(mult != 0, "W6 KUSER TickCountMultiplier readable at 0x7ffe0004", mult);
  ck(*(volatile USHORT *)(KUSER + 0x26c) >= 6, "W6 KUSER NtMajorVersion", *(volatile USHORT *)(KUSER + 0x26c));
  ck(k1 > k0, "W6 KUSER TickCount moves (server-written page seen live)", k1 - k0);
  ck(i1 > i0, "W6 KUSER InterruptTime moves", i1 - i0);
  /* GetTickCount64 is computed from that same page */
  unsigned long long kms = (kuser_tick() * (unsigned long long)mult) >> 24;
  ULONGLONG g = GetTickCount64();
  ck((g > kms ? g - kms : kms - g) < 1000, "W6 GetTickCount64 agrees with KUSER TickCount", g);
}

/* ---- W7: file I/O in C:\ -------------------------------------------- */

static void w7(void) {
  const char *path = "C:\\winprobe.tmp";
  static const char msg[] = "winprobe: C drive round trip 0123456789";
  DeleteFileA(path);
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  ck(h != INVALID_HANDLE_VALUE, "W7 CreateFile C:\\winprobe.tmp", GetLastError());
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD n = 0;
  BOOL wok = WriteFile(h, msg, sizeof msg - 1, &n, NULL);
  ck(wok && n == sizeof msg - 1, "W7 WriteFile", n);
  ck(FlushFileBuffers(h), "W7 FlushFileBuffers", GetLastError());
  CloseHandle(h);
  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  ck(h != INVALID_HANDLE_VALUE, "W7 reopen for read", GetLastError());
  if (h == INVALID_HANDLE_VALUE) return;
  ck(GetFileSize(h, NULL) == sizeof msg - 1, "W7 GetFileSize", GetFileSize(h, NULL));
  char buf[64];
  n = 0;
  BOOL rok = ReadFile(h, buf, sizeof buf, &n, NULL);
  int same = rok && n == sizeof msg - 1;
  for (DWORD i = 0; same && i < n; i++) if (buf[i] != msg[i]) same = 0;
  ck(same, "W7 ReadFile returns the bytes written", n);
  CloseHandle(h);
  WIN32_FIND_DATAA fd;
  HANDLE fh = FindFirstFileA("C:\\*", &fd);
  int found = 0;
  if (fh != INVALID_HANDLE_VALUE) {
    do {
      const char *a = fd.cFileName, *b = "winprobe.tmp";
      int eq = 1;
      for (; *a || *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (x != y) { eq = 0; break; }
      }
      if (eq) found = 1;
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
  }
  ck(found, "W7 FindFirstFile C:\\* lists it", found);
  ck(DeleteFileA(path), "W7 DeleteFile", GetLastError());
  ck(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES, "W7 gone after delete", GetLastError());
}

int main(void) {
  put("[WP] winprobe start\r\n");
  w1_w3();
  w4();
  w5_w6();
  w7();
  char num[24];
  fmt_u64(num, (unsigned long long)fails, 0);
  put("[WP] done rc=");
  put(num);
  put("\r\n");
  return fails;
}
