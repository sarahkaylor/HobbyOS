#!/usr/bin/env python3
"""tools/gate/xptr_click.py -- WP-PTR gate: does a compat X11 client see a
mouse BUTTON?

The N1 measurement found pointer motion and keys reaching a compat client
while 72 mouse presses on a modal button did nothing.  This gate removes the
game from the question: it boots the same desktop (MODE=desktop DESKTOP=hobby,
q35 vmport=on + virtio-tablet-pci, the r1 device set), launches an ordinary
glibc Xlib client (tests/fixtures/ptr/xptr.c) from the Apps menu with real key
events, moves the pointer over the client's own content rect, sends three
left-button press/release pairs over QMP, and grades the client's rows:

    [XP] ButtonPress   -- the client saw a press        (required)
    [XP] ButtonRelease -- the client saw the release    (required)
    [XP] done ... rc=0 -- the client's own verdict      (required)

Exit 0 = green, 1 = red.  Red is the pre-fix state: motion rows and no press.

The image is staged in place (src/user/xcompat/stage.sh + the pinned glibc
2.31 + /XPTR.BIN), exactly as tools/gate/m3_x11_client.py stages /XCLIENT.BIN.

Usage:
    ./tools/gate/xptr_click.py                 # build client, stage, boot, grade
    XPTR_SKIP_BUILD=1 ./tools/gate/xptr_click.py   # reuse obj/xptr/disk.img
"""
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARCH = "intel"
IMG = os.environ.get("XPTR_IMG", os.path.join(ROOT, "disk.img"))
LOG = os.environ.get("XPTR_LOG", os.path.join(ROOT, "obj", "xptr_click.log"))
SHOT_DIR = os.environ.get("XPTR_SHOT_DIR", os.path.join(ROOT, "obj", "xptr-shots"))
QMP_SOCK = os.path.join(ROOT, "qmp-sock-xptr")
BUILD = os.path.join(ROOT, "obj", "xptr")
GPIN = os.environ.get("GLIBC231", "/tmp/glibc231")
DPIN = os.environ.get("GLIBC231_DEV", "/tmp/glibc231-dev")
BOOT_TIMEOUT = int(os.environ.get("XPTR_BOOT_TIMEOUT", "240"))
HOLD_MS = os.environ.get("XPTR_HOLD_MS", "25000")
CLICKS = int(os.environ.get("XPTR_CLICKS", "3"))

LIBC6_SHA = "1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0"
LIBGCC_SHA = "4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c"
LIBC6DEV_SHA = "11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a"
URL_LIBC6 = "http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb"
URL_LIBGCC = ("http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/"
              "libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb")
URL_LIBC6DEV = ("http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/"
                "libc6-dev_2.31-0ubuntu9.18_amd64.deb")


def log(m):
    print(m, flush=True)


