#!/usr/bin/env python3
"""tools/rw/walk_diff.py — diff the guest's [W] walk rows against the host tree.

Usage: walk_diff.py <host-Data-dir> <serial-log> <out-file>

Guest rows are `[W] f <guest path> <size>` and `[W] d <path>`, where the guest
path is /NFS/Data/... .  The host side is the same tree the export serves, so a
missing guest row is a file the guest could not enumerate or stat.

Reports: counts, missing files (host - guest), extra files, size mismatches,
truncated names (a guest name that is a strict prefix of exactly one host
name), and per-directory deltas.
"""
import os
import re
import sys

host_root, serial, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

guest_files = {}
guest_dirs = set()
with open(serial, "r", errors="replace") as f:
    for line in f:
        if not line.startswith("[W] "):
            continue
        parts = line.rstrip("\n")
        if parts.startswith("[W] f "):
            rest = parts[6:]
            path, _, size = rest.rpartition(" ")
            guest_files[path] = size
        elif parts.startswith("[W] d "):
            guest_dirs.add(parts[6:])

prefix = "/NFS/Data"
host_files = {}
host_dirs = set()
for root, dirs, files in os.walk(host_root):
    rel = os.path.relpath(root, host_root)
    gdir = prefix if rel == "." else prefix + "/" + rel.replace(os.sep, "/")
    host_dirs.add(gdir)
    for fn in files:
        gp = gdir + "/" + fn
        host_files[gp] = os.path.getsize(os.path.join(root, fn))

missing = sorted(set(host_files) - set(guest_files))
extra = sorted(set(guest_files) - set(host_files))
size_bad = sorted(p for p in (set(host_files) & set(guest_files))
                  if str(host_files[p]) != guest_files[p])
dirs_missing = sorted(host_dirs - guest_dirs)

# A truncated name: the guest row's last component is a strict prefix of a
# host basename in the same directory and no host file has that exact name.
trunc = []
host_by_dir = {}
for p in host_files:
    d, n = p.rsplit("/", 1)
    host_by_dir.setdefault(d, []).append(n)
for p in guest_files:
    d, n = p.rsplit("/", 1)
    if p in host_files:
        continue
    cands = [h for h in host_by_dir.get(d, []) if h.startswith(n) and len(h) > len(n)]
    if cands:
        trunc.append((p, cands))

lines = []
lines.append("host files: %d  guest files: %d" % (len(host_files), len(guest_files)))
lines.append("host dirs : %d  guest dirs : %d" % (len(host_dirs), len(guest_dirs)))
lines.append("missing files: %d   extra files: %d   size mismatches: %d"
             % (len(missing), len(extra), len(size_bad)))
lines.append("missing dirs : %d   truncated names: %d" % (len(dirs_missing), len(trunc)))
if dirs_missing:
    lines.append("\n-- missing dirs --")
    lines.extend(dirs_missing[:200])
if missing:
    lines.append("\n-- missing files --")
    lines.extend(missing[:400])
if size_bad:
    lines.append("\n-- size mismatches --")
    lines.extend("%s host=%s guest=%s" % (p, host_files[p], guest_files[p])
                 for p in size_bad[:200])
if trunc:
    lines.append("\n-- truncated names --")
    lines.extend("%s  ->  %s" % (p, c) for p, c in trunc[:200])
if extra:
    lines.append("\n-- extra files --")
    lines.extend(extra[:100])

# Per-directory delta, only for directories that differ.
deltas = []
for d in sorted(host_dirs):
    hn = sum(1 for p in host_files if p.rsplit("/", 1)[0] == d)
    gn = sum(1 for p in guest_files if p.rsplit("/", 1)[0] == d and p in host_files)
    if hn != gn:
        deltas.append("%s host=%d guest=%d" % (d, hn, gn))
if deltas:
    lines.append("\n-- per-directory deltas --")
    lines.extend(deltas[:200])

text = "\n".join(lines) + "\n"
with open(out_path, "w") as f:
    f.write(text)
print(text)
