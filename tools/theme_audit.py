#!/usr/bin/env python3
"""
theme_audit.py - Lane V helper: collect color literals across first-party
UI surfaces.  Report-only (no conversions) -- feeds the audit table in
THEME-VERIFY-REPORT.md.

Scans:
  src/user/*.c            (apps, WM-adjacent shells)
  src/user/graphics/*     (window.c / graphics.c / dialog / filedialog)
  src/user/x11/**         (xcalc, xeyes, antfarm -- intentional art)
  src/user/nano/**        (nano -- own palette)
  src/user/pong.c, src/user/millipede.c  (games -- intentional art)
  src/host/*_test.c       (host-side tests: test-local colors)

Patterns: COLOR(r,g,b) calls, literal RGB()/setcolor() calls, and bare
hex/rgb color constants assigned to colors.
"""
import os
import re
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

COLOR_RE = re.compile(r"COLOR\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)")
HEX_RE = re.compile(r"0x([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})\b")
RGBFN_RE = re.compile(r"(?:rgb|setcolor|set_pen|pen|color)\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)", re.I)
TUPLE_RE = re.compile(r"=\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)")

def scan(path):
    hits = []
    try:
        with open(path, "r") as f:
            for i, line in enumerate(f, 1):
                if "#include" in line or "extern" in line and "COLOR" not in line:
                    continue
                colors = []
                for m in COLOR_RE.finditer(line):
                    colors.append("COLOR(%d,%d,%d)" % tuple(int(x) for x in m.groups()))
                for m in HEX_RE.finditer(line):
                    colors.append(m.group(0))
                for m in RGBFN_RE.finditer(line):
                    colors.append("rgb(%s,%s,%s)" % m.groups())
                if colors:
                    hits.append((i, line.rstrip(), sorted(set(colors))))
    except OSError as e:
        print(f"ERR {path}: {e}", file=sys.stderr)
    return hits

def main():
    targets = []
    for pattern in ["src/user/*.c", "src/user/graphics/*.c",
                    "src/user/x11/**/*.c", "src/user/x11/*.c",
                    "src/user/nano/*.c", "src/host/*_test.c",
                    "src/user_include/**/*.h"]:
        targets.extend(sorted(glob.glob(os.path.join(ROOT, pattern), recursive=True)))
    # de-dupe, keep order
    seen = set()
    files = []
    for t in targets:
        if t not in seen:
            seen.add(t)
            files.append(t)
    files = sorted(files)

    total_hits = 0
    for path in files:
        rel = os.path.relpath(path, ROOT)
        hits = scan(path)
        if not hits:
            continue
        print(f"== {rel} ({len(hits)} lines)")
        for ln, line, colors in hits:
            total_hits += 1
            print(f"  {ln}: {line.strip()[:150]}")
            print(f"     -> {', '.join(colors)}")
    print(f"TOTAL colored-lines: {total_hits} across {len(files)} files scanned")

if __name__ == "__main__":
    main()
