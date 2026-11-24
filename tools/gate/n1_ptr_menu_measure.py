#!/usr/bin/env python3
"""n1_ptr_menu_measure.py -- WP-PTR: dismiss the Steam modal WITH A CLICK, prove
the frame, and measure the menu's own fps.

Round 3 (WP-N1) measured 51.02 fps median on a screen that was still the
"Could not initialize Steam API" modal: 72 presses on Ignore did nothing
because the compat libX11 delivered button events to Unity's container XID,
which had selected StructureNotifyMask only.  This run boots the same image
with the fixed libX11 and answers one question: does a press now dismiss the
modal, and if so, what does the game's own menu present at?

Rows it leaves (all from the guest, none from a host clock):
  [XSW] n= t= crc=   the compat libX11's present counter and the guest's own
                     CLOCK_MONOTONIC since the first present (ms) -- fps is
                     dn/dt between consecutive rows, median over intervals;
  [FRAME] t=.. crc=.. -> file.raw   QMP pmemsave of the kernel framebuffer at
                     0x72000000 (1920x1080 BGRA).  pmemsave is NOT atomic: a
                     frame can be torn, so the script dumps more than one and
                     the report only counts frames whose contents were looked
                     at.

Usage: n1_ptr_menu_measure.py --disk /root/n1/disk-ptr.img --log ... --qmp ...
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time


def log(m):
    print(m, flush=True)


class Qmp:
    def __init__(self, path, wait=300):
        self.buf = b""
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        for _ in range(wait):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.greeting()
        self.cmd("qmp_capabilities")

    def greeting(self):
        while True:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                if line.strip():
                    try:
                        return json.loads(line)
                    except Exception:
                        continue
            else:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return {}
                self.buf += chunk

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    resp = json.loads(line)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp
            else:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return {"error": "eof"}
                self.buf += chunk

    def key(self, qcode, pause=0.3):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def move(self, ax, ay):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]})

    def click(self, ax, ay, hold=0.15):
        """A real press/release pair, the way a user clicks: move, down, wait,
        up.  One batched event would deliver the release in the same input
        sync and a Unity IMGUI control can read that as no click at all."""
        self.move(ax, ay)
        time.sleep(0.12)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(hold)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]})

    def dump(self, phys, size, path):
        return self.cmd("pmemsave", {"val": phys, "size": size, "filename": path})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--qmp", default="/root/n1/qmp-ptr")
    ap.add_argument("--ovmf", default="/root/n1/OVMF_CODE_4M.fd")
    ap.add_argument("--app", default="RWLAUNCH.BIN")
    ap.add_argument("--px", default="485,588")
    ap.add_argument("--fb-phys", default="0x72000000")
    ap.add_argument("--fb-w", type=int, default=1920)
    ap.add_argument("--fb-h", type=int, default=1080)
    ap.add_argument("--prefix", default="/root/n1/p")
    ap.add_argument("--boot-timeout", type=int, default=300)
    ap.add_argument("--arm-at", type=int, default=600)
    ap.add_argument("--deadline", type=int, default=2000)
    ap.add_argument("--menu-s", type=int, default=150)
    ap.add_argument("--max-rounds", type=int, default=12)
    ap.add_argument("--smp", type=int, default=8)
    ap.add_argument("--mem", type=int, default=12288)
    a = ap.parse_args()

    if os.path.exists(a.qmp):
        os.remove(a.qmp)
    cmd = ["qemu-system-x86_64", "-M", "q35,vmport=on", "-smp", str(a.smp),
           "-m", f"{a.mem}M", "-pflash", a.ovmf, "-display", "egl-headless,gl=on",
           "-serial", f"file:{a.log}",
           "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
           "-drive", f"file={a.disk},format=raw,id=disk0,if=none",
           "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
           "-device", "edu", "-netdev", "user,id=net0",
           "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
           "-device", "virtio-tablet-pci",
           "-device", "virtio-gpu-gl-pci", "-vga", "none",
           "-action", "shutdown=poweroff",
           "-qmp", f"unix:{a.qmp},server,nowait"]
    if os.path.exists("/dev/kvm"):
        cmd.append("-enable-kvm")
    open(a.log, "w").close()
    log("== qemu: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    def stext():
        try:
            return open(a.log, errors="ignore").read()
        except OSError:
            return ""

    def last_row():
        n = t = c = None
        for l in stext().splitlines():
            if "[XSW] n=" in l:
                m = re.search(r"\[XSW\] n=(\d+)", l)
                tt = re.search(r" t=(\d+)", l)
                cc = re.search(r" crc=(0x[0-9a-f]+)", l)
                if m and tt:
                    n, t = int(m.group(1)), int(tt.group(1))
                    c = cc.group(1) if cc else c
        return n, t, c

    def fps_rows():
        seq, prev = [], None
        for l in stext().splitlines():
            if "[XSW] n=" in l:
                m = re.search(r"\[XSW\] n=(\d+)", l)
                tt = re.search(r" t=(\d+)", l)
                if m and tt:
                    k = (int(m.group(1)), int(tt.group(1)))
                    if k != prev:
                        seq.append(k)
                        prev = k
        return seq

    def fps_window(t_from, t_to):
        seq = [k for k in fps_rows() if t_from <= k[1] <= t_to]
        rates = []
        for a0, b in zip(seq, seq[1:]):
            if b[1] > a0[1]:
                rates.append(((b[0] - a0[0]) * 1000.0 / (b[1] - a0[1]), b[1] - a0[1]))
        return rates

    try:
        end = time.time() + a.boot_timeout
        while time.time() < end and "[MENU] count=" not in stext():
            time.sleep(1)
        menu = {}
        for line in stext().splitlines():
            m = re.search(r"\[MENU\]\s*(\d+)=(\S+)", line)
            if m:
                menu[int(m.group(1))] = m.group(2)
        idx = next((i for i, nm in menu.items()
                    if nm.upper().split(".")[0] == a.app.upper().split(".")[0]), None)
        if idx is None:
            log("[FAIL] app not in menu")
            return 1
        time.sleep(3)
        q = Qmp(a.qmp)
        ups = max(menu) - idx
        log(f"[INFO] launching {a.app} (index {idx}/{len(menu)}, {ups} ups)")
        q.key("f1")
        time.sleep(0.6)
        q.key("end")
        for _ in range(ups):
            q.key("up", pause=0.12)
        q.key("ret")
        log("[INPUT] on-router row: " + next(
            (l for l in stext().splitlines() if "[INPUT] vmmouse" in l), "(absent)"))

        t0 = time.time()
        cx, cy = [int(v) for v in a.px.split(",")]
        ax = int(cx * 32767 / a.fb_w)
        ay = int(cy * 32767 / a.fb_h)
        seen = []
        loader_crc = None
        dismissed_crc = None
        presses = rounds = 0
        dumps = 0
        dismissed_at = None
        last_change = time.time()
        last_n, last_n_t = 1, time.time()
        menu_lo_ms = None
        dismiss_ms = None
        while time.time() - t0 < a.deadline:
            el = int(time.time() - t0)
            n, t, c = last_row()
            if n is None:
                # No present yet (boot / Unity still initialising): the rate
                # computation below needs a number, not None.
                time.sleep(2)
                continue
            if c and loader_crc is None:
                # The first presented picture is the loading art (measured:
                # constant crc while the game loads its 1209 def XMLs, still
                # presenting at ~48 fps).  Pressing on THAT is what run A did by
                # mistake, so the modal is only armed for once the picture has
                # changed.
                loader_crc = c
                log(f"[LOADER] first crc {c} at t={el}s")
            if c and (not seen or seen[-1][1] != c):
                seen.append((el, c, n))
                last_change = time.time()
                log(f"[FRAME] t={el}s new crc {c} n={n}")
                if dismissed_at is not None and dumps < 8:
                    dumps += 1
                    f = f"{a.prefix}-menu{dumps}-{el}s.raw"
                    q.dump(int(a.fb_phys, 16), a.fb_w * a.fb_h * 4, f)
                    log(f"[DUMP] {f}")
            # Present rate over the last few seconds: a "still" screen that is
            # still PRESENTING at ~50 fps is the game sitting on a screen; one
            # that presents 10 times in 160 s is loading.  Round 3 measured both
            # (the load->menu stalls at t=756 s and t=790 s), so a press armed
            # on "crc unchanged" alone fires on the loading screen.
            now = time.time()
            rate = 0.0
            if now - last_n_t >= 5:
                rate = (n - last_n) / (now - last_n_t)
                last_n, last_n_t = n, now
            stable = now - last_change
            if (dismissed_at is not None and menu_lo_ms is None and c
                    and dismiss_ms is not None and t > dismiss_ms + 1500
                    and rate > 20.0 and c != dismissed_crc):
                # Presents have resumed at a fast rate on a NEW frame: the menu
                # is on screen now.  This is the window the fps is measured in.
                menu_lo_ms = t
                log(f"[MENU] presents resumed t={el}s crc={c} rate={rate:.1f} "
                    f"guest_ms={t}")
                for k in (1, 2):
                    f = f"{a.prefix}-menu{k}-{el}s.raw"
                    q.dump(int(a.fb_phys, 16), a.fb_w * a.fb_h * 4, f)
                    log(f"[DUMP] {f}")
            if (dismissed_at is None and el >= a.arm_at and c
                    and loader_crc is not None and c != loader_crc
                    and stable > 20 and rate > 20.0
                    and rounds < a.max_rounds):
                # The same picture for 20 s AND the game presenting it at >20
                # fps: this is the modal, not the loader.
                off_x, off_y = (int(200 * 32767 / a.fb_w),
                                int(200 * 32767 / a.fb_h))
                if rounds == 0:
                    before = f"{a.prefix}-modal-{el}s.raw"
                    q.dump(int(a.fb_phys, 16), a.fb_w * a.fb_h * 4, before)
                    log(f"[DUMP] {before}")
                # Baseline CRC with the pointer OFF the button: a hover change
                # also changes the presented CRC (round 3 recorded three hover
                # CRCs of one dialog), so the press is judged against an
                # off-button baseline taken the same way after it.
                q.move(off_x, off_y)
                time.sleep(1.5)
                _, _, c_off = last_row()
                q.move(ax, ay)
                time.sleep(0.3)
                for _ in range(3):
                    q.click(ax, ay)
                    presses += 1
                    time.sleep(0.4)
                rounds += 1
                q.move(off_x, off_y)
                time.sleep(2.0)
                n2, t2, c2 = last_row()
                log(f"[PRESS] t={el}s rounds={rounds} presses={presses} "
                    f"crc_off_before={c_off} crc_off_after={c2} rate={rate:.1f}")
                if c2 and c_off and c2 != c_off:
                    dismissed_at = time.time()
                    dismiss_ms = t2
                    dismissed_crc = c_off
                    log(f"[DISMISSED] t={el}s {c_off} -> {c2} after {presses} presses")
            elif dismissed_at is not None and menu_lo_ms is not None:
                if t > menu_lo_ms + a.menu_s * 1000:
                    break
            time.sleep(2)

        # Once dismissal is seen, the game loads the menu behind it (measured in
        # round 3: 164 s of stalls at the load->menu transition).  The menu
        # window starts only when presents resume quickly with a NEW crc.
        if dismissed_at is not None and menu_lo_ms is None:
            log("[WAIT] presents did not resume after the dismissal inside the deadline")

        if dismissed_at is None:
            f = f"{a.prefix}-still-modal.raw"
            q.dump(int(a.fb_phys, 16), a.fb_w * a.fb_h * 4, f)
            log(f"[DUMP] {f}")
            log(f"[RESULT] modal never came off after {presses} presses in {rounds} rounds")
        elif menu_lo_ms is not None:
            # The menu window: from 5 s after presents resumed (so the first
            # re-layout frame is not counted) for menu-s of GUEST time.
            t_lo = menu_lo_ms + 5000
            t_hi = menu_lo_ms + a.menu_s * 1000
            rates = fps_window(t_lo, t_hi)
            clean = sorted(r for r, dt in rates if dt < 2000)
            if clean:
                v = clean
                log(f"[MENU-FPS] window=+{20}s..+{20 + a.menu_s}s intervals={len(v)} "
                    f"median={v[len(v)//2]:.2f} p10={v[int(.1*len(v))]:.2f} "
                    f"p90={v[int(.9*len(v))]:.2f} min={v[0]:.2f} max={v[-1]:.2f}")
            else:
                log("[MENU-FPS] no clean intervals in the menu window")
            # And the whole-run rate for the record.
            allr = sorted(r for r, dt in fps_window(0, 10 ** 9) if dt < 2000)
            if allr:
                log(f"[ALL-FPS] intervals={len(allr)} median={allr[len(allr)//2]:.2f}")
        n, t, c = last_row()
        log(f"[TOTAL] presents={n} guest_ms={t} last_crc={c}")
        log("[CRCS] " + "; ".join(f"t={tt}s {cc} n={nn}" for tt, cc, nn in seen))
    finally:
        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=30)
        except Exception:
            proc.kill()
        log("[QEMU] terminated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
