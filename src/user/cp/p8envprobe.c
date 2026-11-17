/* src/user/cp/p8envprobe.c - lane C8-game (docs/cp-p8.md §3, stop 1): how
 * much environment a compat (Linux-ABI) process gets.  A static musl program.
 *
 *   p8envprobe              E0: print what the spawner handed us (envc,
 *                           bytes incl. NULs, the last entry's name), then
 *                           execve(argv[0], {argv[0], "child"}, env) with
 *                           40 entries of 38 chars (1560 bytes with the NULs)
 *   p8envprobe child        E1: count the E00..E39 entries that arrived
 *                           intact; E2: every entry is well-formed (has '=')
 *   p8envprobe spawned40    E3: the same count, for a NATIVE spawner
 *                           (p8launch's spawn_ex with the same 40 entries)
 *
 * Linux (ARG_MAX 2 MiB) passes all rows.  HobbyOS cyberpunk @ 949ba008:
 * E1/E2 (compat execve) pass; E3 (native spawn_ex -> compat) is bounded by
 * the 512-byte per-process env blob (HO_ENV_LEN, process.h:238) and the
 * 24-entry compat stack copy (COMPAT_MAX_ENV, program_loader.c:632), and the
 * excess is dropped silently.
 * Rows: "[P8ENV] E<n> ... PASS|FAIL".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static void census(const char *tag) {
  int n = 0;
  size_t bytes = 0;
  const char *last = "";
  for (char **e = environ; e && *e; e++) {
    n++;
    bytes += strlen(*e) + 1;
    last = *e;
  }
  char name[48];
  size_t k = 0;
  while (last[k] && last[k] != '=' && k < sizeof name - 1) { name[k] = last[k]; k++; }
  name[k] = 0;
  printf("[P8ENV] %s envc=%d bytes=%zu last=%s\n", tag, n, bytes, name);
}

int main(int argc, char **argv) {
  int sp = argc > 1 && strcmp(argv[1], "spawned40") == 0;
  if (argc > 1 && (strcmp(argv[1], "child") == 0 || sp)) {
    census(sp ? "E3 spawned40 sees" : "E1 child sees");
    int got = 0, bad = 0;
    char want[48];
    for (int i = 0; i < 40; i++) {
      snprintf(want, sizeof want, "E%02d=", i);
      for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, want, 4) == 0 && strlen(*e) == 38) { got++; break; }
      }
    }
    for (char **e = environ; e && *e; e++)
      if (!strchr(*e, '=')) bad++;
    printf("[P8ENV] %s env: %d/40 entries intact -> %s\n", sp ? "E3 spawn_ex" : "E1 execve", got,
           got == 40 ? "PASS" : "FAIL");
    printf("[P8ENV] %s malformed entries: %d -> %s\n", sp ? "E4" : "E2", bad, bad == 0 ? "PASS" : "FAIL");
    fflush(stdout);
    return got == 40 && bad == 0 ? 0 : 1;
  }
  census("E0 spawned with");
  static char store[40][48];
  static char *env[41];
  for (int i = 0; i < 40; i++) {
    /* "Exx=" + 34 payload bytes = 38 chars, 39 with the NUL */
    snprintf(store[i], sizeof store[i], "E%02d=%034d", i, i);
    env[i] = store[i];
  }
  env[40] = 0;
  char *cargv[] = { argv[0], (char *)"child", 0 };
  fflush(stdout);
  execve(argv[0], cargv, env);
  perror("[P8ENV] execve");
  printf("[P8ENV] E1 exec env: execve failed -> FAIL\n");
  return 2;
}
