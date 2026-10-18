#!/usr/bin/env python3
"""boot_time.py — measure HobbyOS boot wall-clock time (GB-045 perf pass).

Times from QEMU process start until the desktop start-menu ([MENU]) dump,
reusing the GB-040 evidence_rig boot plumbing.  Prints seconds-since-epoch /
elapsed and appends to a run log.  Baseline numbers feed the GB-045 report.
"""
import argparse, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--arch", default="arm", choices=["arm", "intel"])
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default="evidence/perf/boot_times.jsonl")
    args = ap.parse_args()
    os.chdir(args.root)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    serial = os.path.join(os.path.dirname(args.out) or ".", "boot_serial.log")
    qmp = "/tmp/boottime-qmp"
    qemu_make = ["make", "run", f"ARCH={args.arch}",
                 f"QEMU_ARGS=-display none -qmp unix:{qmp},server,nowait"]
    from evidence_rig import boot
    t0 = time.time()
    q = boot(qemu_make, serial, qmp, args.arch)   # blocks until desktop+[MENU]
    elapsed = round(time.time() - t0, 2)
    rec = {"arch": args.arch, "boot_s": elapsed,
           "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    with open(args.out, "a") as f:
        f.write(json.dumps(rec) + "\n")
    print(f"[boottime] {args.arch} boot to desktop+menu: {elapsed}s")
    print(f"[boottime] appended -> {args.out}")
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(0)

if __name__ == "__main__":
    main()
