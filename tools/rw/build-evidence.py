#!/usr/bin/env python3
"""Assemble docs/rimworld-evidence.md from prose + generated tables.

The two long tables (syscall gap, DSO inventory) are produced by
tools/rw/syscall-map.py and tools/rw/dso-inventory.py so the document cannot
drift from the traces it cites.  Run from the repo root after the harness runs:

    python3 tools/rw/build-evidence.py
"""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tools/rw/out")


def run(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout.strip()


def syscall_table():
    rows = run("python3", "tools/rw/syscall-map.py",
               "tools/rw/out/strace-summary.txt", "--top", "60", "--md")
    return rows


def dso_table():
    lines = run("python3", "tools/rw/dso-inventory.py",
                "tools/rw/out/ld-debug-libs.txt").splitlines()
    keep = {"UnityPlayer.so", "libc.so.6", "libm.so.6", "libpthread.so.0",
            "libdl.so.2", "librt.so.1", "libgcc_s.so.1", "ld-linux-x86-64.so.2"}
    out = ["| DSO | loaded via | requester |", "|---|---|---|"]
    for line in lines:
        if not line.startswith("| `"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if cells[1] == "dlopen" or any(f"`{k}`" in cells[0] for k in keep):
            out.append(f"| {cells[0]} | {cells[1]} | {cells[2]} |")
    return "\n".join(out)


def main():
    doc = open(os.path.join(ROOT, "tools/rw/evidence-template.md")).read()
    doc = doc.replace("{{SYSCALL_TABLE}}", syscall_table())
    doc = doc.replace("{{DSO_TABLE}}", dso_table())
    path = os.path.join(ROOT, "docs/rimworld-evidence.md")
    open(path, "w").write(doc)
    print(f"wrote {path} ({len(doc.splitlines())} lines)")


if __name__ == "__main__":
    main()
