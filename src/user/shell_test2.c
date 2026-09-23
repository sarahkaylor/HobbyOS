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

int main(void) {
    print_console("Shell Advanced Utilities Integration Test Starting...\n");
    
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0 || pipe(out_p) != 0) {
        print_console("shell_test2: failed to create pipes\n");
        return 1;
    }
    
    int pid = spawn2("SH.BIN", in_p[0], out_p[1], -1, 0);
    if (pid < 0) {
        print_console("shell_test2: failed to spawn SH.BIN\n");
        return 1;
    }
    close(in_p[0]);
    close(out_p[1]);
    
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
