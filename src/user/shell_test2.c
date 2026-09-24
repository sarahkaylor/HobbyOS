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

static int my_strstr(const char *haystack, const char *needle) {
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

static int read_until(int fd, char *buf, int max_len, const char *pattern) {
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
  print_console("Shell Advanced Utilities Integration Test Starting...\n");

  int in_p[2], out_p[2];
  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("shell_test2: failed to create pipes\n");
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
        print_console("shell_test2: spawn failed (memory wave), retrying...\n");
      usleep(100000); /* 100 ms */
    }
  }
  if (pid < 0) {
    print_console("shell_test2: FATAL: failed to spawn SH.BIN\n");
    return 1;
  }
  close(in_p[0]);
  close(out_p[1]);

  start_watchdog(in_p[1], out_p[0], "SHTEST2.BIN");

  char buf[2048];

  // Read greeting
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  print_console("[TEST2] Initial prompt read successfully.\n");

  // 1. Test ps
  print_console("[TEST2] Testing 'ps'...\n");
  write(in_p[1], "ps\n", 3);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "SH.BIN")) {
    print_console("shell_test2: FAILED ps validation. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }
  print_console("[TEST2] 'ps' validated successfully.\n");

  // 2. Test free
  print_console("[TEST2] Testing 'free'...\n");
  write(in_p[1], "free\n", 5);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "Mem:")) {
    print_console("shell_test2: FAILED free validation\n");
    return 1;
  }
  print_console("[TEST2] 'free' validated successfully.\n");

  // 3. Test uptime
  print_console("[TEST2] Testing 'uptime'...\n");
  write(in_p[1], "uptime\n", 7);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "up")) {
    print_console("shell_test2: FAILED uptime validation\n");
    return 1;
  }
  print_console("[TEST2] 'uptime' validated successfully.\n");

  // 4. Test ifconfig
  print_console("[TEST2] Testing 'ifconfig'...\n");
  write(in_p[1], "ifconfig\n", 9);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "eth0:") || !my_strstr(buf, "inet")) {
    print_console("shell_test2: FAILED ifconfig validation\n");
    return 1;
  }
  print_console("[TEST2] 'ifconfig' validated successfully.\n");

  // 5. Test touch / mv / cp / rm / cat
  print_console("[TEST2] Testing touch, mv, cp, rm, cat...\n");
  write(in_p[1], "echo file_content > TEMP.TXT\n", 29);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  write(in_p[1], "mv TEMP.TXT TEMP2.TXT\n", 22);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  write(in_p[1], "cp TEMP2.TXT TEMP3.TXT\n", 23);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  write(in_p[1], "rm TEMP2.TXT\n", 13);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  write(in_p[1], "cat TEMP3.TXT\n", 14);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "file_content")) {
    print_console("shell_test2: FAILED file ops validation\n");
    return 1;
  }
  print_console("[TEST2] File operations validated successfully.\n");

  // 6. Test sort / uniq / wc
  print_console("[TEST2] Testing sort, uniq, wc...\n");

  write(in_p[1], "sort SORT.TXT\n", 14);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "apple") || !my_strstr(buf, "orange")) {
    print_console("shell_test2: FAILED sort validation\n");
    return 1;
  }

  write(in_p[1], "sort SORT.TXT | uniq\n", 21);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "apple")) {
    print_console("shell_test2: FAILED uniq validation\n");
    return 1;
  }

  write(in_p[1], "wc SORT.TXT\n", 12);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "3")) {
    print_console("shell_test2: FAILED wc validation\n");
    return 1;
  }
  print_console("[TEST2] Text processing utilities validated successfully.\n");

  // 7. Test ping loopback
  print_console("[TEST2] Testing ping...\n");
  write(in_p[1], "ping 10.0.2.15\n", 15);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "Reply") && !my_strstr(buf, "timed out")) {
    // Since it's self-ping, it might reply or timeout depending on ARP, but the binary must run!
    print_console("shell_test2: FAILED ping execution. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }
  print_console("[TEST2] 'ping' executed successfully.\n");

  // Cleanup
  write(in_p[1], "rm SORT.TXT TEMP3.TXT\n", 22);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  close(in_p[1]);
  while (kill(pid, 0) == 0) {
    yield();
  }

  print_console("\n==================================\n");
  print_console("  ADVANCED SHELL TEST PASSED      \n");
  print_console("==================================\n");

  return 0;
}
