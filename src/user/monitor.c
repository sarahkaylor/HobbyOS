#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

static void print_uint64(uint64_t n) {
    char buf[24];
    int idx = 23;
    buf[23] = '\0';
    if (n == 0) {
        print("0");
        return;
    }
    while (n > 0 && idx >= 0) {
        buf[--idx] = (char)('0' + (n % 10));
        n /= 10;
    }
    print(&buf[idx]);
}

int main(void) {
    // Number of monitoring iterations before exiting
    int iterations = 10;

    for (int i = 0; i < iterations; i++) {
        // --- Memory info ---
        struct sys_meminfo mem;
        int ret = sysinfo(2, &mem, sizeof(mem));
        if (ret < 0) {
            print("monitor: failed to get memory info\n");
            continue;
        }

        // --- CPU info ---
        struct sys_cpuinfo cpu;
        ret = sysinfo(5, &cpu, sizeof(cpu));
        if (ret < 0) {
            print("monitor: failed to get CPU info\n");
            continue;
        }

        // --- Display ---
        print("=== System Monitor ===\n");

        // Memory line
        print("Memory: ");
        print_uint64(mem.free_bytes / (1024 * 1024));
        print(" MB free / ");
        print_uint64(mem.total_bytes / (1024 * 1024));
        print(" MB total\n");

        // Uptime line
        uint64_t uptime_secs = cpu.uptime_ms / 1000;
        uint64_t hours = uptime_secs / 3600;
        uint64_t minutes = (uptime_secs % 3600) / 60;
        uint64_t secs = uptime_secs % 60;
        print("Uptime: ");
        print_uint64(hours);
        print("h ");
        print_uint64(minutes);
        print("m ");
        print_uint64(secs);
        print("s\n");

        // CPU usage
        uint64_t total_run_ms = cpu.uptime_ms * (uint64_t)cpu.num_cpus;
        uint64_t pct = 0;
        if (total_run_ms > 0) {
            uint64_t busy_ms = total_run_ms - cpu.total_idle_ms;
            pct = (busy_ms * 100) / total_run_ms;
            if (pct > 100) pct = 100;
        }
        print("CPU: ~");
        print_uint64(pct);
        print("% used (");
        print("cores: ");
        print_uint64((uint64_t)cpu.num_cpus);
        print(")\n");

        print("---\n");

        // Sleep 1 second before next sample
        sleep(1000);
    }

    print("monitor: finished\n");
    return 0;
}
