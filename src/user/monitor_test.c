#include "libc.h"

__attribute__((section(".text._start")))
void _start(void) {
    int failures = 0;

    print_console("\n========================================\n");
    print_console("System Monitor Test\n");
    print_console("========================================\n");

    // Test 1: Memory info via sysinfo(2)
    print_console("Test 1: sysinfo(2) memory info...\n");
    struct sys_meminfo mem;
    int ret = sysinfo(2, &mem, sizeof(mem));
    if (ret < 0) {
        print_console("  FAIL: sysinfo(2) returned error\n");
        failures++;
    } else if (mem.total_bytes == 0) {
        print_console("  FAIL: total_bytes is zero\n");
        failures++;
    } else if (mem.free_bytes > mem.total_bytes) {
        print_console("  FAIL: free_bytes > total_bytes\n");
        failures++;
    } else {
        print_console("  PASS: total=");
        print_dec((long)mem.total_bytes);
        print_console(" free=");
        print_dec((long)mem.free_bytes);
        print_console("\n");
    }

    // Test 2: CPU info via sysinfo(5)
    print_console("Test 2: sysinfo(5) CPU info...\n");
    struct sys_cpuinfo cpu;
    ret = sysinfo(5, &cpu, sizeof(cpu));
    if (ret < 0) {
        print_console("  FAIL: sysinfo(5) returned error\n");
        failures++;
    } else if (cpu.uptime_ms == 0) {
        print_console("  FAIL: uptime_ms is zero\n");
        failures++;
    } else if (cpu.num_cpus <= 0) {
        print_console("  FAIL: num_cpus is <= 0\n");
        failures++;
    } else {
        print_console("  PASS: uptime=");
        print_dec((long)(cpu.uptime_ms / 1000));
        print_console("s idle=");
        print_dec((long)(cpu.total_idle_ms / 1000));
        print_console("s cpus=");
        print_dec((long)cpu.num_cpus);
        print_console("\n");
    }

    // Test 3: Uptime via sysinfo(1)
    print_console("Test 3: sysinfo(1) uptime...\n");
    int uptime = (int)sysinfo(1, 0, 0);
    if (uptime <= 0) {
        print_console("  FAIL: uptime returned ");
        print_dec((long)uptime);
        print_console("\n");
        failures++;
    } else {
        print_console("  PASS: uptime=");
        print_dec((long)(uptime / 1000));
        print_console("s\n");
    }

    // Test 4: CPU usage sanity check (idle <= uptime * cpus)
    print_console("Test 4: CPU usage sanity check...\n");
    ret = sysinfo(5, &cpu, sizeof(cpu));
    if (ret == 0) {
        uint64_t max_idle = cpu.uptime_ms * (uint64_t)cpu.num_cpus;
        if (cpu.total_idle_ms > max_idle) {
            print_console("  FAIL: idle_time > uptime * cpus\n");
            failures++;
        } else {
            print_console("  PASS: idle time is sane\n");
        }
    } else {
        print_console("  FAIL: sysinfo(5) returned error\n");
        failures++;
    }

    // Test 5: Both sysinfo commands with small buffer should fail gracefully
    print_console("Test 5: Error handling - small buffer...\n");
    char tiny[4];
    ret = sysinfo(2, tiny, 4);
    if (ret >= 0) {
        print_console("  FAIL: sysinfo(2) with small buffer should return error\n");
        failures++;
    } else {
        print_console("  PASS: small buffer rejected\n");
    }

    ret = sysinfo(5, tiny, 4);
    if (ret >= 0) {
        print_console("  FAIL: sysinfo(5) with small buffer should return error\n");
        failures++;
    } else {
        print_console("  PASS: small buffer rejected\n");
    }

    // Test 6: CPU non-idle time makes sense (some busy work should produce >0 busy time)
    print_console("Test 6: Verifying busy time accumulates...\n");
    // Actually read the first sample
    ret = sysinfo(5, &cpu, sizeof(cpu));
    if (ret == 0) {
        uint64_t busy1 = (cpu.uptime_ms * (uint64_t)cpu.num_cpus) - cpu.total_idle_ms;
        // Do some busy work
        for (volatile int j = 0; j < 5000000; j++);
        ret = sysinfo(5, &cpu, sizeof(cpu));
        if (ret == 0) {
            uint64_t busy2 = (cpu.uptime_ms * (uint64_t)cpu.num_cpus) - cpu.total_idle_ms;
            if (busy2 >= busy1) {
                print_console("  PASS: busy time did not decrease\n");
            } else {
                print_console("  FAIL: busy time decreased\n");
                failures++;
            }
        } else {
            print_console("  FAIL: sysinfo(5) second call error\n");
            failures++;
        }
    } else {
        print_console("  FAIL: sysinfo(5) first call error\n");
        failures++;
    }

    // Summary
    print_console("\n========================================\n");
    if (failures == 0) {
        print_console("ALL MONITOR TESTS PASSED SUCCESSFULLY!\n");
        print_console("========================================\n");
    } else {
        print_console("SOME TESTS FAILED! Failures: ");
        print_dec((long)failures);
        print_console("\n========================================\n");
    }

    exit(failures);
}
