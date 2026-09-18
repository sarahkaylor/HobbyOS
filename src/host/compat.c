#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>

#include "../user_include/libc.h"

#undef open
#undef read
#undef write
#undef close
#undef exit
#undef kill
#undef fork
#undef pipe

int ho_open(const char *filename) {
    return open(filename, O_RDWR | O_CREAT, 0666);
}

int ho_close(int fd) {
    return close(fd);
}

int ho_read(int fd, void *buf, int size) {
    return (int)read(fd, buf, size);
}

int ho_write(int fd, const void *buf, int size) {
    return (int)write(fd, buf, size);
}

void ho_exit(int status) {
    exit(status);
}

int ho_kill(int pid, int sig) {
    return kill(pid, sig);
}

int ho_fork(void) {
    return fork();
}

int ho_pipe(int fds[2]) {
    return pipe(fds);
}

// Mock Framebuffer
static uint32_t *mock_fb = NULL;
#define MOCK_FB_SIZE (1024 * 768 * 4)

void print(const char *str) {
    write(1, str, strlen(str));
}

void print_console(const char *str) {
    write(1, str, strlen(str));
}

void print_hex(long val) {
    printf("0x%016lx", val);
}

int spawn2(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, const char *args) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (stdin_fd >= 0 && stdin_fd != 0) {
            dup2(stdin_fd, 0);
            close(stdin_fd);
        }
        if (stdout_fd >= 0 && stdout_fd != 1) {
            dup2(stdout_fd, 1);
            close(stdout_fd);
        }
        if (stderr_fd >= 0 && stderr_fd != 2) {
            dup2(stderr_fd, 2);
            close(stderr_fd);
        }
        
        // Append _host to filename to execute the host version
        char host_filename[256];
        snprintf(host_filename, sizeof(host_filename), "./%s_host", filename);
        
        char *argv[] = {host_filename, NULL};
        execv(host_filename, argv);
        perror("execv failed");
        exit(1);
    }
    return pid;
}

int spawn(const char *filename, const char *args) {
    (void)args;
    return spawn2(filename, -1, -1, -1, NULL);
}

void *map_fb(void) {
    if (!mock_fb) {
        mock_fb = malloc(MOCK_FB_SIZE);
        memset(mock_fb, 0, MOCK_FB_SIZE);
    }
    return mock_fb;
}

static void (*flush_callback)(void) = NULL;

void set_flush_callback(void (*cb)(void)) {
    flush_callback = cb;
}

void flush_fb(void) {
    if (flush_callback) {
        flush_callback();
    }
}

int get_cpuid(void) {
    return 0; // Host runs on "core 0" for tests
}

// Event queue for get_events
#define MAX_MOCK_EVENTS 256
static struct virtio_input_event mock_events[MAX_MOCK_EVENTS];
static int mock_events_head = 0;
static int mock_events_tail = 0;

void inject_mock_event(uint16_t type, uint16_t code, uint32_t value) {
    int next = (mock_events_head + 1) % MAX_MOCK_EVENTS;
    if (next != mock_events_tail) {
        mock_events[mock_events_head].type = type;
        mock_events[mock_events_head].code = code;
        mock_events[mock_events_head].value = value;
        mock_events_head = next;
    }
}

int get_events(void *buf, int max_events) {
    struct virtio_input_event *events = (struct virtio_input_event *)buf;
    int count = 0;
    while (mock_events_tail != mock_events_head && count < max_events) {
        events[count++] = mock_events[mock_events_tail];
        mock_events_tail = (mock_events_tail + 1) % MAX_MOCK_EVENTS;
    }
    return count;
}

int available(int fd) {
    int bytes_available = 0;
    if (ioctl(fd, FIONREAD, &bytes_available) == -1) {
        return -1;
    }
    return bytes_available;
}

/* --- Configurable directory listing ------------------------------------
 * The default listing above is a fixed 7-file set.  Cross-application host
 * tests that need a longer directory (scrolling) or directory entries
 * (Enter-on-dir) install an override here.  mock_read_dir_count < 0 means
 * "empty directory" (every index fails). */
