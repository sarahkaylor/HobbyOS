#!/usr/bin/env python3
"""tools/cp/p4/p4_gate.py -- boot + drive + grade Gate P4 (lane C4-x2).

Called by tools/cp/p4/run-p4.sh (which builds, exports and stages) inside one
QEMU slot (tools/cp/qemu-slot.sh).  One boot of a DESKTOP=icewm disk whose /XSTART.TXT runs
WINEP4.BIN (tools/cp/p4/winep4.c) in the X session.  This script follows the
serial log and drives the guest over QMP the way a user would:

  rawinput  after "[RI] phase free ready": 10 x rel(+7,+3);
            after "[RI] phase clip ready": 10 x rel(+7,+3), 5 x rel(-4,-9),
            then 20 x rel(+40,+2) -- the last batch pushes the pointer into
            the clip rectangle's right edge, so the sums only come out exact
            if the deltas are the device's, not the clamped cursor's
            (XI2 raw motion).  Expected clip sums: dx=850 dy=25.
  modes     the probe changes to 1280x720 and holds 4 s: screendump.
  winecfg   finds the dialog in a screendump (its 245-grey face), clicks the
            Libraries, Graphics and Audio tabs (positions measured on the host
            reference, relative to the dialog), screendumps after each, then
            clicks Cancel.
  notepad   finds the window, clicks into it, types "Hello from HobbyOS P4"
            (QMP send-key), screendumps, then Alt+F4 and "n" (don't save).

Grades (all required for PASS):
  G1 winecfg  the window appeared, every tab click changed the dialog
              (region hash differs from the previous dump), the Audio tab
              did not hang it (Cancel closed it: winecfg step status=0).
  G2 notepad  the window appeared, the text area changed after typing
              (dark text pixels appeared), notepad exited status=0.
  G3 raw      "[RI] phase clip total ... rel=N ... dx=850 dy=25 ... inside=1"
              with rel>0, and "[RI] done rc=0".
  Info rows (not graded): the free phase sums, the modes rows, Xinerama/
  Composite are judged from the X server rows by run-p4.sh.
"""
import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time

W, H = 1920, 1080


def log(m):
    print(f"[P4G] {m}", flush=True)


class Qmp:
    def __init__(self, path, deadline_s=60):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(20)
        end = time.time() + deadline_s
        while True:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > end:
                    raise RuntimeError("QMP socket never appeared")
                time.sleep(0.25)
        self.buf = b""
        self._line()
        self.cmd("qmp_capabilities")

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self._line()
            if "return" in r or "error" in r:
                return r

    def shot(self, path):
        return "return" in self.cmd("screendump", {"filename": path})

    def rel(self, dx, dy):
        ev = []
        if dx:
            ev.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            ev.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        return self.cmd("input-send-event", {"events": ev})

    def move(self, x, y):
        return self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": x * 32767 // W}},
            {"type": "abs", "data": {"axis": "y", "value": y * 32767 // H}}]})

    def click(self, x, y):
        self.move(x, y)
        time.sleep(0.15)
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.12)
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]})

    def keys(self, *qcodes):
        return self.cmd("send-key", {"keys": [{"type": "qcode", "data": q} for q in qcodes]})


QCODE = {" ": ["spc"]}
for ch in "abcdefghijklmnopqrstuvwxyz":
    QCODE[ch] = [ch]
    QCODE[ch.upper()] = ["shift", ch]
for ch in "0123456789":
    QCODE[ch] = [ch]


def type_text(q, text):
    for ch in text:
        q.keys(*QCODE[ch])
        time.sleep(0.12)


def ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        return None, 0, 0
    fields, i = [], 2
    while len(fields) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    return data[i + 1:], fields[0], fields[1]


def bbox_of(path, rgb, step=2, min_count=4000):
    """Bounding box of the pixels of exactly `rgb` (sampled), or None."""
    px, w, h = ppm(path)
    if not px:
        return None
    want = bytes(rgb)
    xs, ys, n = [], [], 0
    for y in range(0, h, step):
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(0, w, step):
            if row[x * 3:x * 3 + 3] == want:
                xs.append(x)
                ys.append(y)
                n += 1
    if n * step * step < min_count:
        return None
    return min(xs), min(ys), max(xs), max(ys), n * step * step


def region_hash(path, box):
    px, w, h = ppm(path)
    x0, y0, x1, y1 = box
    hsh = hashlib.sha256()
    for y in range(max(0, y0), min(h, y1)):
        hsh.update(px[(y * w + max(0, x0)) * 3:(y * w + min(w, x1)) * 3])
    return hsh.hexdigest()[:16]


