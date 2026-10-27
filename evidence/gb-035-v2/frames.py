#!/usr/bin/env python3
"""GB-035-V2 (#218) — independent measurement of clause 2: how many DISTINCT
playfield frame digests does NES.BIN produce?

Reuses the GB-034-V virtio-input harness machinery (menu navigation by key, QMP
screendump).  Boots the intel disk image, launches NES.BIN from the F1 menu, then
samples N frames and hashes the playfield crop (clock/taskbar excluded, the same
crop the GB-046 freeze watchdog uses) so a ticking clock cannot masquerade as
animation.

Prints every digest, the distinct count, and the serial evidence of arrival.
"""
import hashlib, os, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QEMU = "qemu-system-x86_64"
MON = "/tmp/gb035v2-qmp.sock"
LOG = os.path.join(ROOT, "obj", "gb035v2_serial.log")
NFRAMES = int(os.environ.get("NFRAMES", "10"))
CROP = (0, 0, 1820, 1010)          # exclude the clock (top-right) and taskbar


def qmp_start():
    if os.path.exists(MON):
        os.unlink(MON)
    cmd = [QEMU, "-M", "q35", "-smp", "8", "-m", "12288M",
           "-pflash", os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
           "-drive", f"if=none,id=nvme0,format=raw,file={ROOT}/disk.img",
           "-device", "nvme,serial=deadbeef,drive=nvme0",
           "-display", "none", "-serial", f"file:{LOG}",
           "-qmp", f"unix:{MON},server,nowait", "-enable-kvm"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        if os.path.exists(MON):
            break
        time.sleep(0.1)
    return p


class Q:
    def __init__(self):
        import socket
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(MON)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        import json
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        return json.loads(self.f.readline())

    def key(self, qcode, hold=0.12):
        for down in (True, False):
            self.cmd("input-send-event", events=[{"type": "key", "data": {
                "down": down, "key": {"type": "qcode", "data": qcode}}}])
            time.sleep(hold)


def digest(path):
    with open(path, "rb") as fh:
        data = fh.read()
    # parse P6 header
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while data[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j+1].isspace():
            j += 1
        parts.append(data[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    px = data[i:]
    x0, y0, x1, y1 = CROP
    x1, y1 = min(x1, w), min(y1, h)
    hh = hashlib.sha256()
    for y in range(y0, y1):
        hh.update(px[(y*w + x0)*3:(y*w + x1)*3])
    return hh.hexdigest()[:12]


def main():
    p = qmp_start()
    q = Q()
    time.sleep(35)  # boot to desktop
    print("[harness] booted; opening the menu (F1) and launching NES.BIN", flush=True)
    q.key("f1"); time.sleep(1.5)
    # walk the menu to the NES slot: Home, then Down x slot
    slot = int(os.environ.get("NES_SLOT", "22"))
    q.key("home"); time.sleep(0.3)
    for _ in range(slot):
        q.key("down"); time.sleep(0.12)
    q.key("ret"); time.sleep(4)
    serial = open(LOG, "rb").read().decode("utf-8", "replace") if os.path.exists(LOG) else ""
    arrived = "[ARRIVED] NES.BIN" in serial
    launched = "[LAUNCH] NES.BIN" in serial
    print(f"[harness] serial: [LAUNCH] NES.BIN={launched}  [ARRIVED] NES.BIN={arrived}")

    ds = []
    for i in range(NFRAMES):
        pth = f"/tmp/gb035v2_f{i}.ppm"
        q.cmd("screendump", filename=pth)
        time.sleep(0.35)
        while not os.path.exists(pth) or os.path.getsize(pth) < 1000:
            time.sleep(0.1)
        d = digest(pth)
        ds.append(d)
        print(f"[harness] frame {i:02d} digest {d}", flush=True)
        time.sleep(0.6)
    print(f"\n[harness] frames={len(ds)} distinct={len(set(ds))} -> {sorted(set(ds))}")
    p.kill()
    return 0 if len(set(ds)) >= 3 else 1


if __name__ == "__main__":
    sys.exit(main())