def fetch_deb(url, sha, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if not os.path.exists(dest):
        log(f"xptr: fetching {os.path.basename(dest)}")
        subprocess.run(["curl", "-fsS", "-o", dest, url], check=True)
    h = hashlib.sha256(open(dest, "rb").read()).hexdigest()
    if h != sha:
        raise SystemExit(f"xptr: SHA256 MISMATCH {dest}: {h}")


def ensure_pins():
    ld = os.path.join(GPIN, "lib/x86_64-linux-gnu/ld-2.31.so")
    if not os.path.exists(ld):
        t = os.path.join(ROOT, "obj", "m1-pins")
        fetch_deb(URL_LIBC6, LIBC6_SHA, os.path.join(t, "libc6.deb"))
        fetch_deb(URL_LIBGCC, LIBGCC_SHA, os.path.join(t, "libgcc-s1.deb"))
        shutil.rmtree(GPIN, ignore_errors=True)
        os.makedirs(GPIN, exist_ok=True)
        subprocess.run(["dpkg-deb", "-x", os.path.join(t, "libc6.deb"), GPIN], check=True)
        subprocess.run(["dpkg-deb", "-x", os.path.join(t, "libgcc-s1.deb"), GPIN], check=True)
    scrt = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu/Scrt1.o")
    if not os.path.exists(scrt):
        t = os.path.join(ROOT, "obj", "m1-pins")
        fetch_deb(URL_LIBC6DEV, LIBC6DEV_SHA, os.path.join(t, "libc6-dev.deb"))
        shutil.rmtree(DPIN, ignore_errors=True)
        os.makedirs(DPIN, exist_ok=True)
        subprocess.run(["dpkg-deb", "-x", os.path.join(t, "libc6-dev.deb"), DPIN], check=True)


def build_client():
    os.makedirs(BUILD, exist_ok=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    dlib = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu")
    out = os.path.join(BUILD, "xptr")
    cmd = [
        "gcc", "-O2", "-std=gnu11", "-fPIE", "-pie", "-nostdlib", "-nostartfiles",
        "-o", out, os.path.join(ROOT, "tests/fixtures/ptr/xptr.c"),
        os.path.join(dlib, "Scrt1.o"), os.path.join(dlib, "crti.o"),
        "-lX11",
        os.path.join(ldir, "libdl-2.31.so"),
        os.path.join(dlib, "libc_nonshared.a"),
        os.path.join(ldir, "libc.so.6"),
        os.path.join(dlib, "crtn.o"),
        "-Wl,--allow-shlib-undefined",
        "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2",
        f"-Wl,-rpath-link,{ldir}",
        "-Wl,--build-id=none",
    ]
    log("xptr: building the client")
    subprocess.run(cmd, check=True)
    needed = subprocess.run(["readelf", "-dW", out], capture_output=True, text=True).stdout
    log("xptr: client DT_NEEDED: " + ", ".join(
        re.findall(r"Shared library: \[([^\]]+)\]", needed)))
    return out


def build_os():
    log("xptr: building kernel + disk (desktop)")
    env = dict(os.environ)
    env["TOOLS_SKIP"] = "1"
    env["BROWSER_SKIP"] = "1"
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    r = subprocess.run(["make", "-C", ROOT, f"ARCH={ARCH}", "MODE=desktop",
                        "DESKTOP=hobby", "TOOLS_SKIP=1", "BROWSER_SKIP=1",
                        "hobbyos.elf", "disk.img"],
                       env=env, capture_output=True, text=True)
    os.makedirs(os.path.join(ROOT, "obj"), exist_ok=True)
    open(os.path.join(ROOT, "obj", "xptr_build.log"), "w").write(r.stdout + r.stderr)
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-25:])
        raise SystemExit("xptr: BUILD FAILED\n" + tail)


