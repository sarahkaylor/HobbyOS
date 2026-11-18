"""tools/cp/p5/p5lib.py -- shared pieces of the Gate P5 harnesses (lane C5-vk2):
the X-session QEMU command, serial polling, the QMP screendump census.
"""
import fcntl
import hashlib
import os
import shutil
import sys
import time

CLEAR = (15, 18, 28)  # every P5 probe clears to (0.06, 0.07, 0.11) * 255


def qemu_cmd(work, disk, args):
    qmp = os.path.join(work, "qmp-sock")
    serial = os.path.join(work, "serial.log")
    fw = os.path.join(work, "ovmf.fd")
    if not os.path.exists(fw):
        shutil.copy(os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"), fw)
    # The run_doom_elf_test.py / run_icewm_test.py device set (the one that
    # reaches the X session), with this gate's -smp/-m/-cpu.
    cmd = ["qemu-system-x86_64", "-M", "q35", "-smp", str(args.smp), "-m", f"{args.mem}M",
           "-drive", f"if=pflash,format=raw,file={fw}",
           "-display", "none", "-serial", f"file:{serial}",
           "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
           "-drive", f"file={disk},format=raw,id=disk0,if=none",
           "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
           "-device", "edu", "-netdev", "user,id=net0",
           "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
           "-device", "virtio-gpu-pci", "-vga", "none",
           "-action", "shutdown=poweroff",
           "-qmp", f"unix:{qmp},server,nowait",
           "-enable-kvm", "-cpu", args.cpu]
    if os.environ.get("CP_DIAG"):  # C0-console: diagnostic rows back on (docs/cp-console.md)
        cmd += ["-fw_cfg", "name=opt/hobbyos/diag,string=" + os.environ["CP_DIAG"]]
    if os.environ.get("CP_AUDIO_WAV"):  # C4-audio: opt-in virtio-sound -> WAV file (docs/cp-audio.md)
        cmd += ["-audiodev", "wav,id=cpsnd0,path=" + os.environ["CP_AUDIO_WAV"] +
                ",out.frequency=48000,out.channels=2,out.format=s16",
                "-device", "virtio-sound-pci,audiodev=cpsnd0,streams=1"]
    return cmd, qmp, serial


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def wait_for(serial, needle, secs, proc):
    end = time.time() + secs
    while time.time() < end:
        if needle in read(serial):
            return True
        if proc.poll() is not None:
            break
        time.sleep(0.5)
    return needle in read(serial)


def ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i + 1:i + 1 + w * h * 3]


def census(path, clear=CLEAR):
    """clear-colour pixels, saturated pixels, bbox of clear colour, region hash"""
    w, h, px = ppm(path)
    nclear = nsat = 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            o = row + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            if abs(r - clear[0]) <= 3 and abs(g - clear[1]) <= 3 and abs(b - clear[2]) <= 3:
                nclear += 4
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
            elif max(r, g, b) - min(r, g, b) > 80 and max(r, g, b) > 120:
                nsat += 4
    hsh = ""
    if x1 >= 0:
        hh = hashlib.sha256()
        for y in range(y0, y1 + 1):
            hh.update(px[(y * w + x0) * 3:(y * w + x1 + 1) * 3])
        hsh = hh.hexdigest()[:16]
    return {"clear": nclear, "sat": nsat, "bbox": (x0, y0, x1, y1), "hash": hsh, "size": (w, h)}


def acquire_slot():
    """Take one QEMU slot exactly as tools/cp/qemu-slot.sh does (docs/cyberpunk.md
    §7.4): slot 1 = /tmp/cp-qemu.lock, slots 2..CP_QEMU_SLOTS (default 4) =
    /tmp/cp-qemu.lock.<n>; flock(LOCK_EX|LOCK_NB) on each in turn, retry every
    3 s.  Inside an outer qemu-slot.sh (CP_QEMU_SLOT_HELD set) take nothing.
    Returns (file object or None, slot number); release_slot() frees it."""
    held = os.environ.get("CP_QEMU_SLOT_HELD")
    if held:
        return None, int(held)
    n = int(os.environ.get("CP_QEMU_SLOTS", "4"))
    said = False
    while True:
        for i in range(1, n + 1):
            path = "/tmp/cp-qemu.lock" if i == 1 else f"/tmp/cp-qemu.lock.{i}"
            f = open(path, "a")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                f.close()
                continue
            print(f"qemu-slot: slot {i} of {n} held by pid {os.getpid()} {time.strftime('%H:%M:%S')}",
                  file=sys.stderr, flush=True)
            return f, i
        if not said:
            print(f"qemu-slot: all {n} slots busy; waiting", file=sys.stderr, flush=True)
            said = True
        time.sleep(3)


def release_slot(f):
    if f is not None:
        fcntl.flock(f, fcntl.LOCK_UN)
        f.close()


def guests_running():
    """How many QEMU guests run on this host right now (for labelling fps rows)."""
    try:
        out = os.popen("pgrep -c -f '^qemu-system-x86_64 '").read().strip()
        return int(out or "0")
    except ValueError:
        return -1
