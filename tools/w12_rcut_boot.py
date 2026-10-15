#!/usr/bin/env python3
"""w12_rcut_boot.py — w12-rcut: boot the MODE=selfhost disk, run the FINAL
T4.3 acceptance loop (S0 -> S3 + incremental demo) in ONE boot, and record
per-phase host wall times + the raw serial.

Derived from w11_bench_boot.py: tails the serial file, assigns monotonic
host timestamps, maps [SELFHOST] phase=<name>-start/-done markers to
per-stage wall times, and writes a timestamped capture + phases JSON.

Before boot it stages the w12 lane drive artifacts over the image (with
w5_flat_boot.py-compatible --script-copy/--restore-script semantics):
  ::/SRC/SELFHOST.RUN  <- SELFHOST-RUN-w12-full.txt
  ::/SRC/SELFJ8.SH     <- SELFJ8.SH   (make -j8 wrapper)
  ::/SRC/SELFINC.SH    <- SELFINC.SH  (incremental demo)
  ::/SRC/T25.RUN       <- T25-RUN.txt (in-OS run of the built HELLO.BIN)

Exit 0 = ACCEPTANCE-COMPLETE seen; 1 = FAILED-AT/abort marker;
2 = timeout; 3 = harness error.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import run_selfhost_build_test as H  # noqa: E402
LANE = os.path.join(ROOT, "docs", "selfhost", "lanes")

PHASE_RE = re.compile(r"\[SELFHOST\] phase=([A-Za-z0-9_-]+)-(start|done|fail)")


def mcopy(src_abs, dst, image):
    r = subprocess.run(["mcopy", "-o", "-i", image, src_abs, dst],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("mcopy %s: %s" % (dst, r.stderr.strip()))
    print("[w12boot] staged %s -> %s" % (os.path.basename(src_abs), dst))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--phases", required=True)
    ap.add_argument("--timeout", type=int, default=18000)
    ap.add_argument("--expect", default="ACCEPTANCE-COMPLETE")
    ap.add_argument("--fail-on", default="[SELFHOST] FAILED-AT")
    ap.add_argument("--no-stage", action="store_true",
                    help="skip mcopy'ing the w12 lane drive artifacts")
    args = ap.parse_args()

    img = os.path.abspath(args.image)
    if not os.path.exists(img):
        raise SystemExit("image not found: %s" % img)

    if not args.no_stage:
        mcopy(os.path.join(LANE, "SELFHOST-RUN-w12-full.txt"), "::/SRC/SELFHOST.RUN", img)
        mcopy(os.path.join(LANE, "SELFJ8.SH"), "::/SRC/SELFJ8.SH", img)
        mcopy(os.path.join(LANE, "SELFINC.SH"), "::/SRC/SELFINC.SH", img)
        mcopy(os.path.join(LANE, "T25-RUN.txt"), "::/SRC/T25.RUN", img)

    t0 = time.monotonic()
    q = H.boot(img, args.serial, qmp=None)
    out = []
    seen = {}
    last = 0
    outcome = 2
    try:
        if H.wait_for(args.serial, H.BOOT_MARKER, min(120, args.timeout)) is None:
            print("[w12boot] boot marker NOT seen")
            return 3
        print("[w12boot] booted; loop running (poll every ~5 min)")
        deadline = time.monotonic() + args.timeout
        last_poll = time.monotonic()
        while time.monotonic() < deadline:
            with open(args.serial, "rb") as f:
                f.seek(last)
                chunk = f.read().decode("utf-8", "replace")
                last = f.tell()
            if chunk:
                ts = time.monotonic() - t0
                out.append((ts, chunk.replace("\r", "")))
                for m in PHASE_RE.finditer(chunk):
                    name, kind = m.group(1), m.group(2)
                    if kind == "start":
                        seen.setdefault(name, {})["start_s"] = ts
                    else:
                        seen.setdefault(name, {})["done_s"] = ts
                        seen[name]["done"] = (kind == "done")
                if args.fail_on in chunk:
                    outcome = 1
                    break
                if args.expect in chunk:
                    outcome = 0
                    break
            # bounded progress poll (every 300 s print live stage status)
            if time.monotonic() - last_poll > 300:
                last_poll = time.monotonic()
                acts = []
                for name, d in seen.items():
                    if d.get("done_s") is None:
                        acts.append("%s@%.0fs" % (name, d.get("start_s", 0)))
                print("[w12boot] t=%.0fs live: %s" % (time.monotonic() - t0, ", ".join(acts) or "none"))
            time.sleep(1)
        print("[w12boot] outcome=%d (0=pass,1=fail,2=timeout) wall=%.1fs"
              % (outcome, time.monotonic() - t0))
        phases = {}
        for name, d in seen.items():
            phases[name] = {
                "start_s": d.get("start_s"),
                "done_s": d.get("done_s"),
                "elapsed_s": (d.get("done_s") - d.get("start_s"))
                             if d.get("done_s") is not None and d.get("start_s") is not None else None,
                "done": d.get("done", False),
            }
        for name, d in sorted(phases.items()):
            print("  phase %-10s start=%6.2f done=%6.2f elapsed=%6.2f done=%s"
                  % (name, d["start_s"] or -1, d["done_s"] or -1,
                     d["elapsed_s"] or -1, d["done"]))
        with open(args.phases, "w") as f:
            json.dump({"outcome": outcome, "wall_s": time.monotonic() - t0,
                       "expect": args.expect, "phases": phases}, f, indent=1)
        cap = args.serial + ".ts"
        with open(cap, "w") as f:
            for ts, chunk in out:
                f.write("%.3f %s\n" % (ts, chunk))
        print("[w12boot] timestamped capture: %s" % cap)
        return outcome
    finally:
        H.stop_qemu(q)


if __name__ == "__main__":
    sys.exit(main())
