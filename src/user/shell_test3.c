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

/* Read until `pattern` (e.g. the shell prompt "$ ") shows up, or the
   stream ends.  The stream can exceed the caller's window: `ls -l /`
   over a full test wave on disk is ~3.8 KB.  When the buffer fills,
   drop the oldest half and keep scanning — the newest lines (where
   names a test just created appear, in FAT append order) and the
   trailing prompt survive in the window. */
static int read_until(int fd, char *buf, int max_len, const char *pattern) {
  int len = 0;
  int pat_len = 0;
  while (pattern[pat_len]) pat_len++;
  for (;;) {
    char c;
    int r = read(fd, &c, 1);
    if (r <= 0) {
      print_console("[read_until] read returned <= 0\n");
      break;
    }
    if (len >= max_len - 1) {
      int keep = (max_len - 1) / 2;
      for (int i = 0; i < keep; i++)
        buf[i] = buf[len - keep + i];
      len = keep;
    }
    buf[len++] = c;
    buf[len] = '\0';

    if (len >= pat_len) {
      int match = 1;
      for (int i = 0; i < pat_len; i++) {
        if (buf[len - pat_len + i] != pattern[i]) {
          match = 0;
          break;
        }
      }
      if (match) {
        print_console("[read_until] MATCHED: '");
        print_console(buf);
        print_console("'\n");
        return len;
      }
    }
  }
  print_console("[read_until] FINISHED WITHOUT MATCH: '");
  print_console(buf);
  print_console("'\n");
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
  print_console("Shell Folders & Subdirectories Integration Test Starting...\n");

  int in_p[2], out_p[2];
  if (pipe(in_p) != 0 || pipe(out_p) != 0) {
    print_console("shell_test3: failed to create pipes\n");
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
        print_console("shell_test3: spawn failed (memory wave), retrying...\n");
      usleep(100000); /* 100 ms */
    }
  }
  if (pid < 0) {
    print_console("shell_test3: FATAL: failed to spawn SH.BIN\n");
    return 1;
  }
  close(in_p[0]);
  close(out_p[1]);

  start_watchdog(in_p[1], out_p[0], "SHTEST3.BIN");

  char buf[2048];

  // Read greeting
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  print_console("[TEST3] Initial prompt read successfully.\n");

  // 1. Create a directory '/SUB1'
  print_console("[TEST3] Creating directory /SUB1...\n");
  write(in_p[1], "mkdir /SUB1\n", 12);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  // 2. Verify with ls -l /
  print_console("[TEST3] Verifying /SUB1 creation via ls -l...\n");
  write(in_p[1], "ls -l /\n", 8);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "d 0 SUB1")) {
    print_console("shell_test3: FAILED ls -l / validation. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }

  // 3. Change directory to /SUB1
  print_console("[TEST3] Changing directory to /SUB1...\n");
  write(in_p[1], "cd /SUB1\n", 9);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  // 4. Create relative directory SUB2
  print_console("[TEST3] Creating relative directory SUB2...\n");
  write(in_p[1], "mkdir SUB2\n", 11);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  // 5. Verify SUB2 lists in /SUB1
  print_console("[TEST3] Verifying SUB2 inside /SUB1...\n");
  write(in_p[1], "ls -l\n", 6);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "d 0 SUB2")) {
    print_console("shell_test3: FAILED ls -l /SUB1 validation. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }

  // 6. Change directory into SUB2
  print_console("[TEST3] Entering SUB2...\n");
  write(in_p[1], "cd SUB2\n", 8);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  // 7. Write nested file nested.txt
  print_console("[TEST3] Writing nested file...\n");
  write(in_p[1], "echo nested_content_val > nested.txt\n", 37);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  // 8. Cat nested.txt
  print_console("[TEST3] Reading nested file...\n");
  write(in_p[1], "cat nested.txt\n", 15);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "nested_content_val")) {
    print_console("shell_test3: FAILED cat nested.txt. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }

  // 9. Verify ls -l shows nested.txt attributes
  print_console("[TEST3] Verifying file attributes of nested.txt...\n");
  write(in_p[1], "ls -l\n", 6);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "- 19 NESTED.TXT")) {
    print_console("shell_test3: FAILED attributes of nested.txt. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }

  // 10. Go back to root
  print_console("[TEST3] Returning to root directory...\n");
  write(in_p[1], "cd /\n", 5);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  // 11. Cat from root using absolute path
  print_console("[TEST3] Reading absolute path nested file from root...\n");
  write(in_p[1], "cat /SUB1/SUB2/nested.txt\n", 26);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (!my_strstr(buf, "nested_content_val")) {
    print_console("shell_test3: FAILED cat absolute path. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }

  // 12. Delete nested file
  print_console("[TEST3] Deleting nested file...\n");
  write(in_p[1], "rm /SUB1/SUB2/nested.txt\n", 25);
  read_until(out_p[0], buf, sizeof(buf), "$ ");

  // 13. Verify file deleted
  print_console("[TEST3] Verifying deletion...\n");
  write(in_p[1], "ls -l /SUB1/SUB2\n", 17);
  read_until(out_p[0], buf, sizeof(buf), "$ ");
  if (my_strstr(buf, "NESTED.TXT")) {
    print_console("shell_test3: FAILED file deletion check. Output was:\n");
    print_console(buf);
    print_console("\n");
    return 1;
  }

  print_console("[TEST3] Closing shell input pipe...\n");
  close(in_p[1]);

  while (kill(pid, 0) == 0) {
    yield();
  }

  print_console("\n==================================\n");
  print_console("  SUBDIRECTORY INTEGRATION PASSED  \n");
  print_console("==================================\n");
  return 0;
}