def stage(client):
    mk = ["make", "-C", ROOT, f"ARCH={ARCH}", "xcompat_dsos"]
    if os.environ.get("XCOMPAT_NO_PTR_FIX"):
        mk.append("XCOMPAT_NO_PTR_FIX=1")   # the A/B knob: pre-fix routing
    subprocess.run(mk, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["bash", os.path.join(ROOT, "src/user/xcompat/stage.sh"), IMG], check=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")

    def mmd(p):
        subprocess.run(["mmd", "-i", IMG, p], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

    def mc(src, dst):
        subprocess.run(["mcopy", "-o", "-i", IMG, src, dst], check=True)

    for d in ("::/LIB64", "::/lib/x86_64-linux-gnu"):
        mmd(d)
    for src, dst in {
        "ld-2.31.so": "ld-linux-x86-64.so.2",
        "libc-2.31.so": "libc.so.6",
        "libm-2.31.so": "libm.so.6",
        "libpthread-2.31.so": "libpthread.so.0",
        "libdl-2.31.so": "libdl.so.2",
        "librt-2.31.so": "librt.so.1",
        "libgcc_s.so.1": "libgcc_s.so.1",
    }.items():
        mc(os.path.join(ldir, src), f"::/LIB64/{dst}")
    mc(os.path.join(ldir, "ld-2.31.so"), "::/lib64/ld-linux-x86-64.so.2")
    mc(client, "::XPTR.BIN")


class Qmp:
    def __init__(self, path):
        self.buf = b""
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(20)
        for _ in range(600):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.greeting()          # {"QMP": {...}} -- not a return/error
        self.cmd("qmp_capabilities")

    def greeting(self):
        """Read exactly one QMP message (the greeting has neither key)."""
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

    def resp(self):
        while True:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                except Exception:
                    continue
                if "return" in r or "error" in r:
                    return r
            else:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return {"error": "eof"}
                self.buf += chunk

    def cmd(self, execute, arguments=None):
        m = {"execute": execute}
        if arguments:
            m["arguments"] = arguments
        self.sock.sendall(json.dumps(m).encode() + b"\n")
        return self.resp()

    def key(self, qcode, pause=0.35):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def move(self, px, py, pause=0.4):
        ax = round(px / 1920.0 * 32767)
        ay = round(py / 1080.0 * 32767)
        r = self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]})
        if r.get("error"):
            log(f"[QMP] abs -> {r}")
        time.sleep(pause)

    def click(self, pause=0.5):
        r1 = self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.18)
        r2 = self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}}]})
        if r1.get("error") or r2.get("error"):
            log(f"[QMP] btn -> {r1} {r2}")
        time.sleep(pause)

    def shot(self, path):
        self.cmd("screendump", {"filename": path, "format": "ppm"})


def serial_text():
    try:
        return open(LOG, errors="ignore").read()
    except OSError:
        return ""


