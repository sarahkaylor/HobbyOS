#!/usr/bin/env python3
"""probe_editor.py — GB-088-V clause 4: the editor control.

Same measurement as the NES clause, on a different app: F1 -> Apps -> NANO,
type `abc123`, and count the pixels that change in the editor's text area.
Prints [probe] lines so the verdict can quote them.

Usage: ARCH=intel HOBBYOS_ROOT=<tree> python3 probe_editor.py
"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.environ.get("HOBBYOS_ROOT") or os.getcwd()
os.chdir(ROOT)
ARCH = os.environ.get("ARCH", "intel")
W, H = 1920, 1080
RUN = os.path.join(os.environ.get("OUTDIR", "/tmp"), "gb088_editor")
os.makedirs(RUN, exist_ok=True)
SER = os.path.join(RUN, "serial.log")
QMP = os.path.join(RUN, "qmp")


class Q:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX); s.s.settimeout(15)
        for _ in range(200):
            try:
                s.s.connect(p); break
            except Exception:
                time.sleep(0.5)
        s.s.recv(65536); s.c("qmp_capabilities")

    def c(s, ex, args=None):
        m = {"execute": ex}
        if args:
            m["arguments"] = args
        s.s.sendall(json.dumps(m).encode() + b"\n")
        buf = b""
        while True:
            buf += s.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    r = json.loads(part)
                except Exception:
                    continue
                if "return" in r or "error" in r:
                    return r

    def key(s, k, pause=0.15):
        s.c("send-key", {"keys": [{"type": "qcode", "data": k}]})
        time.sleep(pause)

    def shot(s, name):
        p = os.path.join(RUN, name)
        s.c("screendump", {"filename": p})
        time.sleep(0.4)
        return p


def parse_ppm(path):
    raw = open(path, "rb").read(); vals, i = [], 0
    while len(vals) < 4:
        while raw[i:i + 1].isspace():
            i += 1
        if raw[i:i + 1] == b"#":
            while raw[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not raw[j:j + 1].isspace():
            j += 1
        vals.append(raw[i:j]); i = j
    return raw, i + 1, int(vals[1]), int(vals[2])


def diff_full(pa, pb):
    ra, oa, w, h = parse_ppm(pa); rb, ob, wb, hb = parse_ppm(pb)
    row = w * 3
    n = 0
    x0 = y0 = 10 ** 9
    x1 = y1 = -1
    for y in range(h):
        a = ra[oa + y * row: oa + (y + 1) * row]
        b = rb[ob + y * row: ob + (y + 1) * row]
        if a == b:
            continue
        for x in range(0, row, 3):
            if a[x:x + 3] != b[x:x + 3]:
                px = x // 3
                n += 1
                x0 = min(x0, px); x1 = max(x1, px)
                y0 = min(y0, y); y1 = max(y1, y)
    return n, (x0, y0, x1, y1)


subprocess.run(["pkill", "-9", "-f", "qemu-system[-]" + "x86_64" if ARCH == "intel" else "qemu-system[-]aarch64"],
               stderr=subprocess.DEVNULL)
for f in (SER, QMP):
    if os.path.exists(f):
        os.remove(f)
subprocess.Popen(["stdbuf", "-oL", "make", "run", "DESKTOP=hobby", f"ARCH={ARCH}",
                  f"QEMU_ARGS=-display none -qmp unix:{QMP},server,nowait"],
                 stdout=open(SER, "w"), stderr=subprocess.STDOUT)
q = Q(QMP)
t0 = time.time()
while time.time() - t0 < 180:
    if "Desktop starting" in open(SER, errors="ignore").read():
        break
    time.sleep(0.5)
time.sleep(3)

txt = open(SER, errors="ignore").read()
entries = dict((int(a), b) for a, b in re.findall(
    r"\[MENU\]\s*(?:\s*\[\s*\d+\]\s*)*(\d+)(?:\s*[\[\]A-Z0-9 ]*\s*)*=(\s*[A-Za-z0-9_.]+)", txt))
name_idx = {n.strip(): i for i, n in entries.items()}
idx = name_idx.get("NANO.BIN")
print(f"[probe] NANO.BIN menu idx={idx}", flush=True)
if idx is None:
    print("[probe] FATAL: NANO.BIN absent from the menu dump", flush=True)
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(2)

q.key("f1", 0.4); time.sleep(0.4)
q.key("home", 0.3)
for _ in range(idx):
    q.key("down", 0.06)
q.key("ret", 0.3)
time.sleep(3.0)

before = q.shot("ed_before.ppm")
for k in ("a", "b", "c", "1", "2", "3"):
    q.key(k, 0.12)
time.sleep(1.5)
after = q.shot("ed_after.ppm")

n, bbox = diff_full(before, after)
print(f"[probe] before={before}", flush=True)
print(f"[probe] after={after}", flush=True)
print(f"[probe] changed_px={n} bbox={bbox}", flush=True)
print(f"[probe] serial_has_Modified={'Modified' in open(SER, errors='ignore').read()}", flush=True)
subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
print("[probe] done", flush=True)