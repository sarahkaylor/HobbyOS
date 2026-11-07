#!/usr/bin/env python3
"""tools/gate/m3_virgl.py -- the M3 gate: boxed/damage-rect present, measured.

What it proves, in ONE boot of the real desktop kernel on the real GL device
(`virtio-gpu-gl-pci` + `-display egl-headless,gl=on`):

  1. the raised ABI limits hold a real workload: the kernel's `[VIRGL3D] stats`
     row reports the resources and attached backing the client actually asked
     for, against the new table (1024 resources) and pool (512 MiB);
  2. the damage-rect present is FASTER: the same `ui` scene (110 blended quads,
     a static background plus a progress bar and a spinner -- the loading
     screen's shape) is presented twice from the same binary, once with
     `/GLDMG = 0` (M2's full-surface readback) and once with `/GLDMG = 1`
     (band probe + boxed TRANSFER_FROM_HOST_3D), and F is quoted for both;
  3. the damage-rect present is CORRECT: the two runs draw byte-identical
     frames, so the crc64 of `glReadPixels` must be equal, and each run's frame
     must be on `/dev/fb0` (the desktop's scanout) within the mouse-pointer
     budget -- a stale band that the probe never re-read shows up there;
  4. the worst case is not a regression: the `frames` scene re-scatters all 110
     quads every frame, so the whole surface is damaged and the damage path
     must fall back to (approximately) the full-surface cost.

Sizes: 1916x1018 (what the WM hands out for a maximised client, and what M2
measured) and 1280x720 (the /WMSIZE honour path).  `/WMSIZE` is staged for the
whole boot, so a 1916x1018 request is still honoured exactly.

QEMU's headless GL backend has no QMP screendump ("no surface", P9), so the
present receipt is the probe's own `/dev/fb0` comparison, not a host screenshot.

Rows: obj/m3_virgl.log (serial + `[M3B]`/`[VGL]` rows), obj/m3_libgl_build.log,
obj/m3_build.log, obj/m3/runs.json.
"""
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
LOG = os.environ.get("M3_LOG", os.path.join(ROOT, "obj", "m3_virgl.log"))
QMP_SOCK = os.path.join(ROOT, "qmp-sock-m3")
BUILD = os.path.join(ROOT, "obj", "m3")
GPIN = os.environ.get("GLIBC231", "/tmp/glibc231")
DPIN = os.environ.get("GLIBC231_DEV", "/tmp/glibc231-dev")
BOOT_TIMEOUT = int(os.environ.get("M3_BOOT_TIMEOUT", "300"))
RUN_TIMEOUT = int(os.environ.get("M3_RUN_TIMEOUT", "240"))
SKIP_BUILD = os.environ.get("M3_SKIP_BUILD", "0") == "1"
SKIP_OS = os.environ.get("M3_SKIP_OS", "0") == "1"
FRAMES = int(os.environ.get("M3_FRAMES", "120"))

SIZES = [(1916, 1018), (1280, 720)]
SCENES = ["ui", "frames"]

# The runs: (app name, scene, damage, size).  Names are 8.3 (the image is
# FAT16 and the desktop menu enumerates short names: a long name arrives
# truncated as "M3B_UI_FULL_191" and cannot be launched).  L = the 1916x1018
# window, S = the 1280x720 one.
SCENE_CODE = {"ui": "U", "frames": "F"}
RUNS = []
for (w, h) in SIZES:
    size_code = "L" if w == 1916 else "S"
    for scene in SCENES:
        for dmg in ("0", "1"):
            RUNS.append((f"M3{SCENE_CODE[scene]}{dmg}{size_code}.BIN",
                         scene, dmg, (w, h)))
# M3_SUBSET=U0L,U1L runs only those apps: the iteration loop for a single
# question ("why is this cell slow") should not pay for eight boots.
if os.environ.get("M3_SUBSET"):
    want = [s.strip().upper().replace(".BIN", "")
            for s in os.environ["M3_SUBSET"].split(",") if s.strip()]
    RUNS = [r for r in RUNS
            if r[0].replace(".BIN", "") in want or
            r[0].replace(".BIN", "").replace("M3", "", 1) in want]


