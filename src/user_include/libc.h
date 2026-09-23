#ifndef LIBC_H
#define LIBC_H

/* Phase 1 sysroot umbrella: legacy apps include only libc.h, so pull in the
 * standard sysroot headers here. Under HOST_TEST these resolve to glibc's
 * (no -Isrc/libc/include on the host path); on the bare-metal targets they
 * resolve to src/libc/include/. The declarations do not collide with the
 * hand-rolled per-app helpers in existing programs. */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <stddef.h>
#include <stdint.h>

#include "malloc.h"
/* errno.h (in-tree, src/include) is the errno source on the bare-metal
 * targets. Under HOST_TEST the compiler resolves "errno.h" to glibc's, whose
 * `errno` is a macro, so let the host keep glibc's (values are identical —
 * the in-tree header uses Linux numbering). */
#ifndef HOST_TEST
#include "errno.h"
#endif

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

int kill(int pid, int sig);
void yield(void);
int connect(uint32_t ip, uint16_t port, int protocol);
int spawn(const char *filename, const char *args);
int spawn2(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, const char *args);
int pipe(int fds[2]);
int get_args(char *buf, int size);

/* Native extension: copy the process's binary name into buf (for crt0
 * argv[0]). Returns 0 on success, -1 on failure (no errno set). */
int get_progname(char *buf, int size);

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
struct sys_dirent {
    char name[32];
    uint8_t attr;
    uint32_t size;
} __attribute__((packed));

int read_dir(const char *path, int index, struct sys_dirent *ent);
int mkdir(const char *path);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);

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

struct sys_cpuinfo {
    uint64_t uptime_ms;
    uint64_t total_idle_ms;
    int num_cpus;
};

/* cmd 6: wall-clock time (RTC). epoch = seconds since 1970-01-01 UTC;
 * weekday: 0=Sunday .. 6=Saturday. If the platform has no RTC the kernel
 * returns -1 (callers should fall back to uptime via cmd 1). */
struct sys_time {
    uint64_t epoch;
    int year;    /* e.g. 2026 */
    int month;   /* 1-12 */
    int day;     /* 1-31 */
    int hour;    /* 0-23 */
    int minute;  /* 0-59 */
    int second;  /* 0-59 */
    int weekday; /* 0=Sunday .. 6=Saturday */
};

/* cmd 7: filesystem statistics for the filesystem containing the process
 * cwd (the FAT-16 volume, or the NFS server's FSSTAT when cwd is inside an
 * NFS mount). */
struct sys_fsinfo {
    uint64_t total_bytes;
    uint64_t free_bytes;
};

/* cmd 8: snapshot of the kernel mount table. The kernel fills up to
 * size / sizeof(struct sys_mountinfo) entries and returns the number it
 * filled (0 = no mounts, -1 = error). type: 0 = FAT16 (local), 1 = NFS. */
struct sys_mountinfo {
    char point[64];     /* mount point path, e.g. "/nfs"                    */
    char source[64];    /* "local" or "server:/export"                      */
    int  type;          /* 0 = FAT16, 1 = NFS                               */
};

int sysinfo(int cmd, void *buf, int size);
int unlink(const char *filename);
int rename(const char *oldname, const char *newname);

/* Mount an NFS export at `target` (created if missing). `source` has the
 * form "server:/export/path" where server is a dotted-quad IPv4 address
 * and the export path may be omitted ("server:/" or "server"). Returns 0
 * on success, -1 on failure (bad source, unreachable server, bad target,
 * target inside another mount, target "/", ...). */
int mount(const char *source, const char *target);

/* Unmount the NFS export mounted exactly at `target`. 0 on success. */
int umount(const char *target);

#endif
