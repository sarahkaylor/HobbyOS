#!/usr/bin/env python3
"""evidence_pack.py — assemble the S0/S1 acceptance evidence pack (GB-047).

Scans the repo's committed evidence/workstream markers and emits a single
S0/S1 evidence-pack index + an archive manifest ready to place on the shared
store (`/mnt/fast` NAS, or the per-repo `evidence/` bundles).  The pack's job
(gaming.md §M4-acceptance, GB-047 "archive on NAS") is to make the S0/S1
acceptance auditable: native-port evidence, playthrough harnesses/replays, the
perf baseline, and gate history, with real paths and sha256 provenance.

  python3 tools/t5/evidence_pack.py --root <repo> --out S0-S1-EVIDENCE-PACK.md

The scan/aggregate helpers are import-testable (T1).
"""
import argparse, glob, hashlib, json, os, re, time

# evidence markers the pack treats as an S0/S1 artifact
MARKERS = {
    "evidence rig":      "tools/evidence_rig.py",
    "perf cycle":        "tools/perf_game_cycle.py",
    "replays":           "tools/t5/replays/*.jsonl",
    "perf baseline":     "perf-baseline-REPORT.md",
    "perf global":       "perf-global-REPORT.md",
}

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()[:16]


def find_reports(root):
    """Committed *-REPORT.md / EVIDENCE-REPORT.md pointers to the archive."""
    out = []
    for pat in ("*EVIDENCE-REPORT.md", "*-REPORT.md", "S0*", "S1*"):
        for p in glob.glob(os.path.join(root, pat)):
            out.append(p)
    return sorted(set(out))


def find_game_evidence(root):
    """Native-catalog evidence bundles/reports already committed."""
    out = []
    for pat in ("*-EVIDENCE-REPORT.md", "evidence/*/README*"):
        for p in glob.glob(os.path.join(root, pat)):
            out.append(p)
    return sorted(set(out))


def resolve_marker(root, key, pat):
    hits = glob.glob(os.path.join(root, pat))
    return [(os.path.relpath(h, root), sha256(h)) for h in hits]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default="S0-S1-EVIDENCE-PACK.md")
    args = ap.parse_args()
    root = args.root
    os.chdir(root)

    L = []
    L.append("# S0/S1 acceptance evidence pack (GB-047)\n")
    L.append(f"- generated: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")
    L.append("- status: this index + per-artifact hashes; the archive lands on the\n"
             "  shared store (`/mnt/fast` NAS) at acceptance. On swarm-farm-3 no host\n"
             "  currently has `/mnt/fast` mounted, so placement is the documented,\n"
             "  gated sub-step.\n")

    L.append("\n## Native-port / harness evidence (S0)\n")
    L.append("| kind | path | sha16 |\n|---|---|---|\n")
    for name, pat in MARKERS.items():
        for rel, h in resolve_marker(root, name, pat):
            L.append(f"| {name} | `{rel}` | {h} |\n")
    for p in find_game_evidence(root):
        rel = os.path.relpath(p, root)
        L.append(f"| game evidence | `{rel}` | {sha256(p)} |\n")

    L.append("\n## Committed reports (S0/S1 gate history)\n")
    for p in find_reports(root):
        rel = os.path.relpath(p, root)
        L.append(f"- `{rel}` (sha16 {sha256(p)})\n")

    # archive manifest (json) for the NAS placement step
    manifest = {"generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "artifacts": {}}
    for name, pat in MARKERS.items():
        for rel, h in resolve_marker(root, name, pat):
            manifest["artifacts"][rel] = {"kind": name, "sha16": h}
    for p in find_game_evidence(root) + find_reports(root):
        rel = os.path.relpath(p, root)
        if rel not in manifest["artifacts"]:
            manifest["artifacts"][rel] = {"kind": "report", "sha16": sha256(p)}
    with open(os.path.join(root, "S0-S1-EVIDENCE-MANIFEST.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    with open(os.path.join(root, args.out), "w") as f:
        f.write("".join(L))
    print("".join(L))
    print(f"[pack] wrote {args.out} + S0-S1-EVIDENCE-MANIFEST.json "
          f"({len(manifest['artifacts'])} artifacts)")

if __name__ == "__main__":
    main()
