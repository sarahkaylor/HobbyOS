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
#define connect ho_connect
#define sleep ho_sleep
#endif

void print(const char *str);
void print_console(const char *str);
void print_hex(long val);
void print_dec(long val);
void exit(int status);
int fork(void);
void sleep(int ms);

int open(const char *filename);
int close(int fd);
int read(int fd, void *buf, int size);
int write(int fd, const void *buf, int size);
int kill(int pid, int sig);
void yield(void);
int connect(uint32_t ip, uint16_t port, int protocol);
int spawn(const char *filename, const char *args);
int spawn2(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, const char *args);
int pipe(int fds[2]);
int get_args(char *buf, int size);

void gui_add_menu(int idx, const char* name, const char* items);

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

int parse_args(char *arg_str, char *argv[], int max_args);

struct sys_meminfo {
    uint64_t total_bytes;
    uint64_t free_bytes;
};

struct sys_procinfo {
    int pid;
    int parent_pid;
    int state;
    char name[32];
};

struct sys_netinfo {
    uint32_t ip;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint8_t mac[6];
};

int sysinfo(int cmd, void *buf, int size);
int unlink(const char *filename);
int rename(const char *oldname, const char *newname);

#endif
