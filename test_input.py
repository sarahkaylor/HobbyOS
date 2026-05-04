import socket
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
time.sleep(3) # wait for boot and desktop load

try:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect("./qmp-sock")
    f = sock.makefile('rw')
    f.readline()
    
    f.write('{"execute": "qmp_capabilities"}\n')
    f.flush()
    f.readline()

    # Move mouse to File menu
    f.write('{"execute": "input-send-event", "arguments": {"events": [{"type": "abs", "data": {"axis": "x", "value": 20}}, {"type": "abs", "data": {"axis": "y", "value": 26}}]}}\n')
    f.flush()
    f.readline()

    # Click
    f.write('{"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"button": "left", "down": true}}]}}\n')
    f.flush()
    f.readline()
    f.write('{"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"button": "left", "down": false}}]}}\n')
    f.flush()
    f.readline()

    time.sleep(1)

    # Move mouse to Editor in menu (say, index 1)
    f.write('{"execute": "input-send-event", "arguments": {"events": [{"type": "abs", "data": {"axis": "x", "value": 20}}, {"type": "abs", "data": {"axis": "y", "value": 60}}]}}\n')
    f.flush()
    f.readline()

    # Click
    f.write('{"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"button": "left", "down": true}}]}}\n')
    f.flush()
    f.readline()
    f.write('{"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"button": "left", "down": false}}]}}\n')
    f.flush()
    f.readline()

    time.sleep(2)

    # Type 'a'
    f.write('{"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "a"}]}}\n')
    f.flush()
    f.readline()

    time.sleep(1)

    # Type 'b'
    f.write('{"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "b"}]}}\n')
    f.flush()
    f.readline()

    time.sleep(1)

finally:
    proc.terminate()
    proc.wait()

with open("serial.log", "r") as log:
    print(log.read())

