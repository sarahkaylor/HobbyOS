#!/usr/bin/env python3
"""De-interleave one two-writer console line.

The HobbyOS serial console interleaves concurrent writers character by character
(GB-082's corruption family).  When one of the two lines is known verbatim, its
characters can be peeled out in order, leaving the other line intact.

Usage: deinterleave.py <logfile> <line-number> "<known other line>"
"""
import sys

path, lineno, other = sys.argv[1], int(sys.argv[2]), sys.argv[3]
line = open(path, "rb").read().split(b"\n")[lineno - 1].decode("utf-8", "replace")
line = line.replace("\r", "")

out = []
i = 0
for ch in line:
    if i < len(other) and ch == other[i]:
        i += 1          # this char belongs to the known line
    else:
        out.append(ch)

print(f"merged  : {line}")
print(f"peeled  : {other[:i]}   ({i}/{len(other)} chars matched)")
print(f"leftover: {''.join(out)}")