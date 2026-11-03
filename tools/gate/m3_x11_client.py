#!/usr/bin/env python3
"""tools/gate/m3_x11_client.py -- the M3 gate.

Proves, in one boot of the REAL kernel, that a host-built *ordinary glibc*
Xlib program runs on the Linux-ABI compat track with src/user/xcompat's
libX11.so.6 and puts a window on the HobbyOS desktop.

What it does, in order:

  1. extracts the pinned glibc 2.31 runtime (the same pins as
     tools/gate/m1_glibc_hello.sh) so the client's glibc ABI matches the
     interpreter the compat loader starts;
  2. builds tests/fixtures/m3/xclient.c with the HOST gcc against Ubuntu's
     <X11/Xlib.h> and -lX11, linked against the 2.31 startup objects -- an
     ordinary glibc binary whose only DT_NEEDED are libX11.so.6, libdl.so.2
     and libc.so.6 (exactly the shape of the game's own binary);
  3. builds the kernel + disk for the native desktop (MODE=desktop);
  4. stages the compat DSOs into /LIB64 (src/user/xcompat/stage.sh), the
     pinned glibc beside them, and the client as /XCLIENT.BIN;
  5. boots with QMP, reads the desktop's own [MENU] dump, launches
     XCLIENT.BIN from the Apps menu with real key events (F1, End, Up*N,
     Enter), waits for the client's stderr markers in the serial log,
     screendumps the desktop while the window is up, waits for the client's
     clean exit;
  6. grades the serial log: the client must print its own rows, report the
     DSO dladdr() resolved to, paint (an Expose/content rect), and exit 0.

Evidence is the serial log + the screenshot; a build result is not a verdict.
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
ARCH = os.environ.get("M3_ARCH", "intel")
IMG = os.path.join(ROOT, "disk.img")
LOG = os.environ.get("M3_LOG", os.path.join(ROOT, "obj", "m3_x11_client.log"))
SHOT_DIR = os.environ.get("M3_SHOT_DIR", os.path.join(ROOT, "obj", "m3-shots"))
QMP_SOCK = os.path.join(ROOT, "qmp-sock-m3")
BUILD = os.path.join(ROOT, "obj", "m3")
GPIN = os.environ.get("GLIBC231", "/tmp/glibc231")
DPIN = os.environ.get("GLIBC231_DEV", "/tmp/glibc231-dev")
HOLD_MS = os.environ.get("M3_HOLD_MS", "12000")
BOOT_TIMEOUT = int(os.environ.get("M3_BOOT_TIMEOUT", "180"))

LIBC6_SHA = "1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0"
LIBGCC_SHA = "4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c"
LIBC6DEV_SHA = "11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a"
URL_LIBC6 = "http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb"
URL_LIBGCC = ("http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/"
              "libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb")
URL_LIBC6DEV = ("http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/"
                "libc6-dev_2.31-0ubuntu9.18_amd64.deb")


def log(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, **kw)


def fetch_deb(url, sha, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if not os.path.exists(dest):
        log(f"m3: fetching {os.path.basename(dest)}")
        run(["curl", "-fsS", "-o", dest, url], check=True)
    h = hashlib.sha256(open(dest, "rb").read()).hexdigest()
    if h != sha:
        raise SystemExit(f"m3: SHA256 MISMATCH {dest}: {h}")


def ensure_pins():
    ld = os.path.join(GPIN, "lib/x86_64-linux-gnu/ld-2.31.so")
    if not os.path.exists(ld):
        t = os.path.join(ROOT, "obj", "m1-pins")
        fetch_deb(URL_LIBC6, LIBC6_SHA, os.path.join(t, "libc6.deb"))
        fetch_deb(URL_LIBGCC, LIBGCC_SHA, os.path.join(t, "libgcc-s1.deb"))
        shutil.rmtree(GPIN, ignore_errors=True)
        os.makedirs(GPIN, exist_ok=True)
        run(["dpkg-deb", "-x", os.path.join(t, "libc6.deb"), GPIN], check=True)
        run(["dpkg-deb", "-x", os.path.join(t, "libgcc-s1.deb"), GPIN], check=True)
    scrt = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu/Scrt1.o")
    if not os.path.exists(scrt):
        t = os.path.join(ROOT, "obj", "m1-pins")
        fetch_deb(URL_LIBC6DEV, LIBC6DEV_SHA, os.path.join(t, "libc6-dev.deb"))
        shutil.rmtree(DPIN, ignore_errors=True)
        os.makedirs(DPIN, exist_ok=True)
        run(["dpkg-deb", "-x", os.path.join(t, "libc6-dev.deb"), DPIN], check=True)


def build_client():
    os.makedirs(BUILD, exist_ok=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    dlib = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu")
    out = os.path.join(BUILD, "xclient")
    cmd = [
        "gcc", "-O2", "-std=gnu11", "-fPIE", "-pie", "-nostdlib", "-nostartfiles",
        "-o", out, os.path.join(ROOT, "tests/fixtures/m3/xclient.c"),
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
    log("m3: building the client (" + " ".join(cmd[:6]) + " ...)")
    run(cmd, check=True)
    needed = subprocess.run(["readelf", "-dW", out], capture_output=True,
                            text=True).stdout
    log("m3: client DT_NEEDED: " + ", ".join(
        re.findall(r"Shared library: \[([^\]]+)\]", needed)))
    return out


def build_os():
    log("m3: building kernel + disk (desktop)")
    env = dict(os.environ)
    env["TOOLS_SKIP"] = "1"
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    r = run(["make", "-C", ROOT, f"ARCH={ARCH}", "MODE=desktop", "DESKTOP=hobby",
             "hobbyos.elf", "disk.img"], env=env,
            capture_output=True, text=True)
    open(os.path.join(ROOT, "obj", "m3_build.log"), "w").write(r.stdout + r.stderr)
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-25:])
        raise SystemExit("m3: BUILD FAILED\n" + tail)


def stage(client):
    """DSOs -> /LIB64, pinned glibc -> /LIB64 + /lib/x86_64-linux-gnu, client
    -> /XCLIENT.BIN.  Real sonames: LFN open/read is measured working (M1)."""
    run(["bash", os.path.join(ROOT, "src/user/xcompat/stage.sh"), IMG], check=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")

    def mmd(p):
        run(["mmd", "-i", IMG, p], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)

    def mc(src, dst):
        run(["mcopy", "-o", "-i", IMG, src, dst], check=True)

    for d in ("::/LIB64", "::/lib/x86_64-linux-gnu"):
        mmd(d)
    glibc = {
        "ld-2.31.so": "ld-linux-x86-64.so.2",
        "libc-2.31.so": "libc.so.6",
        "libm-2.31.so": "libm.so.6",
        "libpthread-2.31.so": "libpthread.so.0",
        "libdl-2.31.so": "libdl.so.2",
        "librt-2.31.so": "librt.so.1",
        "libgcc_s.so.1": "libgcc_s.so.1",
    }
    for src, dst in glibc.items():
        mc(os.path.join(ldir, src), f"::/LIB64/{dst}")
    mc(os.path.join(ldir, "ld-2.31.so"), "::/lib64/ld-linux-x86-64.so.2")
    mc(client, "::XCLIENT.BIN")
    out = subprocess.run(["mdir", "-i", IMG, "::/LIB64"], capture_output=True,
                         text=True).stdout
    log("m3: ::/LIB64\n" + out.rstrip())


# ---- QMP ------------------------------------------------------------------

class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(240):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared")
        self.sock.recv(65536)
        self.cmd("qmp_capabilities")
        self.px = self.py = 0

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        buf = b""
        while True:
            buf += self.sock.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                if not part:
                    continue
                try:
                    resp = json.loads(part)
                except Exception:
                    continue
                if "return" in resp or "error" in resp:
                    return resp

    def key(self, qcode, pause=0.35):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)

    def shot(self, path):
        self.cmd("screendump", {"filename": path})


def serial_text():
    try:
        return open(LOG, errors="ignore").read()
    except OSError:
        return ""


def wait_serial(needle, timeout, since=0):
    end = time.time() + timeout
    while time.time() < end:
        t = serial_text()
        if needle in t[since:]:
            return True, len(t)
        time.sleep(0.3)
    return False, len(serial_text())


def ppm_colors(path):
    """Exact-pixel census of a P6 PPM (used to prove the window is on screen)."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return {}
    if d[:2] != b"P6":
        return {}
    i, parts = 2, []
    while len(parts) < 3:
        while d[i:i+1].isspace():
            i += 1
        j = i
        while not d[j:j+1].isspace():
            j += 1
        parts.append(int(d[i:j]))
        i = j
    i += 1
    w, h, px = parts[0], parts[1], d[i:]
    counts = {}
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            o = row + x * 3
            c = (px[o], px[o+1], px[o+2])
            counts[c] = counts.get(c, 0) + 1
    return counts


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "obj"), exist_ok=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    ensure_pins()
    client = build_client()
    build_os()
    stage(client)

    log(f"m3: booting (log {LOG})")
    ovmf = None
    for c in (os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
              "/usr/share/OVMF/OVMF_CODE_4M.fd"):
        if os.path.exists(c):
            ovmf = os.path.join(BUILD, "OVMF_CODE_4M.fd")
            if not os.path.exists(ovmf):
                shutil.copy(c, ovmf)
            break
    if not ovmf:
        raise SystemExit("m3: no OVMF_CODE_4M.fd found")

    if os.path.exists(LOG):
        os.remove(LOG)
    qemu_args = f"-display none -enable-kvm -qmp unix:{QMP_SOCK},server,nowait"
    env = dict(os.environ)
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    logf = open(LOG, "w")
    proc = subprocess.Popen(
        ["make", "-C", ROOT, "-o", "disk.img", f"ARCH={ARCH}", "MODE=desktop",
         "DESKTOP=hobby", f"EDK2_X86_64={ovmf}", "run", f"QEMU_ARGS={qemu_args}"],
        cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, env=env)

    ok = False
    shots = {}
    try:
        q = None
        # The desktop announces its whole menu mapping once at load.
        deadline = time.time() + BOOT_TIMEOUT
        menu = {}
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
        # [MENU] is dumped before the taskbar is painted; give the WM a beat.
        time.sleep(3)
        if not q:
            q = Qmp(QMP_SOCK)
        q.shot(os.path.join(SHOT_DIR, "00_desktop.ppm"))
        shots["desktop"] = os.path.join(SHOT_DIR, "00_desktop.ppm")

        app = os.environ.get("M3_APP", "XCLIENT.BIN").upper()
        idx = next((i for i, n in menu.items()
                    if n.upper().split(".")[0] == app.split(".")[0]), None)
        if idx is None:
            log(f"[FAIL] {app} is not in the desktop menu ({len(menu)} entries)")
            return 1
        ups = max(menu) - idx
        log(f"[INFO] {app} at menu index {idx} of {len(menu)} ({ups} ups from the end)")

        # F1 opens the Apps menu (desktop.c: the keyboard path, works on both
        # arches); End jumps to the last row, Up*ups walks to ours, Enter
        # launches.  These are real key events through the PS/2 -> evdev path.
        q.key("f1")
        time.sleep(0.6)
        q.key("end")
        for _ in range(ups):
            q.key("up", pause=0.12)
        q.key("ret")

        if app != "XCLIENT.BIN":
            # Native-app regression path: the same menu launch, but grade the
            # app's own marker and a screenshot instead of the compat rows.
            marker = "[APP] " + app.split(".")[0] + " started"
            seen, _ = wait_serial(marker, 40)
            log("[INFO] native marker: " + ("seen" if seen else "MISSING"))
            time.sleep(3)
            p = os.path.join(SHOT_DIR, "01_window.ppm")
            q.shot(p)
            shots["window"] = p
            log("m3: PASS — native pixel app launched on the same desktop"
                if seen else "m3: FAIL — native app did not start")
            return 0 if seen else 1

        launched, n0 = wait_serial("[XC] xclient up", 60)
        log("[INFO] launch marker: " + ("seen" if launched else "MISSING"))
        on_screen = {}
        if launched:
            painted = "[XC] Expose" in serial_text()
            # Wait until the client has painted AND re-read the framebuffer
            # (3 s after the paint), so the screenshot cannot miss the frame.
            wait_serial("[XC] fb readback mid-hold", 20)
            time.sleep(1.5)
            p = os.path.join(SHOT_DIR, "01_window.ppm")
            q.shot(p)
            shots["window"] = p
            on_screen = ppm_colors(p)
            green = on_screen.get((0, 255, 0), 0)
            blue = on_screen.get((0, 0, 255), 0)
            log("[INFO] window screenshot: " + p +
                (f" (Expose seen, green={green} blue={blue})" if painted
                 else " (no Expose!)"))

        done, _ = wait_serial("[XC] done rc=0", int(HOLD_MS) // 1000 + 40)
        log("[INFO] clean exit: " + ("yes" if done else "NO"))
        time.sleep(1)
        p = os.path.join(SHOT_DIR, "02_after.ppm")
        q.shot(p)
        shots["after"] = p

        txt = serial_text()
        rows = [l for l in txt.splitlines() if "[XC]" in l]
        log("--- client rows ---")
        for l in rows:
            log(l)
        log("--- unhandled Linux syscalls (last 10) ---")
        for l in [l for l in txt.splitlines() if "[LCOMPAT]" in l][-10:]:
            log(l)

        ok = (done and any("XOpenDisplay ->" in l and "libX11.so.6" in l for l in rows)
              and any("LD_LIBRARY_PATH=/LIB64" in l for l in rows)
              and on_screen.get((0, 255, 0), 0) > 500
              and on_screen.get((0, 0, 255), 0) > 500)
        log("")
        log("m3: PASS — a glibc Xlib client opened a window through the compat DSO"
            if ok else "m3: FAIL — see the rows above")
        return 0 if ok else 1
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        time.sleep(1)
        subprocess.run(["fuser", "-k", IMG], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        logf.close()
        for name, p in shots.items():
            png = p[:-4] + ".png"
            subprocess.run(["convert", p, png], stderr=subprocess.DEVNULL)
            log(f"m3: screenshot {name}: {png}")


if __name__ == "__main__":
    sys.exit(main())
