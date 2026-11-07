#!/usr/bin/env python3
"""tools/rw/xml_open_diff.py — did the game OPEN every XML the walker found?

Usage: xml_open_diff.py <host-Data-dir> <serial-log> [<out-file>]

`[XO] <rel>` rows are emitted by the kernel on every successful NFS open of a
path ending in .xml (vfs_open_routed).  This compares that set with the host
tree the export serves:

  * host XMLs the game never opened          -> a loader-side filter / a path
    the enumerator never produced;
  * [XO] paths that are not on the host      -> a wrong path (name truncation);
  * a per-directory table, so a whole subtree going unopened is visible.
"""
import os
import re
import sys

host_root, serial = sys.argv[1], sys.argv[2]
out_path = sys.argv[3] if len(sys.argv) > 3 else None

opened = []
seen = set()
# The guest's serial interleaves kernel rows with the app's writes: an [XO] row
# can be welded onto the tail of a [NFSRTT]/[NFSPIPE] line, so search for the
# marker anywhere and stop the path at the next '[' (8 such rows in one run).
XO_RE = re.compile(r"\[XO\] (/\S*?\.xml)")
with open(serial, "r", errors="replace") as f:
    for line in f:
        for m in XO_RE.finditer(line):
            p = "/NFS" + m.group(1)
            opened.append(p)
            seen.add(p)

host = set()
for root, dirs, files in os.walk(host_root):
    rel = os.path.relpath(root, host_root)
    base = "/NFS/Data" if rel == "." else "/NFS/Data/" + rel.replace(os.sep, "/")
    for fn in files:
        if fn.lower().endswith(".xml"):
            host.add(base + "/" + fn)

missing = sorted(host - seen)
extra = sorted(seen - host)

lines = []
lines.append("host XMLs: %d   opened (unique): %d   open rows: %d"
             % (len(host), len(seen), len(opened)))
lines.append("never opened: %d   opened-but-not-on-host: %d"
             % (len(missing), len(extra)))
if missing:
    lines.append("\n-- host XMLs the game never opened --")
    lines.extend(missing[:200])
if extra:
    lines.append("\n-- opened paths not on the host (truncated names?) --")
    lines.extend(extra[:50])
# Per-directory coverage, only where it is short.
per = {}
for p in host:
    d = p.rsplit("/", 1)[0]
    per.setdefault(d, [0, 0])[0] += 1
for p in seen:
    d = p.rsplit("/", 1)[0]
    per.setdefault(d, [0, 0])[1] += 1
short = ["%s host=%d opened=%d" % (d, h, o) for d, (h, o) in sorted(per.items()) if o < h]
if short:
    lines.append("\n-- directories with unopened XMLs --")
    lines.extend(short[:200])

sd = sorted(p for p in seen if "/SoundDefs/" in p)
lines.append("\n-- SoundDefs opens: %d unique --" % len(sd))
lines.extend(os.path.basename(p) for p in sd)

text = "\n".join(lines) + "\n"
if out_path:
    with open(out_path, "w") as f:
        f.write(text)
print(text)