#define MOCK_READ_DIR_MAX 40
int mock_read_dir_count = 0;                     /* 0 = default 7 files */
char mock_read_dir_names[MOCK_READ_DIR_MAX][32];
uint8_t mock_read_dir_attr = 0;                  /* attr for every entry */
uint32_t mock_read_dir_size = 0;                 /* size for every entry */

void mock_read_dir_reset(void) {
    mock_read_dir_count = 0;
    mock_read_dir_attr = 0;
    mock_read_dir_size = 0;
    for (int i = 0; i < MOCK_READ_DIR_MAX; i++) mock_read_dir_names[i][0] = '\0';
}

/* Read host current directory for mock read_dir
// Returns a few mock files so the file dialog has content to show
int read_dir(const char *path, int index, struct sys_dirent *ent) {
    (void)path;
    static const char *mock_files[] = {
        "EDITOR.BIN",
        "DESKTOP.BIN",
        "SH.BIN",
        "LS.BIN",
        "CAT.BIN",
        "TEST.TXT",
        "NOTES.TXT"
    };
    static const int num_mock = 7;
    if (mock_read_dir_count != 0) {
        if (mock_read_dir_count < 0) return -1;
        if (index < 0 || index >= mock_read_dir_count) return -1;
        memset(ent, 0, sizeof(*ent));
        snprintf(ent->name, sizeof(ent->name), "%s", mock_read_dir_names[index]);
        ent->attr = mock_read_dir_attr;
        ent->size = mock_read_dir_size;
        return 0;
    }
    if (index < 0 || index >= num_mock) return -1;
    memset(ent, 0, sizeof(*ent));
    strcpy(ent->name, mock_files[index]);
    ent->size = 0;
    return 0;
}

int dump_screenshot(const char *filename) {
    if (!mock_fb) return -1;
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n1024 768\n255\n");
    for (int i = 0; i < 1024 * 768; i++) {
        uint32_t color = mock_fb[i];
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        fwrite(&r, 1, 1, f);
        fwrite(&g, 1, 1, f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return 0;
}

int validate_screenshot(const char *expected_filename) {
    if (!mock_fb) return -1;
    FILE *f = fopen(expected_filename, "rb");
    if (!f) {
        printf("[TEST] Expected screenshot '%s' not found. You may need to create it first.\n", expected_filename);
        return -1;
    }
    char header[16];
    if (fgets(header, sizeof(header), f) == NULL || strncmp(header, "P6", 2) != 0) {
        fclose(f); return -1;
    }
    // Skip comments
    int c = getc(f);
    while (c == '#') {
        while (getc(f) != '\n');
        c = getc(f);
    }
    ungetc(c, f);
    int w, h, maxval;
    if (fscanf(f, "%d %d\n%d\n", &w, &h, &maxval) != 3) {
        fclose(f); return -1;
    }
    if (w != 1024 || h != 768) {
        fclose(f); return -1;
    }
    
    int mismatch = 0;
    for (int i = 0; i < 1024 * 768; i++) {
        uint8_t rgb[3];
        if (fread(rgb, 1, 3, f) != 3) {
            printf("[TEST] Unexpected EOF in expected image.\n");
            mismatch = 1;
            break;
        }
        uint32_t color = mock_fb[i];
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        if (r != rgb[0] || g != rgb[1] || b != rgb[2]) {
            printf("[TEST] Pixel mismatch at (%d, %d). Expected (%d,%d,%d), Got (%d,%d,%d)\n", 
                   i % 1024, i / 1024, rgb[0], rgb[1], rgb[2], r, g, b);
            mismatch = 1;
            break;
        }
    }
    fclose(f);
    return mismatch ? -1 : 0;
}

void yield(void) {
    usleep(0);
}

int ho_connect(uint32_t ip, uint16_t port, int protocol) {
    (void)ip; (void)port; (void)protocol;
    return -1;
}

void ho_sleep(int ms) {
    usleep(ms * 1000);
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
    while(name[i] && len < 126) buf[len++] = name[i++];
    buf[len++] = ';';
    
    i = 0;
    while(items[i] && len < 126) buf[len++] = items[i++];
    buf[len++] = '\a';
    
    write(1, buf, len);
}

/* ============================================================== */
/* Additional mocks for desktop app host tests                     */
/* ============================================================== */

