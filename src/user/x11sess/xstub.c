/*
 * Stub X binaries for testing XSESSION.BIN before the real X stack exists
 * (icewm.md M5, lane session).  NEVER shipped by a normal DESKTOP=icewm
 * build: `make icewm-stubs` builds them into obj/<arch>/x11stub/ laid out
 * like third_party/x11/build/<arch>/ (bin/ + disk/), and run_icewm_test.py
 * points X11_BUILD_DIR there.
 *
 * Role (compile-time, -DXSTUB_ROLE=...):
 *   1 server  (XHOBBY.BIN)  logs argv/env, stays up XSTUB_SERVER_MS, then
 *                           exits 3 — simulates an X server crash, so the
 *                           session must fall back to DESKTOP.BIN.
 *   2 wm      (ICEWM.BIN)   logs, then idles (bounded) until killed.
 *   3 bg      (ICEWMBG.BIN) logs and exits 0 at once.
 * All lines are prefixed "[XSTUB]".
 */
#include <stdlib.h>
#include <unistd.h>

#include "libc.h"

#ifndef XSTUB_ROLE
#define XSTUB_ROLE 1
#endif
#ifndef XSTUB_SERVER_MS
#define XSTUB_SERVER_MS 4000
#endif

static void env_line(const char *k) {
  const char *v = getenv(k);
  print("[XSTUB]   ");
  print(k);
  print("=");
  print(v ? v : "(unset)");
  print("\n");
}

int main(int argc, char **argv) {
  print("[XSTUB] role=");
  print(XSTUB_ROLE == 1 ? "server" : XSTUB_ROLE == 2 ? "wm" : "bg");
  print(" argc=");
  print_dec(argc);
  print(" argv:");
  for (int i = 0; i < argc; i++) {
    print(" ");
    print(argv[i]);
  }
  print("\n");
  env_line("DISPLAY");
  env_line("HOME");
  env_line("FONTCONFIG_FILE");
  env_line("PATH");
#if XSTUB_ROLE == 1
  usleep(XSTUB_SERVER_MS * 1000);
  print("[XSTUB] server stub exiting with status 3 (simulated crash)\n");
  return 3;
#elif XSTUB_ROLE == 2
  for (int i = 0; i < 600; i++) usleep(1000 * 1000);
  print("[XSTUB] wm stub idle limit reached\n");
  return 0;
#else
  return 0;
#endif
}
