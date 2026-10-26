#!/usr/bin/env python3
"""Make a probe-hardened copy of tools/t3_tier.sh for GB-101-V2 clause 4.

Two edits only, both labelled in the copy's header:
  1. `set -euo pipefail` -> `set -uo pipefail` (a failing probe must not abort
     the run; the probe outputs are still written, verdicts unchanged);
  2. ROOT is pinned to this repo, because the copy lives under evidence/.
Everything else — the verdict rules, the counters, the messages, the deadline
logic — is byte-identical to the shipped tier.
"""
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
src = os.path.join(root, "tools/t3_tier.sh")
dst = os.path.join(root, "evidence/gb-101-v2/t3_tier_noerrexit.sh")

s = open(src).read()
assert "set -euo pipefail" in s, "errexit line not found"
s = s.replace("set -euo pipefail", "set -uo pipefail", 1)

anchor = 'ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"'
assert anchor in s, "ROOT line not found"
s = s.replace(anchor, f'ROOT="{root}"', 1)

note = ("# GB-101-V2 HARNESS COPY — differs from tools/t3_tier.sh only by:\n"
        "#   (1) `set -uo pipefail` instead of `set -euo pipefail`, so a failing\n"
        "#       probe_guest substitution cannot abort a run (it did in 3 of 5\n"
        "#       monitored runs on this VM: exit 141, no verdict);\n"
        "#   (2) ROOT pinned to the repo, because this copy sits under evidence/.\n"
        "# Verdict rules, counters and messages are the shipped ones.\n")
lines = s.split("\n")
s = "\n".join([lines[0], note + "\n".join(lines[1:])])

open(dst, "w").write(s)
os.chmod(dst, 0o755)
print(f"[copy] {dst} written ({len(s)} bytes)")