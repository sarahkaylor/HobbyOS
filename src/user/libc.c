#include "libc.h"
#include <stdint.h>

#define SYS_WRITE_CONSOLE (1)
#define SYS_EXIT          (2)
#define SYS_FORK          (3)
#define SYS_OPEN          (4)
#define SYS_CLOSE         (5)
#define SYS_READ          (6)
#define SYS_WRITE         (7)
#define SYS_SPAWN         (8)
#define SYS_MAP_FB        (9)
#define SYS_FLUSH_FB      (10)
#define SYS_GET_CPUID     (11)
#define SYS_PIPE          (12)

static long syscall(long num, long a0, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = num;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile (
        "svc #0\n"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
        : "memory"
    );
    return x0;
}

void print(const char *str) { syscall(SYS_WRITE_CONSOLE, (long)str, 0, 0, 0); }

void print_hex(long val) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nibble = (val >> (60 - i * 4)) & 0xF;
        buf[i + 2] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
    }
    buf[18] = '\0';
    print(buf);
}


void exit(void) {
  syscall(SYS_EXIT, 0, 0, 0, 0);
  while (1)
    ; // Wait for the kernel to halt us safely
}

int fork(void) {
  return (int)syscall(SYS_FORK, 0, 0, 0, 0);
}

int open(const char* filename) {
  return (int)syscall(SYS_OPEN, (long)filename, 0, 0, 0);
}

int close(int fd) {
  return (int)syscall(SYS_CLOSE, (long)fd, 0, 0, 0);
}

int read(int fd, void* buf, int size) {
  return (int)syscall(SYS_READ, (long)fd, (long)buf, (long)size, 0);
}

int write(int fd, const void* buf, int size) {
  return (int)syscall(SYS_WRITE, (long)fd, (long)buf, (long)size, 0);
}

int spawn(const char* filename) {
  return (int)syscall(SYS_SPAWN, (long)filename, 0, 0, 0);
}

int pipe(int fds[2]) {
  long res = syscall(SYS_PIPE, (long)fds, 0, 0, 0);
  if (res == 0) return 0;
  return -1;
}

void* map_fb(void) {
  return (void*)syscall(SYS_MAP_FB, 0, 0, 0, 0);
}

void flush_fb(void) {
  syscall(SYS_FLUSH_FB, 0, 0, 0, 0);
}

int get_cpuid(void) {
  return (int)syscall(SYS_GET_CPUID, 0, 0, 0, 0);
}
