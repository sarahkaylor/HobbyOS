#include "libc.h"

__attribute__((section(".text._start"))) void _start(void) {
  print("Spawn Test: Starting...\n");

  print("Spawn Test: Spawning CONSOLE.BIN...\n");
  int pid = spawn2("CONSOLE.BIN", -1, -1);

  if (pid < 0) {
    print("Spawn Test: FAILED to spawn CONSOLE.BIN\n");
  } else {
    print("Spawn Test: Successfully spawned CONSOLE.BIN with PID=");
    // Simple int-to-string conversion for PID since we don't have printf
    char pid_buf[16];
    int i = 0;
    int temp_pid = pid;
    if (temp_pid == 0)
      pid_buf[i++] = '0';
    else {
      while (temp_pid > 0 && i < 15) {
        pid_buf[i++] = (temp_pid % 10) + '0';
        temp_pid /= 10;
      }
    }
    pid_buf[i] = '\0';
    // Reverse pid_buf
    for (int j = 0; j < i / 2; j++) {
      char t = pid_buf[j];
      pid_buf[j] = pid_buf[i - 1 - j];
      pid_buf[i - 1 - j] = t;
    }
    print(pid_buf);
    print("\n");
  }

  print("Spawn Test: Finished. Exiting.\n");
  exit(0);
}
