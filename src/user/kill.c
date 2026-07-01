#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

static int atoi(const char *s) {
    int res = 0;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

int main(void) {
    char arg_buf[256];
    get_args(arg_buf, sizeof(arg_buf));
    char *argv[16];
    int argc = parse_args(arg_buf, argv, 16);
    if (argc < 1) {
        print("Usage: kill [-sig] pid\n");
        return 1;
    }
    int sig = 9;
    int pid_idx = 0;
    if (argv[0][0] == '-') {
        sig = atoi(argv[0] + 1);
        pid_idx = 1;
        if (argc < 2) {
            print("Usage: kill [-sig] pid\n");
            return 1;
        }
    }
    int pid = atoi(argv[pid_idx]);
    if (kill(pid, sig) < 0) {
        print("kill: failed to send signal\n");
        return 1;
    }
    return 0;
}
