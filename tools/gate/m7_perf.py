#!/usr/bin/env python3
"""tools/gate/m7_perf.py -- the M7 performance gate and measurement harness.

Answers one question with numbers: **can the guest rasteriser draw RimWorld's
frame fast enough, at the size the game will actually get?**  It runs one
benchmark (src/user/gl_bench.c) in three places and diffs them:

  guest-softpipe   the REAL kernel, the REAL compat libX11, libGL.so.1 dlopen()ed
                   from /LIB64 in the guest -- the configuration the game gets.
  host-softpipe    the same libGL.so.1 bytes on the host (LD_LIBRARY_PATH), no
                   compat layer, no virtualisation.  guest/host difference is
                   the cost of HobbyOS's Linux-ABI layer.
  host-llvmpipe    the host's system Mesa, 8 threads -- the configuration that
                   measured 33.2 fps median on the real RimWorld menu at
                   1280x720 (docs/rimworld-evidence.md).  softpipe/llvmpipe is
                   therefore the rasteriser ratio, measured on the same scene.

Why three and not two: a two-way guest-vs-llvmpipe diff cannot say whether a
slow number is the compat layer, the guest CPU, or softpipe.  With the same DSO
on the host, each factor is separable.

The scene is 110 textured alpha-blended quads (RimWorld's measured median draw
count/frame) at ~2 screens of overdraw, drawn one DrawArrays each, at
640x480 / 960x540 / 1280x720 / 1916x1018 -- the last being the desktop's
maximised content rect, which is the only window size the WM gives a client.

Modes per size (see gl_bench.c):
  fbo      render to an off-screen FBO, glFinish, never present  (Task 2)
  present  the same, then blit to the window + glXSwapBuffers + XSync
  direct   default framebuffer at the drawable size + swap -- Unity's own path
  sprites  110 fill-free draws: per-draw CPU cost
  sweep    1/110/500 fill-free draws: the per-draw cost as a measured slope

Usage:
  python3 tools/gate/m7_perf.py                 # guest + host, write the tables
  python3 tools/gate/m7_perf.py --host-only     # host comparisons only
  python3 tools/gate/m7_perf.py --guest-only
  python3 tools/gate/m7_perf.py --table-only    # re-render from perf.json

Outputs: obj/m7/perf.json, obj/m7/m7-table.md, obj/m7/m7_guest.log,
obj/m7/host-llvmpipe.log, obj/m7/host-softpipe.log, and screenshots under
obj/m7/shots/.  Exit 0 only if every expected cell produced a row in the guest
AND the guest reported softpipe with a real scene checksum.
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
IMG = os.path.join(ROOT, "disk.img")
OUT = os.environ.get("M7_OUT", os.path.join(ROOT, "obj", "m7"))
LOG = os.path.join(OUT, "m7_guest.log")
JSON_PATH = os.path.join(OUT, "perf.json")
TABLE_PATH = os.path.join(OUT, "m7-table.md")
SHOT_DIR = os.path.join(OUT, "shots")
QMP_SOCK = os.path.join(ROOT, "qmp-sock-m7")
BUILD = os.path.join(OUT, "build")
GPIN = os.environ.get("GLIBC231", "/tmp/glibc231")
DPIN = os.environ.get("GLIBC231_DEV", "/tmp/glibc231-dev")
BOOT_TIMEOUT = int(os.environ.get("M7_BOOT_TIMEOUT", "240"))
# The benchmark's own budget: 13 cells, min 15 frames each.  At the slowest
# measured cell (1916x1018 present, ~0.36 s/frame) that is ~5.5 s per cell.
RUN_TIMEOUT = int(os.environ.get("M7_RUN_TIMEOUT", "300"))
SKIP_MESA = os.environ.get("M7_SKIP_MESA", "0") == "1"
ARCH = os.environ.get("M7_ARCH", "intel")

# The host reference for the real game's menu (docs/rimworld-evidence.md §2).
HOST_GAME_FPS_720P = 33.2
HOST_GAME_DRAWS = 110


def log(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, **kw)


# ---- build ----------------------------------------------------------------

def ensure_pins():
    ld = os.path.join(GPIN, "lib/x86_64-linux-gnu/ld-2.31.so")
    if os.path.exists(ld):
        return
    log("m7: pins missing -- run tools/gate/m4_gl_probe.py once to fetch them")
    raise SystemExit("m7: pinned glibc 2.31 not found at " + GPIN)


def build_libgl():
    out = os.path.join(ROOT, "obj", "mesa", "lib", "libGL.so.1")
    r = run(["make", "ARCH=intel", "xcompat_dsos"], cwd=ROOT,
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise SystemExit("m7: xcompat_dsos build failed")
    if SKIP_MESA and os.path.exists(out):
        log(f"m7: reusing {out} (M7_SKIP_MESA=1)")
    elif not os.path.exists(out):
        log("m7: building libGL.so.1 (tools/mesa/build-hobbyos.sh)")
        env = dict(os.environ, HOME=os.environ.get("HOME", "/home/sarah"))
        env["PATH"] = "/usr/bin:" + env.get("PATH", "")
        b = os.path.join(OUT, "libgl_build.log")
        with open(b, "w") as f:
            r = run(["bash", os.path.join(ROOT, "tools/mesa/build-hobbyos.sh"),
                     "-j", os.environ.get("M7_JOBS", "16")],
                    env=env, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
        if r.returncode != 0:
            tail = "\n".join(open(b, errors="ignore").read().splitlines()[-25:])
            raise SystemExit("m7: libGL BUILD FAILED\n" + tail)
    if not os.path.exists(out):
        raise SystemExit(f"m7: {out} missing")
    return out


def build_bench_guest():
    """The same link shape as the game binary and the M4 probe: a glibc 2.31 PIE
    whose DT_NEEDED are libX11/libdl/libm/libpthread/libc.  libGL is dlopen()ed."""
    os.makedirs(BUILD, exist_ok=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    dlib = os.path.join(DPIN, "usr/lib/x86_64-linux-gnu")
    out = os.path.join(BUILD, "glbench")
    cmd = [
        "gcc", "-O2", "-std=gnu11", "-fPIE", "-pie", "-nostdlib", "-nostartfiles",
        "-o", out, os.path.join(ROOT, "src/user/gl_bench.c"),
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
    log("m7: building the benchmark (glibc 2.31 PIE, guest link shape)")
    run(cmd, check=True)
    needed = run(["readelf", "-dW", out], capture_output=True, text=True).stdout
    log("m7: bench DT_NEEDED: " + ", ".join(
        re.findall(r"Shared library: \[([^\]]+)\]", needed)))
    return out


def build_bench_host():
    """Ordinary host binary.  -lXext with --no-as-needed because libGL.so.1 has
    no DT_NEEDED of its own: RTLD_NOW resolves its XShm* symbols from the
    process, and --as-needed would drop a library nothing references directly."""
    out = os.path.join(BUILD, "glbench_host")
    os.makedirs(BUILD, exist_ok=True)
    log("m7: building the benchmark (host)")
    run(["gcc", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-o", out,
         os.path.join(ROOT, "src/user/gl_bench.c"),
         "-Wl,--no-as-needed", "-lX11", "-lXext", "-ldl", "-lm",
         "-Wl,--as-needed"], check=True)
    return out


def build_os():
    log("m7: kernel + disk (desktop)")
    env = dict(os.environ)
    env["TOOLS_SKIP"] = "1"
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    r = run(["make", "-C", ROOT, f"ARCH={ARCH}", "MODE=desktop", "DESKTOP=hobby",
             "hobbyos.elf", "disk.img"], env=env, capture_output=True, text=True)
    if r.returncode != 0:
        open(os.path.join(OUT, "build.log"), "w").write(r.stdout + r.stderr)
        raise SystemExit("m7: BUILD FAILED\n" +
                         "\n".join((r.stdout + r.stderr).splitlines()[-25:]))


def stage(bench):
    """DSOs -> /LIB64, pinned glibc beside them, the benchmark -> /BENCH.BIN."""
    run(["bash", os.path.join(ROOT, "src/user/xcompat/stage.sh"), IMG], check=True)
    run(["bash", os.path.join(ROOT, "tools/mesa/stage.sh"), IMG], check=True)
    ldir = os.path.join(GPIN, "lib/x86_64-linux-gnu")
    glibc = {
        "ld-2.31.so": "ld-linux-x86-64.so.2",
        "libc-2.31.so": "libc.so.6",
        "libm-2.31.so": "libm.so.6",
        "libpthread-2.31.so": "libpthread.so.0",
        "libdl-2.31.so": "libdl.so.2",
        "librt-2.31.so": "librt.so.1",
        "libgcc_s.so.1": "libgcc_s.so.1",
    }
    for d in ("::/LIB64", "::/lib/x86_64-linux-gnu"):
        run(["mmd", "-i", IMG, d], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
    for src, dst in glibc.items():
        run(["mcopy", "-o", "-i", IMG, os.path.join(ldir, src), f"::/LIB64/{dst}"],
            check=True)
    run(["mcopy", "-o", "-i", IMG, os.path.join(ldir, "ld-2.31.so"),
         "::/lib64/ld-linux-x86-64.so.2"], check=True)
    run(["mcopy", "-o", "-i", IMG, bench, "::BENCH.BIN"], check=True)
    log("m7: staged ::/BENCH.BIN")


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


# ---- parsing --------------------------------------------------------------

ROW = re.compile(r"\[PB\] RESULT (\S.*)$")


def parse_rows(text):
    """[PB] RESULT lines -> [{key: value}] with numbers as floats."""
    rows = []
    for line in text.splitlines():
        m = ROW.search(line)
        if not m:
            continue
        d = {}
        for tok in m.group(1).split():
            if "=" not in tok:
                continue
            k, v = tok.split("=", 1)
            try:
                d[k] = float(v)
            except ValueError:
                d[k] = v
        rows.append(d)
    return rows


def parse_banner(text):
    out = {}
    for line in text.splitlines():
        m = re.match(r"\[PB\] (renderer|version|glsl|scene)=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2).strip()
        m = re.match(r"\[PB\] window content rect (\d+)x(\d+)", line)
        if m:
            out["window"] = f"{m.group(1)}x{m.group(2)}"
    m = re.search(r"\[PB\] CHECK scene (\d+)x(\d+) fnv1a64=(0x[0-9a-f]+) "
                  r"nonbg=(\d+)/(\d+)", text)
    if m:
        out["check"] = {"w": int(m.group(1)), "h": int(m.group(2)),
                        "fnv1a64": m.group(3),
                        "nonbg": int(m.group(4)), "px": int(m.group(5))}
    return out


# ---- host comparison ------------------------------------------------------

def xvfb_start(display=":97"):
    run(["pkill", "-f", f"Xvfb {display}"], stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen(["Xvfb", display, "-screen", "0", "1920x1080x24",
                          "-nolisten", "tcp"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        if os.path.exists(f"/tmp/.X11-unix/X{display[1:]}"):
            return p
        time.sleep(0.1)
    return p


def run_host(bench, label, env_extra, display=":97"):
    env = dict(os.environ)
    env.update(env_extra)
    env["DISPLAY"] = display
    env["PB_WINDOW"] = "1916x1018"   # match the guest's maximised drawable
    env["HOME"] = os.environ.get("HOME", "/home/sarah")
    logf = os.path.join(OUT, f"host-{label}.log")
    log(f"m7: host run [{label}] -> {logf}")
    with open(logf, "w") as f:
        r = run([bench], env=env, stdout=f, stderr=subprocess.STDOUT,
                timeout=RUN_TIMEOUT)
    txt = open(logf, errors="ignore").read()
    rows = parse_rows(txt)
    banner = parse_banner(txt)
    log(f"m7: host[{label}] renderer={banner.get('renderer','?')} rows={len(rows)} "
        f"rc={r.returncode}")
    return {"label": label, "rows": rows, "banner": banner,
            "rc": r.returncode, "log": logf}


# ---- guest ----------------------------------------------------------------

def run_guest():
    log(f"m7: booting (log {LOG})")
    ovmf = None
    for c in (os.path.expanduser("~/.local/share/OVMF/OVMF_CODE_4M.fd"),
              "/usr/share/OVMF/OVMF_CODE_4M.fd"):
        if os.path.exists(c):
            ovmf = os.path.join(BUILD, "OVMF_CODE_4M.fd")
            if not os.path.exists(ovmf):
                shutil.copy(c, ovmf)
            break
    if not ovmf:
        raise SystemExit("m7: no OVMF_CODE_4M.fd found")

    if os.path.exists(LOG):
        os.remove(LOG)
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    run(["fuser", "-k", IMG], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    qemu_args = f"-display none -enable-kvm -qmp unix:{QMP_SOCK},server,nowait"
    env = dict(os.environ, HOME=os.environ.get("HOME", "/home/sarah"))
    logf = open(LOG, "w")
    proc = subprocess.Popen(
        ["make", "-C", ROOT, "-o", "disk.img", f"ARCH={ARCH}", "MODE=desktop",
         "DESKTOP=hobby", f"EDK2_X86_64={ovmf}", "run", f"QEMU_ARGS={qemu_args}"],
        cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, env=env)
    shots = []
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
            log("[FAIL] the desktop never reported its menu (boot failed?)")
            return None
        time.sleep(3)
        q = Qmp(QMP_SOCK)
        p = os.path.join(SHOT_DIR, "00_desktop.ppm")
        q.shot(p)
        shots.append(p)

        app = "BENCH.BIN"
        idx = next((i for i, n in menu.items()
                    if n.upper().split(".")[0] == app.split(".")[0]), None)
        if idx is None:
            log(f"[FAIL] {app} not in the desktop menu ({len(menu)} entries)")
            return None
        ups = max(menu) - idx
        log(f"[INFO] {app} at menu index {idx} of {len(menu)} ({ups} ups)")
        q.key("f1")
        time.sleep(0.6)
        q.key("end")
        for _ in range(ups):
            q.key("up", pause=0.12)
        q.key("ret")

        started, _ = wait_serial("[PB] bench start", 60)
        log("[INFO] launch marker: " + ("seen" if started else "MISSING"))
        # A frame mid-run: proof the benchmark is presenting into a real window.
        wait_serial("mode=present w=1916", RUN_TIMEOUT)
        time.sleep(0.8)
        p = os.path.join(SHOT_DIR, "01_present.ppm")
        q.shot(p)
        shots.append(p)
        done, _ = wait_serial("[PB] DONE", RUN_TIMEOUT)
        log("[INFO] benchmark finished: " + ("yes" if done else "NO"))
        time.sleep(0.5)
        p = os.path.join(SHOT_DIR, "02_after.ppm")
        q.shot(p)
        shots.append(p)
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        time.sleep(1)
        run(["fuser", "-k", IMG], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        logf.close()
        for p in shots:
            png = p[:-4] + ".png"
            run(["convert", p, png], stderr=subprocess.DEVNULL)
            log(f"m7: screenshot {png}")

    txt = serial_text()
    rows = parse_rows(txt)
    banner = parse_banner(txt)
    done = "[PB] DONE fail=0" in txt
    log(f"m7: guest renderer={banner.get('renderer','?')} rows={len(rows)} "
        f"done={done}")
    return {"label": "guest", "rows": rows, "banner": banner, "rc": 0 if done else 1,
            "log": LOG, "done": done}


# ---- analysis -------------------------------------------------------------

def cell(rows, mode, w, h):
    for r in rows:
        if r.get("mode") == mode and int(r.get("w", 0)) == w and int(r.get("h", 0)) == h:
            return r
    return None


def sweep_slope(rows):
    """Per-draw cost from the two fill-free cells that are long enough to
    measure: 110 and 500 draws.  sweep1 is deliberately not used as the low
    point -- it can finish inside one clock tick (0 ms in the guest) and a
    quantised zero turns into a bogus slope."""
    a = cell(rows, "sweep110", 1280, 720)
    b = cell(rows, "sweep500", 1280, 720)
    if not a or not b:
        return None
    dn = b["draws"] - a["draws"]
    if dn <= 0:
        return None
    return (b["ms_p50"] - a["ms_p50"]) * 1000.0 / dn


def summarise(res):
    rows = res["rows"]
    out = {"label": res["label"], "renderer": res["banner"].get("renderer"),
           "version": res["banner"].get("version"),
           "rc": res["rc"]}
    s = {}
    for w, h in ((640, 480), (960, 540), (1280, 720), (1916, 1018)):
        f = cell(rows, "fbo", w, h)
        p = cell(rows, "present", w, h)
        s[f"{w}x{h}"] = {
            "fbo_fps": f["fps_median"] if f else None,
            "fbo_ms": f["ms_p50"] if f else None,
            "fbo_raster_ms": f.get("ms_raster") if f else None,
            "fbo_frames": int(f["frames"]) if f else 0,
            "present_fps": p["fps_median"] if p else None,
            "present_ms": p["ms_p50"] if p else None,
            "present_raster_ms": p.get("ms_raster") if p else None,
            "present_blit_ms": p.get("ms_blit") if p else None,
            "present_swap_ms": p.get("ms_present") if p else None,
        }
    out["sizes"] = s
    d = next((r for r in rows if r.get("mode") == "direct"), None)
    out["direct"] = ({"w": int(d["w"]), "h": int(d["h"]), "fps": d["fps_median"],
                      "ms": d["ms_p50"], "present_ms": d["ms_present"]}
                     if d else None)
    sp = next((r for r in rows if r.get("mode") == "sprites"), None)
    out["sprites"] = ({"fps": sp["fps_median"], "ms": sp["ms_p50"],
                       "us_per_draw": sp["us_per_draw"]} if sp else None)
    out["draw_us_slope"] = sweep_slope(rows)
    out["check"] = res["banner"].get("check")
    return out


def fmt(v, nd=1):
    return "—" if v is None else f"{v:.{nd}f}"


def table_md(data):
    guest = data["runs"].get("guest")
    llvm = data["runs"].get("host-llvmpipe")
    soft = data["runs"].get("host-softpipe")
    L = []
    L.append("### Rasterisation (render to an FBO, never present)")
    L.append("")
    L.append("| size | pixels | guest softpipe | host softpipe | host llvmpipe | "
             "softpipe / llvmpipe |")
    L.append("|---|---|---|---|---|---|")
    for k in ("640x480", "960x540", "1280x720", "1916x1018"):
        w, h = (int(x) for x in k.split("x"))
        g = (guest or {}).get("sizes", {}).get(k, {})
        s = (soft or {}).get("sizes", {}).get(k, {})
        l = (llvm or {}).get("sizes", {}).get(k, {})
        ratio = None
        if g.get("fbo_ms") and l.get("fbo_ms"):
            ratio = g["fbo_ms"] / l["fbo_ms"]
        elif s.get("fbo_ms") and l.get("fbo_ms"):
            ratio = s["fbo_ms"] / l["fbo_ms"]
        L.append(
            f"| {k} | {w*h/1e6:.2f} MP | "
            f"{fmt(g.get('fbo_fps'))} fps / {fmt(g.get('fbo_ms'))} ms | "
            f"{fmt(s.get('fbo_fps'))} fps / {fmt(s.get('fbo_ms'))} ms | "
            f"{fmt(l.get('fbo_fps'))} fps / {fmt(l.get('fbo_ms'), 2)} ms | "
            f"{fmt(ratio, 1)}× |")
    L.append("")
    L.append("### Where the frame time goes (guest softpipe, ms/frame)")
    L.append("")
    L.append("| size | fbo: raster | present: raster | present: blit | "
             "present: swap+XSync | present total |")
    L.append("|---|---|---|---|---|---|")
    for k in ("640x480", "960x540", "1280x720", "1916x1018"):
        g = (guest or {}).get("sizes", {}).get(k, {})
        L.append(f"| {k} | {fmt(g.get('fbo_raster_ms'))} | "
                 f"{fmt(g.get('present_raster_ms'))} | "
                 f"{fmt(g.get('present_blit_ms'))} | "
                 f"{fmt(g.get('present_swap_ms'), 2)} | "
                 f"{fmt(g.get('present_ms'))} |")
    L.append("")
    L.append("### Per-draw cost (fill-free cells, 1280x720)")
    L.append("")
    L.append("| | guest softpipe | host softpipe | host llvmpipe |")
    L.append("|---|---|---|---|")
    def ud(r):
        if not r:
            return "—"
        return "—" if r.get("draw_us_slope") is None else \
            f"{fmt(r['draw_us_slope'], 1)} µs/draw"
    L.append(f"| 1 → 500 draws (slope) | {ud(guest)} | {ud(soft)} | {ud(llvm)} |")
    L.append("")
    d = (guest or {}).get("direct")
    if d:
        L.append(f"Game path (default framebuffer + swap) in the guest: "
                 f"{d['w']}x{d['h']} → {fmt(d['fps'])} fps, "
                 f"{fmt(d['ms'])} ms/frame, of which {fmt(d['present_ms'], 2)} ms "
                 f"is swap/XPutImage.")
    pr = data.get("prediction")
    if pr:
        L.append("")
        L.append("### Calibrated prediction for the real menu "
                 "(fill scaling × per-draw, see docs/rimworld-m7-perf.md §4)")
        L.append("")
        L.append(f"k_fill = {pr['k_fill']:.2f}× "
                 f"(scene fill is that much cheaper than the game's on llvmpipe, "
                 f"from the 33.2 fps @ 1280x720 reference); "
                 f"softpipe per-draw = {pr['draw_us_softpipe']:.0f} µs.")
        L.append("")
        L.append("| size | predicted menu fps | predicted ms/frame |")
        L.append("|---|---|---|")
        for k in ("640x480", "960x540", "1280x720", "1916x1018"):
            v = pr["sizes"].get(k)
            if v:
                L.append(f"| {k} | {v['pred_fps']:.2f} | {v['pred_ms']:.0f} |")
        if pr.get("llvmpipe_sizes"):
            L.append("")
            L.append("Same model on llvmpipe (a threaded software rasteriser, "
                     "no GPU):")
            L.append("")
            L.append("| size | predicted menu fps on llvmpipe | ms/frame |")
            L.append("|---|---|---|")
            for k in ("640x480", "960x540", "1280x720", "1916x1018"):
                v = pr["llvmpipe_sizes"].get(k)
                if v:
                    L.append(f"| {k} | {v['pred_fps']:.1f} | {v['pred_ms']:.0f} |")
    th = data.get("thresholds")
    if th:
        L.append("")
        L.append(f"Measured fit (guest, scene): {th['ms_per_mp']:.0f} ms per MP + "
                 f"{th['fixed_ms']:.0f} ms fixed.  Crosses 20 fps at "
                 f"{th['at_20fps']['size_16_9']} ({th['at_20fps']['mp']:.2f} MP), "
                 f"10 fps at {th['at_10fps']['size_16_9']} "
                 f"({th['at_10fps']['mp']:.2f} MP).")
    return "\n".join(L)


def prediction(data):
    """Calibrated game-fps model, with its assumption stated.

    The scene is cheaper per pixel than Unity's menu, so absolute scene fps is
    NOT a game fps.  What transfers is the *fill* ratio and the measured
    per-draw cost:

      game_ms(S) = scene_fill_ms(S) * k_fill + 110 * draw_us

    k_fill is fixed by the one place where the real game was measured: the host
    reference is 33.2 fps (30.1 ms) at 1280x720 on llvmpipe with 110 draws.
    scene_fill_ms(720p) on llvmpipe is the scene's raster time minus its own
    draw cost, so k_fill = (30.1 - 110*draw_us_llvmpipe) / scene_fill_ms_llvm(720p).

    Two assumptions, both labelled in the doc: the game's frame is fill-bound
    (the evidence doc supports this -- apitrace overhead did not lower its fps),
    and its fill scales with pixels the way the scene's does.
    """
    llvm = data["runs"].get("host-llvmpipe")
    guest = data["runs"].get("guest") or data["runs"].get("host-softpipe")
    if not llvm or not guest:
        return None
    r720 = llvm["sizes"]["1280x720"]["fbo_raster_ms"]
    d_llvm = llvm["draw_us_slope"]
    if not r720 or not d_llvm:
        return None
    draws = HOST_GAME_DRAWS
    game_ms_720 = 1000.0 / HOST_GAME_FPS_720P
    scene_fill_llvm = r720 - draws * d_llvm / 1000.0
    k_fill = (game_ms_720 - draws * d_llvm / 1000.0) / scene_fill_llvm
    d_soft = guest["draw_us_slope"] or 0.0
    out = {"k_fill": k_fill, "draw_us_llvmpipe": d_llvm, "draw_us_softpipe": d_soft,
           "scene_fill_ms_llvmpipe_720p": scene_fill_llvm,
           "host_game_ms_720p": game_ms_720, "sizes": {}}
    for key, v in guest["sizes"].items():
        r = v.get("fbo_raster_ms")
        if not r:
            continue
        fill = max(r - draws * d_soft / 1000.0, 0.0)
        ms = fill * k_fill + draws * d_soft / 1000.0
        out["sizes"][key] = {"pred_fps": 1000.0 / ms, "pred_ms": ms,
                             "scene_fill_ms": fill}
    # The same model on llvmpipe: what a threaded rasteriser would buy, and
    # what the desktop's maximised window costs on its own.
    out["llvmpipe_sizes"] = {}
    for key, v in llvm["sizes"].items():
        r = v.get("fbo_raster_ms")
        if not r:
            continue
        fill = max(r - draws * d_llvm / 1000.0, 0.0)
        ms = fill * k_fill + draws * d_llvm / 1000.0
        out["llvmpipe_sizes"][key] = {"pred_fps": 1000.0 / ms, "pred_ms": ms}
    return out


def threshold_fit(data):
    """Least-squares fit of guest frame time against pixels, and the sizes where
    the measured scene crosses 20 and 10 fps.  Interpolation over four measured
    points, not extrapolation beyond them except where stated."""
    g = data["runs"].get("guest")
    if not g:
        return None
    pts = []
    for k, v in g["sizes"].items():
        if v.get("fbo_ms"):
            w, h = (int(x) for x in k.split("x"))
            pts.append((w * h / 1e6, v["fbo_ms"]))
    if len(pts) < 3:
        return None
    n = len(pts)
    sx = sum(p[0] for p in pts)
    sy = sum(p[1] for p in pts)
    sxx = sum(p[0] * p[0] for p in pts)
    sxy = sum(p[0] * p[1] for p in pts)
    den = n * sxx - sx * sx
    b = (n * sxy - sx * sy) / den
    a = (sy - b * sx) / n
    out = {"ms_per_mp": b, "fixed_ms": a}
    for fps in (20.0, 10.0):
        mp = (1000.0 / fps - a) / b
        # report as a 16:9 size, which is what the game uses
        w = int((mp * 1e6 * 16 / 9) ** 0.5) // 16 * 16
        out[f"at_{int(fps)}fps"] = {"mp": mp, "size_16_9": f"{w}x{int(w*9/16)}"}
    return out


# ---- main -----------------------------------------------------------------

def main():
    args = sys.argv[1:]
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(SHOT_DIR, exist_ok=True)

    if "--table-only" in args:
        data = json.load(open(JSON_PATH))
        # recompute the derived numbers so a table refresh never needs a boot
        data["prediction"] = prediction(data)
        data["thresholds"] = threshold_fit(data)
        json.dump(data, open(JSON_PATH, "w"), indent=1)
        md = table_md(data)
        open(TABLE_PATH, "w").write(md + "\n")
        log(md)
        return 0

    host_only = "--host-only" in args
    guest_only = "--guest-only" in args
    ensure_pins()
    bench_g = None if host_only else build_bench_guest()
    bench_h = None if guest_only else build_bench_host()
    if not host_only:
        build_libgl()
        build_os()
        stage(bench_g)

    data = {"runs": {}, "meta": {"host_game_fps_720p": HOST_GAME_FPS_720P,
                                 "draws": HOST_GAME_DRAWS,
                                 "date": time.strftime("%Y-%m-%d %H:%M:%S")}}
    # --host-only / --guest-only keep the other side's rows: re-running one side
    # (the cheap one) must not throw away a guest boot's evidence.
    if (host_only or guest_only) and os.path.exists(JSON_PATH):
        try:
            prev = json.load(open(JSON_PATH))
            data["runs"].update(prev.get("runs", {}))
            log("m7: merged previous runs: " + ", ".join(prev.get("runs", {})))
        except Exception as e:
            log(f"m7: note: could not merge {JSON_PATH}: {e}")
    xv = None
    try:
        if not guest_only:
            xv = xvfb_start()
            llvm_env = {"LIBGL_ALWAYS_SOFTWARE": "1",
                        "GALLIUM_DRIVER": "llvmpipe",
                        "LP_NUM_THREADS": "8",
                        "LC_ALL": "C"}
            r = run_host(bench_h, "llvmpipe", llvm_env)
            data["runs"]["host-llvmpipe"] = summarise(r)
            soft_env = {"LD_LIBRARY_PATH": os.path.join(ROOT, "obj", "mesa", "lib"),
                        "LC_ALL": "C"}
            r = run_host(bench_h, "softpipe", soft_env)
            data["runs"]["host-softpipe"] = summarise(r)
        if not host_only:
            r = run_guest()
            if r is None:
                return 1
            data["runs"]["guest"] = summarise(r)
    finally:
        if xv:
            xv.terminate()

    data["prediction"] = prediction(data)
    data["thresholds"] = threshold_fit(data)
    json.dump(data, open(JSON_PATH, "w"), indent=1)
    md = table_md(data)
    open(TABLE_PATH, "w").write(md + "\n")
    log("")
    log(md)
    pred = data.get("prediction")
    if pred:
        log("")
        log(f"calibrated against the real game: {HOST_GAME_FPS_720P} fps at "
            f"1280x720 on llvmpipe → scene fill × {pred['k_fill']:.2f} = "
            f"game fill, plus {pred['draw_us_softpipe']:.0f} µs/draw")
        for k, v in pred["sizes"].items():
            log(f"  predicted menu fps @ {k}: {v['pred_fps']:.2f}")
        if pred.get("llvmpipe_sizes"):
            for k, v in pred["llvmpipe_sizes"].items():
                log(f"  same model on llvmpipe @ {k}: {v['pred_fps']:.1f}")

    g = data["runs"].get("guest")
    if g and not host_only:
        ok = (g["rc"] == 0 and g.get("renderer") == "softpipe"
              and len([r for r in g["sizes"].values() if r["fbo_fps"]]) == 4
              and g.get("check"))
        log("")
        log("m7: PASS — guest softpipe measured at all four sizes" if ok
            else "m7: FAIL — see the rows above")
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
