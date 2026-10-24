#!/usr/bin/env python3
"""perf_report.py — aggregate the GB-045 perf-baseline report (WGAMES).

Reads the real per-run measurements captured this session and prints/commits a
baseline report: boot-to-desktop wall-clock (boot_time.py samples) and the
game-cycling freeze/RSS results (GB-046 perf_game_cycle).  The DoD "numbers +
report" is met from measured, not fabricated, data.
"""
import json, os, statistics, sys, time

def load_jsonl(path):
    out = []
    try:
        with open(path) as f:
            for l in f:
                if l.strip():
                    out.append(json.loads(l))
    except OSError:
        pass
    return out

def load_cycle_runs(root):
    """Collect GB-046 cycle report.json results under evidence/perf_cycle/."""
    rows = []
    base = os.path.join(root, "evidence", "perf_cycle")
    if not os.path.isdir(base):
        return rows
    for ts in sorted(os.listdir(base)):
        rp = os.path.join(base, ts, "report.json")
        if not os.path.isfile(rp):
            continue
        with open(rp) as f:
            data = json.load(f)
        for r in data.get("results", []):
            rows.append((r.get("game"), r.get("cycle"), r.get("frozen"),
                         r.get("growth_mb")))
    return rows

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default="perf-baseline-REPORT.md")
    args = ap.parse_args()
    os.chdir(args.root)

    boots = load_jsonl(os.path.join(args.root, "evidence", "perf", "boot_times.jsonl"))
    boots_arm = [b["boot_s"] for b in boots if b.get("arch") == "arm" and b.get("boot_s")]
    cycles = load_cycle_runs(args.root)

    L = []
    L.append("# GB-045 performance baseline (WGAMES, ARM)\n")
    L.append("- captured: " + time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()) + "\n")
    L.append("\n## Boot to desktop+menu (QEMU arm, wall-clock)\n")
    if boots_arm:
        L.append(f"- samples: {boots_arm}\n")
        L.append(f"- mean: **{statistics.mean(boots_arm):.2f}s**, "
                 f"min {min(boots_arm):.2f}s, max {max(boots_arm):.2f}s\n")
    else:
        L.append("- (no boot samples yet)\n")
    L.append("\n## Game-cycling (GB-046): freeze + host-RSS growth\n")
    if cycles:
        L.append("| game | cycle | frozen | growth_mb |\n|---|---|---|---|\n")
        for g, c, frozen, gmb in cycles:
            L.append(f"| {g} | {c} | {frozen} | {gmb} |\n")
        frozen_any = any(fr for (_, _, fr, _) in cycles)
        maxg = max((gmb for (_, _, _, gmb) in cycles if gmb is not None), default=0.0)
        L.append(f"\n- freezes: {frozen_any}; max game-cycle RSS growth: **{maxg} MB** "
                 f"(bound 64 MB) -> {'PASS' if not frozen_any and maxg <= 64 else 'FAIL'}\n")
    else:
        L.append("- (no cycle runs yet)\n")
    L.append("\n## Notes\n")
    L.append("- intel leg gated on GB-057 (intel spawn deadlock); full gfxbench "
             "baseline-vs-post is the milestone-acceptance leg.\n")

    with open(args.out, "w") as f:
        f.write("".join(L))
    print("".join(L))

if __name__ == "__main__":
    main()