void print_dec(long val) {
    printf("%ld", val);
}

/* --- Filesystem mocks -------------------------------------------------
 * Apps under HOST_TEST get inert filesystem calls so tests never touch
 * the real host filesystem. Tests can flip the *_result globals to
 * exercise failure paths. */
int mock_mkdir_result = 0;
int mock_unlink_result = 0;
int mock_rename_result = 0;

/* Call recorders: cross-app host tests assert what the app asked for. */
int  mock_mkdir_calls = 0;
char mock_mkdir_last[64] = "";
int  mock_unlink_calls = 0;
char mock_unlink_last[64] = "";
int  mock_rename_calls = 0;
char mock_rename_last_old[64] = "";
char mock_rename_last_new[64] = "";

int mkdir(const char *path) {
    mock_mkdir_calls++;
    snprintf(mock_mkdir_last, sizeof(mock_mkdir_last), "%s", path ? path : "");
    return mock_mkdir_result;
}

int unlink(const char *filename) {
    mock_unlink_calls++;
    snprintf(mock_unlink_last, sizeof(mock_unlink_last), "%s", filename ? filename : "");
    return mock_unlink_result;
}

int rename(const char *oldname, const char *newname) {
    mock_rename_calls++;
    snprintf(mock_rename_last_old, sizeof(mock_rename_last_old), "%s", oldname ? oldname : "");
    snprintf(mock_rename_last_new, sizeof(mock_rename_last_new), "%s", newname ? newname : "");
    return mock_rename_result;
}

/* --- cwd mocks: a simple in-memory current directory --- */
static char mock_cwd[128] = "/home";

int chdir(const char *path) {
    snprintf(mock_cwd, sizeof(mock_cwd), "%s", path);
    return 0;
}

char *getcwd(char *buf, size_t size) {
    if (!buf) return mock_cwd;
    snprintf(buf, size, "%s", mock_cwd);
    return buf;
}

/* --- sysinfo mock ------------------------------------------------------
 * Canned values so GUI apps (clock, sysmon, files) compile and render on
 * the host. Tests should prefer passing data as function parameters when
 * they need specific values. */
#define MOCK_EPOCH 1789646400ULL   /* 2026-09-17 12:00:00 UTC (Thursday) */

/* --- sysinfo overrides -------------------------------------------------
 * Every command can be overridden so cross-app tests can inject specific
 * memory / CPU / time / process-list / filesystem values.  Disabled (0)
 * by default: the canned values below are used. */
int mock_sysinfo_mem_enabled = 0;
unsigned long long mock_sysinfo_mem_total = 0, mock_sysinfo_mem_free = 0;
int mock_sysinfo_cpu_enabled = 0;
unsigned long long mock_sysinfo_cpu_uptime = 0, mock_sysinfo_cpu_idle = 0;
int mock_sysinfo_num_cpus = 4;
int mock_sysinfo_uptime_enabled = 0;
int mock_sysinfo_uptime_ms = 0;
int mock_sysinfo_time_enabled = 0;
struct sys_time mock_sysinfo_time;
int mock_sysinfo_fs_enabled = 0;
unsigned long long mock_sysinfo_fs_total = 0, mock_sysinfo_fs_free = 0;
int mock_sysinfo_procs_enabled = 0;
struct sys_procinfo mock_sysinfo_procs[8];
int mock_sysinfo_proc_count = 0;

void mock_sysinfo_override_reset(void) {
    mock_sysinfo_mem_enabled = 0;
    mock_sysinfo_cpu_enabled = 0;
    mock_sysinfo_uptime_enabled = 0;
    mock_sysinfo_time_enabled = 0;
    mock_sysinfo_fs_enabled = 0;
    mock_sysinfo_procs_enabled = 0;
    mock_sysinfo_num_cpus = 4;
    mock_sysinfo_proc_count = 0;
}

