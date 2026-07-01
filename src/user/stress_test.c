#include "libc.h"

#define ITERATIONS 120

static void print_num(int n) {
    char buf[16];
    int idx = 0;
    if (n == 0) {
        print_console("0");
        return;
    }
    if (n < 0) {
        print_console("-");
        n = -n;
    }
    while (n > 0) {
        buf[idx++] = (char)('0' + (n % 10));
        n /= 10;
    }
    char out[16];
    for (int i = 0; i < idx; i++) {
        out[i] = buf[idx - 1 - i];
    }
    out[idx] = '\0';
    print_console(out);
}

static int mystrlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int mystrcmp(const char *s1, const char *s2) {
    int i = 0;
    while (s1[i] && s2[i]) {
        if (s1[i] != s2[i]) return s1[i] - s2[i];
        i++;
    }
    return s1[i] - s2[i];
}

static void run_worker(int id, int p2c[2], int c2p[2], const char *filename) {
    // Worker closes unused pipe ends
    close(p2c[1]); // Read-only from p2c
    close(c2p[0]); // Write-only to c2p

    char pattern[16];
    pattern[0] = 'W'; pattern[1] = 'O'; pattern[2] = 'R'; pattern[3] = 'K';
    pattern[4] = 'E'; pattern[5] = 'R'; pattern[6] = (char)('0' + id);
    pattern[7] = '_'; pattern[8] = 'D'; pattern[9] = 'A'; pattern[10] = 'T';
    pattern[11] = 'A'; pattern[12] = '\n'; pattern[13] = '\0';
    int pat_len = mystrlen(pattern);

    while (1) {
        // 1. Read message from parent
        char msg[8];
        int bytes_read = read(p2c[0], msg, 4);
        if (bytes_read != 4) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: failed read from parent.\n");
            exit(1);
        }
        msg[4] = '\0';

        if (mystrcmp(msg, "EXIT") == 0) {
            break; // Shutdown signal received
        }

        if (mystrcmp(msg, "PING") != 0) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: received invalid ping: ");
            print_console(msg); print_console("\n");
            exit(1);
        }

        // 2. Perform File I/O
        int fd = open(filename);
        if (fd < 0) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: failed to open "); print_console(filename); print_console("\n");
            exit(1);
        }

        int written = write(fd, pattern, pat_len);
        if (written != pat_len) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: failed write to "); print_console(filename); print_console("\n");
            exit(1);
        }

        close(fd);

        // Reopen for reading
        fd = open(filename);
        if (fd < 0) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: failed to reopen "); print_console(filename); print_console("\n");
            exit(1);
        }

        char read_buf[32];
        for (int k = 0; k < 32; k++) read_buf[k] = 0;
        int bytes_file_read = read(fd, read_buf, pat_len);
        if (bytes_file_read != pat_len) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: failed read from "); print_console(filename); print_console("\n");
            exit(1);
        }

        if (mystrcmp(read_buf, pattern) != 0) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: data mismatch on "); print_console(filename);
            print_console(". Expected: "); print_console(pattern); print_console(" Got: "); print_console(read_buf); print_console("\n");
            exit(1);
        }

        close(fd);

        // 3. Directory listing read operations to verify other files
        struct sys_dirent ent;
        int dir_res = read_dir("/", 0, &ent);
        if (dir_res != 0) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: failed to read root directory.\n");
            exit(1);
        }

        // 4. Send Pong back to parent
        int p_written = write(c2p[1], "PONG", 4);
        if (p_written != 4) {
            print_console("[WORKER "); print_num(id); print_console("] ERROR: failed to write PONG to parent.\n");
            exit(1);
        }

        // 5. Brief yield
        yield();
    }

    // Finished loop. Send SUCCESS to parent.
    int s_written = write(c2p[1], "SUCCESS", 7);
    if (s_written != 7) {
        print_console("[WORKER "); print_num(id); print_console("] ERROR: failed to write SUCCESS to parent.\n");
        exit(1);
    }

    close(p2c[0]);
    close(c2p[1]);
    exit(0);
}

