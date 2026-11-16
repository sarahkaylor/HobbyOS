#!/usr/bin/env python3
"""tools/cp/p5/boot_wine_p5.py -- boot + grade half of tools/cp/p5/run-wine-p5.sh
(lane C5-vk2, docs/cp-vk.md §8).  Not run on its own: the bash wrapper builds,
owns the /etc/exports edit (exports lock OUTSIDE the QEMU lock) and stages the
disk, then calls this script, which takes one QEMU slot (tools/cp/qemu-slot.sh semantics) for the boot only.

While the guest's WINEP5.BIN runs the probes (XSTART), it screendumps each
probe twice, on its "t=20s" and "t=50s" progress rows, and grades:

  per probe (vkcube: "[VK]", d3d11: "[D3D11]", d3d12: "[D3D12]"):
    R1 "<P> RESULT frames=N wall=W" with W >= 60000 ms, N > 0; "<P> done rc=0";
       "[P5] end <tag> status=0"
    R2 the two screendumps show the probe's frame (>= 20000 clear-colour
       pixels, >= 2000 saturated pixels) and differ inside it (it moved)
    R3 the module rows prove the path: vkcube -> vulkan-1.dll; d3d11 ->
       d3d11.dll + dxgi.dll from Z:\\CP\\P5\\D3D11 (DXVK); d3d12 -> d3d12.dll +
       d3d12core.dll from Z:\\CP\\P5\\D3D12 (vkd3d-proton); D3D adapters name
       llvmpipe
  global: no fault line (page fault / memory fault / FATAL).

Usage: boot_wine_p5.py --out DIR --disk IMG [--probes vkcube,d3d11,d3d12]
                       [--smp 8] [--mem 12288] [--cpu host] [--timeout 7200]
"""
import argparse
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_icewm_test import Qmp  # noqa: E402
from p5lib import acquire_slot, census, guests_running, qemu_cmd, read, release_slot  # noqa: E402

PFX = {"vkcube": "[VK]", "d3d11": "[D3D11]", "d3d12": "[D3D12]"}
MODS = {
    "vkcube": [],  # vkcommon.h prints "[VK] loader vulkan-1.dll (<path>) loaded" instead
    "d3d11": [("d3d11.dll", "\\cp\\p5\\d3d11\\"), ("dxgi.dll", "\\cp\\p5\\d3d11\\")],
    "d3d12": [("d3d12.dll", "\\cp\\p5\\d3d12\\"), ("d3d12core.dll", "\\cp\\p5\\d3d12\\"),
              ("dxgi.dll", "\\cp\\p5\\d3d12\\")],
}


def log(msg):
    print(f"[P5W] {msg}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--probes", default="vkcube,d3d11,d3d12")
    ap.add_argument("--smp", type=int, default=8)
    ap.add_argument("--mem", type=int, default=12288)
    ap.add_argument("--cpu", default="host")
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--grade-only", action="store_true",
                    help="no boot: regrade OUT/serial.log and OUT/<probe>-t{20,50}s.ppm")
    args = ap.parse_args()
    probes = [p for p in args.probes.split(",") if p]
    if args.grade_only:
        serial = os.path.join(args.out, "serial.log")
        shots = {p: [s for s in (os.path.join(args.out, f"{p}-t{t}s.ppm") for t in (20, 50))
                     if os.path.exists(s)] for p in probes}
        return grade(args, probes, serial, shots)

    cmd, qmp_path, serial = qemu_cmd(args.out, args.disk, args)
    if os.path.exists(serial):
        os.remove(serial)
    shots = {p: [] for p in probes}
    log("waiting for a QEMU slot (tools/cp/qemu-slot.sh semantics)")
    lockf, slot = acquire_slot()
    log(f"slot {slot} acquired {time.strftime('%H:%M:%S')}; boot: -smp {args.smp} -m {args.mem}M -cpu {args.cpu} (KVM)")
    t0 = time.time()
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    log(f"qemu pid {proc.pid}")
    try:
        qmp = Qmp(qmp_path, deadline_s=40)
        pending = [(p, t) for p in probes for t in ("t=20s", "t=50s")]
        last_report = 0
        while time.time() - t0 < args.timeout and proc.poll() is None:
            ser = read(serial)
            for (p, t) in list(pending):
                begin = ser.rfind(f"[P5] begin {p} ")
                if begin >= 0 and f"{PFX[p]} {t} " in ser[begin:]:
                    path = os.path.join(args.out, f"{p}-{t.replace('=', '')}.ppm")
                    if qmp.shot(path):
                        shots[p].append(path)
                        log(f"screendump {os.path.basename(path)} at +{time.time() - t0:.0f} s; "
                            f"QEMU guests on the host now (incl. this one): {guests_running()}")
                    pending.remove((p, t))
            if "[P5] done" in ser or "System halt" in ser:
                break
            if time.time() - last_report > 300:
                tail = [ln for ln in ser.splitlines() if ln.startswith("[P5]")][-1:] or ["(no [P5] row yet)"]
                log(f"+{time.time() - t0:.0f} s: {tail[0]}")
                last_report = time.time()
            time.sleep(1)
        time.sleep(2)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        release_slot(lockf)
        log(f"guests on the host at the end of the run: {guests_running()} (incl. none of ours)")
        log(f"boot finished after {time.time() - t0:.0f} s; lock released")

    return grade(args, probes, serial, shots)


