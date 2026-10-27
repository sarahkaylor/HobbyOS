#!/usr/bin/env python3
"""GB-117 gate: run claim_age.py --self-test (deterministic, no network)."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "board", "claim_age.py")


def main():
    r = subprocess.run([sys.executable, TOOL, "--self-test"],
                       capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    ok = (r.returncode == 0) and ("CLAIM_AGE_TEST: ALL PASSED" in out)
    tail = out.strip().splitlines()
    print((tail[-1] if tail else "") or "claim_age self-test: exit 0 but no PASS verdict")
    print("CLAIM_AGE_GATE:", "ALL PASSED" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
