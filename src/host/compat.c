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

void print_hex(long val) {
    printf("0x%016lx", val);
}

int spawn2(const char *filename, int stdin_fd, int stdout_fd) {
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
        
        // Append _host to filename to execute the host version
        char host_filename[256];
        snprintf(host_filename, sizeof(host_filename), "./%s_host", filename);
        
        char *args[] = {host_filename, NULL};
        execv(host_filename, args);
        perror("execv failed");
        exit(1);
    }
    return pid;
}

int spawn(const char *filename) {
    return spawn2(filename, -1, -1);
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

// Read host current directory for mock read_dir
int read_dir(int index, char *buf) {
    if (index == 0) {
        strcpy(buf, "EDITOR.BIN");
        return 0;
    }
    return -1;
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