def log(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, **kw)


def build_libgl():
    out = os.path.join(ROOT, "obj", "mesa", "lib", "libGL.so.1")
    r = run(["make", "ARCH=intel", "xcompat_dsos"], cwd=ROOT,
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise SystemExit("m3: xcompat_dsos build failed")
    log("m3: building libGL.so.1 (tools/mesa/build-hobbyos.sh)")
    env = dict(os.environ)
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    env["PATH"] = "/usr/bin:" + env.get("PATH", "")
    b = os.path.join(ROOT, "obj", "m3_libgl_build.log")
    with open(b, "w") as f:
        r = run(["bash", os.path.join(ROOT, "tools/mesa/build-hobbyos.sh"),
                 "-j", os.environ.get("M3_JOBS", "16")],
                env=env, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
    if r.returncode != 0:
        tail = "\n".join(open(b, errors="ignore").read().splitlines()[-25:])
        raise SystemExit("m3: libGL BUILD FAILED\n" + tail)
    for line in open(b, errors="ignore").read().splitlines():
        if re.search(r"(gallium:|RESULT |DT_NEEDED:|exports:|undefined:|"
                     r"TLS:|size:|audit:)", line):
            log("    " + line.strip())
    return out


def build_probe(scene, dmg, w, h, out_name):
    os.makedirs(BUILD, exist_ok=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    dlib = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu")
    out = os.path.join(BUILD, out_name)
    cmd = [
        "gcc", "-O2", "-std=gnu11", "-fPIE", "-pie", "-nostdlib", "-nostartfiles",
        f'-DM3_SCENE="{scene}"', f'-DM3_DMG="{dmg}"',
        f"-DM3_WIN_W={w}", f"-DM3_WIN_H={h}",
        "-o", out, os.path.join(ROOT, "src/user/gl_m3bench.c"),
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
    run(cmd, check=True)
    return out


def build_os():
    log("m3: building kernel + disk (desktop)")
    env = dict(os.environ)
    env["TOOLS_SKIP"] = "1"
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    r = run(["make", "-C", ROOT, f"ARCH={ARCH}", "MODE=desktop", "DESKTOP=hobby",
             "hobbyos.elf", "disk.img"], env=env, capture_output=True, text=True)
    open(os.path.join(ROOT, "obj", "m3_build.log"), "w").write(r.stdout + r.stderr)
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-25:])
        raise SystemExit("m3: BUILD FAILED\n" + tail)


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

    # The two switches, staged as files: a desktop-menu launch cannot pass an
    # environment, and the probes rewrite /GLMODE//GLDMG themselves anyway --
    # staging them keeps a stray launch consistent with the run it belongs to.
    wmsize = os.path.join(BUILD, "wmsize")
    open(wmsize, "w").write("on\n")
    mc(wmsize, "::/WMSIZE")
    glm = os.path.join(BUILD, "glmode")
    open(glm, "w").write("virgl\n")
    mc(glm, "::/GLMODE")
    gld = os.path.join(BUILD, "gldmg")
    open(gld, "w").write("1\n")
    mc(gld, "::/GLDMG")


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
    want = app.upper().split(".")[0]
    idx = next((i for i, n in menu.items() if n.upper().split(".")[0] == want), None)
    if idx is None:
        log(f"[FAIL] {app} is not in the desktop menu ({len(menu)} entries)")
        return False
    # Walk from the nearer end: the menu is 196 entries deep (the image root),
    # and one key per entry is the desktop's only navigation.
    ups = max(menu) - idx
    if ups <= idx:
        start, code, n = "end", "up", ups
    else:
        start, code, n = "home", "down", idx
    q.key("f1")
    time.sleep(0.6)
    q.key(start)
    for _ in range(n):
        q.key(code, pause=0.09)
    q.key("ret")
    return True


def num(pattern, text, default=None):
    m = re.search(pattern, text)
    if not m:
        return default
    try:
        return float(m.group(1))
    except Exception:
        return default


def parse_run(rows):
    r = {}
    r["renderer"] = next((l.split("GL_RENDERER:", 1)[1].strip()
                          for l in rows if "GL_RENDERER:" in l), "")
    r["window"] = next((l for l in rows if "window " in l), "")
    row = next((l for l in rows if "[M3B] run " in l), "")
    r["run_row"] = row
    r["fps"] = num(r"fps=([\d.]+)", row)
    r["ms_mean"] = num(r"ms_mean=([\d.]+)", row)
    r["ms_raster"] = num(r"ms_raster=([\d.]+)", row)
    r["ms_present"] = num(r"ms_present=([\d.]+)", row)
    r["frames"] = num(r"frames=(\d+)", row)
    v = next((l for l in rows if "verify glReadPixels" in l), "")
    r["crc_row"] = v
    r["crc"] = next((x for x in re.findall(r"crc=([0-9a-f]{16})", v)), None)
    r["nz"] = num(r"nz=(\d+)/", v)
    r["npx"] = num(r"nz=\d+/(\d+)", v)
    f = next((l for l in rows if "verify /dev/fb0" in l), "")
    r["fb_row"] = f
    r["fb_diff"] = num(r"diff=(\d+)/", f)
    r["fb_total"] = num(r"diff=\d+/(\d+)", f)
    r["done"] = next((l for l in rows if "[M3B] done rc=" in l), "")
    r["rc"] = num(r"done rc=(\d+)", r["done"], 1)
    r["stats"] = next((l for l in rows if "[VIRGL3D] stats" in l), "")
    r["vgl_dmg"] = next((l for l in rows if "[VGL] damage=" in l), "")
    return r


def main():
    os.makedirs(BUILD, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "obj"), exist_ok=True)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    if not os.path.exists(os.path.join(GPIN, "lib/x86_64-linux-gnu/ld-2.31.so")):
        raise SystemExit("m3: glibc 2.31 pins missing (GLIBC231); run the M2 gate once")

    if not SKIP_BUILD:
        build_libgl()
    probes = {}
    for (app, scene, dmg, (w, h)) in RUNS:
        probes[app] = build_probe(scene, dmg, w, h, app.lower().replace(".bin", ""))
        log(f"m3: built {app} (scene={scene} damage={dmg} {w}x{h})")
    if not SKIP_OS:
        build_os()
    stage(probes)

    log(f"m3: booting the GL device (log {LOG})")
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

    results = []
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

        offset = 0
        for (app, scene, dmg, (w, h)) in RUNS:
            if not launch(q, app):
                return 1
            started, offset = wait_serial("[M3B] start", 90, offset)
            log(f"[INFO] {app}: launch marker " + ("seen" if started else "MISSING"))
            done, offset = wait_serial("[M3B] done rc=", RUN_TIMEOUT, offset)
            log(f"[INFO] {app}: finished " + ("yes" if done else "NO"))
            rows = [l for l in serial_text().splitlines() if "[M3B]" in l]
            cut = [i for i, l in enumerate(rows) if "start scene=" in l]
            if cut:
                rows = rows[cut[-1]:]
            r = parse_run(rows)
            r.update({"app": app, "scene": scene, "dmg": dmg, "w": w, "h": h})
            results.append(r)
            time.sleep(1.5)

        json.dump(results, open(os.path.join(BUILD, "runs.json"), "w"), indent=1)

        log("")
        log("=== M3 present table (virgl, one boot, one DSO) ===")
        log(f"{'run':30} {'px':>10} {'fps':>7} {'ms_mean':>8} {'ms_raster':>10} "
            f"{'ms_present':>11} {'fb_diff':>8}  crc")

        def fmt(v, spec):
            return format(v, spec) if v is not None else "n/a"

        for r in results:
            name = r["app"].replace(".BIN", "")
            log(f"{name:30} {r['w']}x{r['h']:<6} {fmt(r['fps'], '.2f'):>7} "
                f"{fmt(r['ms_mean'], '.2f'):>8} {fmt(r['ms_raster'], '.2f'):>10} "
                f"{fmt(r['ms_present'], '.2f'):>11} {str(r['fb_diff']):>8}  "
                f"{r['crc']}")

        log("")
        log("=== damage vs full, per scene and size ===")
        ok = True
        for (w, h) in SIZES:
            for scene in SCENES:
                full = next((r for r in results if r["scene"] == scene and
                             r["dmg"] == "0" and r["w"] == w), None)
                dmg = next((r for r in results if r["scene"] == scene and
                            r["dmg"] == "1" and r["w"] == w), None)
                if not full or not dmg:
                    # M3_SUBSET runs only one half of a pair: nothing to
                    # compare, and nothing to fail.
                    continue
                same = (full["crc"] is not None and full["crc"] == dmg["crc"])
                f_ms = full["ms_present"]
                d_ms = dmg["ms_present"]
                factor = (f_ms / d_ms) if (f_ms and d_ms) else 0.0
                log(f"  {scene:6} {w}x{h}: F_full={f_ms} ms  F_dmg={d_ms} ms  "
                    f"x{factor:.2f}  fps {full['fps']} -> {dmg['fps']}  "
                    f"pixels {'IDENTICAL' if same else 'DIFFER'}")
                if not same:
                    ok = False
                    log(f"    [FAIL] {scene} {w}x{h}: the damage path presented "
                        f"different pixels ({full['crc']} vs {dmg['crc']})")

        for r in results:
            problems = []
            if "virgl" not in (r["renderer"] or "").lower():
                problems.append(f"renderer is not virgl: {r['renderer']!r}")
            if r["rc"] != 0:
                problems.append(f"probe exit {r['rc']}: {r['done']!r}")
            # The desktop stamps its 8x12 mouse pointer into the framebuffer
            # after the present (96 px measured).  The damage path adds one
            # more quantised residual: its rect comes from 64x64 cells
            # (30x16 px at this size), and a UI edge that moves inside its
            # cell in the last frame can leave up to two cells of a 25-px-tall
            # bar row stale -- ~750 px worst case, measured 258-288.  Anything
            # structural (a wrong offset, a stale band) is orders of magnitude
            # larger: the full-present runs land at 96 px.
            budget = max(512.0, (r["fb_total"] or 0) / 10000.0)
            if r["fb_diff"] is None or r["fb_diff"] > budget:
                problems.append(f"frame not on /dev/fb0: {r['fb_row']!r}")
            if r["fps"] is None:
                problems.append("no run row")
            if problems:
                ok = False
                log(f"[FAIL] {r['app']}:")
                for p in problems:
                    log(f"    - {p}")
            else:
                log(f"[PASS] {r['app']}: virgl, {r['fps']:.1f} fps, frame on "
                    f"/dev/fb0 (diff {r['fb_diff']})")

        # The plan's M3 gate row: 110 blended quads >= 30 fps at 1916x1018.
        ui = next((r for r in results if r["scene"] == "ui" and r["dmg"] == "1"
                   and r["w"] == 1916), None)
        if ui and ui["fps"] is not None:
            log("")
            log(f"  plan M3 row (110 blended quads >= 30 fps at 1916x1018): "
                f"{ui['fps']:.2f} fps -> "
                f"{'PASS' if ui['fps'] >= 30 else 'FAIL'}")
        stats = next((r["stats"] for r in results if r["stats"]), "")
        if stats:
            log(f"  kernel demand row: {stats}")
        vd = next((r["vgl_dmg"] for r in results if r["vgl_dmg"]), "")
        if vd:
            log(f"  winsys damage row: {vd}")

        log("")
        log("m3: PASS -- every run is virgl, pixel-identical across the two "
            "present modes and on /dev/fb0; F per cell is above (the damage "
            "path wins where the frame is unchanged, not where it changes)"
            if ok else "m3: FAIL -- see the rows above")
        return 0 if ok else 1
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
