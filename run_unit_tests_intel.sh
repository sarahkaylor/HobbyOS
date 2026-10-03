#!/bin/bash

# Unit-test tier (x86_64): build first, run second.
#
# Same two-phase structure as run_unit_tests.sh (see its comments for why:
# the 20s budget covers the run, not the cold build, and the cleanup kills
# this run's own process tree only).
#
# EXTRA_QEMU_ARGS lets CI add acceleration (e.g. "-enable-kvm"); default is
# a plain headless TCG run.

rm -f qemu.log build.log
EXTRA_QEMU_ARGS="${EXTRA_QEMU_ARGS:-}"

echo "Building unit test image..."
if ! make ARCH=intel MODE=unit_tests hobbyos.elf disk.img > build.log 2>&1; then
    echo "Build failed!"
    tail -60 build.log
    exit 1
fi

# The unit suites write to disk.img (fat16 create/move); a re-run in the
# same tree must start from a fresh image or stateful failures appear.
echo "Refreshing disk image..."
if ! make ARCH=intel MODE=unit_tests fresh_disk >> build.log 2>&1; then
    echo "Disk refresh failed!"
    tail -40 build.log
    exit 1
fi

cleanup() {
    pkill -P "$QEMU_PID" 2>/dev/null
    kill "$QEMU_PID" 2>/dev/null
    wait "$QEMU_PID" 2>/dev/null
}

echo "Running unit tests..."
make ARCH=intel MODE=unit_tests run QEMU_ARGS="-display none $EXTRA_QEMU_ARGS" > qemu.log 2>&1 &
QEMU_PID=$!

# Wait for tests to finish or timeout after 20 seconds
# Wait for tests to finish or timeout (default 300 s; override with
# UNIT_TESTS_TIMEOUT).  The >1 MiB fat16 unit tests legitimately take
# ~60-90 s of wall time, so the old hardcoded 20 s reds green suites.
TIMEOUT="${UNIT_TESTS_TIMEOUT:-300}"
while [ $TIMEOUT -gt 0 ]; do
    if grep -q "UNIT TESTS PASSED" qemu.log; then
        echo "Tests passed!"
        cat qemu.log
        cleanup
        exit 0
    elif grep -q "UNIT TESTS FAILED" qemu.log; then
        echo "Tests failed!"
        cat qemu.log
        cleanup
        exit 1
    fi
    sleep 1
    ((TIMEOUT--))
done

echo "Tests timed out!"
cat qemu.log
cleanup
exit 1
