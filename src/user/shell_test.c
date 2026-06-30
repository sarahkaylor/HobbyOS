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

int main(void) {
    print_console("Shell Integration Test Starting...\n");
    
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0 || pipe(out_p) != 0) {
        print_console("shell_test: failed to create pipes\n");
        return 1;
    }
    
    int pid = spawn2("SH.BIN", in_p[0], out_p[1]);
    if (pid < 0) {
        print_console("shell_test: failed to spawn SH.BIN\n");
        return 1;
    }
    close(in_p[0]);
    close(out_p[1]);
    
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