__attribute__((section(".text._start")))
void _start(void) {
    print_console("\n==================================================\n"
                  "[STRESS TEST] Initiating multi-process stress test...\n"
                  "==================================================\n");

    int p2c[4][2];
    int c2p[4][2];
    int pids[4];
    const char* filenames[4] = { "TEST1.TXT", "TEST2.TXT", "TEST3.TXT", "TEST4.TXT" };

    for (int i = 0; i < 4; i++) {
        if (pipe(p2c[i]) < 0) {
            print_console("[STRESS TEST] ERROR: Failed to create p2c pipe ");
            print_num(i);
            print_console("\n");
            exit(1);
        }
        if (pipe(c2p[i]) < 0) {
            print_console("[STRESS TEST] ERROR: Failed to create c2p pipe ");
            print_num(i);
            print_console("\n");
            exit(1);
        }
    }

    print_console("[STRESS TEST] Pipes created. Forking 4 worker processes...\n");

    for (int i = 0; i < 4; i++) {
        int pid = fork();
        if (pid < 0) {
            print_console("[STRESS TEST] ERROR: Failed to fork child ");
            print_num(i);
            print_console("\n");
            exit(1);
        } else if (pid == 0) {
            // Child process i
            run_worker(i, p2c[i], c2p[i], filenames[i]);
            exit(0);
        } else {
            pids[i] = pid;
        }
    }

    // Parent process logic
    for (int i = 0; i < 4; i++) {
        close(p2c[i][0]); // Parent only writes to p2c
        close(c2p[i][1]); // Parent only reads from c2p
    }

    print_console("[STRESS TEST] Parent starting coordination ping-pong loop (120 iterations)...\n");

    for (int step = 0; step < ITERATIONS; step++) {
        if ((step + 1) % 10 == 0 || step == 0) {
            print_console("[STRESS TEST] Progress: Iteration ");
            print_num(step + 1);
            print_console("/");
            print_num(ITERATIONS);
            print_console("\n");
        }

        // Ping all 4 children
        for (int i = 0; i < 4; i++) {
            int written = write(p2c[i][1], "PING", 4);
            if (written != 4) {
                print_console("[STRESS TEST] ERROR: Parent failed write to child ");
                print_num(i);
                print_console("\n");
                exit(1);
            }
        }

        // Read pong from all 4 children
        for (int i = 0; i < 4; i++) {
            char buf[8];
            int bytes_read = read(c2p[i][0], buf, 4);
            if (bytes_read != 4) {
                print_console("[STRESS TEST] ERROR: Parent failed read from child ");
                print_num(i);
                print_console("\n");
                exit(1);
            }
            buf[4] = '\0';
            if (mystrcmp(buf, "PONG") != 0) {
                print_console("[STRESS TEST] ERROR: Parent received invalid pong from child ");
                print_num(i);
                print_console(": ");
                print_console(buf);
                print_console("\n");
                exit(1);
            }
        }

        // Periodically yield or sleep to allow dynamic interleaving
        if (step % 5 == 0) {
            sleep(5);
        } else {
            yield();
        }
    }

    print_console("[STRESS TEST] Ping-pong loop finished. Telling children to exit.\n");

    for (int i = 0; i < 4; i++) {
        int written = write(p2c[i][1], "EXIT", 4);
        if (written != 4) {
            print_console("[STRESS TEST] ERROR: Parent failed exit signal write to child ");
            print_num(i);
            print_console("\n");
            exit(1);
        }
    }

    // Wait for final success confirmation from each child over their pipes
    for (int i = 0; i < 4; i++) {
        char buf[8];
        int bytes_read = read(c2p[i][0], buf, 7);
        if (bytes_read != 7) {
            print_console("[STRESS TEST] ERROR: Parent failed to read final success from child ");
            print_num(i);
            print_console("\n");
            exit(1);
        }
        buf[7] = '\0';
        if (mystrcmp(buf, "SUCCESS") != 0) {
            print_console("[STRESS TEST] ERROR: Child ");
            print_num(i);
            print_console(" reported failure: ");
            print_console(buf);
            print_console("\n");
            exit(1);
        }
        close(p2c[i][1]);
        close(c2p[i][0]);
    }

    print_console("\n==================================================\n");
    print_console("[STRESS TEST] SUCCESS: ALL TESTS PASSED SUCCESSFULLY!\n");
    print_console("==================================================\n");
    exit(0);
}
