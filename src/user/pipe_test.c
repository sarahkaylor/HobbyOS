#include "libc.h"

__attribute__((section(".text._start"))) void _start() {
    int local_fds[2];
    print("Pipe Test Starting...\n");
    pipe(local_fds);
    print("After pipe call\n");
    
    int pid = fork();
    print("Fork returned: ");
    if (pid == 0) print("0\n");
    else if (pid == 1) print("1\n");
    else if (pid == 2) print("2\n");
    else if (pid == -1) print("-1\n");
    else {
        print("UNKNOWN_VALUE: ");
        print_hex((long)pid);
        print("\n");
        // Print the pid manually
        if (pid == 0) print("WTF ZERO\n"); // Just in case
    }

    if (pid == 0) {
        // Child
        print("Child: closing write end\n");
        close(local_fds[1]);
        char buf[64];
        print("Child: reading...\n");
        int n = read(local_fds[0], buf, 63);
        print("Child: read returned\n");
        if (n > 0) {
            buf[n] = '\0';
            print("Child got data!\n");
            print(buf);
            print("\n");
        } else {
            print("Child got no data or error\n");
        }
        close(local_fds[0]);
        print("Child exiting\n");
        exit();
    } else {
        // Parent
        print("Parent: closing read end\n");
        close(local_fds[0]);
        const char *msg = "SUCCESS";
        print("Parent: writing...\n");
        write(local_fds[1], msg, 7);
        print("Parent: closing write end\n");
        close(local_fds[1]);
        print("Parent exiting\n");
        exit();
    }
}
