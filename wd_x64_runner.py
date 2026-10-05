#!/usr/bin/env python3
"""
WD lane x64 windowed-browser runner (worktree-scoped QEMU).

Boots the OS (ARCH=intel MODE=desktop disk.img) under KVM q35 with
virtio-gpu-pci (-vga none so QMP screendump targets the virtio scanout),
drives F1 -> Apps menu (BROWSER preselected) -> Enter to launch the
windowed browser, waits for [WIN] frame/painted, then QMP-screendumps
and pixel-samples the window content area (expects HOME.HTM colors
#40a060 probe on #102030 bg when the OS display path works).

Usage: wd_x64_runner.py [--disk PATH] [--outdir DIR] [--tag NAME]
"""
import argparse, json, os, socket, subprocess, sys, time

W, H = 1024, 768
REPO = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print(msg, flush=True)


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        for _ in range(300):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.sock.recv(1 << 20)
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            buf += self.sock.recv(1 << 20)
            text = buf.decode(errors="ignore")
            for part in text.strip().split("\n"):
                if not part:
                    continue
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp
            if len(buf) > (1 << 20):
                raise RuntimeError("QMP response too large")

    def key(self, qcode, pause=0.5):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def shot(self, path):
        r = self.cmd("screendump", {"filename": path})
        return "return" in r


def serial_text(path):
    try:
        return open(path, errors="ignore").read()
    except OSError:
        return ""


