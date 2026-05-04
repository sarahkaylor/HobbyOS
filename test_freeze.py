import socket
import json
import time
import subprocess
import os

# Start QEMU in background
cmd = [
    "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53", "-smp", "4",
    "-m", "4096M", "-kernel", "hobbyos.elf", "-display", "none",
    "-serial", "file:serial.log",
    "-drive", "if=none,file=disk.img,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-device", "virtio-gpu-device",
    "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device",
    "-semihosting", "-action", "shutdown=poweroff",
    "-qmp", "unix:./qmp-sock,server,nowait"
]

if os.path.exists("./qmp-sock"):
    os.remove("./qmp-sock")
if os.path.exists("serial.log"):
    os.remove("serial.log")

proc = subprocess.Popen(cmd)
time.sleep(2) # wait for boot

try:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect("./qmp-sock")
    f = sock.makefile('rw')
    print("QMP Connected:", f.readline().strip())
    
    # Send qmp_capabilities
    f.write('{"execute": "qmp_capabilities"}\n')
    f.flush()
    print("Caps response:", f.readline().strip())

    # Wait a bit for desktop to load
    time.sleep(3)

    # Click the editor to focus it
    # We don't necessarily know where the window is, but let's assume it's focused or we can click (0,0) maybe?
    # Actually, in make run, is the editor window focused by default?
    # In desktop.c, focused_window = wm_create_window...
    # So it is focused!
    
    # Send character 'a' (Q-code for 'a' is 'a' or hex?)
    # "send-key" takes a key array
    print("Sending key 'a'...")
    f.write('{"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "a"}]}}\n')
    f.flush()
    print("Key press response:", f.readline().strip())
    
    time.sleep(1)

    print("Sending key 'b'...")
    f.write('{"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "b"}]}}\n')
    f.flush()
    print("Key press response:", f.readline().strip())

    time.sleep(1)

finally:
    proc.terminate()
    proc.wait()

with open("serial.log", "r") as log:
    print("\n--- Serial Output ---")
    print(log.read())

