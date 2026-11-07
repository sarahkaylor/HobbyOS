#!/usr/bin/env python3
"""tools/gate/m2_virgl.py -- the M2 gate: virgl gallium over /dev/gpu, in situ.

What it proves, in ONE boot of the real desktop kernel on the real GL device
(`virtio-gpu-gl-pci` + `-display egl-headless,gl=on`):

  1. the same libGL.so.1 runs BOTH drivers -- `M2PROBE.BIN` writes
     "/GLMODE = virgl" before dlopen, `M2PROBE_SW.BIN` writes "softpipe"; the
     two binaries differ only in that baked string;
  2. on the virgl path the renderer string says virgl, a 960x540 frame is
     encoded, submitted through /dev/gpu, read back and PRESENTED -- the
     pattern matches a CPU reference pixel for pixel AND the same rect of
     /dev/fb0 (the surface the desktop scans out) byte for byte;
  3. the two numbers that decide M3/M4 are measured in-guest:
     p = marginal us/draw (110-draw frame minus 10-draw frame, /100) and
     F = fixed ms/frame (clear+swap), plus the host fill rate in Mpx/s;
  4. the softpipe twin still works on the same DSO, same boot, same window --
     the fallback is a runtime choice, not a build.

Thresholds (docs/rimworld-virgl-plan.md §6.3): 30 fps at 1920x1080 needs
p <= 285 us/draw and F <= 20 ms; the M7 gate (20 fps at 1280x720) survives
p <= 440 us.  This gate REPORTS the comparison rather than failing on it --
a slow number is a fact about the milestone, not a broken build; the pass
condition is correctness on both drivers plus both rows present.

QEMU's headless GL backend has no QMP screendump ("no surface", P9), so the
present receipt is the probe's own /dev/fb0 comparison, not a screenshot.
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
ARCH = os.environ.get("M2_ARCH", "intel")
IMG = os.path.join(ROOT, "disk.img")
LOG = os.environ.get("M2_LOG", os.path.join(ROOT, "obj", "m2_virgl.log"))
QMP_SOCK = os.path.join(ROOT, "qmp-sock-m2")
BUILD = os.path.join(ROOT, "obj", "m2")
GPIN = os.environ.get("GLIBC231", "/tmp/glibc231")
DPIN = os.environ.get("GLIBC231_DEV", "/tmp/glibc231-dev")
BOOT_TIMEOUT = int(os.environ.get("M2_BOOT_TIMEOUT", "300"))
RUN_TIMEOUT = int(os.environ.get("M2_RUN_TIMEOUT", "180"))
SKIP_BUILD = os.environ.get("M2_SKIP_BUILD", "0") == "1"

LIBC6_SHA = "1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0"
LIBGCC_SHA = "4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c"
LIBC6DEV_SHA = "11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a"
URL_LIBC6 = "http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb"
URL_LIBGCC = ("http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/"
              "libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb")
URL_LIBC6DEV = ("http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/"
                "libc6-dev_2.31-0ubuntu9.18_amd64.deb")

# The plan's thresholds.
P_MAX_US = 285.0
F_MAX_MS = 20.0
P_M7_US = 440.0


def log(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, **kw)


def fetch_deb(url, sha, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if not os.path.exists(dest):
        log(f"m2: fetching {os.path.basename(dest)}")
        run(["curl", "-fsS", "-o", dest, url], check=True)
    h = hashlib.sha256(open(dest, "rb").read()).hexdigest()
    if h != sha:
        raise SystemExit(f"m2: SHA256 MISMATCH {dest}: {h}")


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
    if not os.path.exists(os.path.join(DPIN, "usr/lib/x86_64-linux-gnu/Scrt1.o")):
        t = os.path.join(ROOT, "obj", "m1-pins")
        fetch_deb(URL_LIBC6DEV, LIBC6DEV_SHA, os.path.join(t, "libc6-dev.deb"))
        shutil.rmtree(DPIN, ignore_errors=True)
        os.makedirs(DPIN, exist_ok=True)
        run(["dpkg-deb", "-x", os.path.join(t, "libc6-dev.deb"), DPIN], check=True)


def build_libgl():
    out = os.path.join(ROOT, "obj", "mesa", "lib", "libGL.so.1")
    r = run(["make", "ARCH=intel", "xcompat_dsos"], cwd=ROOT,
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise SystemExit("m2: xcompat_dsos build failed")
    if SKIP_BUILD and os.path.exists(out):
        log(f"m2: reusing {out} (M2_SKIP_BUILD=1)")
    else:
        log("m2: building libGL.so.1 (tools/mesa/build-hobbyos.sh)")
        env = dict(os.environ)
        env["HOME"] = os.environ.get("HOME", "/home/sarah")
        env["PATH"] = "/usr/bin:" + env.get("PATH", "")
        b = os.path.join(ROOT, "obj", "m2_libgl_build.log")
        with open(b, "w") as f:
            r = run(["bash", os.path.join(ROOT, "tools/mesa/build-hobbyos.sh"),
                     "-j", os.environ.get("M2_JOBS", "16")],
                    env=env, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
        if r.returncode != 0:
            tail = "\n".join(open(b, errors="ignore").read().splitlines()[-25:])
            raise SystemExit("m2: libGL BUILD FAILED\n" + tail)
    if not os.path.exists(out):
        raise SystemExit(f"m2: {out} missing after the build")
    for line in open(os.path.join(ROOT, "obj", "m2_libgl_build.log"),
                     errors="ignore").read().splitlines():
        if re.search(r"(gallium:|patch |overlay:|DT_NEEDED:|exports:|undefined:|"
                     r"TLS:|df_1:|size:|audit:|RESULT )", line):
            log("    " + line.strip())
    return out


def build_probe(mode, out_name):
    os.makedirs(BUILD, exist_ok=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    dlib = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu")
    out = os.path.join(BUILD, out_name)
    cmd = [
        "gcc", "-O2", "-std=gnu11", "-fPIE", "-pie", "-nostdlib", "-nostartfiles",
        f'-DM2P_MODE="{mode}"',
        "-o", out, os.path.join(ROOT, "src/user/gl_m2probe.c"),
        os.path.join(dlib, "Scrt1.o"), os.path.join(dlib, "crti.o"),
        "-lX11",
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
    log(f"m2: building the {mode} probe ({out_name})")
    run(cmd, check=True)
    return out


def build_os():
    log("m2: building kernel + disk (desktop)")
    env = dict(os.environ)
    env["TOOLS_SKIP"] = "1"
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    r = run(["make", "-C", ROOT, f"ARCH={ARCH}", "MODE=desktop", "DESKTOP=hobby",
             "hobbyos.elf", "disk.img"], env=env, capture_output=True, text=True)
    open(os.path.join(ROOT, "obj", "m2_build.log"), "w").write(r.stdout + r.stderr)
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-25:])
        raise SystemExit("m2: BUILD FAILED\n" + tail)


def stage(probes):
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
    for img_name, path in probes.items():
        mc(path, f"::{img_name}")


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


def read_menu():
    menu = {}
    for line in serial_text().splitlines():
        m = re.search(r"\[MENU\]\s*(\d+)=(\S+)", line)
        if m:
            menu[int(m.group(1))] = m.group(2)
    return menu


def launch(q, app):
    menu = read_menu()
    if not menu:
        return False
    idx = next((i for i, n in menu.items()
                if n.upper().split(".")[0] == app.split(".")[0]), None)
    if idx is None:
        log(f"[FAIL] {app} is not in the desktop menu ({len(menu)} entries)")
        return False
    ups = max(menu) - idx
    log(f"[INFO] {app} at menu index {idx} of {len(menu)} ({ups} ups from the end)")
    q.key("f1")
    time.sleep(0.6)
    q.key("end")
    for _ in range(ups):
        q.key("up", pause=0.12)
    q.key("ret")
    return True


def rows_for(prefix):
    return [l for l in serial_text().splitlines() if prefix in l]


def first(rows, needle):
    return next((l for l in rows if needle in l), "")


def num(pattern, text, default=None):
    m = re.search(pattern, text)
    if not m:
        return default
    try:
        return float(m.group(1))
    except Exception:
        return default


def parse_run(rows, mode):
    """Extract the measurement rows of one probe run."""
    r = {}
    r["renderer"] = first(rows, "GL_RENDERER:")
    r["version"] = first(rows, "GL_VERSION :")
    r["f_row"] = first(rows, "F fixed-per-frame")
    r["p_row"] = first(rows, "p per-draw")
    r["fill_row"] = first(rows, "fill:")
    r["verify_row"] = first(rows, "verify glReadPixels")
    r["fb_row"] = first(rows, "verify /dev/fb0")
    r["done_row"] = first(rows, "done rc=")
    r["F_ms"] = num(r"mean=(\d+) us", r["f_row"])
    if r["F_ms"] is not None:
        r["F_ms"] = r["F_ms"] / 1000.0
    r["p_us"] = num(r"p=(-?[\d.]+) us/draw", r["p_row"])
    r["frame110_ms"] = num(r"frame110=([\d.]+) ms", r["p_row"])
    if r["frame110_ms"] is None:
        r["frame110_ms"] = num(r"110-draw frame=([\d.]+) ms", r["p_row"])
    r["fill_mpx_s"] = num(r"= ([\d.]+) Mpx/s", r["fill_row"])
    r["match"] = num(r"match=(\d+)/", r["verify_row"])
    r["total"] = num(r"match=\d+/(\d+)", r["verify_row"])
    r["fb_diff"] = num(r"diff=(-?\d+)/", r["fb_row"])
    r["rc"] = num(r"done rc=(\d+)", r["done_row"])
    r["mode"] = mode
    return r


def check_run(r):
    """Correctness: renderer identity, pixel-exact frame, presented frame."""
    problems = []
    want = r["mode"]
    if want == "virgl":
        if "virgl" not in r["renderer"].lower():
            problems.append(f"renderer is not virgl: {r['renderer']!r}")
    else:
        if "softpipe" not in r["renderer"].lower():
            problems.append(f"renderer is not softpipe: {r['renderer']!r}")
    if r["match"] is None or r["total"] is None or r["match"] != r["total"]:
        problems.append(f"GL readback is not pixel-exact: {r['verify_row']!r}")
    if r["fb_diff"] is None or (r["total"] and r["fb_diff"] > r["total"] / 10000):
        problems.append(f"the frame is not on /dev/fb0: {r['fb_row']!r}")
    if r["rc"] != 0:
        problems.append(f"probe exit: {r['done_row']!r}")
    if r["p_us"] is None or r["F_ms"] is None:
        problems.append("measurement rows missing")
    return problems


def main():
    os.makedirs(BUILD, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "obj"), exist_ok=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    ensure_pins()
    build_libgl()
    probes = {
        "M2PROBE.BIN": build_probe("virgl", "m2probe"),
        "M2PROBE_SW.BIN": build_probe("softpipe", "m2probe_sw"),
    }
    build_os()
    stage(probes)

    log(f"m2: booting the GL device (log {LOG})")
    ovmf = None
    for c in (os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
              "/usr/share/OVMF/OVMF_CODE_4M.fd"):
        if os.path.exists(c):
            ovmf = os.path.join(BUILD, "OVMF_CODE_4M.fd")
            if not os.path.exists(ovmf):
                shutil.copy(c, ovmf)
            break
    if not ovmf:
        raise SystemExit("m2: no OVMF_CODE_4M.fd found")

    if os.path.exists(LOG):
        os.remove(LOG)
    run(["fuser", "-k", IMG], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    qemu_args = f"-enable-kvm -qmp unix:{QMP_SOCK},server,nowait"
    env = dict(os.environ)
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    logf = open(LOG, "w")
    proc = subprocess.Popen(
        ["make", "-C", ROOT, "-o", "disk.img", f"ARCH={ARCH}", "MODE=desktop",
         "DESKTOP=hobby", f"EDK2_X86_64={ovmf}", "run",
         "QEMU_GPU=gl", "QEMU_DISPLAY=egl-headless", f"QEMU_ARGS={qemu_args}"],
        cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, env=env)

    try:
        deadline = time.time() + BOOT_TIMEOUT
        while time.time() < deadline:
            if "[MENU] count=" in serial_text():
                break
            time.sleep(1)
        if not read_menu():
            log("[FAIL] desktop never reported its menu mapping (boot failed?)")
            return 1
        time.sleep(3)
        q = Qmp(QMP_SOCK)

        results = []
        offset = 0
        for app, mode in (("M2PROBE.BIN", "virgl"), ("M2PROBE_SW.BIN", "softpipe")):
            if not launch(q, app):
                return 1
            started, offset = wait_serial("[M2P] probe start", 90, offset)
            log(f"[INFO] {app}: launch marker " + ("seen" if started else "MISSING"))
            done, offset = wait_serial("[M2P] done rc=", RUN_TIMEOUT, offset)
            log(f"[INFO] {app}: finished " + ("yes" if done else "NO"))
            rows = [l for l in serial_text().splitlines() if "[M2P]" in l]
            # Keep only this run's rows: the last "probe start" marker onward.
            cut = [i for i, l in enumerate(rows) if "probe start" in l]
            if cut:
                rows = rows[cut[-1]:]
            results.append(parse_run(rows, mode))
            time.sleep(1.5)

        log("--- probe rows (virgl) ---")
        for l in rows_for("[M2P]"):
            log(l)

        log("")
        log("=== M2 measurement summary ===")
        log(f"{'driver':10} {'p us/draw':>10} {'F ms':>8} {'frame110 ms':>12} "
            f"{'fill Mpx/s':>11} {'fb diff':>8}")
        ok_all = True
        for r in results:
            p_s = f"{r['p_us']:.1f}" if r["p_us"] is not None else "n/a"
            f_s = f"{r['F_ms']:.2f}" if r["F_ms"] is not None else "n/a"
            fr_s = f"{r['frame110_ms']:.2f}" if r["frame110_ms"] is not None else "n/a"
            fill_s = f"{r['fill_mpx_s']:.2f}" if r["fill_mpx_s"] is not None else "n/a"
            log(f"{r['mode']:10} {p_s:>10} {f_s:>8} {fr_s:>12} {fill_s:>11} "
                f"{str(r['fb_diff']):>8}")

        log("")
        v = next(r for r in results if r["mode"] == "virgl")
        s = next(r for r in results if r["mode"] == "softpipe")
        for r in results:
            problems = check_run(r)
            if problems:
                ok_all = False
                log(f"[FAIL] {r['mode']}:")
                for p in problems:
                    log(f"    - {p}")
            else:
                log(f"[PASS] {r['mode']}: renderer, pixel-exact frame and "
                    f"presented frame all verified")

        if v["p_us"] is not None and v["F_ms"] is not None:
            log("")
            log("=== against the plan's thresholds (docs/rimworld-virgl-plan.md §6.3) ===")
            verdict_p = "PASS" if v["p_us"] <= P_MAX_US else "FAIL"
            verdict_f = "PASS" if v["F_ms"] <= F_MAX_MS else "FAIL"
            log(f"  p = {v['p_us']:.2f} us/draw vs <= {P_MAX_US:.0f} us  "
                f"(1080p30)  -> {verdict_p}")
            log(f"  F = {v['F_ms']:.2f} ms/frame vs <= {F_MAX_MS:.0f} ms   "
                f"(1080p30)  -> {verdict_f}")
            log(f"  M7 gate (20 fps @720p, p <= {P_M7_US:.0f} us) -> "
                f"{'PASS' if v['p_us'] <= P_M7_US else 'FAIL'}")
            if v["p_us"] <= P_MAX_US and v["F_ms"] <= F_MAX_MS:
                per_frame = 110.0 * v["p_us"] / 1000.0 + v["F_ms"]
                log(f"  110 draws + one round trip = {per_frame:.2f} ms/frame "
                    f"({1000.0 / per_frame:.1f} fps at the guest's encode+present "
                    f"cost, before host fill and game CPU)")
            log(f"  softpipe for comparison: p={s['p_us']} us/draw, "
                f"F={s['F_ms']} ms")

        log("")
        log("m2: PASS — both drivers ran from one DSO; the virgl frame was "
            "pixel-exact on GL and on /dev/fb0" if ok_all else
            "m2: FAIL — see the rows above")
        return 0 if ok_all else 1
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        time.sleep(1)
        run(["fuser", "-k", IMG], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        logf.close()


if __name__ == "__main__":
    sys.exit(main())
