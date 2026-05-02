#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>
#include <stdint.h>

#include "malloc.h"

#ifdef HOST_TEST
#define open ho_open
#define read ho_read
#define write ho_write
#define close ho_close
#define exit ho_exit
#define kill ho_kill
#define fork ho_fork
#define pipe ho_pipe
#endif

void print(const char *str);
void print_hex(long val);
void exit(int status);
int fork(void);

int open(const char *filename);
int close(int fd);
int read(int fd, void *buf, int size);
int write(int fd, const void *buf, int size);
int kill(int pid, int sig);
int spawn(const char *filename);
int spawn2(const char *filename, int stdin_fd, int stdout_fd);
int pipe(int fds[2]);

void *map_fb(void);
void flush_fb(void);
int get_cpuid(void);

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define ABS_X 0x00
#define ABS_Y 0x01

struct virtio_input_event {
  uint16_t type;
  uint16_t code;
  uint32_t value;
};

int get_events(void *buf, int max_events);
int available(int fd);
int read_dir(int index, char *buf);

#endif
