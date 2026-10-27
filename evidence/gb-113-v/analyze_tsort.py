#!/usr/bin/env python3
"""GB-113-V log analysis: wait-word lines, exit-status vs content failures,
clean-exit vs signal cases, per wave."""
import re
import sys

path = sys.argv[1]
lines = open(path, "rb").read().decode("utf-8", "replace").splitlines()

raw_re = re.compile(r"raw=0x([0-9a-fA-F]+)")
words = []
for i, l in enumerate(lines):
    m = raw_re.search(l)
    if m:
        words.append((i + 1, int(m.group(1), 16), l.strip()))

ts = [l for l in lines if "TSORTTEST" in l]
fails = [l for l in ts if "FAIL" in l and "FAILURES" not in l]
summary = [l for l in ts if "FAILURES" in l]
passes = [l for l in ts if "PASS" in l]
exit_fail = [l for l in fails if "exit status" in l]

print(f"file: {path}")
print(f"total lines: {len(lines)}   halted={'System halt' in ' '.join(lines)}")
print(f"TSORTTEST summary lines: {len(summary)} -> {summary[:6]}")
print(f"TSORTTEST checks: PASS={len(passes)} FAIL={len(fails)}")
print(f"  FAIL with 'exit status': {len(exit_fail)} (the SIGPIPE/decode family)")
print(f"wait-word lines: {len(words)}")
buckets = {}
for _, w, _ in words:
    buckets[w] = buckets.get(w, 0) + 1
print(f"  word histogram: { {hex(k): v for k, v in sorted(buckets.items())} }")
print(f"  clean exits (0x00000000): {buckets.get(0, 0)}")
print(f"  signal deaths (low7 != 0): {sum(v for k, v in buckets.items() if k & 0x7F)}")
print("\n--- first 8 wait-word lines with their preceding check line ---")
for ln, w, txt in words[:8]:
    prev = lines[ln - 2].strip()[:120] if ln >= 2 else ""
    print(f"{ln}: prev={prev!r} -> {txt}")
print("\n--- every FAIL line (check-level) ---")
for l in fails:
    print(l.strip()[:200])
print("\n--- every exit-status FAIL line ---")
for l in exit_fail:
    print(l.strip()[:200])