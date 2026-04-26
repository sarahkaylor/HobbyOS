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

static inline long syscall0(long sys_num) {
  register long x8 __asm__("x8") = sys_num;
  register long x0 __asm__("x0");
  __asm__ volatile("svc #0\n" : "=r"(x0) : "r"(x8) : "memory");
  return x0;
}

static inline long syscall1(long sys_num, long arg0) {
  register long x8 __asm__("x8") = sys_num;
  register long x0 __asm__("x0") = arg0;
  __asm__ volatile("svc #0\n" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

static inline long syscall3(long sys_num, long arg0, long arg1, long arg2) {
  register long x8 __asm__("x8") = sys_num;
  register long x0 __asm__("x0") = arg0;
  register long x1 __asm__("x1") = arg1;
  register long x2 __asm__("x2") = arg2;
  __asm__ volatile("svc #0\n" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}

void print(const char *str) { syscall1(SYS_WRITE_CONSOLE, (long)str); }

void exit(void) {
  syscall0(SYS_EXIT);
  while (1)
    ; // Wait for the kernel to halt us safely
}

int fork(void) {
  return (int)syscall0(SYS_FORK);
}

int open(const char* filename) {
  return (int)syscall1(SYS_OPEN, (long)filename);
}

int close(int fd) {
  return (int)syscall1(SYS_CLOSE, (long)fd);
}

int read(int fd, void* buf, int size) {
  return (int)syscall3(SYS_READ, (long)fd, (long)buf, (long)size);
}

int write(int fd, const void* buf, int size) {
  return (int)syscall3(SYS_WRITE, (long)fd, (long)buf, (long)size);
}

int spawn(const char* filename) {
  return (int)syscall1(SYS_SPAWN, (long)filename);
}
