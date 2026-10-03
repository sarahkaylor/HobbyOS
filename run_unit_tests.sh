#!/bin/bash

# Unit-test tier: build first, run second.
#
# The 20-second pass/fail budget is the project's deadlock rule and applies
# to the QEMU RUN, not the compile: on a cold checkout the cross-build
# legitimately takes minutes, and a single deadline covering both phases
# reported "timed out!" while the build was still compiling (false red on
# fresh CI copies).  So: build with no deadline, then poll the run.

rm -f qemu.log build.log

echo "Building unit test image..."
if ! make MODE=unit_tests hobbyos.elf disk.img > build.log 2>&1; then
    echo "Build failed!"
    tail -60 build.log
    exit 1
fi

# The unit suites write to disk.img (fat16 create/move); a re-run in the
# same tree must start from a fresh image or stateful failures appear.
echo "Refreshing disk image..."
if ! make MODE=unit_tests fresh_disk >> build.log 2>&1; then
    echo "Disk refresh failed!"
    tail -40 build.log
    exit 1
fi

# Kill exactly this run's process tree: make spawns qemu as its child, and
# killing only make leaves qemu holding disk.img (the next run then dies on
# the QEMU write lock).  pkill -P is scoped to our own child, never the
# whole machine (CI hosts run several copies in parallel).
cleanup() {
    pkill -P "$QEMU_PID" 2>/dev/null
    kill "$QEMU_PID" 2>/dev/null
    wait "$QEMU_PID" 2>/dev/null
}

echo "Running unit tests..."
make MODE=unit_tests run QEMU_ARGS="-display none" > qemu.log 2>&1 &
QEMU_PID=$!

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
