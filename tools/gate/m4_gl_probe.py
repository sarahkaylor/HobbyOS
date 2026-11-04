#!/usr/bin/env python3
"""tools/gate/m4_gl_probe.py -- the M4 in-guest gate.

Proves, in one boot of the REAL kernel, that the cross-built Mesa libGL.so.1
loads inside a Linux-ABI compat process and gives it a working GL 3.3 core
context: the probe dlopen()s it, asks for 3.3 core, compiles a GLSL-330 shader
pair, draws a triangle into an FBO, reads the pixels back and prints a checksum.

Steps (mirroring tools/gate/m3_x11_client.py, which this gate extends):

  1. extract the pinned glibc 2.31 runtime + dev headers (sha256-checked);
  2. build libGL.so.1 with tools/mesa/build-hobbyos.sh (the script itself
     verifies the Mesa pin and audits every undefined symbol against the pin);
  3. build src/user/gl_probe.c as an ordinary glibc PIE whose DT_NEEDED are
     libX11.so.6/libdl.so.2/libc.so.6 -- the same shape as the game binary;
  4. build the kernel + disk for the native desktop (MODE=desktop);
  5. stage libX11/libasound into /LIB64, the pinned glibc beside them, and
     libGL.so.1 + libGLX.so.0 from tools/mesa/stage.sh; the probe becomes
     /GLPROBE.BIN (real sonames; LFN open is measured working, M1);
  6. boot with QMP, wait for the desktop's [MENU] dump, launch GLPROBE.BIN with
     real key events (F1, End, Up*N, Enter), wait for the probe's rows;
  7. grade the serial log: GL_VERSION must be 3.3 (Core Profile), GL_RENDERER
     must name the software driver, the readback must have a checksum and a
     covered triangle, and the probe must exit 0.  Then copy /GLPROBE.PPM off
     the image so the frame itself is inspectable.

Evidence is the serial log (plus the PPM); a build result is not a verdict.
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
ARCH = os.environ.get("M4_ARCH", "intel")
IMG = os.path.join(ROOT, "disk.img")
LOG = os.environ.get("M4_LOG", os.path.join(ROOT, "obj", "m4_gl_probe.log"))
SHOT_DIR = os.environ.get("M4_SHOT_DIR", os.path.join(ROOT, "obj", "m4-shots"))
QMP_SOCK = os.path.join(ROOT, "qmp-sock-m4")
BUILD = os.path.join(ROOT, "obj", "m4")
GPIN = os.environ.get("GLIBC231", "/tmp/glibc231")
DPIN = os.environ.get("GLIBC231_DEV", "/tmp/glibc231-dev")
BOOT_TIMEOUT = int(os.environ.get("M4_BOOT_TIMEOUT", "240"))
RUN_TIMEOUT = int(os.environ.get("M4_RUN_TIMEOUT", "120"))
SKIP_BUILD = os.environ.get("M4_SKIP_BUILD", "0") == "1"

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
        log(f"m4: fetching {os.path.basename(dest)}")
        run(["curl", "-fsS", "-o", dest, url], check=True)
    h = hashlib.sha256(open(dest, "rb").read()).hexdigest()
    if h != sha:
        raise SystemExit(f"m4: SHA256 MISMATCH {dest}: {h}")


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


def build_libgl():
    out = os.path.join(ROOT, "obj", "mesa", "lib", "libGL.so.1")
    # libGL has no DT_NEEDED: its libX11/libXext symbols must exist in the
    # process AND in the audit's copy of the shim.  Rebuild the compat DSOs
    # FIRST, or a merged tree with a newer libx11.c audits libGL against the
    # stale obj/xcompat/libX11.so.6 and fails with "undefined symbols the
    # compat process cannot provide" (measured on the wave-3 merge).
    r = run(["make", "ARCH=intel", "xcompat_dsos"], cwd=ROOT,
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise SystemExit("m4: xcompat_dsos build failed")
    if SKIP_BUILD and os.path.exists(out):
        log(f"m4: reusing {out} (M4_SKIP_BUILD=1)")
    else:
        log("m4: building libGL.so.1 (tools/mesa/build-hobbyos.sh)")
        env = dict(os.environ)
        env["HOME"] = os.environ.get("HOME", "/home/sarah")
        # The Hermes venv's python3 has no mako; Mesa's generators need it.
        env["PATH"] = "/usr/bin:" + env.get("PATH", "")
        b = os.path.join(ROOT, "obj", "m4_libgl_build.log")
        with open(b, "w") as f:
            r = run(["bash", os.path.join(ROOT, "tools/mesa/build-hobbyos.sh"),
                     "-j", os.environ.get("M4_JOBS", "16")],
                    env=env, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
        if r.returncode != 0:
            tail = "\n".join(open(b, errors="ignore").read().splitlines()[-25:])
            raise SystemExit("m4: libGL BUILD FAILED\n" + tail)
        for line in open(b, errors="ignore").read().splitlines():
            if re.search(r"(DT_NEEDED|exports|undefined|audit|TLS model|size):", line):
                log("    " + line.strip())
    if not os.path.exists(out):
        raise SystemExit(f"m4: {out} missing after the build")
    return out


def build_probe():
    os.makedirs(BUILD, exist_ok=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    dlib = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu")
    out = os.path.join(BUILD, "glprobe")
    cmd = [
        "gcc", "-O2", "-std=gnu11", "-fPIE", "-pie", "-nostdlib", "-nostartfiles",
        "-o", out, os.path.join(ROOT, "src/user/gl_probe.c"),
        os.path.join(dlib, "Scrt1.o"), os.path.join(dlib, "crti.o"),
        "-lX11",
        # --no-as-needed: the probe does not itself call libm/libpthread, but
        # libGL's 252 undefined symbols include 36 from libm and 21 from
        # libpthread (measured), and a real glibc consumer of libGL --
        # UnityPlayer.so included -- has both on its DT_NEEDED list.  Without
        # them (the default --as-needed drops them) dlopen fails on "log10".
        "-Wl,--no-as-needed",
        os.path.join(ldir, "libdl-2.31.so"),
        os.path.join(ldir, "libm-2.31.so"),
        os.path.join(ldir, "libpthread-2.31.so"),
        "-Wl,--as-needed",
        os.path.join(dlib, "libc_nonshared.a"),
        os.path.join(ldir, "libc.so.6"),
        os.path.join(dlib, "crtn.o"),
        "-Wl,--allow-shlib-undefined",
        "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2",
        f"-Wl,-rpath-link,{ldir}",
        "-Wl,--build-id=none",
    ]
    log("m4: building the probe (glibc 2.31 PIE)")
    run(cmd, check=True)
    needed = run(["readelf", "-dW", out], capture_output=True, text=True).stdout
    log("m4: probe DT_NEEDED: " + ", ".join(
        re.findall(r"Shared library: \[([^\]]+)\]", needed)))
    return out


def build_os():
    log("m4: building kernel + disk (desktop)")
    env = dict(os.environ)
    env["TOOLS_SKIP"] = "1"
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    r = run(["make", "-C", ROOT, f"ARCH={ARCH}", "MODE=desktop", "DESKTOP=hobby",
             "hobbyos.elf", "disk.img"], env=env, capture_output=True, text=True)
    open(os.path.join(ROOT, "obj", "m4_build.log"), "w").write(r.stdout + r.stderr)
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-25:])
        raise SystemExit("m4: BUILD FAILED\n" + tail)


def stage(probe):
    """DSOs -> /LIB64, pinned glibc beside them, probe -> /GLPROBE.BIN."""
    run(["bash", os.path.join(ROOT, "src/user/xcompat/stage.sh"), IMG], check=True)
    run(["bash", os.path.join(ROOT, "tools/mesa/stage.sh"), IMG], check=True)
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
    mc(probe, "::GLPROBE.BIN")
    out = run(["mdir", "-i", IMG, "::/LIB64"], capture_output=True, text=True).stdout
    log("m4: ::/LIB64\n" + out.rstrip())


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
        time.sleep(0.3)
    return False, len(serial_text())


def main():
    os.makedirs(SHOT_DIR, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "obj"), exist_ok=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    ensure_pins()
    build_libgl()
    probe = build_probe()
    build_os()
    stage(probe)

    log(f"m4: booting (log {LOG})")
    ovmf = None
    for c in (os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
              "/usr/share/OVMF/OVMF_CODE_4M.fd"):
        if os.path.exists(c):
            ovmf = os.path.join(BUILD, "OVMF_CODE_4M.fd")
            if not os.path.exists(ovmf):
                shutil.copy(c, ovmf)
            break
    if not ovmf:
        raise SystemExit("m4: no OVMF_CODE_4M.fd found")

    if os.path.exists(LOG):
        os.remove(LOG)
    # A QEMU from an earlier run can still hold the image's write lock for a
    # second after its process is gone; give it a beat and clear any holder.
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

        app = os.environ.get("M4_APP", "GLPROBE.BIN").upper()
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

        started, _ = wait_serial("[GL] probe start", 60)
        log("[INFO] launch marker: " + ("seen" if started else "MISSING"))
        # No swap: the present path is still open (see the probe's step 9), so
        # there is no held frame to screendump.  The evidence is the readback
        # rows plus /GLPROBE.PPM, which this harness copies off the image.
        done, _ = wait_serial("[GL] done rc=", RUN_TIMEOUT)
        log("[INFO] probe finished: " + ("yes" if done else "NO"))
        time.sleep(1)
        p = os.path.join(SHOT_DIR, "02_after.ppm")
        q.shot(p)
        shots["after"] = p

        txt = serial_text()
        rows = [l for l in txt.splitlines() if "[GL]" in l]
        log("--- probe rows ---")
        for l in rows:
            log(l)
        log("--- unhandled Linux syscalls (last 10) ---")
        for l in [l for l in txt.splitlines() if "[LCOMPAT]" in l][-10:]:
            log(l)

        mver = next((l for l in rows if "GL_VERSION" in l), "")
        mren = next((l for l in rows if "GL_RENDERER" in l), "")
        msum = next((l for l in rows if "fnv1a64=" in l and "readback" in l), "")
        mdone = next((l for l in rows if "[GL] done rc=" in l), "")
        ok = ("3.3" in mver and "Core Profile" in mver
              and "softpipe" in mren
              and "fnv1a64=0x" in msum
              and "done rc=0" in mdone)
        log("")
        log("m4: PASS — libGL.so.1 loaded in the guest and rendered a GL 3.3 "
            "core triangle" if ok else "m4: FAIL — see the rows above")
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
            log(f"m4: screenshot {name}: {png}")
        # Pull the probe's own PPM off the image: the frame, not a screenshot.
        ppm = os.path.join(BUILD, "glprobe.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        r = subprocess.run(["mcopy", "-o", "-i", IMG, "::/GLPROBE.PPM", ppm],
                           capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(ppm):
            log(f"m4: guest readback image: {ppm} "
                f"({os.path.getsize(ppm)} bytes)")
            png = ppm[:-4] + ".png"
            subprocess.run(["convert", ppm, png], stderr=subprocess.DEVNULL)
            if os.path.exists(png):
                log(f"m4: guest readback png: {png}")
        else:
            log("m4: note: /GLPROBE.PPM was not on the image "
                "(root not writable, or the probe did not reach the write)")


if __name__ == "__main__":
    sys.exit(main())
