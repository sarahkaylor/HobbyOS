#!/usr/bin/env python3
"""GB-059-V liveness probe.

Boots QEMU exactly as tools/evidence_rig.py does (same `make run` line, same
QMP client, same by-name slot resolution and same menu-navigation code — the
rig is imported, not re-implemented) and then holds the launched program open
long enough to take THREE screendumps a few seconds apart.

Purpose: clause 1 of GB-059-V says a launched title must produce a *live*
frame.  One screendump cannot distinguish "rendering" from "a stale frame left
on the framebuffer", so this measures frame-to-frame change over time:
  * a live/animated program  -> digests differ across the three samples
  * a static program (control) -> digests identical across the three samples
The control is what makes the liveness claim falsifiable.

usage: probe_liveness.py --bin XNIBBLER.BIN --arch arm --label xnibbler
"""
import argparse, hashlib, importlib.util, json, os, subprocess, sys, time
from datetime import datetime, timezone

ROOT = os.getcwd()
spec = importlib.util.spec_from_file_location("rig", os.path.join(ROOT, "tools", "evidence_rig.py"))
rig = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rig)


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def colors(path):
    data = open(path, "rb").read()
    px = data.split(b"\n", 3)[3]
    return len(set(px[i:i + 3] for i in range(0, len(px), 3)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--arch", default="arm")
    ap.add_argument("--label", default=None)
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--gap", type=float, default=3.0)
    ap.add_argument("--boot-timeout", type=int, default=180)
    ap.add_argument("--no-launch", action="store_true",
                    help="control run: sample the bare desktop three times; digests must be "
                         "IDENTICAL, which proves the liveness measure can distinguish a static "
                         "screen from an animating one")
    ap.add_argument("--keys", default="", help="comma-separated qcodes to send after launch, e.g. ret,left")
    a = ap.parse_args()

    label = a.label or a.bin.split(".")[0].lower()
    out = os.path.join("evidence", "gb-059-v", f"liveness-{label}")
    os.makedirs(out, exist_ok=True)
    serial = os.path.join(out, "serial.log")
    qmp = f"/tmp/gb059v-qmp-{label}"

    cmd = ["make", "run", f"ARCH={a.arch}",
           f"QEMU_ARGS=-display none -qmp unix:{qmp},server,nowait"]
    print(f"[probe] boot {a.arch}: {' '.join(cmd)}")
    q = rig.boot(cmd, serial, qmp, a.arch, timeout_s=a.boot_timeout)

    slot, warn = rig.resolve_launch_slot(serial, a.bin)
    print(f"[probe] resolve_launch_slot({a.bin}) -> {slot} warn={warn!r}")
    if slot is None:
        print(f"[probe] FATAL: {a.bin} not in [MENU] dump")
        subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
        sys.exit(3)

    if not a.no_launch:
        rig.launch_from_menu(q, slot)
        for code in [c.strip() for c in a.keys.split(",") if c.strip()]:
            q.key(code, 0.25)
            print(f"[probe] key {code}")
    samples = []
    for i in range(a.samples):
        time.sleep(a.gap if i else 1.5)
        p = os.path.join(out, f"frame{i}.ppm")
        q.screendump(p)
        s = sha(p)
        samples.append({"i": i, "at_s": round((i) * a.gap + (1.5 if i == 0 else 0), 1),
                        "sha256": s, "distinct_colors": colors(p),
                        "bytes": os.path.getsize(p)})
        print(f"[probe] frame{i}: sha256={s[:16]} colors={samples[-1]['distinct_colors']}")

    distinct = len({s["sha256"] for s in samples})
    txt = open(serial, errors="ignore").read()
    launched = f"[LAUNCH] {a.bin}" in txt
    arrived = f"[ARRIVED] {a.bin}" in txt
    if a.no_launch:
        # Control: the screen must be static, so the measure must report ONE digest.
        ok = distinct == 1
        print(f"[probe] CONTROL (no launch) bin={a.bin} distinct_frame_digests={distinct}/{a.samples} "
              f"-> {'STATIC as expected (measure can distinguish animating screens)' if ok else 'UNEXPECTED: screen changed without a launch'}")
    else:
        ok = distinct > 1 and launched and arrived
        print(f"[probe] RESULT bin={a.bin} slot={slot} distinct_frame_digests={distinct}/{a.samples} "
              f"launch_line={launched} arrived_line={arrived}")

    json.dump({"bin": a.bin, "arch": a.arch, "slot": slot, "samples": samples,
               "distinct_digests": distinct, "n_samples": a.samples,
               "launch_line": launched, "arrived_line": arrived,
               "captured_at": datetime.now(timezone.utc).isoformat(),
               "commit": subprocess.run(["git", "rev-parse", "HEAD"],
                                        capture_output=True, text=True).stdout.strip()},
              open(os.path.join(out, "liveness.json"), "w"), indent=2)
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    print(f"[probe] evidence: {out}")
    sys.exit(0 if ok else 4)


if __name__ == "__main__":
    main()