#!/usr/bin/env python3
"""tools/gate/m3_window_probe.py -- the M3 *window* gate (SDL-shaped creation).

M3 proved a glibc Xlib client can open a window (XCreateSimpleWindow ->
XMapWindow).  Unity has not reached XCreateWindow yet, so this gate proves the
*half of the shim SDL's X11_CreateWindow will use*, before the game gets there:

    XOpenDisplay
    XMatchVisualInfo(dpy, screen, DefaultDepth, TrueColor, &vinfo)
    XCreateColormap(dpy, root, vinfo.visual, AllocNone)
    XCreateWindow(..., 320, 240, 0, vinfo.depth, InputOutput, vinfo.visual,
                  CWColormap|CWEventMask|CWBorderPixel, &attrs)
    XSetWMProtocols(dpy, w, &WM_DELETE_WINDOW, 1)
    XChangeProperty(_NET_WM_NAME/UTF8_STRING, _NET_WM_PID/CARDINAL)
    XStoreName, XMapWindow
    XNextEvent loop until MapNotify + ConfigureNotify -> paint -> XFlush
    ... wait for the WM_DELETE_WINDOW ClientMessage -> exit 0

Client: src/user/xcompat/sdl_window_probe.c -- an ordinary glibc PIE built with
the host gcc against Ubuntu's <X11/Xlib.h> and -lX11, linked against the pinned
glibc 2.31 startup objects, so its DT_NEEDED are exactly libX11.so.6,
libdl.so.2, libc.so.6 (the game binary's shape).  Nothing in it knows HobbyOS
exists.

Steps (the harness shape of tools/gate/m3_x11_client.py):

  1. extract the pinned glibc 2.31 runtime + dev objects (sha256-checked);
  2. build the compat DSOs, and a -DXC_TRACE copy of libX11.so.6 so every X
     call the shim serves shows up in the serial log;
  3. build the probe (pinned glibc, PIE);
  4. build the kernel + disk for the native desktop (MODE=desktop);
  5. stage the DSOs into /LIB64 (traced libX11 wins), the pinned glibc beside
     them, the probe as /WPROBE.BIN;
  6. boot with QMP, read the desktop's [MENU] dump, launch WPROBE.BIN from the
     Apps menu with real key events (F1, End, Up*N, Enter);
  7. wait for the paint row, screendump the desktop (the window must be on
     screen), then press F4 -- the desktop's close path writes ESC [ D ~ into
     the window's input pipe and the shim must turn that into the ClientMessage
     SDL reads;
  8. grade the serial log (every call row, the content rect, the three
     geometry sources agreeing, the ClientMessage) + the screenshot census.

Evidence is the serial log + the screenshot; a build is not a verdict.
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
ARCH = os.environ.get("M3W_ARCH", "intel")
IMG = os.path.join(ROOT, "disk.img")
LOG = os.environ.get("M3W_LOG", os.path.join(ROOT, "obj", "m3_window_probe.log"))
SHOT_DIR = os.environ.get("M3W_SHOT_DIR", os.path.join(ROOT, "obj", "m3w-shots"))
QMP_SOCK = os.path.join(ROOT, "qmp-sock-m3w")
BUILD = os.path.join(ROOT, "obj", "m3w")
GPIN = os.environ.get("GLIBC231", "/tmp/glibc231")
DPIN = os.environ.get("GLIBC231_DEV", "/tmp/glibc231-dev")
BOOT_TIMEOUT = int(os.environ.get("M3W_BOOT_TIMEOUT", "240"))

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
        log(f"m3w: fetching {os.path.basename(dest)}")
        run(["curl", "-fsS", "-o", dest, url], check=True)
    h = hashlib.sha256(open(dest, "rb").read()).hexdigest()
    if h != sha:
        raise SystemExit(f"m3w: SHA256 MISMATCH {dest}: {h}")


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


def build_dsos():
    log("m3w: building the compat DSOs + the traced libX11")
    for target in ("xcompat_dsos", "xcompat_dsos_trace"):
        r = run(["make", "-f", "src/user/xcompat/Makefile.inc", f"ARCH={ARCH}",
                 target], cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            tail = "\n".join((r.stdout + r.stderr).splitlines()[-20:])
            raise SystemExit(f"m3w: {target} FAILED\n" + tail)
    trace = os.path.join(ROOT, "obj/xcompat/trace/libX11.so.6")
    if not os.path.exists(trace):
        raise SystemExit(f"m3w: {trace} missing")
    return trace


def build_probe():
    os.makedirs(BUILD, exist_ok=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    dlib = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu")
    out = os.path.join(BUILD, "sdl_window_probe")
    cmd = [
        "gcc", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-fPIE", "-pie",
        "-nostdlib", "-nostartfiles",
        "-o", out, os.path.join(ROOT, "src/user/xcompat/sdl_window_probe.c"),
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
    log("m3w: building the probe (glibc 2.31 PIE)")
    r = run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("m3w: PROBE BUILD FAILED\n" + r.stdout + r.stderr)
    needed = subprocess.run(["readelf", "-dW", out], capture_output=True,
                            text=True).stdout
    log("m3w: probe DT_NEEDED: " + ", ".join(
        re.findall(r"Shared library: \[([^\]]+)\]", needed)))
    return out


def build_os():
    log("m3w: building kernel + disk (desktop)")
    env = dict(os.environ)
    env["TOOLS_SKIP"] = "1"
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    r = run(["make", "-C", ROOT, f"ARCH={ARCH}", "MODE=desktop", "DESKTOP=hobby",
             "hobbyos.elf", "disk.img"], env=env, capture_output=True, text=True)
    open(os.path.join(ROOT, "obj", "m3w_build.log"), "w").write(r.stdout + r.stderr)
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-25:])
        raise SystemExit("m3w: BUILD FAILED\n" + tail)


def stage(probe, traced_libx11):
    """DSOs -> /LIB64 (traced libX11 last), pinned glibc beside them, probe
    -> /WPROBE.BIN.  Real sonames: LFN open is measured working (M1)."""
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
    # Last, so the traced copy is what the loader maps.
    mc(traced_libx11, "::/LIB64/libX11.so.6")
    mc(probe, "::WPROBE.BIN")
    out = subprocess.run(["mdir", "-i", IMG, "::/LIB64"], capture_output=True,
                         text=True).stdout
    log("m3w: ::/LIB64\n" + out.rstrip())


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
        time.sleep(0.2)
    return False, len(serial_text())


def ppm_colors(path):
    """Exact-pixel census of a P6 PPM (proves the window is on screen)."""
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
    trace = build_dsos()
    probe = build_probe()
    build_os()
    stage(probe, trace)

    log(f"m3w: booting (log {LOG})")
    ovmf = None
    for c in (os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
              "/usr/share/OVMF/OVMF_CODE_4M.fd"):
        if os.path.exists(c):
            ovmf = os.path.join(BUILD, "OVMF_CODE_4M.fd")
            if not os.path.exists(ovmf):
                shutil.copy(c, ovmf)
            break
    if not ovmf:
        raise SystemExit("m3w: no OVMF_CODE_4M.fd found")

    if os.path.exists(LOG):
        os.remove(LOG)
    subprocess.run(["fuser", "-k", IMG], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    time.sleep(2)
    qemu_args = f"-display none -enable-kvm -qmp unix:{QMP_SOCK},server,nowait"
    env = dict(os.environ)
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    logf = open(LOG, "w")
    proc = subprocess.Popen(
        ["make", "-C", ROOT, "-o", "disk.img", f"ARCH={ARCH}", "MODE=desktop",
         "DESKTOP=hobby", f"EDK2_X86_64={ovmf}", "run", f"QEMU_ARGS={qemu_args}"],
        cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, env=env)

    shots = {}
    try:
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
        time.sleep(3)
        q = Qmp(QMP_SOCK)
        q.shot(os.path.join(SHOT_DIR, "00_desktop.ppm"))
        shots["desktop"] = os.path.join(SHOT_DIR, "00_desktop.ppm")

        app = os.environ.get("M3W_APP", "WPROBE.BIN").upper()
        idx = next((i for i, n in menu.items()
                    if n.upper().split(".")[0] == app.split(".")[0]), None)
        if idx is None:
            log(f"[FAIL] {app} is not in the desktop menu ({len(menu)} entries)")
            return 1
        ups = max(menu) - idx
        log(f"[INFO] {app} at menu index {idx} of {len(menu)} ({ups} ups from the end)")

        q.key("f1")
        time.sleep(0.6)
        q.key("end")
        for _ in range(ups):
            q.key("up", pause=0.12)
        q.key("ret")

        started, _ = wait_serial("[WP] sdl_window_probe up", 60)
        log("[INFO] launch marker: " + ("seen" if started else "MISSING"))

        painted, _ = wait_serial("[WP] paint:", 90)
        log("[INFO] paint row: " + ("seen" if painted else "MISSING"))
        time.sleep(1.0)
        p = os.path.join(SHOT_DIR, "01_window.ppm")
        q.shot(p)
        shots["window"] = p
        on_screen = ppm_colors(p)
        green = on_screen.get((0, 255, 0), 0)
        blue = on_screen.get((0, 0, 255), 0)
        log(f"[INFO] window screenshot: {p} (green={green} blue={blue})")

        # The desktop's close path: F4 -> ESC [ D ~ -> the shim's ClientMessage.
        time.sleep(0.5)
        log("[INFO] sending F4 to close the focused window")
        q.key("f4")
        done, _ = wait_serial("[WP] done rc=0", 40)
        log("[INFO] clean WM_DELETE_WINDOW shutdown: " + ("yes" if done else "NO"))
        time.sleep(1)
        p = os.path.join(SHOT_DIR, "02_after.ppm")
        q.shot(p)
        shots["after"] = p

        txt = serial_text()
        rows = [l for l in txt.splitlines() if "[WP]" in l]
        shim = [l for l in txt.splitlines() if "[XC]" in l]
        log("--- probe rows ---")
        for l in rows:
            log(l)
        log("--- shim trace rows (XC_TRACE) ---")
        for l in shim:
            log(l)
        log("--- unhandled Linux syscalls (last 10) ---")
        for l in [l for l in txt.splitlines() if "[LCOMPAT]" in l][-10:]:
            log(l)

        def has(*needles):
            return all(any(n in l for l in rows) for n in needles)

        checks = {
            "dladdr names /LIB64/libX11.so.6": any(
                "XOpenDisplay ->" in l and "libX11.so.6" in l for l in rows),
            "XMatchVisualInfo hit": has("XMatchVisualInfo(depth=24, TrueColor) -> 1"),
            "real Visual layout": has("vinfo.visual->visualid=0x21"),
            "XCreateColormap non-zero": has("XCreateColormap(root, visual, AllocNone) -> 0x"),
            "XCreateWindow returned a window": has("XCreateWindow(320x240"),
            "XSetWMProtocols ok": has("XSetWMProtocols(w, {WM_DELETE_WINDOW}, 1) -> 1"),
            "_NET_WM_NAME stored": has("XChangeProperty(_NET_WM_NAME"),
            "_NET_WM_PID stored": has("XChangeProperty(_NET_WM_PID"),
            "XGetWindowProperty reads it back": has(
                "XGetWindowProperty(_NET_WM_NAME) -> 0"),
            "XStoreName ok": has("XStoreName(\"SDL-shaped window probe\") -> 1"),
            "XMapWindow ok": has("XMapWindow -> 1"),
            "MapNotify": has("MapNotify window="),
            "ConfigureNotify": has("ConfigureNotify window="),
            "three geometry sources agree": has("-> AGREE"),
            "ClientMessage WM_DELETE_WINDOW": has(
                "WM_DELETE_WINDOW: match (WM_PROTOCOLS/format 32/"),
            "clean exit": done,
            "shim traced XCreateWindow": any("[XC] XCreateWindow" in l for l in shim),
            "shim traced XMapWindow": any("[XC] XMapWindow" in l for l in shim),
            "screenshot has the green block": green > 500,
            "screenshot has the blue block": blue > 500,
        }
        log("")
        bad = [k for k, v in checks.items() if not v]
        for k, v in checks.items():
            log(f"  [{'ok' if v else 'FAIL'}] {k}")
        ok = not bad
        log("")
        log("m3w: PASS — the shim served SDL's whole XCreateWindow sequence and "
            "delivered WM_DELETE_WINDOW as a ClientMessage"
            if ok else "m3w: FAIL — " + ", ".join(bad))
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
            log(f"m3w: screenshot {name}: {png}")


if __name__ == "__main__":
    sys.exit(main())