def count_rgb(path, box, pred):
    px, w, h = ppm(path)
    x0, y0, x1, y1 = box
    n = 0
    for y in range(max(0, y0), min(h, y1)):
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(max(0, x0), min(w, x1)):
            if pred(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]):
                n += 1
    return n


class Serial:
    def __init__(self, path, proc):
        self.path, self.proc, self.pos, self.text = path, proc, 0, ""

    def poll(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                chunk = f.read()
        except FileNotFoundError:
            return
        self.pos += len(chunk)
        self.text += chunk.decode("utf-8", "replace").replace("\r", "")

    def wait(self, pattern, timeout, since=0):
        rx = re.compile(pattern)
        t0 = time.time()
        while time.time() - t0 < timeout:
            self.poll()
            m = rx.search(self.text, since)
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.5)
        return None


def qemu_cmd(out, disk, ovmf, mem):
    qmp = os.path.join(out, "qmp.sock")
    serial = os.path.join(out, "serial.log")
    cmd = ["qemu-system-x86_64", "-M", "q35,vmport=on", "-enable-kvm", "-cpu", "host",
           "-smp", "8", "-m", mem,
           "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
           "-display", "none", "-serial", f"file:{serial}",
           "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
           "-drive", f"file={disk},format=raw,id=disk0,if=none",
           "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
           "-device", "edu", "-netdev", "user,id=net0",
           "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
           "-device", "virtio-gpu-pci", "-vga", "none",
           "-action", "shutdown=poweroff",
           "-qmp", f"unix:{qmp},server,nowait"]
    return cmd, qmp, serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--ovmf", required=True)
    ap.add_argument("--mem", default="12288M")
    ap.add_argument("--boot-timeout", type=int, default=4200)
    a = ap.parse_args()
    out = a.out
    cmd, qmp_path, serial = qemu_cmd(out, a.disk, a.ovmf, a.mem)
    for p in (qmp_path, serial):
        if os.path.exists(p):
            os.remove(p)
    log("qemu: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))
    log(f"qemu pid {proc.pid}")
    ser = Serial(serial, proc)
    res = {"G1": "FAIL", "G2": "FAIL", "G3": "FAIL"}
    notes = []
    shot_n = [0]

    def shot(name):
        p = os.path.join(out, f"{shot_n[0]:02d}-{name}.ppm")
        shot_n[0] += 1
        q.shot(p)
        time.sleep(0.5)
        return p

    try:
        q = Qmp(qmp_path)
        if not ser.wait(r"session running|\[XSESS\] XSTART: spawned", 300):
            notes.append("the X session never started")
            raise SystemExit
        time.sleep(5)
        shot("session")
        if not ser.wait(r"\[P4\] mount .* ok", 120):
            notes.append("NFS mount failed")
            raise SystemExit
        if not ser.wait(r"\[P4\] begin rawinput", a.boot_timeout):
            notes.append("never reached the rawinput step (wineboot)")
            raise SystemExit
        # --- rawinput ---------------------------------------------------------
        m = ser.wait(r"\[RI\] phase free ready", 300)
        if m:
            time.sleep(1)
            for _ in range(10):
                q.rel(7, 3)
                time.sleep(0.06)
            ser.wait(r"\[RI\] phase free total[^\n]*", 120)
        m = ser.wait(r"\[RI\] phase clip ready", 300)
        if m:
            time.sleep(2)
            for _ in range(10):
                q.rel(7, 3)
                time.sleep(0.06)
            for _ in range(5):
                q.rel(-4, -9)
                time.sleep(0.06)
            for _ in range(20):
                q.rel(40, 2)
                time.sleep(0.06)
            time.sleep(0.5)
            shot("rawinput-clip")
        m = ser.wait(r"\[RI\] phase clip total[^\n]*", 200)
        if m:
            row = m.group(0)
            log("clip row: " + row)
            g = dict(re.findall(r"(\w+)=(-?\d+)", row))
            if (g.get("dx") == "850" and g.get("dy") == "25" and int(g.get("rel", "0")) > 0
                    and "inside=1" in row):
                res["G3"] = "PASS"
        ser.wait(r"\[P4\] end rawinput", 120)
        if not re.search(r"\[RI\] done rc=0", ser.text):
            res["G3"] = "FAIL"
        # --- modes --------------------------------------------------------------
        if ser.wait(r"\[RI\] modes hold", 200):
            time.sleep(1.5)
            p = shot("modes-1280x720")
            b = bbox_of(p, (255, 0, 255), min_count=1000)
            log(f"modes: fullscreen (magenta) window bbox on the glass {b}")
            notes.append(f"info: fullscreen window bbox {b} (expect (0, 0, 1278, 718) at 1280x720)")
        ser.wait(r"\[P4\] end modes", 200)
        # --- winecfg ------------------------------------------------------------
        if ser.wait(r"\[P4\] begin winecfg", 200):
            box = None
            t0 = time.time()
            while time.time() - t0 < 300 and box is None:
                time.sleep(4)
                p = shot("winecfg-wait")
                b = bbox_of(p, (245, 245, 245))
                if b and 300 < b[2] - b[0] < 700 and 300 < b[3] - b[1] < 700:
                    box = b
            if box:
                x0, y0 = box[0], box[1]
                region = (x0, y0, box[2] + 1, box[3] + 1)
                log(f"winecfg dialog face bbox {box}")
                time.sleep(3)
                prev = region_hash(shot("winecfg-0-applications"), region)
                changed = 0
                # (dx, dy) of each tab label from the dialog's top-left
                # (host reference, tools/cp/p4/host_ref.py winecfg-0.png).
                for name, (dx, dy) in (("libraries", (141, 36)), ("graphics", (221, 36)),
                                       ("audio", (203, 18))):
                    q.click(x0 + dx, y0 + dy)
                    time.sleep(4)
                    p = shot(f"winecfg-{name}")
                    hcur = region_hash(p, region)
                    log(f"winecfg tab {name}: region hash {prev} -> {hcur}")
                    if hcur != prev:
                        changed += 1
                    prev = hcur
                q.click(x0 + 282, y0 + 427)   # Cancel
                time.sleep(3)
                shot("winecfg-closed")
                m = ser.wait(r"\[P4\] end winecfg status=(\S+)", 120)
                st = m.group(1) if m else "?"
                log(f"winecfg: {changed}/3 tab clicks changed the dialog; exit status {st}")
                if changed == 3 and st == "0":
                    res["G1"] = "PASS"
            else:
                notes.append("winecfg dialog never found in a screendump")
            ser.wait(r"\[P4\] end winecfg", 900)
        # --- notepad ------------------------------------------------------------
        if ser.wait(r"\[P4\] begin notepad", 300):
            box = None
            t0 = time.time()
            while time.time() - t0 < 300 and box is None:
                time.sleep(4)
                p = shot("notepad-wait")
                b = bbox_of(p, (255, 255, 255), min_count=40000)
                if b and b[2] - b[0] > 200 and b[3] - b[1] > 150:
                    box = b
            if box:
                region = (box[0], box[1], box[2] + 1, box[3] + 1)
                log(f"notepad edit area bbox {box}")
                q.click((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
                time.sleep(1)
                p0 = shot("notepad-empty")
                dark0 = count_rgb(p0, region, lambda r, g, b: r < 80 and g < 80 and b < 80)
                type_text(q, "Hello from HobbyOS P4")
                time.sleep(3)
                p1 = shot("notepad-typed")
                dark1 = count_rgb(p1, region, lambda r, g, b: r < 80 and g < 80 and b < 80)
                log(f"notepad: dark pixels in the edit area {dark0} -> {dark1}")
                q.keys("alt", "f4")
                time.sleep(3)
                shot("notepad-save-prompt")
                q.keys("n")
                m = ser.wait(r"\[P4\] end notepad status=(\S+)", 120)
                st = m.group(1) if m else "?"
                log(f"notepad exit status {st}")
                if dark1 > dark0 + 150 and st == "0":
                    res["G2"] = "PASS"
            else:
                notes.append("notepad window never found in a screendump")
        ser.wait(r"\[P4\] done", 600)
    except SystemExit:
        pass
    except Exception as e:  # noqa: BLE001
        notes.append(f"driver exception: {e!r}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
    ser.poll()
    # NFS transport failures zero-fill pages (C3-nfs2): such a run is void.
    nfs_bad = {k: len(re.findall(rx, ser.text)) for k, rx in (
        ("rpc-no-reply", r"rpc: no reply from server"),
        ("nflk-rc-1", r"\[NFLK\][^\n]*rc=-1"),
        ("nfspipe-stall", r"\[NFSPIPE\] stall"))}
    log("NFS failure rows: " + " ".join(f"{k}={v}" for k, v in nfs_bad.items()))
    if any(nfs_bad.values()):
        notes.append("VOID: NFS failure rows present (pages may have been zero-filled)")
    for n in notes:
        log("NOTE: " + n)
    for k in ("G1", "G2", "G3"):
        log(f"{k} {res[k]}")
    with open(os.path.join(out, "grades.json"), "w") as f:
        json.dump({"grades": res, "notes": notes}, f, indent=1)
    return 0 if all(v == "PASS" for v in res.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
