#!/bin/bash

# Clear old log
rm -f qemu.log

echo "Building and running unit tests..."

# Run the test target in background
make unit_tests > qemu.log 2>&1 &
QEMU_PID=$!

# Wait for tests to finish or timeout after 10 seconds
TIMEOUT=10
while [ $TIMEOUT -gt 0 ]; do
    if grep -q "UNIT TESTS PASSED" qemu.log; then
        echo "Tests passed!"
        cat qemu.log
        kill $QEMU_PID 2>/dev/null || true
        exit 0
    elif grep -q "UNIT TESTS FAILED" qemu.log; then
        echo "Tests failed!"
        cat qemu.log
        kill $QEMU_PID 2>/dev/null || true
        exit 1
    fi
    sleep 1
    ((TIMEOUT--))
done

echo "Tests timed out!"
cat qemu.log
kill $QEMU_PID 2>/dev/null || true
exit 1
