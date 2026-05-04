import socket
import json
import time
import subprocess
import os

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
    f.readline() # greeting
    
    f.write('{"execute": "qmp_capabilities"}\n')
    f.flush()
    f.readline()

    time.sleep(2)

    # Click the desktop menu
    print("Clicking desktop menu...")
    f.write('{"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "btn1"}]}}\n')
    f.flush()
    f.readline()
    
    # We need to simulate absolute coordinates to click the menu!
    # "send-key" only sends keys, NOT absolute mouse movement!
    # For VirtIO tablet, we can't easily use QMP send-key for absolute movement?
    # Wait, QMP has `input-send-event`!
