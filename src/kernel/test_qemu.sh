#!/bin/bash
make run > qemu.log 2>&1 &
QEMU_PID=$!
sleep 5
kill $QEMU_PID || true
cat qemu.log