def wait_serial(needles, timeout, since=0):
    if isinstance(needles, str):
        needles = [needles]
    end = time.time() + timeout
    while time.time() < end:
        t = serial_text()[since:]
        if all(n in t for n in needles):
            return True, len(serial_text())
        time.sleep(0.3)
    return False, len(serial_text())


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "obj"), exist_ok=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    if not os.environ.get("XPTR_SKIP_BUILD"):
        ensure_pins()
        client = build_client()
        if not os.environ.get("XPTR_SKIP_OS"):
            build_os()
        stage(client)
    ovmf = os.environ.get("EDK2_X86_64")
    for c in (os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
              "/usr/share/OVMF/OVMF_CODE_4M.fd"):
        if not ovmf and os.path.exists(c):
            ovmf = c
    if not ovmf or not os.path.exists(ovmf):
        raise SystemExit("xptr: no OVMF_CODE_4M.fd found")

    if os.path.exists(LOG):
        os.remove(LOG)
    qemu_args = f"-display none -enable-kvm -qmp unix:{QMP_SOCK},server,nowait"
    logf = open(LOG, "w")
    env = dict(os.environ)
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    proc = subprocess.Popen(
        ["make", "-C", ROOT, "-o", "disk.img", f"ARCH={ARCH}", "MODE=desktop",
         "DESKTOP=hobby", f"EDK2_X86_64={ovmf}", "run", f"QEMU_ARGS={qemu_args}"],
        cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, env=env)
    try:
        menu = {}
        deadline = time.time() + BOOT_TIMEOUT
        while time.time() < deadline:
            txt = serial_text()
            for line in txt.splitlines():
                m = re.search(r"\[MENU\]\s*(\d+)=(\S+)", line)
                if m:
                    menu[int(m.group(1))] = m.group(2)
            if "[MENU] count=" in txt:
                break
            time.sleep(1)
        if not menu:
            log("[FAIL] desktop never reported its menu mapping (boot failed?)")
            return 1
        time.sleep(3)
        q = Qmp(QMP_SOCK)
        log("[INFO] query-mice: " + json.dumps(q.cmd("query-mice")))
        idx = next((i for i, n in menu.items() if n.upper().startswith("XPTR")), None)
        if idx is None:
            log(f"[FAIL] XPTR.BIN not in the desktop menu ({len(menu)} entries)")
            return 1
        ups = max(menu) - idx
        log(f"[INFO] XPTR.BIN at menu index {idx} of {len(menu)} ({ups} ups)")
        q.key("f1"); time.sleep(0.6)
        q.key("end")
        for _ in range(ups):
            q.key("up", pause=0.12)
        q.key("ret")

        up, _ = wait_serial("[XP] up", 90)
        log("[INFO] client launched: " + ("yes" if up else "NO"))
        if not up:
            log(serial_text()[-3000:])
            return 1
        # Search from the start of the log, not from a mark taken after the
        # "[XP] up" row was polled: the ConfigureNotify row can already be in
        # the file by then, and a mark taken afterwards skips past it (the
        # same trap hobbyos-gui-test records for click receipts).
        geom, _ = wait_serial("[XP] geom", 60)
        txt = serial_text()
        m = None
        for line in txt.splitlines():
            m = re.search(r"\[XP\] geom gx=(\d+) gy=(\d+) gw=(\d+) gh=(\d+) "
                          r"cx=(\d+) cy=(\d+)", line) or m
        log("[INFO] content rect: " + ("seen" if geom else "MISSING"))
        if not geom:
            log(txt[-3000:])
            return 1
        gx, gy, gw, gh, cx, cy = (int(x) for x in m.groups())
        log(f"[INFO] content gx={gx} gy={gy} {gw}x{gh} -> click ({cx},{cy})")

        # Motion first: proves the pointer reaches the client, so a later
        # missing press cannot be blamed on "the pointer never arrived".
        mark = len(serial_text())
        q.move(gx + 40, gy + 40)
        q.move(cx, cy)
        after_abs = serial_text()[mark:]
        # A relative report as well: on a boot where ABS is routed to a device
        # the guest does not drive, REL still reaches the PS/2 mouse, and the
        # two rows together name which path is live.
        mark = len(serial_text())
        q.cmd("input-send-event", {"events": [
            {"type": "rel", "data": {"axis": "x", "value": 40}},
            {"type": "rel", "data": {"axis": "y", "value": 40}}]})
        time.sleep(0.8)
        after_rel = serial_text()[mark:]
        log("[INFO] motion rows after abs=" +
            str(after_abs.count("MotionNotify")) +
            " after rel=" + str(after_rel.count("MotionNotify")))
        q.shot(os.path.join(SHOT_DIR, "click_target.ppm"))
        for _ in range(CLICKS):
            q.click()
        done, _ = wait_serial("[XP] done", int(HOLD_MS) // 1000 + 40)
        time.sleep(0.5)
        q.shot(os.path.join(SHOT_DIR, "after_click.ppm"))

        txt = serial_text()
        rows = [l for l in txt.splitlines() if l.startswith("[XP]") or l.startswith("[XE]")
                or l.startswith("[BTN]") or l.startswith("[VMW]")]
        log("--- rows ---")
        for l in rows:
            log(l)
        dm = None
        for line in txt.splitlines():
            dm = re.search(r"\[XP\] done presses=(\d+) releases=(\d+) motions=(\d+) "
                           r"exposes=(\d+) rc=(\d+)", line) or dm
        pressed = any("[XP] ButtonPress" in l for l in txt.splitlines())
        released = any("[XP] ButtonRelease" in l for l in txt.splitlines())
        motions = any("[XP] MotionNotify" in l for l in txt.splitlines())
        log("")
        log(f"[INFO] motion={motions} press={pressed} release={released} "
            f"client_rc={dm.group(5) if dm else '-'}")
        ok = bool(done and pressed and released and dm and dm.group(5) == "0")
        log("xptr: PASS — a compat X11 client received ButtonPress/ButtonRelease"
            if ok else "xptr: FAIL — no button event reached the client")
        return 0 if ok else 1
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        time.sleep(1)
        subprocess.run(["pkill", "-f", QMP_SOCK], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        subprocess.run(["fuser", "-k", IMG], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        logf.close()


if __name__ == "__main__":
    sys.exit(main())
