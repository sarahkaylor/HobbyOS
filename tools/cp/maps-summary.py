#!/usr/bin/env python3
"""tools/cp/maps-summary.py — /proc/<pid>/maps snapshots of Wine processes ->
the address-space facts P1 (docs/cyberpunk.md AD-3, F1) needs.

For every snapshot (tools/cp/run-wine-host.sh --maps DIR writes them, first
line `# pid N cmdline: ...`):
  * every mapping below 8 GiB (HobbyOS x64 identity-maps 0-8 GiB today,
    src/include/vm.h:11-27);
  * any mapping intersecting a HOLE (default the x64 kernel image window
    [0x70000000, 0x78000000), lane C1-as);
  * the five largest single mappings (reservations show as ---p);
  * the lowest and highest user address.

Usage: maps-summary.py DIR [--hole 0x70000000-0x78000000]
"""
import argparse
import glob
import os
import sys

GiB = 1 << 30


def load(path):
    head, rows = "", []
    for line in open(path, errors="replace"):
        if line.startswith("#"):
            head = line[1:].strip()
            continue
        f = line.split(None, 5)
        if len(f) < 5:
            continue
        s, e = (int(x, 16) for x in f[0].split("-"))
        rows.append((s, e, f[1], f[5].strip() if len(f) > 5 else ""))
    return head, rows


def short(p):
    return os.path.basename(p) if p.startswith("/") else p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--hole", default="0x70000000-0x78000000")
    a = ap.parse_args()
    hs, he = (int(x, 16) for x in a.hole.split("-"))
    files = sorted(glob.glob(os.path.join(a.dir, "*.maps")))
    if not files:
        print(f"maps-summary: no *.maps in {a.dir}", file=sys.stderr)
        return 2
    print(f"# /proc/<pid>/maps summary ({len(files)} processes); hole "
          f"[{hs:#x}, {he:#x})\n")
    any_hole = False
    for f in files:
        head, rows = load(f)
        user = [r for r in rows if r[0] < (1 << 47)]
        print(f"## {head}\n")
        print(f"- lowest user mapping: `{user[0][0]:#x}`; highest user end: "
              f"`{max(r[1] for r in user):#x}`; mappings: {len(user)}")
        hole = [r for r in user if r[0] < he and r[1] > hs]
        any_hole |= bool(hole)
        print(f"- intersecting the hole: {len(hole)}"
              + ("".join(f"\n  - `{s:#x}-{e:#x} {p} {short(n)}`" for s, e, p, n in hole)))
        low = [r for r in user if r[0] < 8 * GiB]
        print(f"- below 8 GiB ({len(low)}):")
        for s, e, p, n in low:
            print(f"  - `{s:#012x}-{e:#012x}` {p} {(e - s) / (1 << 20):9.2f} MiB {short(n)}")
        big = sorted(user, key=lambda r: r[0] - r[1])[:5]
        print("- largest five:")
        for s, e, p, n in big:
            print(f"  - `{s:#x}-{e:#x}` {p} {(e - s) / (1 << 20):.1f} MiB {short(n)}")
        print()
    print(f"**Any process maps inside [{hs:#x}, {he:#x}): {'YES' if any_hole else 'no'}**")
    return 0


if __name__ == "__main__":
    sys.exit(main())
