#!/usr/bin/env python3
"""Canonical content manifest for a source tree (Skia fetch-pin gate).

Emits one digest that is stable across re-extractions of the same git tree,
regardless of tarball container bytes: googlesource "+archive" gzip output is
NOT byte-reproducible (observed 2026-09-30 on skia@588b550a: two downloads of
the same commit gave container sha256 307628b6... and 3f2de0db...).

Manifest rule (deterministic):
  line    := "<relpath>\\t<sha256-of-file-bytes>"   for regular files
           | "<relpath>\\tLINK:<readlink>"          for symlinks
  digest  := sha256 of the byte-sorted, newline-joined lines (UTF-8, "\\n")

Usage:  manifest.py <tree-root> [--exclude-path RELPATH]... [--list OUT.FILE]
Prints: "manifest_sha256=<digest>  files=<n>  root=<root>"

--exclude-path skips EXACT root-relative paths (repeated for each). The Skia
recipe excludes the three WebKit-authored files overlaid by fetch.sh
(CMakeLists.txt, WebKitSkiaConfig.h, README.WebKit) so the digest always
describes the pristine upstream tree whether or not the overlay has run.
"""
import hashlib
import os
import sys


def manifest(root, excludes):
    lines = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root)
            if rel in excludes:
                continue
            if os.path.islink(p):
                lines.append(f"{rel}\tLINK:{os.readlink(p)}")
            else:
                h = hashlib.sha256()
                with open(p, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        h.update(chunk)
                lines.append(f"{rel}\t{h.hexdigest()}")
    lines.sort()
    blob = "\n".join(lines) + "\n"
    return hashlib.sha256(blob.encode("utf-8")).hexdigest(), lines


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit("usage: manifest.py <tree-root> [--exclude-path REL]... [--list OUT]")
    root = args[0]
    excludes = set()
    out = None
    i = 1
    while i < len(args):
        if args[i] == "--exclude-path":
            excludes.add(args[i + 1])
            i += 2
        elif args[i] == "--list":
            out = args[i + 1]
            i += 2
        else:
            sys.exit(f"unknown argument: {args[i]}")
    digest, lines = manifest(root, excludes)
    if out:
        with open(out, "w") as fh:
            fh.write("\n".join(lines) + "\n")
    print(f"manifest_sha256={digest}  files={len(lines)}  root={root}")


if __name__ == "__main__":
    main()
