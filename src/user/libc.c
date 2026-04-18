#include "libc.h"

static inline long syscall1(long sys_num, long arg0) {
    register long x8 __asm__("x8") = sys_num;
    register long x0 __asm__("x0") = arg0;
    __asm__ volatile (
        "svc #0\n"
        : "+r"(x0)
        : "r"(x8)
        : "memory"
    );
    return x0;
}

static inline long syscall0(long sys_num) {
    register long x8 __asm__("x8") = sys_num;
    register long x0 __asm__("x0");
    __asm__ volatile (
        "svc #0\n"
        : "=r"(x0)
        : "r"(x8)
        : "memory"
    );
    return x0;
}

void print(const char* str) {
    syscall1(1, (long)str);
}

void exit(void) {
    syscall0(2);
    while(1); // Wait for the kernel to halt us safely
}