def grade(args, probes, serial, shots):
    ser = read(serial)
    lines = ser.splitlines()
    fails = []
    log("--- [P5] rows and probe rows (serial) ---")
    for ln in lines:
        if ln.startswith("[P5]") or any(ln.startswith(PFX[p]) for p in probes):
            print(ln, flush=True)
    for p in probes:
        b = ser.rfind(f"[P5] begin {p} ")
        e = ser.find(f"[P5] end {p} ", b) if b >= 0 else -1
        span = ser[b:e + 200] if b >= 0 else ""
        rows = [ln for ln in span.splitlines() if ln.startswith(PFX[p])]
        res = [r for r in rows if r.startswith(f"{PFX[p]} RESULT ")]
        ok = True
        if not res:
            fails.append(f"{p}: no RESULT row"); ok = False
        else:
            kv = dict(t.split("=", 1) for t in res[-1].split() if t.count("=") == 1)
            if float(kv.get("wall", "0")) < 60000 or int(kv.get("frames", "0")) <= 0:
                fails.append(f"{p}: {res[-1]} (< 60 s)"); ok = False
        if f"{PFX[p]} done rc=0" not in rows:
            fails.append(f"{p}: no '{PFX[p]} done rc=0'"); ok = False
        endrow = [ln for ln in span.splitlines() if ln.startswith(f"[P5] end {p} ")]
        if not endrow or " status=0 " not in endrow[-1] + " ":
            fails.append(f"{p}: launcher end row {endrow[-1] if endrow else '(none)'}"); ok = False
        for dll, where in MODS[p]:
            mrow = [r for r in rows if f" module {dll} " in r.replace("  ", " ") or r.split()[2:3] == [dll]]
            if not mrow:
                fails.append(f"{p}: no module row for {dll}"); ok = False
            elif where and where not in mrow[-1].lower():
                fails.append(f"{p}: {dll} not from {where}: {mrow[-1]}"); ok = False
        if p == "vkcube" and not any(r.startswith("[VK] loader vulkan-1.dll") for r in rows):
            fails.append("vkcube: no '[VK] loader vulkan-1.dll' row"); ok = False
        if p == "vkcube" and not any("llvmpipe" in r for r in rows if r.startswith("[VK] device: ")):
            fails.append("vkcube: device is not llvmpipe"); ok = False
        if p in ("d3d11", "d3d12"):
            ad = [r for r in rows if " adapter: " in r]
            if not ad or "llvmpipe" not in ad[-1]:
                fails.append(f"{p}: adapter row {ad[-1] if ad else '(none)'}"); ok = False
        cs = []
        for path in shots[p]:
            c = census(path)
            cs.append(c)
            log(f"{p} {os.path.basename(path)}: clear={c['clear']} saturated={c['sat']} bbox={c['bbox']} hash={c['hash']}")
            subprocess.run([sys.executable, os.path.join(REPO, "skills/hobbyos-screenshot/scripts/ppm2png.py"),
                            path, path[:-4] + ".png"], capture_output=True)
            if c["clear"] < 20000 or c["sat"] < 2000:
                fails.append(f"{p}: {os.path.basename(path)} not recognisable"); ok = False
        if len(cs) < 2:
            fails.append(f"{p}: {len(cs)} screendump(s)"); ok = False
        elif cs[0]["hash"] == cs[1]["hash"]:
            fails.append(f"{p}: screendumps identical (not moving)"); ok = False
        log(f"{p}: {'PASS' if ok else 'FAIL'}")
    # NFS evidence rule (docs/cp-vk.md 8.5), for the hard-mount kernel
    # (cyberpunk 8b9fc2c9+): every unanswered attempt prints "rpc: no reply"
    # and is retried ("[NFSHARD] retry ... waited_ms=N"), so no-reply rows alone
    # lose nothing.  VOID on "[NFSHARD] GIVE UP", a failed lookup
    # ("[NFLK] retry ... rc=-1"), or no-reply rows with no hard-mount rows at
    # all (an old kernel that may have zero-filled the read).
    import re
    noreply = [ln for ln in lines if "rpc: no reply from server" in ln]
    hard = [ln for ln in lines if "[NFSHARD] retry" in ln]
    waited = [int(m.group(1)) for ln in hard for m in [re.search(r"waited_ms=(\d+)", ln)] if m]
    giveup = [ln for ln in lines if "[NFSHARD] GIVE UP" in ln]
    nflk = [ln for ln in lines if "[NFLK] retry" in ln and "rc=-1" in ln]
    nfs_bad = giveup + nflk + (noreply if noreply and not hard else [])
    log(f"NFS: no-reply rows={len(noreply)} [NFSHARD] retry rows={len(hard)} "
        f"longest waited_ms={max(waited) if waited else 0} GIVE UP={len(giveup)} NFLK rc=-1={len(nflk)}")
    if nfs_bad:
        log(f"VOID: {len(nfs_bad)} NFS failure row(s), first: {nfs_bad[0][:160]}")
    bad = [ln for ln in lines if any(k in ln for k in ("memory fault", "page fault", "FATAL"))]
    if bad:
        fails.append(f"{len(bad)} fault line(s): {bad[0][:160]}")
    for f in fails:
        log("FAIL: " + f)
    if nfs_bad:
        log("VERDICT: VOID (NFS failure rows; not evidence either way)")
        return 3
    if not fails:
        log("PASS: " + ", ".join(probes) + " each ran >= 60 s under Wine with two distinct, recognisable screendumps")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
