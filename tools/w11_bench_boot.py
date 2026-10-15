#!/usr/bin/env python3
"""w11_bench_boot.py — w11-makej: boot the MODE=selfhost disk, drive the
benchmark RUN script, and record per-marker host wall times.

The serial file gets no timestamps of its own, so this runner tails it and
assigns a monotonic host timestamp to every chunk as it appears.  Phase
markers ([SELFHOST] phase=<name>-start/-done) then yield per-stage wall
times, and the in-band BM-* lines carry the correctness evidence.

Exit 0 = ACCEPTANCE-COMPLETE seen; 1 = FAILED-AT/abort marker; 2 = timeout;
3 = harness error.  Writes:
  --serial <path>            timestamped capture: "<t.secs> <text>"
  --phases <path>            JSON: {"phases": {name: {"start_s","done_s",
                            "elapsed_s","done"}}}
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

PHASE_RE = re.compile(r"\[SELFHOST\] phase=([A-Za-z0-9_-]+)-(start|done|fail)")
BM_RE = re.compile(r"^BM-([A-Za-z0-9_-]+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--serial", default="./serial-w11.log")
    ap.add_argument("--phases", default="./w11-phases.json")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--expect", default="ACCEPTANCE-COMPLETE")
    ap.add_argument("--fail-on", default="[SELFHOST] FAILED-AT")
    ap.add_argument("--script-copy", default=None,
                    help="mcopy this file over ::/SRC/SELFHOST.RUN before boot")
    ap.add_argument("--restore-script", default=None,
                    help="mcopy this file back to ::/SRC/SELFHOST.RUN after boot")
    args = ap.parse_args()

    def mcopy(src, dst):
        r = subprocess.run(["mcopy", "-o", "-i", args.image, src, dst],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("mcopy %s: %s" % (dst, r.stderr.strip()))

    img = os.path.abspath(args.image)
    if not os.path.exists(img):
        raise SystemExit("image not found: %s" % img)
    if args.script_copy:
        mcopy(args.script_copy, "::/SRC/SELFHOST.RUN")
        print("[w11boot] staged RUN -> ::/SRC/SELFHOST.RUN")

    t0 = time.monotonic()
    q = H.boot(img, args.serial, qmp=None)
    out = []
    seen = {}     # phase -> dict(start_s, done_s)
    last = 0
    outcome = 2
    try:
        if H.wait_for(args.serial, H.BOOT_MARKER, min(120, args.timeout)) is None:
            print("[w11boot] boot marker NOT seen")
            return 3
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            with open(args.serial, "rb") as f:
                f.seek(last)
                chunk = f.read().decode("utf-8", "replace")
                last = f.tell()
            if chunk:
                ts = time.monotonic() - t0
                out.append((ts, chunk.replace("\r", "")))
                text = chunk
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
            else:
                time.sleep(0.2)
        print("[w11boot] outcome=%d (0=pass,1=fail,2=timeout) wall=%.1fs"
              % (outcome, time.monotonic() - t0))
        # per-phase elapsed
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
                       "phases": phases}, f, indent=1)
        cap = args.serial + ".ts"
        with open(cap, "w") as f:
            for ts, chunk in out:
                f.write("%.3f %s\n" % (ts, chunk))
        print("[w11boot] timestamped capture: %s" % cap)
        return outcome
    finally:
        H.stop_qemu(q)
        if args.restore_script:
            mcopy(args.restore_script, "::/SRC/SELFHOST.RUN")
            print("[w11boot] restored RUN")


if __name__ == "__main__":
    sys.exit(main())
