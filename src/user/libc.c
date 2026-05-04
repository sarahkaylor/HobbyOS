#include "libc.h"
#include <stdint.h>

#define SYS_WRITE_CONSOLE (1)
#define SYS_EXIT (2)
#define SYS_FORK (3)
#define SYS_OPEN (4)
#define SYS_CLOSE (5)
#define SYS_READ (6)
#define SYS_WRITE (7)
#define SYS_SPAWN (8)
#define SYS_MAP_FB (9)
#define SYS_FLUSH_FB (10)
#define SYS_GET_CPUID (11)
#define SYS_PIPE (12)
#define SYS_GET_EVENTS (13)
#define SYS_AVAILABLE (14)
#define SYS_READ_DIR (15)
#define SYS_KILL (16)
#define SYS_YIELD (17)

static long syscall(long num, long a0, long a1, long a2, long a3) {
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
}

void print(const char *s) {
  int len = 0;
  while (s[len])
    len++;
  
  int written = 0;
  while (written < len) {
    int res = write(1, s + written, len - written);
    if (res < 0) {
      syscall(SYS_WRITE_CONSOLE, (long)(s + written), len - written, 0, 0);
      break;
    }
    written += res;
  }
}

void print_hex(long val) {
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    int nibble = (val >> (60 - i * 4)) & 0xF;
    buf[i + 2] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
  }
  buf[18] = '\0';
  print(buf);
}

void exit(int status) {
  syscall(SYS_EXIT, (long)status, 0, 0, 0);
  while (1)
    ; // Wait for the kernel to halt us safely
}

int kill(int pid, int sig) { return (int)syscall(SYS_KILL, (long)pid, (long)sig, 0, 0); }

int fork(void) { return (int)syscall(SYS_FORK, 0, 0, 0, 0); }

int open(const char *filename) {
  return (int)syscall(SYS_OPEN, (long)filename, 0, 0, 0);
}

int close(int fd) { return (int)syscall(SYS_CLOSE, (long)fd, 0, 0, 0); }

int read(int fd, void *buf, int size) {
  return (int)syscall(SYS_READ, (long)fd, (long)buf, (long)size, 0);
}

int write(int fd, const void *buf, int size) {
  return (int)syscall(SYS_WRITE, (long)fd, (long)buf, (long)size, 0);
}

void yield(void) {
  syscall(SYS_YIELD, 0, 0, 0, 0);
}

int spawn2(const char *filename, int stdin_fd, int stdout_fd) {
  return (int)syscall(SYS_SPAWN, (long)filename, (long)stdin_fd,
                      (long)stdout_fd, 0);
}

int spawn(const char *filename) {
  int fd = -1;
  return spawn2(filename, fd, fd);
}

int pipe(int fds[2]) {
  long res = syscall(SYS_PIPE, (long)fds, 0, 0, 0);
  if (res == 0)
    return 0;
  return -1;
}

void *map_fb(void) { return (void *)syscall(SYS_MAP_FB, 0, 0, 0, 0); }

__attribute__((weak)) void flush_fb(void) { syscall(SYS_FLUSH_FB, 0, 0, 0, 0); }

int get_cpuid(void) { return (int)syscall(SYS_GET_CPUID, 0, 0, 0, 0); }

__attribute__((weak)) int get_events(void *buf, int max_events) {
  return (int)syscall(SYS_GET_EVENTS, (long)buf, (long)max_events, 0, 0);
}

int available(int fd) { return (int)syscall(SYS_AVAILABLE, (long)fd, 0, 0, 0); }

__attribute__((weak)) int read_dir(int index, char *buf) {
  return (int)syscall(SYS_READ_DIR, (long)index, (long)buf, 0, 0);
}

void gui_add_menu(int idx, const char* name, const char* items) {
    char buf[128];
    int len = 0;
    buf[len++] = '\033';
    buf[len++] = ']';
    buf[len++] = 'M';
    buf[len++] = '0' + idx;
    buf[len++] = ';';
    
    int i = 0;
    while(name[i]) buf[len++] = name[i++];
    buf[len++] = ';';
    
    i = 0;
    while(items[i]) buf[len++] = items[i++];
    buf[len++] = '\a';
    
    write(1, buf, len);
}
