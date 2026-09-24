#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((weak))
long syscall(long num, long a0, long a1, long a2, long a3);

__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

int my_strstr(const char *haystack, const char *needle) {
  if (!*needle) return 1;
  for (int i = 0; haystack[i]; i++) {
    int match = 1;
    for (int j = 0; needle[j]; j++) {
      if (haystack[i + j] != needle[j]) {
        match = 0;
        break;
      }
    }
    if (match) return 1;
  }
  return 0;
}

int read_until(int fd, char *buf, int max_len, const char *pattern) {
  int len = 0;
  while (len < max_len - 1) {
    char c;
    int r = read(fd, &c, 1);
    if (r <= 0) break;
    buf[len++] = c;
    buf[len] = '\0';

    int pat_len = 0;
    while (pattern[pat_len]) pat_len++;
    if (len >= pat_len) {
      int match = 1;
      for (int i = 0; i < pat_len; i++) {
        if (buf[len - pat_len + i] != pattern[i]) {
          match = 0;
          break;
        }
      }
      if (match) return len;
    }
  }
  return len;
}

/* Stall watchdog: fork a helper that fails this test loudly instead of
   letting a shell-protocol deadlock wedge the whole boot suite (seen
   under memory pressure when a shell's answer to a command never
   arrives).  The child drops its copies of the protocol pipes first:
   fork() copies the fd table, and the extra references would otherwise
   keep the pipe ends alive after this process dies.  On stall it dumps
   the process table, kills the stuck pair, and lets the suite move on. */
static void start_watchdog(int in_w, int out_r, const char *test_name) {
  int wd = fork();
  if (wd != 0)
    return;

  close(in_w);
  close(out_r);

  for (int i = 0; i < 250; i++) {
    usleep(100000); /* 100 ms; total patience 25 s */
    if (kill(getppid(), 0) != 0)
      exit(0); /* the test finished normally */
  }

  struct sys_procinfo procs[64];
  int n = sysinfo(3, procs, sizeof(procs));
  int pp = getppid();

  /* The parent pid may have been recycled to a stranger: stand down
     unless the pid is still our test. */
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (procs[i].pid == pp && my_strstr(procs[i].name, test_name))
      found = 1;
  }
  if (!found)
    exit(0);

  print_console("SHELLTEST WATCHDOG: protocol stalled 25 s; FAILING TEST. "
                "Process table:\n");
  for (int i = 0; i < n; i++) {
    print_console("  pid=");
    print_dec(procs[i].pid);
    print_console(" ppid=");
    print_dec(procs[i].parent_pid);
    print_console(" state=");
    print_dec(procs[i].state);
    print_console(" ");
    print_console(procs[i].name);
    print_console("\n");
  }

  /* Take down the stuck pair so the boot suite can complete: the shells
     this test spawned are children of the parent; then the parent. */
  for (int i = 0; i < n; i++) {
    if (procs[i].parent_pid == pp && procs[i].pid != getpid())
      kill(procs[i].pid, 9);
  }
  kill(pp, 9);
  exit(0);
}

int main(void) {
  print_console("Shell Integration Test Starting...\n");

  int in_p[2], out_p[2];
  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("shell_test: failed to create pipes\n");
    return 1;
  }

  int pid = -1;

  /* The boot test suite can transiently exhaust the kernel's physical
     block pool (32 blocks) while ~40 programs start in a burst; retry so a
     scheduling wave can't silently kill the test (bounded at 20 s). */
  for (int attempt = 0; attempt < 200 && pid < 0; attempt++) {
    pid = spawn2("SH.BIN", in_p[0], out_p[1], -1, 0);
    if (pid < 0) {
      if (attempt == 0)
        print_console("shell_test: spawn failed (memory wave), retrying...\n");
      usleep(100000); /* 100 ms */
    }
  }
  if (pid < 0) {
    print_console("shell_test: FATAL: failed to spawn SH.BIN\n");
    return 1;
  }
  close(in_p[0]);
  close(out_p[1]);

  start_watchdog(in_p[1], out_p[0], "SHTEST.BIN");

  char buf[1024];

  // 1. Read greeting and prompt
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  print_console("[TEST] Initial prompt read successfully.\n");

  // 2. Send 'help'
  print_console("[TEST] Sending 'help' command...\n");
  write(in_p[1], "help\n", 5);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "HobbyOS Bash-like Shell")) {
    print_console("shell_test: FAILED help validation\n");
    return 1;
  }
  print_console("[TEST] 'help' output validated successfully.\n");

  // 3. Send 'cd /home'
  print_console("[TEST] Sending 'cd /home' command...\n");
  write(in_p[1], "cd /home\n", 9);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "user@hobbyos:/home$")) {
    print_console("shell_test: FAILED cd prompt validation\n");
    return 1;
  }
  print_console("[TEST] 'cd' prompt update validated successfully.\n");

  // 4. Send 'cat SHTEST.TXT'
  print_console("[TEST] Sending 'cat SHTEST.TXT' command...\n");
  write(in_p[1], "cat SHTEST.TXT\n", 15);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "HobbyOS Terminal Test File")) {
    print_console("shell_test: FAILED cat validation. Buffer: ");
    print_console(buf);
    print_console("\n");
    return 1;
  }
  print_console("[TEST] 'cat' file output validated successfully.\n");

  // 5. Send 'cat SHTEST.TXT | grep line'
  print_console("[TEST] Sending piped 'cat SHTEST.TXT | grep line' command...\n");
  write(in_p[1], "cat SHTEST.TXT | grep line\n", 27);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "This is line number two.") || !my_strstr(buf, "Line five is the last line")) {
    print_console("shell_test: FAILED pipe validation. Buffer: ");
    print_console(buf);
    print_console("\n");
    return 1;
  }
  print_console("[TEST] Piped command output validated successfully.\n");

  // 5a. Send 'echo hello'
  print_console("[TEST] Sending 'echo hello' command...\n");
  write(in_p[1], "echo hello\n", 11);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "hello")) {
    print_console("shell_test: FAILED echo validation\n");
    return 1;
  }
  print_console("[TEST] 'echo' command validated successfully.\n");

  // 5b. Send 'clear'
  print_console("[TEST] Sending 'clear' command...\n");
  write(in_p[1], "clear\n", 6);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "\f")) {
    print_console("shell_test: FAILED clear validation\n");
    return 1;
  }
  print_console("[TEST] 'clear' command validated successfully.\n");

  // 5c. Send 'echo redirected > OUT.TXT'
  print_console("[TEST] Sending 'echo redirected > OUT.TXT' command...\n");
  write(in_p[1], "echo redirected > OUT.TXT\n", 26);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  print_console("[TEST] Redirection command sent.\n");

  // 5d. Send 'cat OUT.TXT'
  print_console("[TEST] Sending 'cat OUT.TXT' command...\n");
  write(in_p[1], "cat OUT.TXT\n", 12);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "redirected")) {
    print_console("shell_test: FAILED redirection content validation. Buffer: ");
    print_console(buf);
    print_console("\n");
    return 1;
  }
  print_console("[TEST] Redirection content validated successfully.\n");

  // 6. Close input pipe (EOF)
  print_console("[TEST] Closing input pipe (sending EOF)...\n");
  close(in_p[1]);

  // Wait for shell exit
  while (kill(pid, 0) == 0) {
    yield();
  }

  print_console("\n==================================\n");
  print_console("  SHELL INTEGRATION TEST PASSED   \n");
  print_console("==================================\n");

  return 0;
}
