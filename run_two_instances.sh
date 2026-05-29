#!/bin/bash

# Clear old logs
rm -f host.log receiver.log disk_guest.img

echo "======================================================"
echo " Building HobbyOS for Intel (x86_64) Unit Tests... "
echo "======================================================"
make ARCH=intel MODE=unit_tests disk.img

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

# Create a copy of the disk image for the Receiver to avoid QEMU locking conflicts
cp disk.img disk_guest.img

echo "======================================================"
echo " Starting Host (Provider) Instance... "
echo "======================================================"
# Host has the physical -device edu card and listens on port 12345 for the network socket link
/opt/homebrew/bin/qemu-system-x86_64 \
    -M q35 -smp 4 -m 1024M \
    -kernel hobbyos.elf \
    -nographic -serial file:host.log \
    -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
    -drive file=disk.img,format=raw,id=disk0,if=none \
    -device nvme,drive=disk0,serial=1234,bus=pcie.1 \
    -device edu \
    -device virtio-net-pci,disable-legacy=off,disable-modern=on,netdev=net0,mac=52:54:00:12:34:56 \
    -netdev socket,id=net0,listen=:12345 \
    -fw_cfg name=opt/pcishare,string=host:0x1234:0x11e8 \
    -action shutdown=poweroff &
HOST_PID=$!

# Wait for the Host to startup and open its TCP listening port
sleep 2

echo "======================================================"
echo " Starting Receiver (Consumer) Instance... "
echo "======================================================"
# Receiver connects to the Host's socket and emulates the edu card
/opt/homebrew/bin/qemu-system-x86_64 \
    -M q35 -smp 4 -m 1024M \
    -kernel hobbyos.elf \
    -nographic -serial file:receiver.log \
    -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
    -drive file=disk_guest.img,format=raw,id=disk0,if=none \
    -device nvme,drive=disk0,serial=1234,bus=pcie.1 \
    -device virtio-net-pci,disable-legacy=off,disable-modern=on,netdev=net0,mac=52:54:00:12:34:57 \
    -netdev socket,id=net0,connect=127.0.0.1:12345 \
    -fw_cfg name=opt/pcishare,string=guest:0x1234:0x11e8 \
    -action shutdown=poweroff &
RECEIVER_PID=$!

echo "======================================================"
echo " Waiting for Remote PCIe RDMA Sharing tests to finish... "
echo "======================================================"

TIMEOUT=25
while [ $TIMEOUT -gt 0 ]; do
    if grep -q "UNIT TESTS PASSED" receiver.log; then
        echo "UNIT TESTS PASSED!"
        echo ""
        echo "--- HOST (PROVIDER) LOG ---"
        cat host.log
        echo ""
        echo "--- RECEIVER (CONSUMER) LOG ---"
        cat receiver.log
        kill $HOST_PID $RECEIVER_PID 2>/dev/null || true
        exit 0
    elif grep -q "UNIT TESTS FAILED" receiver.log; then
        echo "UNIT TESTS FAILED!"
        echo ""
        echo "--- HOST (PROVIDER) LOG ---"
        cat host.log
        echo ""
        echo "--- RECEIVER (CONSUMER) LOG ---"
        cat receiver.log
        kill $HOST_PID $RECEIVER_PID 2>/dev/null || true
        exit 1
    fi
    sleep 1
    ((TIMEOUT--))
done

echo "Tests timed out!"
echo ""
echo "--- HOST (PROVIDER) LOG ---"
cat host.log
echo ""
echo "--- RECEIVER (CONSUMER) LOG ---"
cat receiver.log
kill $HOST_PID $RECEIVER_PID 2>/dev/null || true
exit 1