def serial_contains(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in serial_text(path):
            return True
        time.sleep(0.4)
    return False


def sample_ppm(path, points):
    """P6 PPM from QEMU screendump; return RGB at (x,y)."""
    with open(path, "rb") as fh:
        data = fh.read()
    # header: P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = [int(x) for x in parts[1].split()]
    maxv = int(parts[2])
    assert maxv == 255, maxv
    px = parts[3]
    out = {}
    for (x, y) in points:
        if x < 0 or y < 0 or x >= w or y >= h:
            out[(x, y)] = None
            continue
        i = (y * w + x) * 3
        out[(x, y)] = tuple(px[i:i + 3])
    return w, h, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--outdir", default="/tmp/wd-x64")
    ap.add_argument("--tag", default="run")
    ap.add_argument("--mem", default="4096M")
    ap.add_argument("--ovmf", default=os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"))
    ap.add_argument("--no-kvm", action="store_true")
    ap.add_argument("--kill-only", action="store_true")
    ap.add_argument("--app", default="BROWSER.BIN",
                    help="Apps-menu program to launch (e.g. BROWSER.BIN, XEYES.BIN)")
    ap.add_argument("--pre-key", default=None,
                    help="optional extra keys before the launch (e.g. f1:menu)")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    serial = os.path.join(args.outdir, f"{args.tag}-serial.log")
    qmp_sock = os.path.join(args.outdir, f"{args.tag}-qmp.sock")
    ppm = os.path.join(args.outdir, f"{args.tag}-window.ppm")
    for p in (serial, qmp_sock, ppm):
        if os.path.exists(p):
            os.remove(p)

    if args.kill_only:
        subprocess.run(["pkill", "-9", "-f", "qemu-system-x86_64"], stderr=subprocess.DEVNULL)
        return 0

    accel = "tcg" if args.no_kvm else "kvm"
    q = subprocess.Popen([
        "qemu-system-x86_64",
        "-M", "q35", "-accel", accel, "-smp", "1", "-m", args.mem,
        "-pflash", args.ovmf,
        "-display", "none", "-vga", "none", "-device", "virtio-gpu-pci",
        "-serial", f"file:{serial}",
        "-qmp", f"unix:{qmp_sock},server,nowait",
        "-drive", f"file={args.disk},format=raw,id=disk0,if=none",
        "-device", "pcie-root-port,id=pcie.1,bus=pcie.0,slot=1",
        "-device", "nvme,drive=disk0,serial=1234,bus=pcie.1",
        "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56",
        "-action", "shutdown=poweroff",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not serial_contains(serial, "Desktop starting", 180):
            log("[FAIL] desktop never booted")
            tail(serial)
            return 1
        log("[OK] desktop booted")

        # Apps menu mapping from serial.
        mm = {}
        deadline = time.time() + 120
        while not mm and time.time() < deadline:
            for line in serial_text(serial).splitlines():
                if "[MENU]" in line:
                    for tok in line.split():
                        if "=" in tok:
                            try:
                                k, v = tok.split("=", 1)
                                mm[int(k)] = v.strip()
                            except ValueError:
                                pass
            if not mm:
                time.sleep(0.5)
        log(f"[INFO] menu: {mm}")
        if args.app not in mm.values():
            log(f"[FAIL] {args.app} not in Apps menu")
            tail(serial)
            return 1

        qmp = Qmp(qmp_sock)
        # Launch: pre-key (F1 opens the Apps menu preselected on BROWSER)
        # then Enter launches.  For a non-browser app, select it in the
        # menu first (arrow down from the top to its index).
        app_idx = [k for k, v in mm.items() if v == args.app][0]
        pinned_order = ["BROWSER.BIN", "CONSOLE.BIN", "FILES.BIN", "CALC.BIN",
                        "XCALC.BIN", "ANTFARM.BIN", "XEYES.BIN", "CLOCK.BIN",
                        "SYSMON.BIN"]
        if args.app in pinned_order:
            ui_idx = pinned_order.index(args.app)
        else:
            ui_idx = app_idx
        if args.pre_key:
            qmp.key(args.pre_key)               # F1: opens menu @ BROWSER (ui 0)
            time.sleep(1.5)
        if args.app != "BROWSER.BIN":
            for _ in range(ui_idx):
                qmp.key("down", pause=0.4)
        time.sleep(0.3)
        qmp.key("ret")
        log(f"[OK] Enter: {args.app} launching (ui_idx={ui_idx})")

        if args.app == "BROWSER.BIN":
            def try_launch():
                qmp.key("f1")               # open Apps menu (pre-selects BROWSER)
                time.sleep(1.2)
                qmp.key("ret")
                log("[OK] F1+Enter sent (browser)")
            for attempt in range(3):
                try_launch()
                if serial_contains(serial, "[WIN] BOOT", 40):
                    break
                log(f"[WARN] no [WIN] BOOT after attempt {attempt+1}; retrying")
            for needle in ("[WIN] BOOT", "[WIN] created", "[WIN] page", "[WIN] geom",
                           "[WIN] load-ok", "[WIN] frame", "[WIN] painted"):
                if not serial_contains(serial, needle, 150):
                    log(f"[FAIL] missing {needle}")
                else:
                    log(f"[OK] {needle}")
        else:
            log(f"[INFO] launching {args.app}; waiting for a stable frame")
            time.sleep(6)
            for needle in ("[WIN] created", "[WIN] geom"):
                serial_contains(serial, needle, 60)
            time.sleep(1.5)
        time.sleep(1.5)
        ok = qmp.shot(ppm)
        log(f"[INFO] screendump -> {ppm} ok={ok}")
        if not ok:
            tail(serial)
            return 1

        w, h, px = sample_ppm(ppm, [
            (102, 100), (800, 300), (102, 650), (512, 640), (512, 300), (40, 200)])
        log(f"[PIXEL] screendump {w}x{h}:")
        for (x, y), rgb in px.items():
            log(f"  ({x},{y}) = {rgb}")
        # Save summary
        with open(os.path.join(args.outdir, f"{args.tag}-pixels.json"), "w") as fh:
            json.dump({"size": [w, h], "pixels": {f"{x},{y}": list(rgb) if rgb else None
                                                   for (x, y), rgb in px.items()}}, fh, indent=2)
        log(f"[INFO] serial tail ({os.path.getsize(serial)} B):")
        tail(serial)
        return 0
    finally:
        q.terminate()
        try:
            q.wait(timeout=8)
        except subprocess.TimeoutExpired:
            q.kill()


def tail(serial, n=60):
    lines = serial_text(serial).splitlines()
    for ln in lines[-n:]:
        log("  | " + ln)


if __name__ == "__main__":
    sys.exit(main())
