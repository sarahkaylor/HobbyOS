#!/usr/bin/env python3
"""Cross-check the fetched Skia tree against the WebKit side (read-only).

Verifies the provenance claim behind the pin: the tree fetched from
skia.googlesource.com @588b550a must be byte-identical, file-for-file, to
(a) the WebKit 2.54.0 fork clone's vendored copy (modulo the 3 WebKit-authored
files + 9 files dropped by WebKit's ignore rules) and (b) the Skia subset of
the WPE 2.54.0 release tarball (a strict prune of it, plus the 2 build files).

Usage:
  compare-with-webkit.py [--upstream DIR] [--clone DIR] [--tarball DIR]
Compares the FETCHED+OVERLAID tree (fetch.sh output: upstream @588b550a plus
the 3 WebKit-authored files) against:
  --clone     a WebKit fork clone @2.54.0   (~/webkit-hobbyos/Source/ThirdParty/skia)
  --tarball   the WPE 2.54.0 release tarball's Skia subset (extraction; searched
              in this worktree, then in ~/Documents/GitHub/HobbyOS)
Expected shapes: vs clone -- 0 content diffs, 0 clone-only files, ~9
upstream-only files (WebKit's ignore rules dropped them).  vs tarball -- 0
content diffs over the common subset; the tarball side is a strict prune.
Trees that do not exist are skipped with a note.
"""
import argparse
import hashlib
import os
import sys

WEBKIT_FILES = {"CMakeLists.txt", "WebKitSkiaConfig.h", "README.WebKit"}


def walk(root):
    rels = set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for f in filenames:
            rels.add(os.path.relpath(os.path.join(dirpath, f), root))
    return rels


def sha(path):
    if os.path.islink(path):
        return "LINK:" + os.readlink(path)
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def compare(up_dir, other_dir, other_name, expect_only_other, expect_only_up):
    up, other = walk(up_dir), walk(other_dir)
    common = up & other
    diffs = []
    for rel in sorted(common):
        pa, pb = os.path.join(up_dir, rel), os.path.join(other_dir, rel)
        try:
            if sha(pa) != sha(pb):
                diffs.append(rel)
        except FileNotFoundError as e:  # dangling symlink side
            diffs.append(f"{rel} (unreadable: {e.filename})")
    only_other = sorted(other - up)
    only_up = sorted(up - other)
    print(f"\n== upstream vs {other_name} ==")
    print(f"   common={len(common)} content-diffs={len(diffs)}")
    for d in diffs[:40]:
        print(f"   DIFF: {d}")
    print(f"   only in {other_name}: {len(only_other)}")
    for f in only_other[:20]:
        tag = " (expected: WebKit-authored)" if f in WEBKIT_FILES else ""
        print(f"     {f}{tag}")
    print(f"   only in upstream: {len(only_up)}")
    for f in only_up[:20]:
        print(f"     {f}")
    ok = not diffs
    if expect_only_other is not None:
        unexpected = [f for f in only_other if f not in expect_only_other]
        if unexpected:
            print(f"   UNEXPECTED extra files in {other_name}: {unexpected[:10]}")
            ok = False
    if expect_only_up is not None and len(only_up) > expect_only_up:
        print(f"   NOTE: {len(only_up)} upstream-only files (expected >0 for the pruned tarball)")
    print(f"   verdict: {'CONTENT-IDENTICAL' if ok else 'DIFFERS -- review'}")
    return ok


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    vendor = os.path.dirname(here)
    repo = os.path.dirname(os.path.dirname(vendor))
    tarball_default = os.path.join(repo, "third_party", "wpewebkit-2.54.0", "Source", "ThirdParty", "skia")
    if not os.path.isdir(tarball_default):
        alt = os.path.expanduser("~/Documents/GitHub/HobbyOS/third_party/wpewebkit-2.54.0/Source/ThirdParty/skia")
        if os.path.isdir(alt):
            tarball_default = alt
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", default=os.path.join(vendor, "src"))
    ap.add_argument("--clone", default=os.path.expanduser("~/webkit-hobbyos/Source/ThirdParty/skia"))
    ap.add_argument("--tarball", default=tarball_default)
    args = ap.parse_args()

    if not os.path.isdir(args.upstream):
        sys.exit(f"upstream tree missing: {args.upstream} (run fetch.sh first)")

    ok = True
    if os.path.isdir(args.clone):
        ok &= compare(args.upstream, args.clone, "fork clone (2.54.0)",
                      expect_only_other=WEBKIT_FILES, expect_only_up=9)
    else:
        print(f"[skip] clone tree not present: {args.clone}")
    if os.path.isdir(args.tarball):
        ok &= compare(args.upstream, args.tarball, "WPE tarball skia subset",
                      expect_only_other=WEBKIT_FILES, expect_only_up=None)
    else:
        print(f"[skip] tarball extraction not present: {args.tarball}")
    print("\nRESULT:", "all checked trees content-identical" if ok else "differences found")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