int sysinfo(int cmd, void *buf, int size) {
    if (cmd == 1) {
        if (mock_sysinfo_uptime_enabled) return mock_sysinfo_uptime_ms;
        return 12345; /* uptime ms, returned by value like the kernel */
    }
    if (cmd == 2) {
        struct sys_meminfo *m = (struct sys_meminfo *)buf;
        if (size < (int)sizeof(*m)) return -1;
        if (mock_sysinfo_mem_enabled) {
            m->total_bytes = mock_sysinfo_mem_total;
            m->free_bytes = mock_sysinfo_mem_free;
            return 0;
        }
        m->total_bytes = 64ULL * 1024 * 1024;
        m->free_bytes = 40ULL * 1024 * 1024;
        return 0;
    }
    if (cmd == 3) {
        struct sys_procinfo *p = (struct sys_procinfo *)buf;
        int max = size / (int)sizeof(*p);
        if (mock_sysinfo_procs_enabled) {
            int n = mock_sysinfo_proc_count;
            if (n > 8) n = 8;
            if (n > max) n = max;
            if (n < 0) n = 0;
            for (int i = 0; i < n; i++) p[i] = mock_sysinfo_procs[i];
            return n;
        }
        if (max < 3) return 0;
        memset(p, 0, sizeof(p[0]) * 3);
        p[0].pid = 1; p[0].parent_pid = 0; p[0].state = 1;
        strcpy(p[0].name, "DESKTOP.BIN");
        p[1].pid = 2; p[1].parent_pid = 1; p[1].state = 0;
        strcpy(p[1].name, "EDITOR.BIN");
        p[2].pid = 3; p[2].parent_pid = 1; p[2].state = 0;
        strcpy(p[2].name, "SHELL");
        return 3;
    }
    if (cmd == 4) {
        struct sys_netinfo *n = (struct sys_netinfo *)buf;
        if (size < (int)sizeof(*n)) return -1;
        n->ip = 0x0A00020F; /* 10.0.2.15 */
        n->subnet_mask = 0xFFFFFF00;
        n->gateway = 0x0A000202;
        n->mac[0] = 0x52; n->mac[1] = 0x54; n->mac[2] = 0x00;
        n->mac[3] = 0x12; n->mac[4] = 0x34; n->mac[5] = 0x56;
        return 0;
    }
    if (cmd == 5) {
        struct sys_cpuinfo *c = (struct sys_cpuinfo *)buf;
        if (size < (int)sizeof(*c)) return -1;
        if (mock_sysinfo_cpu_enabled) {
            c->uptime_ms = mock_sysinfo_cpu_uptime;
            c->total_idle_ms = mock_sysinfo_cpu_idle;
            c->num_cpus = mock_sysinfo_num_cpus;
            return 0;
        }
        c->uptime_ms = 12345;
        c->total_idle_ms = 4000;
        c->num_cpus = 4;
        return 0;
    }
    if (cmd == 6) {
        struct sys_time *t = (struct sys_time *)buf;
        if (size < (int)sizeof(*t)) return -1;
        if (mock_sysinfo_time_enabled) {
            *t = mock_sysinfo_time;
            return 0;
        }
        t->epoch = MOCK_EPOCH;
        t->year = 2026; t->month = 9; t->day = 17;
        t->hour = 12; t->minute = 0; t->second = 0;
        t->weekday = 4; /* Thursday */
        return 0;
    }
    if (cmd == 7) {
        struct sys_fsinfo *f = (struct sys_fsinfo *)buf;
        if (size < (int)sizeof(*f)) return -1;
        if (mock_sysinfo_fs_enabled) {
            f->total_bytes = mock_sysinfo_fs_total;
            f->free_bytes = mock_sysinfo_fs_free;
            return 0;
        }
        f->total_bytes = 64ULL * 1024 * 1024;
        f->free_bytes = 40ULL * 1024 * 1024;
        return 0;
    }
    return -1;
}

